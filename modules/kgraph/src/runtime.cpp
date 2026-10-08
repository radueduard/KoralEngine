//
// kgraph: a graph, running. Nodes are worked out in the order their wires need, each built again only when what
// goes into it changed; the passes among them are put in the scene's frame graph, which orders them by what they
// read and write.
//

#include <kgraph/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <set>

#include <builderDescriptions.h>
#include <capiInterop.h>
#include <commandBuffer.h>
#include <descriptorSet.h>
#include <frameGraph.h>
#include <koral_c.h>

#include "names.h"

namespace kgraph
{
    Resource::~Resource()
    {
        if (handle) koral_resource_release(handle);
    }

    namespace {
        namespace kb = kor::builders;

        /** What one input of a node comes to: a value, or the resource it is wired to. */
        struct Resolved {
            Value value;
            std::shared_ptr<Resource> resource;
            bool linked = false;
        };
        using Inputs = std::map<std::string, Resolved>;

        struct Output {
            Value value;
            std::shared_ptr<Resource> resource;
        };

        template <typename T> using Expected = std::expected<T, std::string>;
        std::unexpected<std::string> Fail(std::string message) { return std::unexpected(std::move(message)); }

        /** A resource a pass uses, under the name it is imported into the frame graph by. */
        struct Use {
            std::string name;
            std::shared_ptr<Resource> resource;     // the image or buffer itself: a view's source
            bool image = true;
            bool read = true, write = false;
        };

        /** What a pipeline node worked out besides the pipeline. */
        struct PipelineInfo {
            kor::Resource<kor::DescriptorSet> set;  // set 0: what is wired, bound once
            bool hasSet = false;
            std::vector<Use> uses;
            std::vector<std::string> perEntity;     // what is not wired: set 1, an entity's to supply
            std::vector<Port> ports;                // its shaders' parameters and constants, as inputs
        };

        struct PassContext {
            kor::ResourceRef<const kor::Image> screen;
        };

        /** What a pass node comes to: what it uses, and what it records. */
        struct PassPlan {
            std::vector<Use> uses;
            bool readsScreen = false, writesScreen = false;
            std::function<void(kor::CommandBuffer&, const PassContext&)> record;

            /** What it declares to the frame graph, as text: the graph is worked out again when this changes. */
            [[nodiscard]] std::string Declaration() const
            {
                std::string text = std::format("{}{}", readsScreen ? "r" : "", writesScreen ? "w" : "");
                for (const auto& use : uses) text += std::format("|{}:{}{}", use.name, use.read ? "r" : "", use.write ? "w" : "");
                return text;
            }
        };

        class GraphPass final : public kor::RenderPass {
        public:
            GraphPass(std::string name, std::shared_ptr<const PassPlan> plan) : RenderPass(std::move(name)), plan(std::move(plan)) {}

            void Setup(kor::PassBuilder& builder) override
            {
                bool writes = plan->writesScreen;
                for (const auto& use : plan->uses) {
                    if (use.write) { builder.Write(use.name); writes = true; }
                    else builder.Read(use.name);
                }
                if (plan->readsScreen) builder.Read(kor::FrameGraph::Screen, kor::Image::Usage::eTransferSrc);
                if (plan->writesScreen) builder.Write(kor::FrameGraph::Screen, kor::Image::Usage::eTransferDst);
                if (!writes) builder.SideEffect();   // it only reads: kept all the same, since it was asked for
            }
            void Initialize(const kor::PassResources& resources) override
            {
                if (plan->readsScreen || plan->writesScreen) _context.screen = resources.ImageNamed(kor::FrameGraph::Screen);
            }
            void Record(kor::CommandBuffer& commands) const override
            {
                if (plan->record) plan->record(commands, _context);
            }

            std::shared_ptr<const PassPlan> plan;

        private:
            PassContext _context;
        };

        // ---- values -------------------------------------------------------------------------------------------

        Expected<Value> Combine(const std::string& op, const Value& a, const Value& b)
        {
            const auto apply = [&](const double x, const double y) -> double {
                if (op == "Add") return x + y;
                if (op == "Subtract") return x - y;
                if (op == "Multiply") return x * y;
                if (op == "Divide") return x / y;
                if (op == "Min") return std::min(x, y);
                return std::max(x, y);
            };
            const double* x = a.Number();
            const double* y = b.Number();
            const Value::List* xs = a.Items();
            const Value::List* ys = b.Items();
            if (x && y) return Value(apply(*x, *y));
            if ((xs || x) && (ys || y)) {
                const std::size_t count = xs ? xs->size() : ys->size();
                if (xs && ys && xs->size() != ys->size()) return Fail(std::format("a vector of {} and one of {} do not go together", xs->size(), ys->size()));
                Value::List out;
                for (std::size_t i = 0; i < count; ++i) {
                    const double* l = xs ? (*xs)[i].Number() : x;
                    const double* r = ys ? (*ys)[i].Number() : y;
                    if (!l || !r) return Fail("a vector's components are numbers");
                    out.emplace_back(apply(*l, *r));
                }
                return Value(std::move(out));
            }
            return Fail("'a' and 'b' are numbers, or vectors of them");
        }

        Expected<Value> Map(const std::string& op, const Value& a)
        {
            const auto apply = [&](const double x) -> double {
                if (op == "Negate") return -x;
                if (op == "Abs") return std::abs(x);
                if (op == "Floor") return std::floor(x);
                if (op == "Ceil") return std::ceil(x);
                return std::round(x);
            };
            if (const double* x = a.Number()) return Value(apply(*x));
            if (const auto* xs = a.Items()) {
                Value::List out;
                for (const auto& item : *xs) {
                    if (!item.Number()) return Fail("a vector's components are numbers");
                    out.emplace_back(apply(*item.Number()));
                }
                return Value(std::move(out));
            }
            return Fail("'a' is a number, or a vector of them");
        }

        // ---- a node's values, as a builder takes them -------------------------------------------------------------

        Expected<std::uint64_t> Enumerator(const kb::Type& type, const Value& value)
        {
            if (const double* number = value.Number()) return static_cast<std::uint64_t>(*number);
            const std::string* name = value.String();
            if (!name || !type.enumeration) return Fail(std::format("one of {}'s values, by name", type.name));
            for (const auto& entry : type.enumeration->values)
                if (entry.name == *name) return entry.value;
            std::string known;
            for (const auto& entry : type.enumeration->values) known += std::format("{}{}", known.empty() ? "" : ", ", entry.name);
            return Fail(std::format("'{}' is none of {}'s values: {}", *name, type.name, known));
        }

        Expected<kb::Value> Convert(const kb::Type& type, const Resolved& input);

        Expected<kb::Value> ConvertValue(const kb::Type& type, const Value& value)
        {
            return Convert(type, Resolved { value, nullptr, false });
        }

        Expected<kb::Value> Convert(const kb::Type& type, const Resolved& input)
        {
            using kb::Kind;
            const Value& value = input.value;
            if (type.kind == Kind::eResource) {
                if (!input.resource) {
                    if (!input.linked && value.IsNull()) return kb::Value {};   // not given: null, where a builder takes that
                    return Fail(std::format("a {} is wired in, from the node that makes it", type.name));
                }
                if (input.resource->screen) return Fail("the screen is written by a pass, and cannot be given here");
                if (type.name != "Resource" && input.resource->kind != type.name)
                    return Fail(std::format("takes a {}, and was given a {}", type.name, input.resource->kind));
                return kb::Value::Of(input.resource->handle);
            }
            if (input.resource) return Fail("takes a value, and was wired to a resource");
            if (value.IsNull()) return kb::Value {};
            switch (type.kind) {
            case Kind::eBool:
                if (const bool* b = value.Bool()) return kb::Value::Of(*b);
                return Fail("true or false");
            case Kind::eInt:
                if (const double* n = value.Number()) return kb::Value::Of(static_cast<std::int64_t>(std::llround(*n)));
                return Fail("a number");
            case Kind::eUInt:
                if (const double* n = value.Number(); n && *n >= 0) return kb::Value::Of(static_cast<std::uint64_t>(std::llround(*n)));
                return Fail("a number, not below zero");
            case Kind::eFloat:
                if (const double* n = value.Number()) return kb::Value::Of(*n);
                return Fail("a number");
            case Kind::eString:
                if (const std::string* s = value.String()) return kb::Value::Of(*s);
                return Fail("text");
            case Kind::eEnum:
            case Kind::eChoice: {
                const auto entry = Enumerator(type, value);
                if (!entry) return Fail(entry.error());
                return kb::Value::Of(*entry);
            }
            case Kind::eFlags: {
                // Any of its values: one name, or a list of them.
                std::uint64_t bits = 0;
                if (const auto* names = value.Items()) {
                    for (const auto& name : *names) {
                        const auto entry = Enumerator(type, name);
                        if (!entry) return Fail(entry.error());
                        bits |= *entry;
                    }
                } else {
                    const auto entry = Enumerator(type, value);
                    if (!entry) return Fail(entry.error());
                    bits = *entry;
                }
                return kb::Value::Of(bits);
            }
            case Kind::eArray:
            case Kind::eList: {
                const auto* items = value.Items();
                if (!items) return Fail(type.kind == Kind::eArray ? std::format("a list of {}", type.length) : "a list");
                if (type.kind == Kind::eArray && items->size() != type.length) return Fail(std::format("a list of {}, and was given {}", type.length, items->size()));
                std::vector<kb::Value> out;
                for (const auto& item : *items) {
                    auto converted = ConvertValue(*type.element, item);
                    if (!converted) return converted;
                    out.push_back(std::move(*converted));
                }
                return kb::Value::List(std::move(out));
            }
            case Kind::eStruct: {
                // Its fields, by name; one not named is zero.
                const auto* members = value.Named();
                if (!members) return Fail(std::format("the fields of a {}, by name", type.name));
                for (const auto& [name, member] : *members)
                    if (std::ranges::find(type.fields, name, &kb::Field::name) == type.fields.end())
                        return Fail(std::format("a {} has no field '{}'", type.name, name));
                std::vector<kb::Value> out;
                for (const auto& field : type.fields) {
                    const Value* member = value.Find(field.name);
                    if (!member) { out.emplace_back(); continue; }
                    auto converted = ConvertValue(*field.type, *member);
                    if (!converted) return Fail(std::format("{}: {}", field.name, converted.error()));
                    out.push_back(std::move(*converted));
                }
                return kb::Value::List(std::move(out));
            }
            case Kind::eUnion: {
                // One of its fields: the member given says which.
                const auto* members = value.Named();
                if (!members || members->size() != 1) return Fail("one of its alternatives, by name");
                for (std::size_t i = 0; i < type.fields.size(); ++i) {
                    if (type.fields[i].name != (*members)[0].first) continue;
                    auto converted = ConvertValue(*type.fields[i].type, (*members)[0].second);
                    if (!converted) return converted;
                    return kb::Value::List({ kb::Value::Of(static_cast<std::uint64_t>(i)), std::move(*converted) });
                }
                return Fail(std::format("it has no alternative '{}'", (*members)[0].first));
            }
            default:
                return Fail("cannot be given in a graph");
            }
        }

        /** The image a resource is, or is a view of. */
        std::shared_ptr<Resource> ImageBehind(const std::shared_ptr<Resource>& resource)
        {
            if (!resource) return nullptr;
            if (resource->kind == "Image") return resource;
            if (resource->kind == "ImageView") return ImageBehind(resource->source);
            return nullptr;
        }
        std::shared_ptr<Resource> BufferBehind(const std::shared_ptr<Resource>& resource)
        {
            if (!resource) return nullptr;
            if (resource->kind == "Buffer") return resource;
            if (resource->kind == "BufferView") return BufferBehind(resource->source);
            return nullptr;
        }
        std::string ImportName(const Resource& resource) { return "kgraph." + resource.node; }
    }

    // ---- the runtime ---------------------------------------------------------------------------------------------

    struct Runtime::Impl {
        struct Built {
            std::string signature;
            NodeStatus status;
            std::map<std::string, Output, std::less<>> outputs;
            std::uint64_t stamp = 0;                    // changes whenever it is built again: what those wired to it watch
            std::shared_ptr<PipelineInfo> pipeline;
            std::shared_ptr<PassPlan> pass;
            kor::ResourceRef<const kor::Shader> watched;  // a shader node's shader, told of when it is edited
            kor::u64 watch = 0;
        };

        kor::FrameGraph& frameGraph;
        std::map<std::string, Built, std::less<>> built;
        std::map<std::string, GraphPass*, std::less<>> passes;
        std::vector<std::string> passOrder;
        std::uint64_t counter = 0;
        std::atomic<bool> reloaded = false;

        explicit Impl(kor::FrameGraph& graph) : frameGraph(graph) {}

        void Unwatch(Built& node)
        {
            if (node.watch != 0 && node.watched.Alive()) const_cast<kor::Shader&>(*node.watched).UnregisterReloadCallback(node.watch);
            node.watch = 0;
            node.watched = {};
        }

        ~Impl()
        {
            for (auto& node : built | std::views::values) Unwatch(node);
            for (auto* pass : passes | std::views::values) frameGraph.Remove(*pass);
        }

        // ---- one node ---------------------------------------------------------------------------------------

        /** The settings a builder node was given, as calls on its builder. */
        Expected<std::vector<kb::Call>> Calls(const kb::Description& description, const Inputs& inputs, std::vector<kb::Value>& constructor,
                                              const std::function<bool(std::string_view)>& handledElsewhere)
        {
            std::set<std::string> used;
            for (const auto& field : description.constructor) {
                const auto it = inputs.find(std::string(field.name));
                auto value = Convert(*field.type, it == inputs.end() ? Resolved {} : it->second);
                if (!value) return Fail(std::format("'{}' {}", field.name, value.error()));
                if (it != inputs.end()) used.insert(it->first);
                constructor.push_back(std::move(*value));
            }
            std::vector<kb::Call> calls;
            for (const auto& setting : description.settings) {
                // How many times it is given: once, or — for one given more than once — up to the highest index named.
                int count = setting.repeatable ? 0 : 1;
                if (setting.repeatable)
                    for (const auto& name : inputs | std::views::keys)
                        if (name.starts_with(std::string(setting.name) + "["))
                            count = std::max(count, std::atoi(name.c_str() + setting.name.size() + 1) + 1);
                for (int index = 0; index < count; ++index) {
                    kb::Call call { &setting, {} };
                    bool given = false;
                    for (const auto& argument : setting.arguments) {
                        const std::string name = InputName(setting, argument, setting.repeatable ? index : -1);
                        const auto it = inputs.find(name);
                        if (it == inputs.end()) { call.arguments.emplace_back(); continue; }
                        auto value = Convert(*argument.type, it->second);
                        if (!value) return Fail(std::format("'{}' {}", name, value.error()));
                        used.insert(name);
                        given = true;
                        call.arguments.push_back(std::move(*value));
                    }
                    if (given) calls.push_back(std::move(call));
                }
            }
            for (const auto& name : inputs | std::views::keys)
                if (!used.contains(name) && !handledElsewhere(name)) return Fail(std::format("a {} has no input '{}'", description.name, name));
            return calls;
        }

        Expected<std::shared_ptr<Resource>> BuildWith(const kb::Description& description, const std::string& nodeId, const std::vector<kb::Value>& constructor,
                                                      const std::vector<kb::Call>& calls)
        {
            const auto made = kb::Build(description, constructor, calls);
            if (!made) return Fail(made.error().message);
            auto resource = std::make_shared<Resource>();
            resource->handle = *made;
            resource->kind = std::string(description.result);
            resource->node = nodeId;
            if (!koral_resource_valid(*made)) {
                const char* why = koral_resource_error_message(*made);
                return Fail(why && *why ? std::string(why) : std::format("the {} could not be built", description.name));
            }
            return resource;
        }

        Expected<void> BuildBuilder(const Node& node, const kb::Description& description, const Inputs& inputs, Built& out)
        {
            std::vector<kb::Value> constructor;
            const auto calls = Calls(description, inputs, constructor, [](std::string_view) { return false; });
            if (!calls) return Fail(calls.error());
            auto resource = BuildWith(description, node.id, constructor, *calls);
            if (!resource) return Fail(resource.error());
            // A view is of something: kept, for a pass that uses the view to say it uses that.
            if (description.name == "ImageView") if (const auto it = inputs.find("image"); it != inputs.end()) (*resource)->source = it->second.resource;
            if (description.name == "BufferView") if (const auto it = inputs.find("buffer"); it != inputs.end()) (*resource)->source = it->second.resource;
            if (description.name == "Shader") {
                // Told when the file is edited: what was built from it is built again (Update).
                out.watched = kor::capi::ShaderOf((*resource)->handle);
                out.watch = const_cast<kor::Shader&>(*out.watched).RegisterReloadCallback([this] { reloaded = true; });
            }
            out.outputs["out"].resource = std::move(*resource);
            return {};
        }

        /**
         * A pipeline: its shaders' parameters are split by what is wired. What is wired goes in set 0, which the
         * pipeline keeps a descriptor set for; what is not goes in set 1, for each entity to supply.
         */
        Expected<void> BuildPipeline(const Node& node, const kb::Description& description, const Inputs& inputs, Built& out)
        {
            auto info = std::make_shared<PipelineInfo>();

            // Every parameter and constant its shaders declare, once each.
            struct Parameter { std::string name; kor::DescriptorType type; kor::Shader::AccessKind access; };
            std::vector<Parameter> parameters;
            std::vector<kor::Shader::SpecializationConstant> constants;
            for (const auto& input : inputs | std::views::values) {
                if (!input.resource || input.resource->kind != "Shader") continue;
                const auto shader = kor::capi::ShaderOf(input.resource->handle);
                for (const auto& set : shader->BlockLayout().descriptorSets | std::views::values)
                    for (const auto& descriptor : set.descriptors | std::views::values) {
                        const std::string& name = descriptor.name.empty() ? descriptor.blockName : descriptor.name;
                        const auto known = std::ranges::find(parameters, name, &Parameter::name);
                        if (known == parameters.end()) parameters.push_back({ name, descriptor.type, descriptor.access });
                        else if (known->access != descriptor.access) known->access = kor::Shader::AccessKind::eReadWrite;
                    }
                for (const auto& constant : shader->BlockLayout().specializationConstants | std::views::values)
                    if (!constant.name.empty() && std::ranges::find(constants, constant.name, &kor::Shader::SpecializationConstant::name) == constants.end())
                        constants.push_back(constant);
            }
            const auto isImage = [](const kor::DescriptorType type) {
                return type == kor::DescriptorType::eSampledImage || type == kor::DescriptorType::eStorageImage || type == kor::DescriptorType::eCombinedImageSampler;
            };
            for (const auto& parameter : parameters) {
                const char* kind = isImage(parameter.type) ? "Image"
                    : parameter.type == kor::DescriptorType::eSampler ? "Sampler"
                    : parameter.type == kor::DescriptorType::eAccelerationStructure ? "AccelerationStructure"
                    : parameter.type == kor::DescriptorType::eUniformTexelBuffer || parameter.type == kor::DescriptorType::eStorageTexelBuffer ? "BufferView" : "Buffer";
                info->ports.push_back({ "param." + parameter.name, PortKind::eResource, kind, false, {}, nullptr });
                if (parameter.type == kor::DescriptorType::eCombinedImageSampler)
                    info->ports.push_back({ "param." + parameter.name + ".sampler", PortKind::eResource, "Sampler", false, {}, nullptr });
            }
            for (const auto& constant : constants)
                info->ports.push_back({ "constant." + constant.name, PortKind::eValue, constant.scalar == 3 ? "bool" : "number", false, {}, nullptr });
            out.pipeline = info;    // its inputs are known from here, built or not

            const auto ours = [&](const std::string_view name) {
                return std::ranges::any_of(info->ports, [&](const Port& port) { return port.name == name; });
            };
            for (const auto& name : inputs | std::views::keys)
                if ((name.starts_with("param.") || name.starts_with("constant.")) && !ours(name)) {
                    std::string known;
                    for (const auto& port : info->ports) known += std::format("{}{}", known.empty() ? "" : ", ", port.name);
                    return Fail(std::format("its shaders have no '{}'. They have: {}", name, known.empty() ? "nothing" : known));
                }

            std::vector<kb::Value> constructor;
            auto calls = Calls(description, inputs, constructor, ours);
            if (!calls) return Fail(calls.error());

            // The split, as bindings.
            const kb::Setting* binding = description.FindSetting("binding");
            const kb::Setting* constantSetting = description.FindSetting("specializationConstantNamed");
            std::uint64_t wired = 0, unwired = 0;
            std::vector<const Parameter*> bound;
            for (const auto& parameter : parameters) {
                const auto it = inputs.find("param." + parameter.name);
                const bool given = it != inputs.end() && it->second.resource;
                calls->push_back({ binding, { kb::Value::Of(parameter.name), kb::Value::Of(given ? std::uint64_t { 0 } : std::uint64_t { 1 }),
                                              kb::Value::Of(given ? wired++ : unwired++) } });
                if (given) bound.push_back(&parameter);
                else info->perEntity.push_back(parameter.name);
            }
            for (const auto& constant : constants) {
                const auto it = inputs.find("constant." + constant.name);
                if (it == inputs.end() || it->second.value.IsNull()) continue;
                const Value& value = it->second.value;
                std::vector<std::byte> bytes(constant.size);
                const double* number = value.Number();
                const bool* flag = value.Bool();
                if (!number && !flag) return Fail(std::format("'constant.{}' is a number, or true or false", constant.name));
                const double n = number ? *number : (*flag ? 1.0 : 0.0);
                if (constant.scalar == 0 && constant.size == 4) { const auto v = static_cast<float>(n); std::memcpy(bytes.data(), &v, 4); }
                else if (constant.scalar == 4 || (constant.scalar == 0 && constant.size == 8)) { std::memcpy(bytes.data(), &n, 8); }
                else if (constant.size == 8) { const auto v = static_cast<std::int64_t>(std::llround(n)); std::memcpy(bytes.data(), &v, 8); }
                else { const auto v = static_cast<std::int32_t>(std::llround(n)); std::memcpy(bytes.data(), &v, 4); }
                calls->push_back({ constantSetting, { kb::Value::Of(constant.name), kb::Value::Of(std::move(bytes)) } });
            }

            auto resource = BuildWith(description, node.id, constructor, *calls);
            if (!resource) return Fail(resource.error());
            KoralResource* handle = (*resource)->handle;
            const kor::ResourceRef<const kor::Pipeline> pipeline =
                description.name == "ComputePipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::ComputePipelineOf(handle))
                : description.name == "GraphicsPipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::GraphicsPipelineOf(handle))
                : kor::ResourceRef<const kor::Pipeline>(kor::capi::RayTracingPipelineOf(handle));

            // What is wired, bound once.
            if (!bound.empty()) {
                kor::DescriptorSet::Builder set(pipeline, 0);
                for (const Parameter* parameter : bound) {
                    const std::string port = "param." + parameter->name;
                    const auto& given = inputs.at(port).resource;
                    if (given->screen) return Fail(std::format("'{}': the screen is written by a pass, and cannot be a parameter", port));
                    const std::string& kind = given->kind;
                    const auto wrong = [&](const char* takes) { return Fail(std::format("'{}' takes {}, and was given a {}", port, takes, kind)); };
                    switch (parameter->type) {
                    case kor::DescriptorType::eSampler:
                        if (kind != "Sampler") return wrong("a Sampler");
                        set.Write(parameter->name, kor::capi::SamplerOf(given->handle));
                        break;
                    case kor::DescriptorType::eSampledImage:
                    case kor::DescriptorType::eStorageImage:
                        if (kind == "Image") set.Write(parameter->name, kor::capi::ImageOf(given->handle));
                        else if (kind == "ImageView") set.Write(parameter->name, kor::capi::ImageViewOf(given->handle));
                        else return wrong("an Image or an ImageView");
                        break;
                    case kor::DescriptorType::eCombinedImageSampler: {
                        const auto sampler = inputs.find(port + ".sampler");
                        if (sampler == inputs.end() || !sampler->second.resource || sampler->second.resource->kind != "Sampler")
                            return Fail(std::format("'{}' is sampled: '{}.sampler' needs a Sampler", port, port));
                        if (kind == "Image") set.Write(parameter->name, kor::capi::ImageOf(given->handle), kor::capi::SamplerOf(sampler->second.resource->handle));
                        else if (kind == "ImageView") set.Write(parameter->name, kor::capi::ImageViewOf(given->handle), kor::capi::SamplerOf(sampler->second.resource->handle));
                        else return wrong("an Image or an ImageView");
                        break;
                    }
                    case kor::DescriptorType::eUniformBuffer:
                    case kor::DescriptorType::eStorageBuffer:
                        if (kind != "Buffer") return wrong("a Buffer");
                        set.Write(parameter->name, kor::capi::BufferOf(given->handle));
                        break;
                    case kor::DescriptorType::eUniformTexelBuffer:
                    case kor::DescriptorType::eStorageTexelBuffer:
                        if (kind != "BufferView") return wrong("a BufferView");
                        set.Write(parameter->name, kor::capi::BufferViewOf(given->handle));
                        break;
                    case kor::DescriptorType::eAccelerationStructure:
                        if (kind != "AccelerationStructure") return wrong("an AccelerationStructure");
                        set.Write(parameter->name, kor::capi::AccelerationStructureOf(given->handle));
                        break;
                    }
                    // What a pass running the pipeline reads and writes, for the frame graph to order it by.
                    const bool writes = parameter->access != kor::Shader::AccessKind::eRead
                        && (parameter->type == kor::DescriptorType::eStorageImage || parameter->type == kor::DescriptorType::eStorageBuffer
                            || parameter->type == kor::DescriptorType::eStorageTexelBuffer);
                    const bool reads = !writes || parameter->access != kor::Shader::AccessKind::eWrite;
                    if (const auto image = ImageBehind(given)) info->uses.push_back({ ImportName(*image), image, true, reads, writes });
                    else if (const auto buffer = BufferBehind(given)) info->uses.push_back({ ImportName(*buffer), buffer, false, reads, writes });
                }
                info->set = set.Build();
                if (!info->set.Valid())
                    return Fail(info->set.Failure() ? info->set.Failure()->message : std::string("its descriptor set could not be built"));
                info->hasSet = true;
            }
            out.outputs["out"].resource = std::move(*resource);
            return {};
        }

        Expected<void> BuildPass(const Node& node, const Inputs& inputs, Built& out)
        {
            auto plan = std::make_shared<PassPlan>();
            const auto image = [&](const char* name) -> Expected<std::shared_ptr<Resource>> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || !it->second.resource) return Fail(std::format("'{}' needs an image wired in", name));
                if (it->second.resource->screen) return it->second.resource;
                if (!ImageBehind(it->second.resource)) return Fail(std::format("'{}' takes an Image, and was given a {}", name, it->second.resource->kind));
                return ImageBehind(it->second.resource);
            };
            const auto lacks = [](const Resource& resource, const kor::Image::Usage usage) {
                return !(kor::capi::ImageOf(resource.handle)->UsageFlags() & usage);
            };

            if (node.type == "Clear") {
                const auto target = image("image");
                if (!target) return Fail(target.error());
                kor::Vec4 color(0.f, 0.f, 0.f, 1.f);
                if (const auto it = inputs.find("color"); it != inputs.end() && !it->second.value.IsNull()) {
                    const auto* items = it->second.value.Items();
                    if (!items || items->size() != 4 || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; }))
                        return Fail("'color' is four numbers: red, green, blue, alpha");
                    for (int i = 0; i < 4; ++i) color[i] = static_cast<float>(*(*items)[static_cast<std::size_t>(i)].Number());
                }
                if ((*target)->screen) {
                    plan->writesScreen = true;
                    plan->record = [color](kor::CommandBuffer& cb, const PassContext& context) { cb.ClearColorImage(context.screen, color); };
                } else {
                    if (lacks(**target, kor::Image::Usage::eTransferDst)) return Fail("the image needs eTransferDst among its usage, to be cleared");
                    plan->uses.push_back({ ImportName(**target), *target, true, false, true });
                    plan->record = [color, ref = kor::capi::ImageOf((*target)->handle)](kor::CommandBuffer& cb, const PassContext&) { cb.ClearColorImage(ref, color); };
                }
            } else if (node.type == "Blit") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen) return Fail("'source' cannot be the screen");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be copied from");
                plan->uses.push_back({ ImportName(**source), *source, true, true, false });
                const auto from = kor::capi::ImageOf((*source)->handle);
                if ((*destination)->screen) {
                    plan->writesScreen = true;
                    plan->record = [from](kor::CommandBuffer& cb, const PassContext& context) { cb.Blit(from, context.screen); };
                } else {
                    if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be copied to");
                    plan->uses.push_back({ ImportName(**destination), *destination, true, false, true });
                    plan->record = [from, to = kor::capi::ImageOf((*destination)->handle)](kor::CommandBuffer& cb, const PassContext&) { cb.Blit(from, to); };
                }
            } else {   // Dispatch
                const auto it = inputs.find("pipeline");
                if (it == inputs.end() || !it->second.resource || it->second.resource->kind != "ComputePipeline")
                    return Fail("'pipeline' needs a ComputePipeline wired in");
                const Built& source = built.at(it->second.resource->node);
                const auto info = source.pipeline;
                if (!info) return Fail("'pipeline' is not a pipeline node's output");
                if (!info->perEntity.empty()) {
                    std::string names;
                    for (const auto& name : info->perEntity) names += std::format("{}{}", names.empty() ? "" : ", ", name);
                    return Fail(std::format("the pipeline has parameters nothing is wired to ({}): running it for each entity that supplies them is not built yet", names));
                }
                kor::u32 groups[3] { 1, 1, 1 };
                const char* axes[3] { "groups.x", "groups.y", "groups.z" };
                for (int axis = 0; axis < 3; ++axis) {
                    const auto given = inputs.find(axes[axis]);
                    if (given == inputs.end() || given->second.value.IsNull()) continue;
                    const double* count = given->second.value.Number();
                    if (!count || *count < 1 || *count > 65535) return Fail(std::format("'{}' is a number of groups, 1 or more", axes[axis]));
                    groups[axis] = static_cast<kor::u32>(std::llround(*count));
                }
                plan->uses = info->uses;
                plan->record = [pipeline = kor::capi::ComputePipelineOf(it->second.resource->handle), info, x = groups[0], y = groups[1], z = groups[2]](
                                   kor::CommandBuffer& cb, const PassContext&) {
                    cb.BindComputePipeline(pipeline);
                    if (info->hasSet) cb.BindDescriptorSet(0, info->set);
                    cb.Dispatch(x, y, z);
                };
            }
            out.pass = std::move(plan);
            return {};
        }

        Expected<void> BuildValue(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto value = [&](const char* name) -> const Value& {
                static const Value none;
                const auto it = inputs.find(name);
                return it == inputs.end() ? none : it->second.value;
            };
            Value& result = out.outputs["out"].value;
            if (node.type == "Number") {
                if (!value("value").Number()) return Fail("'value' is a number");
                result = value("value");
            } else if (node.type == "Bool") {
                if (!value("value").Bool()) return Fail("'value' is true or false");
                result = value("value");
            } else if (node.type == "String") {
                if (!value("value").String()) return Fail("'value' is text");
                result = value("value");
            } else if (node.type == "Vector") {
                Value::List components;
                for (const char* name : { "x", "y", "z", "w" }) {
                    if (value(name).IsNull()) break;
                    if (!value(name).Number()) return Fail(std::format("'{}' is a number", name));
                    components.push_back(value(name));
                }
                if (components.empty()) return Fail("it needs 'x' at least");
                result = Value(std::move(components));
            } else if (node.type == "Component") {
                const auto* items = value("vector").Items();
                const double* index = value("index").Number();
                if (!items) return Fail("'vector' is a vector");
                const std::size_t at = index ? static_cast<std::size_t>(std::max(*index, 0.0)) : 0;
                if (at >= items->size()) return Fail(std::format("the vector has {} components, and component {} was asked for", items->size(), at));
                result = (*items)[at];
            } else if (node.type == "ImageExtent") {
                const auto it = inputs.find("image");
                const auto image = it == inputs.end() ? nullptr : ImageBehind(it->second.resource);
                if (!image) return Fail("'image' needs an image wired in");
                const auto extent = kor::capi::ImageOf(image->handle)->Extent();
                result = Value(Value::List { extent.x, extent.y, extent.z });
            } else if (inputs.contains("b") || node.type == "Add" || node.type == "Subtract" || node.type == "Multiply" || node.type == "Divide"
                       || node.type == "Min" || node.type == "Max") {
                auto combined = Combine(node.type, value("a"), value("b"));
                if (!combined) return Fail(combined.error());
                result = std::move(*combined);
            } else {
                auto mapped = Map(node.type, value("a"));
                if (!mapped) return Fail(mapped.error());
                result = std::move(*mapped);
            }
            return {};
        }

        Expected<void> Build(const Node& node, const Inputs& inputs, Built& out)
        {
            const NodeType* type = FindType(node.type);
            if (!type) return Fail(std::format("there is no node type '{}'", node.type));
            if (type->builder) {
                return IsPipeline(type->name) ? BuildPipeline(node, *type->builder, inputs, out) : BuildBuilder(node, *type->builder, inputs, out);
            }
            // The engine's own: only the inputs its type lists.
            for (const auto& name : inputs | std::views::keys)
                if (std::ranges::find(type->inputs, name, &Port::name) == type->inputs.end()) return Fail(std::format("a {} has no input '{}'", type->name, name));
            if (type->category == "Passes") return BuildPass(node, inputs, out);
            if (node.type == "Screen") {
                auto screen = std::make_shared<Resource>();
                screen->kind = "Image";
                screen->node = node.id;
                screen->screen = true;
                out.outputs["out"].resource = std::move(screen);
                return {};
            }
            if (node.type == "Reference") {
                const auto it = inputs.find("node");
                const std::string* target = it == inputs.end() ? nullptr : it->second.value.String();
                if (!target) return Fail("'node' is the id of the node it stands for");
                const auto found = built.find(*target);
                if (found == built.end()) return Fail(std::format("there is no node '{}'", *target));
                if (!found->second.status.ok) return Fail(std::format("'{}', which it stands for, failed", *target));
                out.outputs = found->second.outputs;
                return {};
            }
            return BuildValue(node, inputs, out);
        }

        // ---- the whole graph ----------------------------------------------------------------------------------

        void Evaluate(const Graph& graph)
        {
            // Nodes that are gone take what they made with them.
            for (auto it = built.begin(); it != built.end();) {
                if (graph.Find(it->first)) { ++it; continue; }
                Unwatch(it->second);
                it = built.erase(it);
            }

            // What each node waits for: what it is wired to, and what it stands for.
            std::map<std::string, std::vector<std::string>> needs;
            for (const auto& node : graph.nodes) {
                auto& list = needs[node.id];
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link) list.push_back(input.link->node);
                if (node.type == "Reference")
                    if (const auto it = node.inputs.find("node"); it != node.inputs.end() && it->second.value.String()) list.push_back(*it->second.value.String());
            }
            std::vector<const Node*> order;
            std::set<std::string> done, visiting, circular;
            const std::function<void(const Node&)> visit = [&](const Node& node) {
                if (done.contains(node.id)) return;
                if (!visiting.insert(node.id).second) { circular.insert(node.id); return; }
                for (const auto& other : needs[node.id])
                    if (const Node* found = graph.Find(other)) visit(*found);
                visiting.erase(node.id);
                done.insert(node.id);
                order.push_back(&node);
            };
            for (const auto& node : graph.nodes) visit(node);

            for (const Node* node : order) {
                Built& out = built[node->id];
                const auto fail = [&](std::string why) {
                    // Failed for another reason than last time, or for the first: what was built is let go of.
                    Unwatch(out);
                    out.outputs.clear();
                    out.pass.reset();
                    out.status.ok = false;
                    out.status.error = std::move(why);
                    out.signature.clear();
                    out.stamp = ++counter;
                };
                if (circular.contains(node->id)) { fail("it is wired, through other nodes, to itself"); continue; }

                // What goes into it, and a signature of that: its own values, and for each wire which build of what.
                Inputs inputs;
                std::string signature = node->type;
                std::string blocked;
                for (const auto& [name, input] : node->inputs) {
                    Resolved resolved;
                    if (input.link) {
                        resolved.linked = true;
                        const auto source = built.find(input.link->node);
                        if (!graph.Find(input.link->node) || source == built.end()) { blocked = std::format("'{}' is wired to '{}', which is not in the graph", name, input.link->node); break; }
                        if (!source->second.status.ok) { blocked = std::format("'{}' comes from '{}', which failed", name, input.link->node); break; }
                        const auto output = source->second.outputs.find(input.link->output);
                        if (output == source->second.outputs.end()) { blocked = std::format("'{}' has no output '{}'", input.link->node, input.link->output); break; }
                        resolved.value = output->second.value;
                        resolved.resource = output->second.resource;
                        signature += std::format("|{}=@{}.{}#{}", name, input.link->node, input.link->output, source->second.stamp);
                    } else {
                        resolved.value = input.value;
                        signature += std::format("|{}={}", name, input.value.ToJson());
                    }
                    inputs.emplace(name, std::move(resolved));
                }
                if (node->type == "Reference")
                    if (const auto it = node->inputs.find("node"); it != node->inputs.end() && it->second.value.String())
                        if (const auto target = built.find(*it->second.value.String()); target != built.end()) signature += std::format("|#{}", target->second.stamp);
                if (!blocked.empty()) { fail(std::move(blocked)); continue; }
                if (out.status.ok && out.signature == signature) continue;   // nothing that goes into it changed

                Unwatch(out);
                Built fresh;
                fresh.status.builds = out.status.builds + 1;
                const auto result = Build(*node, inputs, fresh);
                if (!result) {
                    const auto pipeline = fresh.pipeline;   // its inputs are still worth knowing
                    Unwatch(fresh);
                    fail(result.error());
                    out.status.builds = fresh.status.builds;
                    out.pipeline = pipeline;
                    continue;
                }
                fresh.status.ok = true;
                fresh.signature = std::move(signature);
                fresh.stamp = ++counter;
                out = std::move(fresh);
            }
            Install(graph);
        }

        /**
         * The order passes go to the frame graph in. It runs the writers of a resource in the order they were added,
         * and what they use says more than that: a pass that only writes a resource makes what one that reads and
         * writes it works on, so it goes first. Where usage does not say, the graph's own order stands.
         */
        [[nodiscard]] std::vector<std::string> OverwritesFirst(std::vector<std::string> ids) const
        {
            const std::size_t count = ids.size();
            std::vector<std::vector<std::size_t>> after(count);
            std::vector<std::size_t> waits(count, 0);
            for (std::size_t first = 0; first < count; ++first)
                for (std::size_t second = 0; second < count; ++second) {
                    if (first == second) continue;
                    const auto& overwrites = built.at(ids[first]).pass->uses;
                    const auto& modifies = built.at(ids[second]).pass->uses;
                    const bool ordered = std::ranges::any_of(overwrites, [&](const Use& made) {
                        return made.write && !made.read && std::ranges::any_of(modifies, [&](const Use& used) { return used.name == made.name && used.read && used.write; });
                    });
                    if (ordered) { after[first].push_back(second); ++waits[second]; }
                }
            std::vector<std::string> order;
            std::vector<bool> placed(count, false);
            while (order.size() < count) {
                // The earliest in the graph that waits for nothing; in a circle of them, simply the earliest.
                std::size_t next = count;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i] && waits[i] == 0) next = i;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i]) next = i;
                placed[next] = true;
                order.push_back(ids[next]);
                for (const std::size_t follower : after[next]) if (waits[follower] > 0) --waits[follower];
            }
            return order;
        }

        /** Puts the passes that built in the frame graph, and takes out those that are gone. */
        void Install(const Graph& graph)
        {
            std::vector<std::string> wanted;
            for (const auto& node : graph.nodes)
                if (const auto it = built.find(node.id); it != built.end() && it->second.status.ok && it->second.pass) wanted.push_back(node.id);

            wanted = OverwritesFirst(std::move(wanted));

            // What they use, under the names they declare.
            for (const auto& id : wanted)
                for (const auto& use : built.at(id).pass->uses) {
                    if (use.image) frameGraph.Import(use.name, kor::capi::ImageOf(use.resource->handle));
                    else frameGraph.Import(use.name, kor::capi::BufferOf(use.resource->handle));
                }

            if (wanted != passOrder) {
                // Which passes there are, or their order, changed: the order they are added in breaks ties, so all go in again.
                for (auto* pass : passes | std::views::values) frameGraph.Remove(*pass);
                passes.clear();
                for (const auto& id : wanted) passes.emplace(id, &frameGraph.Add<GraphPass>(id, built.at(id).pass));
                passOrder = std::move(wanted);
                return;
            }
            bool declarations = false;
            for (const auto& id : wanted) {
                GraphPass* pass = passes.at(id);
                const auto& plan = built.at(id).pass;
                if (pass->plan == plan) continue;
                declarations = declarations || pass->plan->Declaration() != plan->Declaration();
                pass->plan = plan;
            }
            if (declarations) frameGraph.Invalidate();
        }
    };

    Runtime::Runtime(kor::FrameGraph& frameGraph) : _impl(std::make_unique<Impl>(frameGraph)) {}
    Runtime::~Runtime() = default;

    void Runtime::Set(Graph graph)
    {
        _graph = std::move(graph);
        _impl->Evaluate(_graph);
    }

    void Runtime::Update()
    {
        if (!_impl->reloaded.exchange(false)) return;
        // Which shader it was is not said: every shader node counts as built again, and what is wired to one follows.
        for (auto& node : _impl->built | std::views::values)
            if (node.watch != 0) node.stamp = ++_impl->counter;
        _impl->Evaluate(_graph);
    }

    const NodeStatus* Runtime::Status(const std::string_view id) const
    {
        const auto it = _impl->built.find(id);
        return it == _impl->built.end() ? nullptr : &it->second.status;
    }

    bool Runtime::Ok() const
    {
        return std::ranges::all_of(_impl->built | std::views::values, [](const auto& node) { return node.status.ok; });
    }

    std::vector<std::string> Runtime::Problems() const
    {
        std::vector<std::string> problems;
        for (const auto& node : _graph.nodes)
            if (const auto* status = Status(node.id); status && !status->ok) problems.push_back(std::format("{}: {}", node.id, status->error));
        return problems;
    }

    std::shared_ptr<Resource> Runtime::ResourceOf(const std::string_view id, const std::string_view output) const
    {
        const auto it = _impl->built.find(id);
        if (it == _impl->built.end()) return nullptr;
        const auto out = it->second.outputs.find(output);
        return out == it->second.outputs.end() ? nullptr : out->second.resource;
    }

    kor::ResourceRef<const kor::Image> Runtime::ImageOf(const std::string_view id, const std::string_view output) const
    {
        const auto image = ImageBehind(ResourceOf(id, output));
        return image && image->handle ? kor::capi::ImageOf(image->handle) : kor::ResourceRef<const kor::Image>();
    }

    Value Runtime::ValueOf(const std::string_view id, const std::string_view output) const
    {
        const auto it = _impl->built.find(id);
        if (it == _impl->built.end()) return {};
        const auto out = it->second.outputs.find(output);
        return out == it->second.outputs.end() ? Value {} : out->second.value;
    }

    std::vector<Port> Runtime::PortsOf(const std::string_view id) const
    {
        const Node* node = _graph.Find(id);
        if (!node) return {};
        std::vector<Port> ports;
        if (const NodeType* type = FindType(node->type)) ports = type->inputs;
        if (const auto it = _impl->built.find(id); it != _impl->built.end() && it->second.pipeline)
            ports.insert(ports.end(), it->second.pipeline->ports.begin(), it->second.pipeline->ports.end());
        return ports;
    }

    std::vector<std::string> Runtime::PerEntityParameters(const std::string_view id) const
    {
        const auto it = _impl->built.find(id);
        return it != _impl->built.end() && it->second.pipeline ? it->second.pipeline->perEntity : std::vector<std::string> {};
    }
}
