#include <keditor/document.h>

#include <algorithm>
#include <set>
#include <cmath>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include <kecs/json.h>
#include <kgraph/catalog.h>
#include <kgraph/meshes.h>
#include <kgraph/entities.h>

namespace keditor
{
    using kgraph::Value;

    namespace {
        using Held = std::shared_ptr<kgraph::Resource>;

        /** What a field of plain numbers is made of: how many, of what. */
        struct Numbers {
            char scalar = 0;        // 'f' float, 'd' double, 'i' int, 'u' unsigned, 'b' bool
            std::size_t bytes = 0;  // of one
            std::size_t count = 0;  // 1: shown as a number, not a list
        };
        bool NumbersOf(const std::string_view type, Numbers& out)
        {
            static const std::pair<std::string_view, Numbers> scalars[] {
                { "f32", { 'f', 4, 1 } }, { "f64", { 'd', 8, 1 } }, { "bool", { 'b', 1, 1 } }, { "bool32", { 'b', 4, 1 } },
                { "i8", { 'i', 1, 1 } }, { "i16", { 'i', 2, 1 } }, { "i32", { 'i', 4, 1 } }, { "i64", { 'i', 8, 1 } },
                { "u8", { 'u', 1, 1 } }, { "u16", { 'u', 2, 1 } }, { "u32", { 'u', 4, 1 } }, { "u64", { 'u', 8, 1 } },
                { "mat4", { 'f', 4, 16 } }, { "quat", { 'f', 4, 4 } } };
            for (const auto& [name, numbers] : scalars)
                if (name == type) { out = numbers; return true; }
            for (const auto& [prefix, scalar] : { std::pair<std::string_view, char> { "vec", 'f' }, { "ivec", 'i' }, { "uvec", 'u' }, { "bvec", 'u' } })
                if (type.size() == prefix.size() + 1 && type.starts_with(prefix) && type.back() >= '2' && type.back() <= '4') {
                    out = { scalar, 4, static_cast<std::size_t>(type.back() - '0') };
                    return true;
                }
            return false;
        }
        double ReadNumber(const Numbers& numbers, const std::byte* at)
        {
            switch (numbers.scalar) {
            case 'f': { float v; std::memcpy(&v, at, 4); return v; }
            case 'd': { double v; std::memcpy(&v, at, 8); return v; }
            case 'i': { std::int64_t v = 0; std::memcpy(&v, at, numbers.bytes);
                        const int shift = static_cast<int>(64 - numbers.bytes * 8); return static_cast<double>((v << shift) >> shift); }
            default: { std::uint64_t v = 0; std::memcpy(&v, at, numbers.bytes); return static_cast<double>(v); }
            }
        }
        void WriteNumber(const Numbers& numbers, std::byte* at, const double value)
        {
            switch (numbers.scalar) {
            case 'f': { const auto v = static_cast<float>(value); std::memcpy(at, &v, 4); break; }
            case 'd': std::memcpy(at, &value, 8); break;
            case 'i': { const auto v = static_cast<std::int64_t>(std::llround(value)); std::memcpy(at, &v, numbers.bytes); break; }
            case 'b': { const std::uint64_t v = value != 0.0 ? 1 : 0; std::memcpy(at, &v, numbers.bytes); break; }
            default: { const auto v = static_cast<std::uint64_t>(std::llround(std::max(value, 0.0))); std::memcpy(at, &v, numbers.bytes); break; }
            }
        }

        bool Structural(const std::string_view component) { return component == "Name"; }
    }

    bool HoldsResource(const std::string_view type)
    {
        return type == "resource" || type == "Image" || type == "Buffer" || type == "Sampler" || type == "BufferView" || type == "AccelerationStructure"
            || type == "Mesh";
    }
    bool Editable(const std::string_view type)
    {
        Numbers numbers;
        return type == "string" || HoldsResource(type) || NumbersOf(type, numbers);
    }

    namespace {
        std::map<const kor::FrameGraph*, Document*>& Documents()
        {
            static std::map<const kor::FrameGraph*, Document*> documents;
            return documents;
        }
    }

    namespace {
        std::map<const kor::FrameGraph*, const kor::FrameGraph*>& Aliases()
        {
            static std::map<const kor::FrameGraph*, const kor::FrameGraph*> aliases;
            return aliases;
        }
    }

    Document* Document::Of(const kor::FrameGraph* frameGraph)
    {
        if (const auto alias = Aliases().find(frameGraph); alias != Aliases().end()) frameGraph = alias->second;
        const auto it = Documents().find(frameGraph);
        return it == Documents().end() ? nullptr : it->second;
    }

    void Document::Alias(const kor::FrameGraph* other, const kor::FrameGraph* frameGraph)
    {
        if (frameGraph) Aliases()[other] = frameGraph;
        else Aliases().erase(other);
    }

    Document::Document(kor::FrameGraph& frameGraph) : _frameGraph(&frameGraph), _runtime(std::make_unique<kgraph::Runtime>(frameGraph, &_world))
    {
        Documents()[_frameGraph] = this;
        kgraph::RegisterCodecs(_codecs, _runtime.get());
        _world.Id<Label>();
        _world.Id<kgraph::Mesh>();
    }
    Document::~Document()
    {
        _scripts.Shutdown();    // while the world they are told of is whole
        if (const auto it = Documents().find(_frameGraph); it != Documents().end() && it->second == this) Documents().erase(it);
    }

    void Document::Update()
    {
        _runtime->Update();
        // A shader edited under the graph: what it took off the nodes is a change like any other.
        for (auto& taken : _runtime->TakeReset()) {
            _notes.push_back(std::format("'{}' was taken off: its shader no longer has it", taken));
            Changed();
        }
        // What the passes say of the entities: it changes with the world, a frame after whatever changed it.
        std::size_t digest = 0;
        const auto mix = [&](const std::size_t value) { digest ^= value + 0x9e3779b97f4a7c15ull + (digest << 6) + (digest >> 2); };
        for (const auto& node : Graph().nodes) {
            // Whether each node is built can change with nothing edited: a shader's file turning up, or being mended.
            if (const auto* status = _runtime->Status(node.id)) {
                mix(status->ok ? 1 : 2);
                mix(std::hash<std::string>{}(status->error));
            }
            if (const auto* report = _runtime->ReportOf(node.id)) {
                mix(report->ran);
                mix(report->skipped);
                mix(std::hash<std::string>{}(report->why));
            }
        }
        if (digest != _reports) { _reports = digest; ++_reported; }
    }

    // ---- the graph ----------------------------------------------------------------------------------------------

    void Document::Apply(kgraph::Graph graph)
    {
        _runtime->Set(std::move(graph));
        Changed();
    }
    void Document::SetGraph(kgraph::Graph graph) { Apply(std::move(graph)); }

    std::string Document::AddNode(const std::string_view type, const double x, const double y, const std::string_view id, const std::string_view pass)
    {
        if (!kgraph::FindType(type)) return {};
        kgraph::Graph graph = Graph();
        std::string name(id);
        if (name.empty() || graph.Find(name)) {
            std::string base(id.empty() ? type : id);
            if (id.empty()) std::ranges::transform(base, base.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            name = base;
            for (int n = 2; graph.Find(name); ++n) name = std::format("{} {}", base, n);
        }
        auto& node = graph.Add(name, std::string(type));
        node.x = x;
        node.y = y;
        node.pass = std::string(pass);
        if (!pass.empty() && std::ranges::find(graph.passes, pass) == graph.passes.end()) graph.passes.emplace_back(pass);
        // It starts with what a node of its kind needs, set to something that works: an image has a size, a format
        // and a usage from the first, and is an image, not a list of things to fill in before it is one.
        for (const auto& port : kgraph::PortsFor(*kgraph::FindType(type), node))
            if (!port.optional && port.kind == kgraph::PortKind::eValue && !port.initial.IsNull()) node.Set(port.name, port.initial);
        Apply(std::move(graph));
        return name;
    }

    // ---- passes -----------------------------------------------------------------------------------------------

    bool Document::AddPass(const std::string_view name, const int index)
    {
        kgraph::Graph graph = Graph();
        if (name.empty() || std::ranges::find(graph.passes, name) != graph.passes.end()) return false;
        if (index >= 0 && static_cast<std::size_t>(index) < graph.passes.size()) graph.passes.insert(graph.passes.begin() + index, std::string(name));
        else graph.passes.emplace_back(name);
        Apply(std::move(graph));
        return true;
    }

    bool Document::RenamePass(const std::string_view pass, const std::string_view name)
    {
        kgraph::Graph graph = Graph();
        const auto at = std::ranges::find(graph.passes, pass);
        if (at == graph.passes.end() || name.empty() || std::ranges::find(graph.passes, name) != graph.passes.end()) return false;
        *at = std::string(name);
        for (auto& node : graph.nodes)
            if (node.pass == pass) node.pass = std::string(name);
        Apply(std::move(graph));
        return true;
    }

    bool Document::MovePass(const std::string_view pass, const int index)
    {
        kgraph::Graph graph = Graph();
        const auto at = std::ranges::find(graph.passes, pass);
        if (at == graph.passes.end()) return false;
        const std::string name = *at;
        graph.passes.erase(at);
        const auto to = std::clamp<std::ptrdiff_t>(index, 0, static_cast<std::ptrdiff_t>(graph.passes.size()));
        graph.passes.insert(graph.passes.begin() + to, name);
        Apply(std::move(graph));
        return true;
    }

    bool Document::RemovePass(const std::string_view pass, const bool keepNodes)
    {
        kgraph::Graph graph = Graph();
        if (std::erase(graph.passes, pass) == 0) return false;
        std::set<std::string> gone;
        for (auto& node : graph.nodes)
            if (node.pass == pass) {
                if (keepNodes) node.pass.clear();
                else gone.insert(node.id);
            }
        if (!gone.empty()) {
            std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return gone.contains(node.id); });
            // What was wired to one of them is no longer wired to anything; a Reference to one stands for nothing.
            for (auto& node : graph.nodes)
                std::erase_if(node.inputs, [&](const auto& input) {
                    return (input.second.link && gone.contains(input.second.link->node))
                        || (node.type == "Reference" && input.first == "node" && input.second.value.String() && gone.contains(*input.second.value.String()));
                });
        }
        Apply(std::move(graph));
        return true;
    }

    bool Document::SetNodePass(const std::string_view id, const std::string_view pass)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node) return false;
        node->pass = std::string(pass);
        if (!pass.empty() && std::ranges::find(graph.passes, pass) == graph.passes.end()) graph.passes.emplace_back(pass);
        Apply(std::move(graph));
        return true;
    }

    std::string Document::Import(const std::string_view id, const std::string_view pass, const double x, const double y)
    {
        kgraph::Graph graph = Graph();
        if (!graph.Find(id)) return {};
        // One that is there already is the one: a thing is imported into a pass once.
        for (const auto& node : graph.nodes)
            if (node.type == "Reference" && node.pass == pass)
                if (const auto target = node.inputs.find("node"); target != node.inputs.end() && target->second.value.String() && *target->second.value.String() == id) return node.id;
        std::string name = pass.empty() ? std::format("{} (imported)", id) : std::format("{} (in {})", id, pass);
        for (int n = 2; graph.Find(name); ++n) name = std::format("{} (in {}) {}", id, pass, n);
        auto& node = graph.Add(name, "Reference");
        node.title = std::string(id);
        node.x = x;
        node.y = y;
        node.pass = std::string(pass);
        node.Set("node", std::string(id));
        Apply(std::move(graph));
        return name;
    }

    std::vector<std::string> Document::Importable(const std::string_view pass) const
    {
        const auto& passes = Graph().passes;
        const auto here = std::ranges::find(passes, pass);
        std::vector<std::string> ids;
        for (const auto& node : Graph().nodes) {
            if (node.type == "Reference" || node.pass == pass) continue;
            const kgraph::NodeType* type = kgraph::FindType(node.type);
            if (!type || type->outputs.empty() || type->category == "Passes") continue;    // a pass node gives nothing to take
            // From the passes before this one, and from what is in no pass.
            if (!node.pass.empty() && std::ranges::find(passes.begin(), here, node.pass) == here) continue;
            ids.push_back(node.id);
        }
        return ids;
    }

    void Document::RemoveNodes(const std::vector<std::string>& ids)
    {
        kgraph::Graph graph = Graph();
        const auto gone = [&](const std::string& id) { return std::ranges::find(ids, id) != ids.end(); };
        std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return gone(node.id); });
        for (auto& node : graph.nodes)
            std::erase_if(node.inputs, [&](const auto& input) { return input.second.link && gone(input.second.link->node); });
        Apply(std::move(graph));
    }

    void Document::MoveNodes(const std::vector<std::string>& ids, const double dx, const double dy)
    {
        kgraph::Graph graph = Graph();
        for (const auto& id : ids)
            if (auto* node = graph.Find(id)) { node->x += dx; node->y += dy; }
        Apply(std::move(graph));
    }

    bool Document::SetTitle(const std::string_view id, std::string title)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node) return false;
        node->title = std::move(title);
        Apply(std::move(graph));
        return true;
    }

    namespace {
        /** An input is given whole or in its parts, not both: giving one takes the other away. */
        void OneWayOfGiving(kgraph::Node& node, const std::string_view input)
        {
            const kgraph::NodeType* type = kgraph::FindType(node.type);
            if (!type) return;
            for (const auto& part : kgraph::PartsOf(*type, input)) node.inputs.erase(part);
            if (const auto dot = input.rfind('.'); dot != std::string_view::npos && dot + 2 == input.size()) {
                const std::string whole(input.substr(0, dot));
                if (std::ranges::contains(kgraph::PartsOf(*type, whole), input)) node.inputs.erase(whole);
            }
        }
    }

    bool Document::SetInput(const std::string_view id, const std::string_view input, Value value)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node) return false;
        OneWayOfGiving(*node, input);
        node->Set(std::string(input), std::move(value));
        Apply(std::move(graph));
        return true;
    }

    bool Document::Connect(const std::string_view fromNode, const std::string_view output, const std::string_view id, const std::string_view input)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node || !graph.Find(fromNode)) return false;
        OneWayOfGiving(*node, input);
        node->Wire(std::string(input), std::string(fromNode), std::string(output));
        Apply(std::move(graph));
        return true;
    }

    bool Document::ClearInput(const std::string_view id, const std::string_view input)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node || node->inputs.erase(std::string(input)) == 0) return false;
        Apply(std::move(graph));
        return true;
    }

    std::size_t Document::DisconnectOutput(const std::string_view id, const std::string_view output)
    {
        kgraph::Graph graph = Graph();
        std::size_t taken = 0;
        for (auto& node : graph.nodes)
            taken += std::erase_if(node.inputs, [&](const auto& input) { return input.second.link && input.second.link->node == id && input.second.link->output == output; });
        if (taken > 0) Apply(std::move(graph));
        return taken;
    }

    // ---- entities -----------------------------------------------------------------------------------------------

    kecs::Entity Document::CreateEntity(const std::string_view name, const kecs::Entity parent)
    {
        const kecs::Entity entity = _world.Create();
        _world.Rename(entity, std::string(name));
        if (!parent.IsNull()) _world.SetParent(entity, parent, -1, false);
        Changed();
        return entity;
    }

    bool Document::SetParent(const kecs::Entity entity, const kecs::Entity parent, const int index)
    {
        if (!_world.SetParent(entity, parent, index)) return false;
        Changed();
        return true;
    }

    bool Document::SetPosition(const kecs::Entity entity, const kecs::Vec3& position)
    {
        if (!_world.SetPosition(entity, position)) return false;
        Changed();
        return true;
    }

    bool Document::SetRotation(const kecs::Entity entity, const kecs::Vec3& degrees)
    {
        if (!_world.SetRotation(entity, degrees)) return false;
        Changed();
        return true;
    }

    bool Document::SetScale(const kecs::Entity entity, const kecs::Vec3& scale)
    {
        if (!_world.SetScale(entity, scale)) return false;
        Changed();
        return true;
    }

    bool Document::DestroyEntity(const kecs::Entity entity)
    {
        if (!_world.Destroy(entity)) return false;
        Changed();
        return true;
    }

    std::vector<kecs::Entity> Document::Entities() const { return _world.All(); }

    std::string Document::NameOf(const kecs::Entity entity) const { return _world.NameOf(entity); }

    bool Document::Rename(const kecs::Entity entity, std::string name)
    {
        if (!_world.Rename(entity, std::move(name))) return false;
        Changed();
        return true;
    }

    kecs::ComponentId Document::AddTag(const std::string_view name)
    {
        if (name.empty()) return kecs::NoComponent;
        if (const kecs::ComponentId known = _world.Find(name); known != kecs::NoComponent) return known;
        kecs::ComponentDescription tag;
        tag.name = std::string(name);
        _tags.emplace_back(name);
        Changed();
        return _world.Register(std::move(tag));
    }

    std::vector<std::string> Document::ComponentNames() const
    {
        std::vector<std::string> names;
        for (kecs::ComponentId id = 0; id < _world.ComponentCount(); ++id)
            if (const auto& name = _world.Describe(id).name; !Structural(name)) names.push_back(name);
        std::ranges::sort(names);
        return names;
    }

    std::vector<std::string> Document::ComponentsOf(const kecs::Entity entity) const
    {
        std::vector<std::string> names;
        if (!_world.Alive(entity)) return names;
        for (const kecs::ComponentId id : _world.ComponentsOf(entity))
            if (const auto& name = _world.Describe(id).name; !Structural(name)) names.push_back(name);
        return names;
    }

    bool Document::AddComponent(const kecs::Entity entity, const std::string_view component)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || _world.Has(entity, id)) return false;
        _world.Add(entity, id);
        Changed();
        return true;
    }

    bool Document::RemoveComponent(const kecs::Entity entity, const std::string_view component)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.Remove(entity, id)) return false;
        Changed();
        return true;
    }

    bool Document::SetEnabled(const kecs::Entity entity, const std::string_view component, const bool enabled)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.SetEnabled(entity, id, enabled)) return false;
        Changed();
        return true;
    }

    bool Document::IsEnabled(const kecs::Entity entity, const std::string_view component) const
    {
        const kecs::ComponentId id = _world.Find(component);
        return id != kecs::NoComponent && _world.Alive(entity) && _world.IsEnabled(entity, id);
    }

    Value Document::Field(const kecs::Entity entity, const std::string_view component, const std::string_view field) const
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity)) return {};
        const auto* bytes = static_cast<const std::byte*>(_world.Get(entity, id));
        const kecs::FieldDescription* found = _world.Describe(id).FindField(field);
        if (!bytes || !found) return {};
        const std::byte* at = bytes + found->offset;
        if (found->type == "string") return Value(*reinterpret_cast<const std::string*>(at));
        if (HoldsResource(found->type)) {
            const Held& held = *reinterpret_cast<const Held*>(at);
            return held && !held->node.empty() ? Value(held->node) : Value {};
        }
        Numbers numbers;
        if (!NumbersOf(found->type, numbers)) return {};
        if (numbers.count == 1) return numbers.scalar == 'b' ? Value(ReadNumber(numbers, at) != 0.0) : Value(ReadNumber(numbers, at));
        Value::List list;
        for (std::size_t i = 0; i < numbers.count; ++i) list.emplace_back(ReadNumber(numbers, at + i * numbers.bytes));
        return Value(std::move(list));
    }

    bool Document::SetField(const kecs::Entity entity, const std::string_view component, const std::string_view field, const Value& value, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.Has(entity, id)) return fail(std::format("the entity has no '{}'", component));
        auto* bytes = static_cast<std::byte*>(const_cast<void*>(std::as_const(_world).Get(entity, id)));
        const kecs::FieldDescription* found = _world.Describe(id).FindField(field);
        if (!found) return fail(std::format("'{}' has no field '{}'", component, field));
        std::byte* at = bytes + found->offset;
        Numbers numbers;
        if (found->type == "string") {
            if (!value.String()) return fail(std::format("'{}' is text", field));
            *reinterpret_cast<std::string*>(at) = *value.String();
        } else if (HoldsResource(found->type)) {
            Held held;
            if (!value.IsNull()) {
                if (!value.String()) return fail(std::format("'{}' holds a resource: the id of the node that makes it", field));
                held = _runtime->ResourceOf(*value.String());
                if (!held) return fail(std::format("there is no node '{}' that makes a resource", *value.String()));
            }
            // What it held may be in what this frame has drawn with: let go of once that is done.
            _runtime->Retire(std::exchange(*reinterpret_cast<Held*>(at), std::move(held)));
        } else if (NumbersOf(found->type, numbers)) {
            if (numbers.count == 1) {
                if (const bool* flag = value.Bool()) WriteNumber(numbers, at, *flag ? 1.0 : 0.0);
                else if (const double* number = value.Number()) WriteNumber(numbers, at, *number);
                else return fail(std::format("'{}' is a {}", field, numbers.scalar == 'b' ? "true or false" : "number"));
            } else {
                const auto* items = value.Items();
                if (!items || items->size() != numbers.count || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; }))
                    return fail(std::format("'{}' is {} numbers", field, numbers.count));
                for (std::size_t i = 0; i < numbers.count; ++i) WriteNumber(numbers, at + i * numbers.bytes, *(*items)[i].Number());
            }
        } else {
            return fail(std::format("'{}' is a {}, which cannot be set here", field, found->type));
        }
        _world.MarkChanged(entity, id);
        Changed();
        return true;
    }

    std::vector<std::string> Document::NodesFor(const std::string_view fieldType) const
    {
        std::vector<std::string> ids;
        for (const auto& node : Graph().nodes) {
            if (node.type == "Reference" || node.type == "Screen") continue;
            const auto resource = _runtime->ResourceOf(node.id);
            if (!resource || resource->screen) continue;
            const bool fits = fieldType == "resource" || resource->kind == fieldType || (fieldType == "Image" && resource->kind == "ImageView");
            if (fits) ids.push_back(node.id);
        }
        // And the meshes there are without a node for any of them.
        if (fieldType == "Mesh" || fieldType == "resource")
            for (const auto& name : kgraph::DefaultMeshNames()) ids.push_back(kgraph::DefaultMeshId(name));
        return ids;
    }

    // ---- entities as data ---------------------------------------------------------------------------------------

    namespace {
        /** A float as the number someone typed, not as the nearest double to what a float holds of it: 0.3, not 0.30000001192. */
        double Tidy(const double value)
        {
            if (!std::isfinite(value) || value == 0.0) return value;
            const double scale = std::pow(10.0, 6 - static_cast<int>(std::ceil(std::log10(std::abs(value)))));
            return std::round(value * scale) / scale;
        }
        Value Tidied(const Value& value)
        {
            if (const double* number = value.Number()) return Value(Tidy(*number));
            if (const auto* items = value.Items()) {
                Value::List out;
                for (const auto& item : *items) out.push_back(Tidied(item));
                return Value(std::move(out));
            }
            return value;
        }
        Value Three(const kecs::Vec3& v) { return Value(Value::List { Tidy(v.x), Tidy(v.y), Tidy(v.z) }); }
        bool Three(const Value* value, kecs::Vec3& out)
        {
            const auto* items = value ? value->Items() : nullptr;
            if (!items || items->size() != 3 || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; })) return false;
            out = { static_cast<float>(*(*items)[0].Number()), static_cast<float>(*(*items)[1].Number()), static_cast<float>(*(*items)[2].Number()) };
            return true;
        }
    }

    Value Document::WriteEntity(const kecs::Entity entity) const
    {
        Value::Members out;
        out.emplace_back("name", _world.NameOf(entity));
        out.emplace_back("position", Three(_world.PositionOf(entity)));
        out.emplace_back("rotation", Three(_world.RotationOf(entity)));
        out.emplace_back("scale", Three(_world.ScaleOf(entity)));
        Value::Members components;
        Value::List disabled;
        for (const std::string& component : ComponentsOf(entity)) {
            Value::Members fields;
            for (const auto& field : _world.Describe(_world.Find(component)).fields) {
                fields.emplace_back(field.name, Tidied(Field(entity, component, field.name)));
            }
            components.emplace_back(component, Value(std::move(fields)));
            if (!IsEnabled(entity, component)) disabled.emplace_back(component);
        }
        out.emplace_back("components", Value(std::move(components)));
        if (!disabled.empty()) out.emplace_back("disabled", Value(std::move(disabled)));
        Value::List children;
        for (const kecs::Entity child : _world.ChildrenOf(entity)) children.push_back(WriteEntity(child));
        out.emplace_back("children", Value(std::move(children)));
        return Value(std::move(out));
    }

    kecs::Entity Document::ReadEntity(const Value& data, const kecs::Entity parent, std::vector<std::string>* problems)
    {
        const auto problem = [&](std::string what) { if (problems) problems->push_back(std::move(what)); };
        const Value* name = data.Find("name");
        const kecs::Entity entity = CreateEntity(name && name->String() ? *name->String() : std::string("Entity"), parent);
        kecs::Vec3 three;
        if (Three(data.Find("position"), three)) _world.SetPosition(entity, three);
        if (Three(data.Find("rotation"), three)) _world.SetRotation(entity, three);
        if (Three(data.Find("scale"), three)) _world.SetScale(entity, three);
        if (const Value* components = data.Find("components"); components && components->Named())
            for (const auto& [component, fields] : *components->Named()) {
                if (!AddComponent(entity, component)) { problem(std::format("there is no component '{}': left out", component)); continue; }
                if (!fields.Named()) continue;
                for (const auto& [field, given] : *fields.Named()) {
                    if (given.IsNull()) continue;
                    std::string error;
                    if (!SetField(entity, component, field, given, &error)) problem(std::format("'{}.{}': {}", component, field, error));
                }
            }
        if (const Value* disabled = data.Find("disabled"); disabled && disabled->Items())
            for (const auto& component : *disabled->Items())
                if (component.String()) SetEnabled(entity, *component.String(), false);
        if (const Value* children = data.Find("children"); children && children->Items())
            for (const auto& child : *children->Items()) ReadEntity(child, entity, problems);
        Changed();
        return entity;
    }

    std::string Document::UniqueName(const std::string& name, const std::vector<std::string>& siblings)
    {
        if (std::ranges::find(siblings, name) == siblings.end()) return name;
        // "Cube 2" copied is "Cube 3", not "Cube 2 2".
        std::string base = name;
        if (const auto space = base.rfind(' '); space != std::string::npos && space + 1 < base.size()
            && std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space) + 1, base.end(), [](const unsigned char c) { return std::isdigit(c) != 0; }))
            base.erase(space);
        for (int n = 2;; ++n)
            if (std::string candidate = std::format("{} {}", base, n); std::ranges::find(siblings, candidate) == siblings.end()) return candidate;
    }

    kecs::Entity Document::Duplicate(const kecs::Entity entity)
    {
        if (!_world.Alive(entity)) return kecs::NullEntity;
        const kecs::Entity parent = _world.ParentOf(entity);
        const auto& siblings = parent.IsNull() ? _world.Roots() : _world.ChildrenOf(parent);
        std::vector<std::string> names;
        for (const kecs::Entity sibling : siblings) names.push_back(_world.NameOf(sibling));
        const auto place = static_cast<int>(std::ranges::find(siblings, entity) - siblings.begin()) + 1;
        const kecs::Entity copy = ReadEntity(WriteEntity(entity), parent);
        _world.Rename(copy, UniqueName(_world.NameOf(entity), names));
        _world.SetParent(copy, parent, place, false);
        Changed();
        return copy;
    }

    std::string Document::WorldJson() const { return kecs::Save(_world, _codecs).json; }

    bool Document::RestoreWorld(const std::string_view json, std::string& error)
    {
        for (const kecs::Entity root : std::vector(_world.Roots())) _world.Destroy(root);
        const auto loaded = kecs::Load(_world, json, _codecs);
        Changed();
        if (!loaded.ok) { error = loaded.error; return false; }
        return true;
    }

    // ---- the file -----------------------------------------------------------------------------------------------

    std::string Document::ToJson() const
    {
        std::string tags;
        for (const auto& tag : _tags) tags += std::format("{}{}", tags.empty() ? "" : ", ", kecs::json::Quote(tag));
        return std::format("{{\n\"koral-scene\": 1,\n\"tags\": [{}],\n\"graph\": {},\n\"world\": {}\n}}\n", tags, Graph().ToJson(), kecs::Save(_world, _codecs).json);
    }

    bool Document::FromJson(const std::string_view json, std::string& error)
    {
        kecs::json::Value file;
        if (!kecs::json::Parse(json, file, error)) return false;
        const auto* version = file.Find("koral-scene");
        const auto* graphText = file.Find("graph");
        const auto* worldText = file.Find("world");
        if (!version || version->number != 1.0 || !graphText || !worldText) { error = "it is not a scene: 'koral-scene' 1, with a 'graph' and a 'world'"; return false; }

        kgraph::Graph graph;
        if (!kgraph::Graph::FromJson(graphText->raw, graph, error)) return false;
        // The graph first: it makes the components its pipelines' entities have, and the resources they hold.
        _runtime->Set(std::move(graph));
        if (const auto* tags = file.Find("tags"))
            for (const auto& tag : tags->items) AddTag(tag.string);
        const auto loaded = kecs::Load(_world, worldText->raw, _codecs);
        // A scene saved before a name was the world's own had it as a component: the name is taken from it.
        for (const kecs::Entity entity : loaded.entities)
            if (const Label* label = _world.Get<Label>(entity)) {
                _world.Rename(entity, label->text);
                _world.Remove<Label>(entity);
            }
        if (!loaded.ok) { error = loaded.error; return false; }
        for (const auto& problem : loaded.problems) _notes.push_back(problem);
        Changed();
        _saved = _revision;
        return true;
    }

    bool Document::Save(const std::filesystem::path& path, std::string& error)
    {
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { error = std::format("'{}' cannot be written", path.string()); return false; }
        out << ToJson();
        if (!out.flush()) { error = std::format("'{}' could not be written whole", path.string()); return false; }
        _saved = _revision;
        return true;
    }

    bool Document::Load(const std::filesystem::path& path, std::string& error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) { error = std::format("'{}' cannot be read", path.string()); return false; }
        std::stringstream text;
        text << in.rdbuf();
        return FromJson(text.str(), error);
    }
}
