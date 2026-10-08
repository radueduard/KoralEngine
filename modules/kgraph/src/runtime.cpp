//
// kgraph: a graph, running. Nodes are worked out in the order their wires need, each built again only when what
// goes into it changed; the passes among them are put in the scene's frame graph, which orders them by what they
// read and write.
//

#include <kgraph/runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_map>

#include <builderDescriptions.h>
#include <capiInterop.h>
#include <commandBuffer.h>
#include <descriptorSet.h>
#include <frameGraph.h>
#include <framebuffer.h>
#include <graphicsPipeline.h>
#include <computePipeline.h>
#include <imageView.h>
#include <bufferView.h>
#include <koral_c.h>
#include <log.h>
#include <mesh.h>
#include <pipeline.h>
#include <shader.h>
#include <vertexLayout.h>

#include <kgraph/entities.h>

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

        using Held = std::shared_ptr<Resource>;

        /** An image or a buffer a pass uses: what the frame graph orders passes by. Told apart by which object it is. */
        struct Member {
            const void* id = nullptr;
            kor::ResourceRef<const kor::Image> image;
            kor::ResourceRef<const kor::Buffer> buffer;
        };
        struct Use {
            Member what;
            bool read = true, write = false;
        };

        /** The image a resource is, or is a view of; empty for anything else. */
        kor::ResourceRef<const kor::Image> ImageRefOf(const Resource* resource)
        {
            if (!resource || !resource->handle) return {};
            if (resource->kind == "Image") return kor::capi::ImageOf(resource->handle);
            if (resource->kind == "ImageView") return kor::capi::ImageViewOf(resource->handle)->SourceImage();
            return {};
        }
        kor::ResourceRef<const kor::Buffer> BufferRefOf(const Resource* resource)
        {
            if (!resource || !resource->handle) return {};
            if (resource->kind == "Buffer") return kor::capi::BufferOf(resource->handle);
            if (resource->kind == "BufferView") return kor::capi::BufferViewOf(resource->handle)->SourceBuffer();
            return {};
        }
        Member MemberOf(const Resource* resource)
        {
            Member member;
            if ((member.image = ImageRefOf(resource)).Get()) member.id = member.image.Get();
            else if ((member.buffer = BufferRefOf(resource)).Get()) member.id = member.buffer.Get();
            return member;
        }
        Member MemberOf(kor::ResourceRef<const kor::Image> image)
        {
            Member member;
            member.id = image.Get();
            member.image = std::move(image);
            return member;
        }

        // ---- the component a pipeline's entities have -----------------------------------------------------------

        /** One resource of a generated component: a parameter the node left unwired. */
        struct Slot {
            std::string field;              // the component's field
            std::string parameter;          // the shader's parameter it fills
            kor::DescriptorType type {};
            bool sampler = false;           // the sampler of the sampled image in the slot before
            bool read = true, write = false;
        };

        /**
         * How a generated component is laid out: a shared reference for each resource, then the shader's push
         * constants, byte for byte as the shader has them. The component type's context.
         */
        struct EntityLayout {
            std::vector<Slot> slots;
            std::map<std::string, Held> samplers;   // a sampled image's sampler, when the node wires one for every entity
            kor::u32 pushFirst = 0, pushSize = 0;   // the range of the pipeline's push constants it holds
            std::size_t pushAt = 0;                 // where that range is, in the component
            std::size_t size = 0;
            std::vector<kecs::FieldDescription> fields;
        };

        std::size_t SlotCount(const kecs::ComponentDescription& type) { return static_cast<const EntityLayout*>(type.context.get())->slots.size(); }

        void ConstructEntity(void* at, const kecs::ComponentDescription& type)
        {
            std::memset(at, 0, type.size);
            for (std::size_t i = 0; i < SlotCount(type); ++i) ::new (static_cast<Held*>(at) + i) Held();
        }
        void CopyEntity(void* to, const void* from, const kecs::ComponentDescription& type)
        {
            std::memcpy(to, from, type.size);
            for (std::size_t i = 0; i < SlotCount(type); ++i) ::new (static_cast<Held*>(to) + i) Held(static_cast<const Held*>(from)[i]);
        }
        void RelocateEntity(void* to, void* from, const kecs::ComponentDescription& type)
        {
            std::memcpy(to, from, type.size);
            for (std::size_t i = 0; i < SlotCount(type); ++i) {
                Held& source = static_cast<Held*>(from)[i];
                ::new (static_cast<Held*>(to) + i) Held(std::move(source));
                source.~Held();
            }
        }
        void DestroyEntity(void* at, const kecs::ComponentDescription& type)
        {
            for (std::size_t i = 0; i < SlotCount(type); ++i) static_cast<Held*>(at)[i].~Held();
        }
        void AssignHeld(void* to, const void* from, const kecs::FieldDescription&) { *static_cast<Held*>(to) = *static_cast<const Held*>(from); }

        bool IsImage(const kor::DescriptorType type)
        {
            return type == kor::DescriptorType::eSampledImage || type == kor::DescriptorType::eStorageImage || type == kor::DescriptorType::eCombinedImageSampler;
        }
        /** The kind of resource a parameter takes, as a node's output and a component's field are typed. */
        const char* KindOf(const kor::DescriptorType type)
        {
            return IsImage(type) ? "Image"
                : type == kor::DescriptorType::eSampler ? "Sampler"
                : type == kor::DescriptorType::eAccelerationStructure ? "AccelerationStructure"
                : type == kor::DescriptorType::eUniformTexelBuffer || type == kor::DescriptorType::eStorageTexelBuffer ? "BufferView" : "Buffer";
        }
        bool Takes(const kor::DescriptorType type, const std::string& kind)
        {
            return IsImage(type) ? kind == "Image" || kind == "ImageView" : kind == KindOf(type);
        }

        /** What a push constant is called as a component's field, and how many bytes of it are the value. */
        std::pair<std::string, std::size_t> ConstantType(const kor::Pipeline::PushConstantMember& member)
        {
            const bool plain = !member.aggregate && member.count == 1;
            if (plain && member.columns == 1 && member.rows >= 1 && member.rows <= 4 && member.scalar <= 3) {
                static constexpr const char* scalars[] { "f32", "i32", "u32", "bool32" };
                static constexpr const char* vectors[] { "vec", "ivec", "uvec", "bvec" };
                if (member.rows == 1) return { scalars[member.scalar], 4 };
                return { std::format("{}{}", vectors[member.scalar], member.rows), member.rows * std::size_t { 4 } };
            }
            if (plain && member.scalar == 4 && member.rows == 1 && member.columns == 1) return { "f64", 8 };
            if (plain && member.scalar == 0 && member.rows == 4 && member.columns == 4 && member.size == 64) return { "mat4", 64 };
            return { std::format("bytes{}", member.size), member.size };
        }

        // ---- pipelines and passes -------------------------------------------------------------------------------

        /** What a pipeline node worked out besides the pipeline. */
        struct PipelineInfo {
            kor::ResourceRef<const kor::Pipeline> pipeline;
            kor::Resource<kor::DescriptorSet> set;  // set 0: what is wired, bound once
            bool hasSet = false;
            std::vector<Use> uses;                  // what set 0 reads and writes
            std::map<std::string, Held> wired;      // parameter → what is wired to it
            std::vector<std::string> perEntity;     // what is not wired: set 1, an entity's to supply
            std::vector<Port> ports;                // its shaders' parameters and constants, as inputs

            std::shared_ptr<const EntityLayout> layout;         // null when nothing is per entity
            kecs::ComponentId component = kecs::NoComponent;    // the component of that layout, in the world
            std::vector<kor::ResourceRef<const kor::Shader>> shaders;   // every shader it is made of
            std::vector<Held> named;                            // those it names itself, kept while it is
            Held resource;                                      // the pipeline itself
            Held framebuffer;                                   // a graphics pipeline's
            kor::ResourceRef<const kor::Shader> vertexShader;
        };

        /** One entity a pass runs for, as readied this frame. */
        struct Item {
            kor::ResourceRef<const kor::DescriptorSet> set;
            kor::ResourceRef<const kor::Mesh> mesh;
            std::size_t push = 0;           // where its push constants are, in EntityRun::pushed
            kor::u32 groups[3] { 1, 1, 1 };
            bool indexed = false;
        };

        enum class DrawWith { eAuto, eDraw, eDrawIndexed };

        /** A pass that runs for each entity: which, and what was made for each. */
        struct EntityRun {
            std::shared_ptr<const PipelineInfo> info;
            std::vector<std::string> with, without;
            bool draws = false;
            DrawWith command = DrawWith::eAuto;
            int cover = -1;                         // the slot whose image decides how many groups
            kor::u32 groups[3] { 1, 1, 1 };
            kor::u32 groupSize[3] { 1, 1, 1 };

            struct Cached {
                std::vector<const Resource*> key;   // what its descriptor set was made of
                std::vector<Held> held;             // kept, while the set refers to them
                kor::Resource<kor::DescriptorSet> set;
                std::vector<Member> members;        // a slot each
                bool seen = false;
            };
            std::unordered_map<std::uint64_t, Cached> cache;

            // As readied this frame: read while the pass records.
            std::vector<Item> items;
            std::vector<std::byte> pushed;
            std::vector<std::vector<Member>> members;   // a slot each: every entity's, once each
            bool changed = true;                        // members changed since the frame graph was last told
            PassReport report;
        };

        struct PassContext {
            kor::ResourceRef<const kor::Image> screen;
        };

        /** What a pass node comes to: what it uses, and what it records. */
        struct PassPlan {
            std::vector<Use> uses;
            bool readsScreen = false, writesScreen = false;
            std::shared_ptr<EntityRun> entities;    // when it runs for each entity
            std::function<void(kor::CommandBuffer&, const PassContext&)> record;
        };

        /** What a pass declares to the frame graph, under the name the resource — or the set it is in — is imported by. */
        struct Declared {
            std::string name;
            bool read = false, write = false;
            bool operator==(const Declared&) const = default;
        };

        class GraphPass final : public kor::RenderPass {
        public:
            GraphPass(std::string name, std::shared_ptr<const PassPlan> plan, std::vector<Declared> declared)
                : RenderPass(std::move(name)), plan(std::move(plan)), declared(std::move(declared)) {}

            void Setup(kor::PassBuilder& builder) override
            {
                bool writes = plan->writesScreen;
                for (const auto& use : declared) {
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
            std::vector<Declared> declared;

        private:
            PassContext _context;
        };

        /** Puts what a parameter is given in a descriptor set being built. */
        Expected<void> WriteParameter(kor::DescriptorSet::Builder& set, const std::string& parameter, const kor::DescriptorType type,
                                      const Resource& given, const Resource* sampler, const std::string& what)
        {
            if (given.screen) return Fail(std::format("'{}': the screen is written by a pass, and cannot be a parameter", what));
            const std::string& kind = given.kind;
            const auto wrong = [&](const char* takes) { return Fail(std::format("'{}' takes {}, and was given a {}", what, takes, kind)); };
            switch (type) {
            case kor::DescriptorType::eSampler:
                if (kind != "Sampler") return wrong("a Sampler");
                set.Write(parameter, kor::capi::SamplerOf(given.handle));
                break;
            case kor::DescriptorType::eSampledImage:
            case kor::DescriptorType::eStorageImage:
                if (kind == "Image") set.Write(parameter, kor::capi::ImageOf(given.handle));
                else if (kind == "ImageView") set.Write(parameter, kor::capi::ImageViewOf(given.handle));
                else return wrong("an Image or an ImageView");
                break;
            case kor::DescriptorType::eCombinedImageSampler:
                if (!sampler || !sampler->handle || sampler->kind != "Sampler") return Fail(std::format("'{}' is sampled: '{}.sampler' needs a Sampler", what, what));
                if (kind == "Image") set.Write(parameter, kor::capi::ImageOf(given.handle), kor::capi::SamplerOf(sampler->handle));
                else if (kind == "ImageView") set.Write(parameter, kor::capi::ImageViewOf(given.handle), kor::capi::SamplerOf(sampler->handle));
                else return wrong("an Image or an ImageView");
                break;
            case kor::DescriptorType::eUniformBuffer:
            case kor::DescriptorType::eStorageBuffer:
                if (kind != "Buffer") return wrong("a Buffer");
                set.Write(parameter, kor::capi::BufferOf(given.handle));
                break;
            case kor::DescriptorType::eUniformTexelBuffer:
            case kor::DescriptorType::eStorageTexelBuffer:
                if (kind != "BufferView") return wrong("a BufferView");
                set.Write(parameter, kor::capi::BufferViewOf(given.handle));
                break;
            case kor::DescriptorType::eAccelerationStructure:
                if (kind != "AccelerationStructure") return wrong("an AccelerationStructure");
                set.Write(parameter, kor::capi::AccelerationStructureOf(given.handle));
                break;
            }
            return {};
        }

        /** What a pipeline's shaders ask for: every parameter and constant, once each. */
        struct Parameter {
            std::string name;
            kor::DescriptorType type {};
            kor::Shader::AccessKind access {};
            [[nodiscard]] bool Writes() const
            {
                return access != kor::Shader::AccessKind::eRead
                    && (type == kor::DescriptorType::eStorageImage || type == kor::DescriptorType::eStorageBuffer || type == kor::DescriptorType::eStorageTexelBuffer);
            }
            [[nodiscard]] bool Reads() const { return !Writes() || access != kor::Shader::AccessKind::eWrite; }
        };
        struct Interface {
            std::vector<Parameter> parameters;
            std::vector<kor::Shader::SpecializationConstant> constants;
            std::vector<Port> ports;    // `param.<name>` and `constant.<name>`

            [[nodiscard]] const Port* Find(const std::string_view name) const
            {
                const auto it = std::ranges::find(ports, name, &Port::name);
                return it == ports.end() ? nullptr : &*it;
            }
        };
        Interface Reflect(const std::vector<kor::ResourceRef<const kor::Shader>>& shaders)
        {
            Interface out;
            for (const auto& shader : shaders) {
                for (const auto& set : shader->BlockLayout().descriptorSets | std::views::values)
                    for (const auto& descriptor : set.descriptors | std::views::values) {
                        const std::string& name = descriptor.name.empty() ? descriptor.blockName : descriptor.name;
                        const auto known = std::ranges::find(out.parameters, name, &Parameter::name);
                        if (known == out.parameters.end()) out.parameters.push_back({ name, descriptor.type, descriptor.access });
                        else if (known->access != descriptor.access) known->access = kor::Shader::AccessKind::eReadWrite;
                    }
                for (const auto& constant : shader->BlockLayout().specializationConstants | std::views::values)
                    if (!constant.name.empty() && std::ranges::find(out.constants, constant.name, &kor::Shader::SpecializationConstant::name) == out.constants.end())
                        out.constants.push_back(constant);
            }
            for (const auto& parameter : out.parameters) {
                out.ports.push_back({ "param." + parameter.name, PortKind::eResource, KindOf(parameter.type), false, {}, nullptr });
                if (parameter.type == kor::DescriptorType::eCombinedImageSampler)
                    out.ports.push_back({ "param." + parameter.name + ".sampler", PortKind::eResource, "Sampler", false, {}, nullptr });
            }
            for (const auto& constant : out.constants)
                out.ports.push_back({ "constant." + constant.name, PortKind::eValue, constant.scalar == 3 ? "bool" : "number", false, {}, nullptr });
            return out;
        }

        std::uint64_t KeyOf(const kecs::Entity entity) { return (std::uint64_t { entity.index } << 32) | entity.generation; }

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

        /** Whether a pipeline can draw a mesh: empty when it can, and why not otherwise. */
        std::string Fits(const kor::Mesh& mesh, const kor::GraphicsPipeline& pipeline, const kor::ResourceRef<const kor::Shader>& vertexShader)
        {
            const auto& wanted = pipeline.VertexAttributeDescriptions();
            if (!wanted || wanted->empty() || !vertexShader.Get()) return {};
            const auto has = mesh.Layout().Resolve(*vertexShader);
            if (!has) return std::format("its mesh does not fit the pipeline: {}", has.error().message);
            for (const auto& attribute : *wanted) {
                const auto it = std::ranges::find(*has, attribute.location, &kor::VertexInputAttributeDescription::location);
                if (it == has->end() || it->binding != attribute.binding || it->offset != attribute.offset || it->channelCount != attribute.channelCount
                    || it->channelType != attribute.channelType)
                    return std::format("its mesh lays out the vertex input at location {} differently from the pipeline", attribute.location);
            }
            if (const auto& bindings = pipeline.VertexBindingDescriptions())
                for (const auto& binding : *bindings) {
                    const auto& meshBindings = mesh.Layout().bindings;
                    const auto it = std::ranges::find(meshBindings, binding.binding, &kor::VertexInputBindingDescription::binding);
                    if (it == meshBindings.end() || it->stride != binding.stride || it->inputRate != binding.inputRate)
                        return std::format("its mesh's vertex buffer {} is not spaced as the pipeline's", binding.binding);
                }
            return {};
        }

        /**
         * A vertex layout, from a list of what a vertex has: `{ "semantic": "POSITION", "type": "eFloat", "count": 3 }`.
         * Attributes are packed in the order given, a vertex buffer (`binding`) at a time; the result is what a
         * pipeline's `vertexShader.layout` and a Mesh's `vertexLayout` take.
         */
        Expected<Value> LayoutValue(const Value& attributes)
        {
            const auto* items = attributes.Items();
            if (!items || items->empty()) return Fail("'attributes' is a list of what a vertex has: { \"semantic\": \"POSITION\", \"type\": \"eFloat\", \"count\": 3 }");
            static const std::map<std::string, std::size_t, std::less<>> sizes {
                { "eFloat", 4 }, { "eInt", 4 }, { "eUInt", 4 }, { "eShort", 2 }, { "eUShort", 2 }, { "eByte", 1 }, { "eUByte", 1 }, { "eDouble", 8 } };
            std::map<std::uint32_t, std::pair<std::size_t, bool>> bindings;   // binding → bytes so far, an instance at a time
            Value::List out;
            double position = -1;
            for (const auto& item : *items) {
                if (!item.Named()) return Fail("each attribute is its fields, by name");
                for (const auto& name : *item.Named() | std::views::keys)
                    if (name != "semantic" && name != "namespace" && name != "type" && name != "count" && name != "binding" && name != "instanced")
                        return Fail(std::format("an attribute has no field '{}'. It has: semantic, namespace, type, count, binding, instanced", name));
                const Value* semantic = item.Find("semantic");
                const Value* type = item.Find("type");
                const Value* count = item.Find("count");
                const Value* binding = item.Find("binding");
                const Value* instanced = item.Find("instanced");
                const Value* space = item.Find("namespace");
                if (!semantic || !semantic->String()) return Fail("an attribute needs its 'semantic': what it is, as the shader names it");
                const std::string typeName = type && type->String() ? *type->String() : "eFloat";
                const auto size = sizes.find(typeName);
                if (size == sizes.end()) return Fail(std::format("'{}' is no channel type: eFloat, eInt, eUInt, eShort, eUShort, eByte, eUByte, eDouble", typeName));
                if (!count || !count->Number() || *count->Number() < 1 || *count->Number() > 4) return Fail(std::format("'{}' needs its 'count', 1 to 4", *semantic->String()));
                const auto channels = static_cast<std::size_t>(*count->Number());
                const auto index = static_cast<std::uint32_t>(binding && binding->Number() ? *binding->Number() : 0);
                auto& [bytes, perInstance] = bindings[index];
                if (instanced && instanced->Bool() && *instanced->Bool()) perInstance = true;
                if (*semantic->String() == "POSITION" && position < 0) position = static_cast<double>(out.size());
                out.emplace_back(Value::Members {
                    { "semantic", *semantic->String() }, { "semanticNamespace", space && space->String() ? *space->String() : std::string() },
                    { "binding", index }, { "offset", bytes }, { "channelType", typeName }, { "channelCount", channels },
                    { "location", -1 }, { "locations", 1 }, { "locationStride", 0 } });
                bytes += size->second * channels;
            }
            Value::List buffers;
            for (const auto& [index, binding] : bindings)
                buffers.emplace_back(Value::Members { { "binding", index }, { "stride", binding.first }, { "inputRate", binding.second ? "eInstance" : "eVertex" } });
            return Value(Value::Members { { "bindings", std::move(buffers) }, { "attributes", std::move(out) }, { "positionAttribute", position } });
        }
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
            std::optional<kecs::Migration> migration;   // a pipeline's: what its component last became
            // The shaders it is made of — a shader node's own, a pipeline's named on it — each told of when it is edited.
            std::vector<std::pair<kor::ResourceRef<const kor::Shader>, kor::u64>> watches;
        };

        kor::FrameGraph& frameGraph;
        kecs::World* world;
        std::map<std::string, Built, std::less<>> built;
        std::map<std::string, GraphPass*, std::less<>> passes;
        std::vector<std::string> wanted;        // the passes that built, in the graph's order
        std::vector<std::string> passOrder;     // as they were added to the frame graph
        struct Set {
            std::shared_ptr<kor::ResourceSet> set;
            std::vector<const void*> members;
        };
        std::map<std::string, Set> sets;        // the frame graph's sets of entities' resources, by the name they are imported by
        std::vector<std::string> reset;
        std::uint64_t counter = 0;
        std::atomic<bool> reloaded = false;
        // A shader could not be made — its file is not there yet, or does not compile and nothing is watching it:
        // the nodes that failed are tried again every so often, for the file to turn up.
        bool shaderFailed = false;
        std::chrono::steady_clock::time_point lastRetry = std::chrono::steady_clock::now();

        Impl(kor::FrameGraph& graph, kecs::World* entities) : frameGraph(graph), world(entities) {}

        void Unwatch(Built& node)
        {
            for (auto& [shader, watch] : node.watches)
                if (shader.Alive()) const_cast<kor::Shader&>(*shader).UnregisterReloadCallback(watch);
            node.watches.clear();
        }

        void Watch(Built& node, kor::ResourceRef<const kor::Shader> shader)
        {
            const kor::u64 watch = const_cast<kor::Shader&>(*shader).RegisterReloadCallback([this] { reloaded = true; });
            node.watches.emplace_back(std::move(shader), watch);
        }

        ~Impl()
        {
            for (auto& node : built | std::views::values) Unwatch(node);
            for (auto* pass : passes | std::views::values) frameGraph.Remove(*pass);
            for (auto& set : sets | std::views::values) set.set->Clear();
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
            // The framework keeps a shader by its module and entry point, one that failed included: asked for again
            // it hands back the same failure, though the file may be there now, or mended. It is tried afresh.
            if (!koral_resource_valid(*made) && description.name == "Shader") koral_resource_retry(*made);
            if (!koral_resource_valid(*made)) {
                if (description.name == "Shader") shaderFailed = true;
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
                Watch(out, kor::capi::ShaderOf((*resource)->handle));
            }
            out.outputs["out"].resource = std::move(*resource);
            return {};
        }

        /** The shaders wired to a pipeline node, in the order of its inputs. */
        static std::vector<kor::ResourceRef<const kor::Shader>> ShadersOf(const Inputs& inputs, kor::ResourceRef<const kor::Shader>* vertex = nullptr)
        {
            std::vector<kor::ResourceRef<const kor::Shader>> shaders;
            for (const auto& [name, input] : inputs) {
                if (!input.resource || input.resource->kind != "Shader") continue;
                shaders.push_back(kor::capi::ShaderOf(input.resource->handle));
                if (vertex && name == "vertexShader.shader") *vertex = shaders.back();
            }
            return shaders;
        }

        /**
         * A pipeline: its shaders' parameters are split by what is wired. What is wired goes in set 0, which the
         * pipeline keeps a descriptor set for; what is not goes in set 1, for each entity to supply in the component
         * made for it here, along with the shaders' push constants.
         */
        /**
         * A pipeline node's inputs, as its builder takes them. The node names each shader by the stage it is — a
         * module and an entry point — and what goes with one by the stage too: here each named shader is made, and
         * everything is put under the name the builder's setting has.
         */
        Expected<Inputs> AsTheBuilderTakesThem(const Node& node, const kb::Description& description, const Inputs& given, PipelineInfo& info, Built& out)
        {
            Inputs inputs = given;
            const std::string stages = description.name == "GraphicsPipeline" ? StagesOf(node) : std::string();
            if (const auto it = inputs.find("stages"); it != inputs.end()) {
                const std::string* chosen = it->second.value.String();
                if (description.name != "GraphicsPipeline") return Fail(std::format("a {} has no input 'stages'", description.name));
                if (!it->second.value.IsNull() && (!chosen || (*chosen != "eVertex" && *chosen != "eMesh"))) return Fail("'stages' is eVertex or eMesh");
                inputs.erase(it);
            }
            const kb::Description* shaderType = kb::Find("Shader");
            const kb::Setting* entryPoint = shaderType ? shaderType->FindSetting("entryPoint") : nullptr;
            for (const auto& setting : description.settings) {
                if (HiddenPipelineSetting(setting.name)) continue;
                const std::string_view mode = StageMode(setting.name);
                // How many times it is given: once, or up to the highest index any of its inputs names.
                int count = 1;
                if (setting.repeatable) {
                    count = 0;
                    for (const auto& argument : setting.arguments)
                        for (const std::string& shown : { PipelineInputName(setting, argument), InputName(setting, argument) }) {
                            const std::string prefix = shown.substr(0, shown.find("[]") + 1);
                            for (const auto& name : given | std::views::keys)
                                if (name.starts_with(prefix)) count = std::max(count, std::atoi(name.c_str() + prefix.size()) + 1);
                        }
                }
                for (int index = 0; index < count; ++index) {
                    for (const auto& argument : setting.arguments) {
                        const std::string shown = PipelineInputName(setting, argument, setting.repeatable ? index : -1);
                        const std::string taken = InputName(setting, argument, setting.repeatable ? index : -1);
                        // One that belongs to the other way of drawing is refused before anything is made of it.
                        if (!mode.empty() && mode != stages)
                            for (const std::string& name : { shown + ".module", shown + ".entry", shown, taken })
                                if (given.contains(name) && (IsShader(argument) || name == shown || name == taken))
                                    return Fail(std::format("'{}' is part of a pipeline whose 'stages' is {}, and this one's is {}", name, mode, stages));
                        if (IsShader(argument)) {
                            const auto module = inputs.find(shown + ".module");
                            const auto entry = inputs.find(shown + ".entry");
                            const std::string* file = module == inputs.end() ? nullptr : module->second.value.String();
                            const std::string* function = entry == inputs.end() ? nullptr : entry->second.value.String();
                            if (module != inputs.end() && module->second.linked) return Fail(std::format("'{}.module' is the module's name, and cannot be wired", shown));
                            if (file && !file->empty()) {
                                if (inputs.contains(taken)) return Fail(std::format("'{}' is named and wired both: one or the other", shown));
                                if (!entryPoint) return Fail("the framework describes no Shader builder");
                                // The entry point, when it is not said: the stage's name and Main, as shaders here are written.
                                std::string main = function && !function->empty() ? *function : shown.substr(0, shown.find_first_of(".[")) + "Main";
                                auto shader = BuildWith(*shaderType, node.id, {}, { { entryPoint, { kb::Value::Of(*file), kb::Value::Of(main) } } });
                                if (!shader) {
                                    // The commonest of them, said as what to do about it.
                                    if (shader.error().find("cannot open file") != std::string::npos)
                                        return Fail(std::format("'{}': there is no {}.slang among the shaders. Put it in the project's shaders folder (the project has to be "
                                                                "saved for it to have one); it is picked up when it is there.", shown, *file));
                                    return Fail(std::format("'{}': {}", shown, shader.error()));
                                }
                                (*shader)->kind = "Shader";
                                Watch(out, kor::capi::ShaderOf((*shader)->handle));
                                info.named.push_back(*shader);
                                Resolved resolved;
                                resolved.resource = std::move(*shader);
                                resolved.linked = true;
                                inputs[taken] = std::move(resolved);
                            } else if (function && !function->empty()) {
                                return Fail(std::format("'{}.entry' is an entry point of a module: '{}.module' says which", shown, shown));
                            }
                            inputs.erase(shown + ".module");
                            inputs.erase(shown + ".entry");
                        } else if (shown != taken) {
                            if (const auto it = inputs.find(shown); it != inputs.end()) {
                                inputs[taken] = it->second;
                                inputs.erase(shown);
                            }
                        }
                    }
                }
            }
            return inputs;
        }

        Expected<void> BuildPipeline(const Node& node, const kb::Description& description, const Inputs& given, Built& out)
        {
            auto info = std::make_shared<PipelineInfo>();
            const auto translated = AsTheBuilderTakesThem(node, description, given, *info, out);
            if (!translated) return Fail(translated.error());
            const Inputs& inputs = *translated;
            info->shaders = ShadersOf(inputs, &info->vertexShader);
            const Interface interface = Reflect(info->shaders);
            info->ports = interface.ports;
            out.pipeline = info;    // its inputs are known from here, built or not

            const auto ours = [&](const std::string_view name) { return interface.Find(name) != nullptr; };
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
            std::vector<const Parameter*> bound, loose;
            for (const auto& parameter : interface.parameters) {
                const auto it = inputs.find("param." + parameter.name);
                const bool given = it != inputs.end() && it->second.resource;
                calls->push_back({ binding, { kb::Value::Of(parameter.name), kb::Value::Of(given ? std::uint64_t { 0 } : std::uint64_t { 1 }),
                                              kb::Value::Of(given ? wired++ : unwired++) } });
                if (given) { bound.push_back(&parameter); info->wired[parameter.name] = it->second.resource; }
                else { loose.push_back(&parameter); info->perEntity.push_back(parameter.name); }
            }
            for (const auto& constant : interface.constants) {
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
            info->pipeline =
                description.name == "ComputePipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::ComputePipelineOf(handle))
                : description.name == "GraphicsPipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::GraphicsPipelineOf(handle))
                : kor::ResourceRef<const kor::Pipeline>(kor::capi::RayTracingPipelineOf(handle));
            if (const auto it = inputs.find("framebuffer"); it != inputs.end()) info->framebuffer = it->second.resource;

            const auto samplerOf = [&](const Parameter& parameter) -> Held {
                const auto it = inputs.find("param." + parameter.name + ".sampler");
                return it == inputs.end() ? nullptr : it->second.resource;
            };

            // What is wired, bound once.
            if (!bound.empty()) {
                kor::DescriptorSet::Builder set(info->pipeline, 0);
                for (const Parameter* parameter : bound) {
                    const std::string port = "param." + parameter->name;
                    const Held& given = inputs.at(port).resource;
                    const auto written = WriteParameter(set, parameter->name, parameter->type, *given, samplerOf(*parameter).get(), port);
                    if (!written) return Fail(written.error());
                    // What a pass running the pipeline reads and writes, for the frame graph to order it by.
                    if (Member member = MemberOf(given.get()); member.id) info->uses.push_back({ std::move(member), parameter->Reads(), parameter->Writes() });
                }
                info->set = set.Build();
                if (!info->set.Valid())
                    return Fail(info->set.Failure() ? info->set.Failure()->message : std::string("its descriptor set could not be built"));
                info->hasSet = true;
            }

            // What is not: a component, for each entity to supply it in.
            const auto& constants = info->pipeline->PushConstants();
            if (!loose.empty() || !constants.empty()) {
                auto layout = std::make_shared<EntityLayout>();
                for (const Parameter* parameter : loose) {
                    layout->slots.push_back({ parameter->name, parameter->name, parameter->type, false, parameter->Reads(), parameter->Writes() });
                    if (parameter->type != kor::DescriptorType::eCombinedImageSampler) continue;
                    // Its sampler: one for every entity when the node wires it, and each entity's own otherwise.
                    if (Held sampler = samplerOf(*parameter)) layout->samplers[parameter->name] = std::move(sampler);
                    else layout->slots.push_back({ parameter->name + ".sampler", parameter->name, kor::DescriptorType::eSampler, true, true, false });
                }
                for (std::size_t i = 0; i < layout->slots.size(); ++i) {
                    const Slot& slot = layout->slots[i];
                    layout->fields.push_back({ slot.field, KindOf(slot.type), i * sizeof(Held), sizeof(Held), &AssignHeld });
                }
                kor::u32 first = ~kor::u32 { 0 }, end = 0;
                for (const auto& member : constants | std::views::values) {
                    first = std::min(first, member.offset);
                    end = std::max(end, member.offset + member.size);
                }
                const auto aligned = [](const std::size_t bytes) { return (bytes + 15) / 16 * 16; };
                layout->pushAt = aligned(layout->slots.size() * sizeof(Held));
                if (!constants.empty()) {
                    layout->pushFirst = first;
                    layout->pushSize = end - first;
                    for (const auto& [name, member] : constants) {
                        if (member.aggregate || member.count != 1) continue;    // its fields and elements are listed on their own
                        const auto [type, size] = ConstantType(member);
                        layout->fields.push_back({ name, type, layout->pushAt + member.offset - first, size, nullptr });
                    }
                }
                layout->size = std::max<std::size_t>(aligned(layout->pushAt + layout->pushSize), 16);
                info->layout = layout;
                if (world) {
                    const auto registered = Register(node.id, *info, out);
                    if (!registered) return Fail(registered.error());
                }
            }
            info->resource = *resource;
            out.outputs["out"].resource = std::move(*resource);
            return {};
        }

        /** Makes the world's component of a pipeline's layout, or gives the one it has its new shape. */
        Expected<void> Register(const std::string& name, PipelineInfo& info, Built& out)
        {
            const EntityLayout& layout = *info.layout;
            kecs::ComponentDescription description;
            description.name = name;
            description.size = layout.size;
            description.alignment = 16;
            description.construct = &ConstructEntity;
            description.copy = &CopyEntity;
            description.relocate = &RelocateEntity;
            description.destroy = &DestroyEntity;
            description.fields = layout.fields;
            description.context = info.layout;

            const kecs::ComponentId existing = world->Find(name);
            if (existing == kecs::NoComponent) {
                info.component = world->Register(std::move(description));
                return {};
            }
            const kecs::ComponentDescription& now = world->Describe(existing);
            if (now.construct != &ConstructEntity)
                return Fail(std::format("its component would be named '{}' after it, and there is one so named already: the node needs another id", name));
            const bool same = now.size == layout.size && std::ranges::equal(now.fields, layout.fields, [](const kecs::FieldDescription& a, const kecs::FieldDescription& b) {
                return a.SameAs(b) && a.offset == b.offset;
            });
            if (!same) {
                try { out.migration = world->Reshape(existing, std::move(description)); }
                catch (const std::exception& e) { return Fail(e.what()); }
            }
            info.component = existing;
            return {};
        }

        Expected<void> BuildPass(const Node& node, const Inputs& inputs, Built& out)
        {
            auto plan = std::make_shared<PassPlan>();
            const auto image = [&](const char* name) -> Expected<Held> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || !it->second.resource) return Fail(std::format("'{}' needs an image wired in", name));
                if (it->second.resource->screen) return it->second.resource;
                if (!ImageRefOf(it->second.resource.get()).Get()) return Fail(std::format("'{}' takes an Image, and was given a {}", name, it->second.resource->kind));
                return it->second.resource;
            };
            const auto lacks = [](const Resource& resource, const kor::Image::Usage usage) {
                return !(ImageRefOf(&resource)->UsageFlags() & usage);
            };
            const auto value = [&](const char* name) -> const Value& {
                static const Value none;
                const auto it = inputs.find(name);
                return it == inputs.end() ? none : it->second.value;
            };
            const auto names = [&](const char* name) -> Expected<std::vector<std::string>> {
                std::vector<std::string> out;
                const Value& given = value(name);
                if (given.IsNull()) return out;
                const auto* items = given.Items();
                if (!items || !std::ranges::all_of(*items, [](const Value& v) { return v.String() != nullptr; }))
                    return Fail(std::format("'{}' is a list of components, by name", name));
                for (const auto& item : *items) out.push_back(*item.String());
                return out;
            };
            const auto counts = [&](const char* prefix, kor::u32 (&into)[3], const char* what) -> Expected<void> {
                const char* axes[3] { ".x", ".y", ".z" };
                for (int axis = 0; axis < 3; ++axis) {
                    const std::string name = std::string(prefix) + axes[axis];
                    const Value& given = value(name.c_str());
                    if (given.IsNull()) continue;
                    const double* count = given.Number();
                    if (!count || *count < 1 || *count > 65535) return Fail(std::format("'{}' is {}, 1 or more", name, what));
                    into[axis] = static_cast<kor::u32>(std::llround(*count));
                }
                return {};
            };
            // The pipeline a pass runs, and what its node worked out.
            const auto pipelineOf = [&](const char* kind) -> Expected<std::shared_ptr<const PipelineInfo>> {
                const auto it = inputs.find("pipeline");
                if (it == inputs.end() || !it->second.resource || it->second.resource->kind != kind) return Fail(std::format("'pipeline' needs a {} wired in", kind));
                const auto source = built.find(it->second.resource->node);
                if (source == built.end() || !source->second.pipeline) return Fail("'pipeline' is not a pipeline node's output");
                return std::shared_ptr<const PipelineInfo>(source->second.pipeline);
            };
            // The entities a pass runs for: those with the pipeline's component, narrowed.
            const auto entitiesOf = [&](const std::shared_ptr<const PipelineInfo>& info) -> Expected<std::shared_ptr<EntityRun>> {
                if (!world) return Fail("it runs for each entity, and the runtime was given no world of them");
                auto run = std::make_shared<EntityRun>();
                run->info = info;
                auto with = names("with");
                if (!with) return Fail(with.error());
                auto without = names("without");
                if (!without) return Fail(without.error());
                run->with = std::move(*with);
                run->without = std::move(*without);
                if (info->layout) run->members.resize(info->layout->slots.size());
                return run;
            };

            if (node.type == "Clear") {
                const auto target = image("image");
                if (!target) return Fail(target.error());
                kor::Vec4 color(0.f, 0.f, 0.f, 1.f);
                if (const Value& given = value("color"); !given.IsNull()) {
                    const auto* items = given.Items();
                    if (!items || items->size() != 4 || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; }))
                        return Fail("'color' is four numbers: red, green, blue, alpha");
                    for (int i = 0; i < 4; ++i) color[i] = static_cast<float>(*(*items)[static_cast<std::size_t>(i)].Number());
                }
                if ((*target)->screen) {
                    plan->writesScreen = true;
                    plan->record = [color](kor::CommandBuffer& cb, const PassContext& context) { cb.ClearColorImage(context.screen, color); };
                } else {
                    if (lacks(**target, kor::Image::Usage::eTransferDst)) return Fail("the image needs eTransferDst among its usage, to be cleared");
                    const auto ref = ImageRefOf(target->get());
                    plan->uses.push_back({ MemberOf(ref), false, true });
                    plan->record = [color, ref](kor::CommandBuffer& cb, const PassContext&) { cb.ClearColorImage(ref, color); };
                }
            } else if (node.type == "Blit") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen) return Fail("'source' cannot be the screen");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be copied from");
                const auto from = ImageRefOf(source->get());
                plan->uses.push_back({ MemberOf(from), true, false });
                if ((*destination)->screen) {
                    plan->writesScreen = true;
                    plan->record = [from](kor::CommandBuffer& cb, const PassContext& context) { cb.Blit(from, context.screen); };
                } else {
                    if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be copied to");
                    const auto to = ImageRefOf(destination->get());
                    plan->uses.push_back({ MemberOf(to), false, true });
                    plan->record = [from, to](kor::CommandBuffer& cb, const PassContext&) { cb.Blit(from, to); };
                }
            } else if (node.type == "Dispatch") {
                const auto info = pipelineOf("ComputePipeline");
                if (!info) return Fail(info.error());
                kor::u32 groups[3] { 1, 1, 1 }, groupSize[3] { 1, 1, 1 };
                if (const auto read = counts("groups", groups, "a number of groups"); !read) return Fail(read.error());
                if (const auto read = counts("groupSize", groupSize, "how many a group works on"); !read) return Fail(read.error());

                std::shared_ptr<EntityRun> run;
                if ((*info)->layout) {
                    auto entities = entitiesOf(*info);
                    if (!entities) return Fail(entities.error());
                    run = std::move(*entities);
                } else if (!value("with").IsNull() || !value("without").IsNull()) {
                    return Fail("the pipeline has everything wired, and runs once: there are no entities for 'with' and 'without' to narrow");
                }
                // An image to cover: as many groups as it takes, an entity's own image or one the node wires.
                if (const Value& cover = value("cover"); !cover.IsNull()) {
                    const std::string* name = cover.String();
                    if (!name) return Fail("'cover' is the name of one of the pipeline's image parameters");
                    bool found = false;
                    if (run)
                        for (std::size_t i = 0; i < (*info)->layout->slots.size() && !found; ++i) {
                            const Slot& slot = (*info)->layout->slots[i];
                            if (slot.field == *name && !slot.sampler && IsImage(slot.type)) { run->cover = static_cast<int>(i); found = true; }
                        }
                    if (const auto wired = (*info)->wired.find(*name); !found && wired != (*info)->wired.end()) {
                        if (const auto covered = ImageRefOf(wired->second.get()); covered.Get()) {
                            const auto extent = covered->Extent();
                            for (int axis = 0; axis < 3; ++axis) groups[axis] = std::max<kor::u32>((static_cast<kor::u32>(extent[axis]) + groupSize[axis] - 1) / groupSize[axis], 1);
                            found = true;
                        }
                    }
                    if (!found) return Fail(std::format("'cover' names one of the pipeline's image parameters, and '{}' is not one", *name));
                }
                if (run) {
                    std::ranges::copy(groups, run->groups);
                    std::ranges::copy(groupSize, run->groupSize);
                }
                plan->uses = (*info)->uses;
                plan->entities = run;
                plan->record = [pipeline = kor::ResourceRef<const kor::ComputePipeline>(kor::capi::ComputePipelineOf(inputs.at("pipeline").resource->handle)),
                                info = *info, run, x = groups[0], y = groups[1], z = groups[2]](kor::CommandBuffer& cb, const PassContext&) {
                    if (run && run->items.empty()) return;
                    cb.BindComputePipeline(pipeline);
                    if (info->hasSet) cb.BindDescriptorSet(0, info->set);
                    if (!run) { cb.Dispatch(x, y, z); return; }
                    const EntityLayout& layout = *info->layout;
                    for (const Item& item : run->items) {
                        if (item.set.Get()) cb.BindDescriptorSet(1, item.set);
                        if (layout.pushSize) cb.PushConstantBytes({ run->pushed.data() + item.push, layout.pushSize }, layout.pushFirst);
                        cb.Dispatch(item.groups[0], item.groups[1], item.groups[2]);
                    }
                };
            } else {   // Draw
                const auto info = pipelineOf("GraphicsPipeline");
                if (!info) return Fail(info.error());
                Held target = (*info)->framebuffer;
                if (const auto it = inputs.find("framebuffer"); it != inputs.end() && it->second.resource) target = it->second.resource;
                if (!target || !target->handle || target->kind != "Framebuffer")
                    return Fail("it draws into a framebuffer: the pipeline's, or one wired to 'framebuffer'. To show it, Blit one of the framebuffer's images to the screen");
                const auto framebuffer = kor::capi::FramebufferOf(target->handle);

                DrawWith command = DrawWith::eAuto;
                if (const Value& given = value("command"); !given.IsNull()) {
                    const std::string* name = given.String();
                    if (name && *name == "eAuto") command = DrawWith::eAuto;
                    else if (name && *name == "eDraw") command = DrawWith::eDraw;
                    else if (name && *name == "eDrawIndexed") command = DrawWith::eDrawIndexed;
                    else return Fail("'command' is eAuto (indexed when the mesh has indices), eDraw or eDrawIndexed");
                }
                kor::u32 instances = 1;
                if (const Value& given = value("instances"); !given.IsNull()) {
                    if (!given.Number() || *given.Number() < 1) return Fail("'instances' is how many of each mesh, 1 or more");
                    instances = static_cast<kor::u32>(std::llround(*given.Number()));
                }
                bool load = false;
                if (const Value& given = value("load"); !given.IsNull()) {
                    if (!given.Bool()) return Fail("'load' is true to draw over what the framebuffer holds, false to clear it first");
                    load = *given.Bool();
                }

                auto entities = entitiesOf(*info);
                if (!entities) return Fail(entities.error());
                const std::shared_ptr<EntityRun> run = std::move(*entities);
                run->draws = true;
                run->command = command;

                for (kor::u32 i = 0; i < framebuffer->ColorAttachmentCount(); ++i)
                    if (const auto view = framebuffer->ColorAttachment(i); view.Get()) plan->uses.push_back({ MemberOf(view->SourceImage()), load, true });
                if (framebuffer->HasDepthAttachment())
                    if (const auto view = framebuffer->DepthAttachment(); view.Get()) plan->uses.push_back({ MemberOf(view->SourceImage()), true, true });
                plan->uses.insert(plan->uses.end(), (*info)->uses.begin(), (*info)->uses.end());
                plan->entities = run;
                plan->record = [pipeline = kor::capi::GraphicsPipelineOf(inputs.at("pipeline").resource->handle), framebuffer, info = *info, run, instances, load, target](
                                   kor::CommandBuffer& cb, const PassContext&) {
                    kor::RenderInfo render(framebuffer);
                    if (load) render.SetColorLoadOperation(kor::LoadOperation::eLoad).SetDepthLoadOperation(kor::LoadOperation::eLoad).SetStencilLoadOperation(kor::LoadOperation::eLoad);
                    const auto extent = framebuffer->Extent();
                    cb.BeginRendering(render);   // with nothing to draw, it still clears
                    if (!run->items.empty()) {
                        cb.BindGraphicsPipeline(pipeline);
                        cb.SetViewport(0, 0, extent.x, extent.y).SetScissor(0, 0, extent.x, extent.y);
                        if (info->hasSet) cb.BindDescriptorSet(0, info->set);
                    }
                    const EntityLayout* layout = info->layout.get();
                    for (const Item& item : run->items) {
                        if (item.set.Get()) cb.BindDescriptorSet(1, item.set);
                        if (layout && layout->pushSize) cb.PushConstantBytes({ run->pushed.data() + item.push, layout->pushSize }, layout->pushFirst);
                        cb.BindMesh(item.mesh);
                        if (item.indexed) cb.DrawIndexed(kor::WholeSize, instances);
                        else cb.Draw(kor::WholeSize, instances);
                    }
                    cb.EndRendering();
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
                const auto image = it == inputs.end() ? kor::ResourceRef<const kor::Image>() : ImageRefOf(it->second.resource.get());
                if (!image.Get()) return Fail("'image' needs an image wired in");
                const auto extent = image->Extent();
                result = Value(Value::List { extent.x, extent.y, extent.z });
            } else if (node.type == "VertexLayout") {
                auto layout = LayoutValue(value("attributes"));
                if (!layout) return Fail(layout.error());
                result = std::move(*layout);
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

        /** A mesh of a simple shape, and the layout of its vertices for a pipeline to be built against. */
        Expected<void> BuildPrimitive(const Node& node, const Inputs& inputs, Built& out)
        {
            struct Vertex { kor::Vec3 position; kor::Vec3 normal; kor::Vec2 uv; };
            std::string shape = "eQuad";
            if (const auto it = inputs.find("shape"); it != inputs.end() && !it->second.value.IsNull()) {
                if (!it->second.value.String()) return Fail("'shape' is eQuad, eCube or eTriangle");
                shape = *it->second.value.String();
            }
            std::vector<Vertex> vertices;
            std::vector<kor::u32> indices;
            // One face: a unit square across `right` and `up`, half a unit out along where they face.
            const auto face = [&](const kor::Vec3 right, const kor::Vec3 up, const float out) {
                const kor::Vec3 normal = kor::Cross(right, up);
                const auto first = static_cast<kor::u32>(vertices.size());
                for (const auto [x, y] : { std::pair { -0.5f, -0.5f }, std::pair { 0.5f, -0.5f }, std::pair { -0.5f, 0.5f }, std::pair { 0.5f, 0.5f } })
                    vertices.push_back({ right * x + up * y + normal * out, normal, { x + 0.5f, y + 0.5f } });
                for (const kor::u32 corner : { 0u, 1u, 2u, 2u, 1u, 3u }) indices.push_back(first + corner);
            };
            if (shape == "eQuad") {
                face({ 1, 0, 0 }, { 0, 1, 0 }, 0.f);
            } else if (shape == "eCube") {
                face({ 1, 0, 0 }, { 0, 1, 0 }, 0.5f);
                face({ -1, 0, 0 }, { 0, 1, 0 }, 0.5f);
                face({ 0, 0, -1 }, { 0, 1, 0 }, 0.5f);
                face({ 0, 0, 1 }, { 0, 1, 0 }, 0.5f);
                face({ 1, 0, 0 }, { 0, 0, -1 }, 0.5f);
                face({ 1, 0, 0 }, { 0, 0, 1 }, 0.5f);
            } else if (shape == "eTriangle") {
                vertices = { { { -0.5f, -0.5f, 0 }, { 0, 0, 1 }, { 0, 0 } }, { { 0.5f, -0.5f, 0 }, { 0, 0, 1 }, { 1, 0 } }, { { 0, 0.5f, 0 }, { 0, 0, 1 }, { 0.5f, 1 } } };
                indices = { 0, 1, 2 };
            } else {
                return Fail(std::format("'{}' is no shape: eQuad, eCube or eTriangle", shape));
            }
            auto mesh = kor::Mesh::Builder()
                .SetVertexBuffer(0, kor::Mesh::MakeBuffer(vertices, kor::Buffer::Usage::eVertex))
                .SetIndexBuffer(kor::Mesh::MakeBuffer(indices, kor::Buffer::Usage::eIndex))
                .SetVertexLayout(kor::VertexLayout {
                    .bindings = { kor::VertexInputBindingDescription(0, sizeof(Vertex)) },
                    .attributes = { kor::VertexLayout::Attribute("POSITION", "", 0, offsetof(Vertex, position), kor::ChannelType::eFloat, 3),
                                    kor::VertexLayout::Attribute("NORMAL", "", 0, offsetof(Vertex, normal), kor::ChannelType::eFloat, 3),
                                    kor::VertexLayout::Attribute("UV", "", 0, offsetof(Vertex, uv), kor::ChannelType::eFloat, 2) } })
                .Build();
            if (!mesh.Valid()) return Fail(mesh.Failure() ? mesh.Failure()->message : std::string("the mesh could not be built"));
            Held resource = MakeResource(std::move(mesh));
            resource->node = node.id;
            out.outputs["out"].resource = std::move(resource);
            auto layout = LayoutValue(Value(Value::List {
                Value::Members { { "semantic", "POSITION" }, { "count", 3 } }, Value::Members { { "semantic", "NORMAL" }, { "count", 3 } },
                Value::Members { { "semantic", "UV" }, { "count", 2 } } }));
            if (!layout) return Fail(layout.error());
            out.outputs["layout"].value = std::move(*layout);
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
            if (node.type == "Primitive") return BuildPrimitive(node, inputs, out);
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
                if (!fresh.migration) fresh.migration = std::move(out.migration);
                out = std::move(fresh);
            }
            Install(graph);
        }

        // ---- each frame: the entities ---------------------------------------------------------------------------

        /** Readies a pass that runs per entity: which entities it runs for now, and what each needs made. */
        void Prepare(EntityRun& run)
        {
            run.items.clear();
            run.pushed.clear();
            run.report = {};
            const PipelineInfo& info = *run.info;
            const EntityLayout* layout = info.layout.get();
            const std::size_t slots = layout ? layout->slots.size() : 0;
            for (auto& cached : run.cache | std::views::values) cached.seen = false;

            kecs::QueryDescription query;
            bool nobody = false;
            if (info.component != kecs::NoComponent) query.all.push_back(info.component);
            const kecs::ComponentId meshId = run.draws ? world->Id<Mesh>() : kecs::NoComponent;
            if (run.draws) query.all.push_back(meshId);
            for (const auto& name : run.with) {
                const kecs::ComponentId id = world->Find(name);
                if (id == kecs::NoComponent) { nobody = true; run.report.why = std::format("there is no component '{}' yet: nothing has it", name); }
                else query.all.push_back(id);
            }
            for (const auto& name : run.without)
                if (const kecs::ComponentId id = world->Find(name); id != kecs::NoComponent) query.none.push_back(id);

            std::unordered_map<const kor::Mesh*, std::string> fits;
            const auto graphics = run.draws ? kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf(info.resource->handle)) : kor::ResourceRef<const kor::GraphicsPipeline>();

            if (!nobody) world->Each(query, [&](const kecs::ChunkView& view) {
                const auto* column = layout ? static_cast<const std::byte*>(view.Column(info.component)) : nullptr;
                const auto* meshes = run.draws ? static_cast<const Mesh*>(view.Column(meshId)) : nullptr;
                for (std::size_t row = 0; row < view.Count(); ++row) {
                    if (!view.AllMatch() && !view.Matches(row)) continue;
                    const kecs::Entity entity = view.EntityAt(row);
                    const std::byte* bytes = column ? column + row * layout->size : nullptr;
                    const auto skip = [&](const std::string& why) {
                        ++run.report.skipped;
                        if (run.report.why.empty()) run.report.why = std::format("entity {}: {}", entity.index, why);
                    };
                    Item item;

                    if (run.draws) {
                        const Resource* held = meshes[row].mesh.get();
                        if (!held || !held->handle || held->kind != "Mesh") { skip("its Mesh has no mesh in it"); continue; }
                        item.mesh = kor::capi::MeshOf(held->handle);
                        const auto [known, fresh] = fits.try_emplace(item.mesh.Get());
                        if (fresh) known->second = Fits(*item.mesh, *graphics, info.vertexShader);
                        if (!known->second.empty()) { skip(known->second); continue; }
                        const bool indices = item.mesh->IndexCount().has_value();
                        if (run.command == DrawWith::eDrawIndexed && !indices) { skip("its mesh has no indices, and the node draws indexed"); continue; }
                        item.indexed = run.command == DrawWith::eDrawIndexed || (run.command == DrawWith::eAuto && indices);
                    }

                    if (slots > 0) {
                        const Held* held = reinterpret_cast<const Held*>(bytes);
                        std::string lacking;
                        for (std::size_t i = 0; i < slots && lacking.empty(); ++i) {
                            const Slot& slot = layout->slots[i];
                            if (!held[i] || !held[i]->handle) lacking = std::format("'{}' has nothing in it", slot.field);
                            else if (!Takes(slot.type, held[i]->kind)) lacking = std::format("'{}' takes a {}, and holds a {}", slot.field, KindOf(slot.type), held[i]->kind);
                        }
                        if (!lacking.empty()) { skip(lacking); continue; }

                        auto& cached = run.cache[KeyOf(entity)];
                        bool same = cached.set.Valid() && cached.key.size() == slots;
                        for (std::size_t i = 0; same && i < slots; ++i) same = cached.key[i] == held[i].get();
                        if (!same) {
                            kor::DescriptorSet::Builder set(info.pipeline, 1);
                            std::string failed;
                            for (std::size_t i = 0; i < slots && failed.empty(); ++i) {
                                const Slot& slot = layout->slots[i];
                                if (slot.sampler) continue;
                                const Resource* sampler = nullptr;
                                if (slot.type == kor::DescriptorType::eCombinedImageSampler) {
                                    if (i + 1 < slots && layout->slots[i + 1].sampler) sampler = held[i + 1].get();
                                    else if (const auto shared = layout->samplers.find(slot.parameter); shared != layout->samplers.end()) sampler = shared->second.get();
                                }
                                if (const auto written = WriteParameter(set, slot.parameter, slot.type, *held[i], sampler, slot.field); !written) failed = written.error();
                            }
                            kor::Resource<kor::DescriptorSet> made;
                            if (failed.empty()) {
                                made = set.Build();
                                if (!made.Valid()) failed = made.Failure() ? made.Failure()->message : std::string("its descriptor set could not be built");
                            }
                            if (!failed.empty()) { run.cache.erase(KeyOf(entity)); run.changed = true; skip(failed); continue; }
                            cached.set = std::move(made);
                            cached.key.assign(slots, nullptr);
                            cached.held.assign(held, held + slots);
                            cached.members.assign(slots, Member {});
                            for (std::size_t i = 0; i < slots; ++i) {
                                cached.key[i] = held[i].get();
                                if (!layout->slots[i].sampler) cached.members[i] = MemberOf(held[i].get());
                            }
                            run.changed = true;
                        }
                        cached.seen = true;
                        item.set = cached.set;
                        if (run.cover >= 0 && cached.members[static_cast<std::size_t>(run.cover)].image.Get()) {
                            const auto extent = cached.members[static_cast<std::size_t>(run.cover)].image->Extent();
                            for (int axis = 0; axis < 3; ++axis)
                                item.groups[axis] = std::max<kor::u32>((static_cast<kor::u32>(extent[axis]) + run.groupSize[axis] - 1) / run.groupSize[axis], 1);
                        } else {
                            std::ranges::copy(run.groups, item.groups);
                        }
                    } else {
                        std::ranges::copy(run.groups, item.groups);
                    }

                    if (layout && layout->pushSize) {
                        item.push = run.pushed.size();
                        run.pushed.insert(run.pushed.end(), bytes + layout->pushAt, bytes + layout->pushAt + layout->pushSize);
                    }
                    run.items.push_back(std::move(item));
                }
            });
            run.report.ran = run.items.size();

            // Entities that are gone, or no longer run, let go of what was made for them.
            if (std::erase_if(run.cache, [](const auto& entry) { return !entry.second.seen; }) > 0) run.changed = true;
            if (!run.changed) return;
            // Every entity's resources, a slot at a time and once each: what the frame graph orders this pass by.
            for (std::size_t i = 0; i < slots; ++i) {
                run.members[i].clear();
                if (layout->slots[i].sampler) continue;
                std::set<const void*> had;
                for (const auto& cached : run.cache | std::views::values)
                    if (cached.members[i].id && had.insert(cached.members[i].id).second) run.members[i].push_back(cached.members[i]);
                std::ranges::sort(run.members[i], {}, &Member::id);
            }
        }

        /** Readies every pass that runs per entity. Whether what any of them uses changed. */
        bool PrepareAll()
        {
            bool changed = false;
            if (!world) return false;
            for (const auto& id : wanted) {
                const auto& plan = built.at(id).pass;
                if (!plan->entities) continue;
                Prepare(*plan->entities);
                changed = changed || plan->entities->changed;
                plan->entities->changed = false;
            }
            return changed;
        }

        // ---- the frame graph ------------------------------------------------------------------------------------

        /**
         * The order passes go to the frame graph in. It runs the writers of a resource in the order they were added,
         * and what they use says more than that: a pass that only writes a resource makes what one that reads and
         * writes it works on, so it goes first. Where usage does not say, the graph's own order stands.
         */
        [[nodiscard]] static std::vector<std::size_t> OverwritesFirst(const std::vector<std::vector<Declared>>& declared)
        {
            const std::size_t count = declared.size();
            std::vector<std::vector<std::size_t>> after(count);
            std::vector<std::size_t> waits(count, 0);
            for (std::size_t first = 0; first < count; ++first)
                for (std::size_t second = 0; second < count; ++second) {
                    if (first == second) continue;
                    const bool ordered = std::ranges::any_of(declared[first], [&](const Declared& made) {
                        return made.write && !made.read && std::ranges::any_of(declared[second], [&](const Declared& used) { return used.name == made.name && used.read && used.write; });
                    });
                    if (ordered) { after[first].push_back(second); ++waits[second]; }
                }
            std::vector<std::size_t> order;
            std::vector<bool> placed(count, false);
            while (order.size() < count) {
                // The earliest in the graph that waits for nothing; in a circle of them, simply the earliest.
                std::size_t next = count;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i] && waits[i] == 0) next = i;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i]) next = i;
                placed[next] = true;
                order.push_back(next);
                for (const std::size_t follower : after[next]) if (waits[follower] > 0) --waits[follower];
            }
            return order;
        }

        /**
         * Tells the frame graph what each pass uses, and puts the passes in it.
         *
         * A resource a pass names by a wire is imported by itself. The resources entities hold are not known one by
         * one — they come and go — so every parameter a pass fills per entity is a set of them. Two uses that have
         * a resource in common are uses of the same thing, and are given the same name: that is what lets a pass
         * sampling the images entities hold be ordered after the one that writes them.
         */
        void Resolve()
        {
            struct Used {
                std::size_t pass = 0;
                bool read = false, write = false, dynamic = false;
                std::vector<Member> members;
                std::string key;
            };
            std::vector<Used> uses;
            for (std::size_t pass = 0; pass < wanted.size(); ++pass) {
                const auto& plan = built.at(wanted[pass]).pass;
                for (const auto& use : plan->uses) uses.push_back({ pass, use.read, use.write, false, { use.what }, {} });
                if (!plan->entities || !plan->entities->info->layout) continue;
                const auto& slots = plan->entities->info->layout->slots;
                for (std::size_t i = 0; i < slots.size(); ++i)
                    if (!slots[i].sampler) uses.push_back({ pass, slots[i].read, slots[i].write, true, plan->entities->members[i], wanted[pass] + "/" + slots[i].field });
            }

            // Uses with a resource in common, together.
            std::vector<std::size_t> parent(uses.size());
            for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = i;
            const std::function<std::size_t(std::size_t)> root = [&](const std::size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
            std::unordered_map<const void*, std::size_t> firstUse;
            for (std::size_t i = 0; i < uses.size(); ++i)
                for (const auto& member : uses[i].members) {
                    const auto [known, fresh] = firstUse.try_emplace(member.id, i);
                    if (!fresh) parent[root(i)] = root(known->second);
                }

            struct Group {
                bool dynamic = false;
                std::string name;
                std::vector<Member> members;
                std::set<const void*> had;
            };
            std::map<std::size_t, Group> groups;
            for (std::size_t i = 0; i < uses.size(); ++i) {
                Group& group = groups[root(i)];
                if (uses[i].dynamic && !group.dynamic) { group.dynamic = true; group.name = "kgraph.set." + uses[i].key; }
                for (const auto& member : uses[i].members)
                    if (group.had.insert(member.id).second) group.members.push_back(member);
            }
            std::set<std::string> live;
            for (auto& group : groups | std::views::values) {
                if (!group.dynamic) {
                    const Member& member = group.members.front();
                    group.name = std::format("kgraph.{:x}", reinterpret_cast<std::uintptr_t>(member.id));
                    if (member.image.Get()) frameGraph.Import(group.name, member.image);
                    else frameGraph.Import(group.name, member.buffer);
                    continue;
                }
                live.insert(group.name);
                Set& set = sets[group.name];
                if (!set.set) {
                    set.set = std::make_shared<kor::ResourceSet>();
                    frameGraph.ImportSet(group.name, set.set);
                }
                std::vector<const void*> ids(group.had.begin(), group.had.end());
                if (ids == set.members) continue;
                set.set->Clear();
                for (const auto& member : group.members) {
                    if (member.image.Get()) set.set->Add(member.image);
                    else set.set->Add(member.buffer);
                }
                set.members = std::move(ids);
            }
            // A set nothing uses any more holds on to nothing.
            for (auto& [name, set] : sets)
                if (!live.contains(name) && !set.members.empty()) { set.set->Clear(); set.members.clear(); }

            std::vector<std::vector<Declared>> declared(wanted.size());
            for (std::size_t i = 0; i < uses.size(); ++i) {
                const std::string& name = groups.at(root(i)).name;
                auto& list = declared[uses[i].pass];
                const auto known = std::ranges::find(list, name, &Declared::name);
                if (known == list.end()) list.push_back({ name, uses[i].read, uses[i].write });
                else { known->read = known->read || uses[i].read; known->write = known->write || uses[i].write; }
            }

            std::vector<std::string> order;
            std::vector<std::vector<Declared>> ordered;
            for (const std::size_t index : OverwritesFirst(declared)) {
                order.push_back(wanted[index]);
                ordered.push_back(std::move(declared[index]));
            }
            if (order != passOrder) {
                // Which passes there are, or their order, changed: the order they are added in breaks ties, so all go in again.
                for (auto* pass : passes | std::views::values) frameGraph.Remove(*pass);
                passes.clear();
                for (std::size_t i = 0; i < order.size(); ++i)
                    passes.emplace(order[i], &frameGraph.Add<GraphPass>(order[i], built.at(order[i]).pass, std::move(ordered[i])));
                passOrder = std::move(order);
                return;
            }
            bool declarations = false;
            for (std::size_t i = 0; i < order.size(); ++i) {
                GraphPass* pass = passes.at(order[i]);
                const auto& plan = built.at(order[i]).pass;
                if (pass->plan != plan) {
                    declarations = declarations || pass->plan->readsScreen != plan->readsScreen || pass->plan->writesScreen != plan->writesScreen;
                    pass->plan = plan;
                }
                if (pass->declared != ordered[i]) {
                    pass->declared = std::move(ordered[i]);
                    declarations = true;
                }
            }
            if (declarations) frameGraph.Invalidate();
        }

        /** Puts the passes that built in the frame graph, and takes out those that are gone. */
        void Install(const Graph& graph)
        {
            wanted.clear();
            for (const auto& node : graph.nodes)
                if (const auto it = built.find(node.id); it != built.end() && it->second.status.ok && it->second.pass) wanted.push_back(node.id);
            PrepareAll();
            Resolve();
        }

        /**
         * After a shader was edited: what is wired to a parameter it no longer has, or has as another kind, is taken
         * off the node — and a constant it no longer has. What keeps its name and kind stays as it is.
         */
        void ResetStale(Graph& graph)
        {
            for (auto& node : graph.nodes) {
                const NodeType* type = FindType(node.type);
                if (!type || !type->builder || !IsPipeline(type->name)) continue;
                std::vector<kor::ResourceRef<const kor::Shader>> shaders;
                const auto outputOf = [&](const Input& input) -> Held {
                    if (!input.link) return nullptr;
                    const auto source = built.find(input.link->node);
                    if (source == built.end()) return nullptr;
                    const auto output = source->second.outputs.find(input.link->output);
                    return output == source->second.outputs.end() ? nullptr : output->second.resource;
                };
                for (const auto& input : node.inputs | std::views::values)
                    if (const Held resource = outputOf(input); resource && resource->kind == "Shader" && resource->handle)
                        shaders.push_back(kor::capi::ShaderOf(resource->handle));
                // And those it names itself, as they were when it was last built.
                if (const auto it = built.find(node.id); it != built.end() && it->second.pipeline)
                    for (const Held& named : it->second.pipeline->named)
                        if (named && named->handle) shaders.push_back(kor::capi::ShaderOf(named->handle));
                if (shaders.empty() || !std::ranges::all_of(shaders, [](const auto& shader) { return shader.Valid(); })) continue;   // one that does not compile says nothing yet
                const Interface interface = Reflect(shaders);
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    const std::string& name = it->first;
                    bool stale = false;
                    if (name.starts_with("param.") || name.starts_with("constant.")) {
                        const Port* port = interface.Find(name);
                        stale = !port;
                        if (port && port->kind == PortKind::eResource)
                            if (const Held given = outputOf(it->second); given && !given->screen)
                                stale = port->type == "Image" ? given->kind != "Image" && given->kind != "ImageView" : given->kind != port->type;
                    }
                    if (!stale) { ++it; continue; }
                    reset.push_back(std::format("{}.{}", node.id, name));
                    it = node.inputs.erase(it);
                }
            }
        }
    };

    Runtime::Runtime(kor::FrameGraph& frameGraph, kecs::World* world) : _impl(std::make_unique<Impl>(frameGraph, world)) {}
    Runtime::~Runtime() = default;

    // A node that cannot be built keeps why (Status), and an entity that cannot be run is reported (ReportOf): the
    // framework's own account of each failure is kept out of the log, which a graph being edited would fill with it.

    void Runtime::Set(Graph graph)
    {
        const kor::log::Quiet quiet;
        _graph = std::move(graph);
        _impl->Evaluate(_graph);
    }

    void Runtime::Update()
    {
        const kor::log::Quiet quiet;
        if (_impl->reloaded.exchange(false)) {
            _impl->ResetStale(_graph);
            // Which shader it was is not said: every shader node counts as built again, and what is wired to one follows.
            // A pipeline that names its shaders is itself one of them: it is built again, whatever its inputs say.
            for (auto& node : _impl->built | std::views::values) {
                if (node.watches.empty()) continue;
                node.stamp = ++_impl->counter;
                if (node.pipeline) node.signature.clear();
            }
            _impl->Evaluate(_graph);
            return;
        }
        if (_impl->shaderFailed && std::chrono::steady_clock::now() - _impl->lastRetry > std::chrono::milliseconds(500)) {
            _impl->lastRetry = std::chrono::steady_clock::now();
            _impl->shaderFailed = false;    // set again by whichever still fails
            for (auto& node : _impl->built | std::views::values)
                if (!node.status.ok) node.signature.clear();
            _impl->Evaluate(_graph);
            return;
        }
        if (_impl->PrepareAll()) _impl->Resolve();
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
        return ImageRefOf(ResourceOf(id, output).get());
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
        if (const NodeType* type = FindType(node->type)) ports = PortsFor(*type, *node);
        if (const auto it = _impl->built.find(id); it != _impl->built.end() && it->second.pipeline)
            ports.insert(ports.end(), it->second.pipeline->ports.begin(), it->second.pipeline->ports.end());
        return ports;
    }

    std::vector<std::string> Runtime::PerEntityParameters(const std::string_view id) const
    {
        const auto it = _impl->built.find(id);
        return it != _impl->built.end() && it->second.pipeline ? it->second.pipeline->perEntity : std::vector<std::string> {};
    }

    kecs::ComponentId Runtime::ComponentOf(const std::string_view id) const
    {
        const auto it = _impl->built.find(id);
        return it != _impl->built.end() && it->second.status.ok && it->second.pipeline ? it->second.pipeline->component : kecs::NoComponent;
    }

    const kecs::Migration* Runtime::MigrationOf(const std::string_view id) const
    {
        const auto it = _impl->built.find(id);
        return it != _impl->built.end() && it->second.migration ? &*it->second.migration : nullptr;
    }

    const PassReport* Runtime::ReportOf(const std::string_view id) const
    {
        const auto it = _impl->built.find(id);
        return it != _impl->built.end() && it->second.status.ok && it->second.pass && it->second.pass->entities ? &it->second.pass->entities->report : nullptr;
    }

    std::vector<std::string> Runtime::TakeReset() { return std::exchange(_impl->reset, {}); }
}
