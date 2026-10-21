#include <keditor/document.h>

#include <algorithm>
#include <set>
#include <cmath>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>

#include <kecs/json.h>
#include <kgraph/catalog.h>
#include <kgraph/meshes.h>
#include <kgraph/entities.h>
#include <kscript/declarations.h>
#include <shader.h>

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

        // What the editor keeps on an entity of its own, which is no component to show, give or copy: a name from an
        // older file, and which prefab an instance is of.
        bool Structural(const std::string_view component) { return component == "Name" || component == "Prefab" || component == "PrefabPart"; }
    }

    bool HoldsResource(const std::string_view type)
    {
        return type == "resource" || type == "Image" || type == "Buffer" || type == "Sampler" || type == "BufferView" || type == "AccelerationStructure"
            || type == "Mesh";
    }
    bool Editable(const std::string_view type)
    {
        Numbers numbers;
        return type == "string" || type == "entity" || HoldsResource(type) || NumbersOf(type, numbers);
    }

    Value EntityValue(const kecs::Entity entity)
    {
        if (entity.IsNull()) return {};
        return Value(Value::Members { { "entity", static_cast<double>(entity.index) }, { "generation", static_cast<double>(entity.generation) } });
    }

    std::optional<kecs::Entity> EntityOfValue(const Value& value)
    {
        if (value.IsNull()) return kecs::NullEntity;
        const Value* index = value.Find("entity");
        const Value* generation = value.Find("generation");
        if (!index || !index->Number() || !generation || !generation->Number()) return std::nullopt;
        return kecs::Entity { static_cast<std::uint32_t>(*index->Number()), static_cast<std::uint32_t>(*generation->Number()) };
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
        _world.Id<PrefabLink>();
        _world.Id<PrefabPart>();
        _world.Id<kgraph::Mesh>();
        // What every scene has, whatever its project: the engine's package — the Camera, the lights, the Sky — and its
        // shaders' modules, for any shader to import (`import entity;`).
        for (auto& problem : kscript::DeclarePackage(_scripts)) _notes.push_back(std::move(problem));
        static const bool searched = (kor::Shader::AddSearchPath(kscript::PackageDirectory() / "shaders"), true);
        (void)searched;
    }

    void Document::DeclareScripts(const std::filesystem::path& folder)
    {
        std::error_code ignored;
        if (!std::filesystem::exists(folder, ignored)) return;     // a folder of scripts, or one script's file
        auto declared = kscript::DeclareFiles(folder);
        for (auto& problem : declared.problems) _notes.push_back(std::move(problem));
        for (auto& type : declared.types) {
            if (type.makesResources) { DeclareResourceScript(std::move(type)); continue; }    // what a Script node runs, no component
            std::string error;
            const std::string name = type.name;
            if (_scripts.Declare(std::move(type), &error) == kecs::NoComponent) _notes.push_back(std::format("{}: {}", name, error));
        }
        Changed();
    }

    namespace {
        kgraph::ResourceScript ResourceScriptOf(kscript::ScriptType type)
        {
            kgraph::ResourceScript script;
            script.name = std::move(type.name);
            for (auto& declared : type.resources) {
                kgraph::ScriptResource resource { std::move(declared.name), std::move(declared.kind), {} };
                for (auto& [input, json] : declared.settings) {
                    kecs::json::Value parsed;
                    std::string ignored;
                    if (kecs::json::Parse(json, parsed, ignored)) resource.settings.emplace_back(std::move(input), kgraph::Value::FromJson(parsed));
                }
                script.resources.push_back(std::move(resource));
            }
            script.fields = std::move(type.fields);
            script.initial = std::move(type.initial);
            script.size = type.size;
            return script;
        }
    }

    void Document::DeclareResourceScript(kscript::ScriptType type)
    {
        // One that runs already is what its host made of it: newer than what its source is read as.
        for (const auto& running : _runtime->ResourceScripts())
            if (running.name == type.name && running.fill) return;
        _runtime->SetResourceScript(ResourceScriptOf(std::move(type)));
        Changed();
    }

    void Document::RegisterResourceScript(kscript::ScriptType type, std::function<std::string(std::span<kgraph::Contents>, std::span<const std::byte>)> fill)
    {
        kgraph::ResourceScript script = ResourceScriptOf(std::move(type));
        script.fill = std::move(fill);
        // What its fields start as is its source's to say: a host registers the shape, and not that. And what it makes,
        // when the host does not say.
        for (const auto& declared : _runtime->ResourceScripts()) {
            if (declared.name != script.name) continue;
            if (script.initial.empty() && declared.size == script.size) script.initial = declared.initial;
            if (script.resources.empty()) script.resources = declared.resources;
            for (auto& field : script.fields)
                if (const auto was = std::ranges::find(declared.fields, field.name, &kecs::FieldDescription::name);
                    was != declared.fields.end() && !std::isfinite(field.min) && !std::isfinite(field.max) && !field.logarithmic) {
                    field.min = was->min;
                    field.max = was->max;
                    field.logarithmic = was->logarithmic;
                }
        }
        _runtime->SetResourceScript(std::move(script));
        Changed();
    }

    void Document::UnregisterResourceScript(const std::string_view name)
    {
        // Its code is going: it is declared still, as its source says, and nothing runs it.
        for (auto script : _runtime->ResourceScripts())
            if (script.name == name && script.fill) {
                script.fill = nullptr;
                _runtime->SetResourceScript(std::move(script));
                Changed();
            }
    }

    std::vector<kgraph::ResourceScript> Document::ResourceScripts() const { return _runtime->ResourceScripts(); }
    Document::~Document()
    {
        _scripts.Shutdown();    // while the world they are told of is whole
        if (const auto it = Documents().find(_frameGraph); it != Documents().end() && it->second == this) Documents().erase(it);
    }

    void Document::Update()
    {
        _runtime->Update();
        // A model that was being read, read: its things are made under the entity that stood for it.
        for (auto it = _importing.begin(); it != _importing.end();) {
            std::string why;
            if (const auto model = _runtime->ModelOf(it->first, &why)) Imported(it->second, *model);
            else if (why.empty()) { ++it; continue; }
            else { _notes.push_back(why); Changed(); }
            it = _importing.erase(it);
        }
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
                mix(std::hash<std::string>{}(status->note));    // a script it waits for, run now
            }
            if (const auto* report = _runtime->ReportOf(node.id)) {
                // (How many it is for, in sight or out of it: which of them are in sight changes as the camera turns, and
                // is nothing the panels need to be made again for every frame.)
                mix(report->ran + report->culled);
                mix(report->skipped);
                mix(std::hash<std::string>{}(report->why));
            }
        }
        if (digest != _reports) { _reports = digest; ++_reported; }
    }

    // ---- the graph ----------------------------------------------------------------------------------------------

    void Document::Apply(kgraph::Graph graph)
    {
        graph.OrderByRate();    // what is made once comes first, then the fixed step, whatever the edit did to the order
        // Where every node is seen at once: what is gone is not anywhere there, and what is made there is where it was put.
        std::erase_if(graph.overview, [&](const auto& entry) { return !graph.Find(entry.first); });
        if (_makeInOverview && std::ranges::any_of(graph.nodes, [&](const kgraph::Node& node) { return !Graph().Find(node.id); })) {
            // The first made there holds the rest where they are seen, so that it is beside what it was put beside.
            if (graph.overview.empty())
                for (const auto& [id, at] : OverviewPlaces())
                    if (graph.Find(id)) graph.overview[id] = at;
            for (const auto& node : graph.nodes)
                if (!graph.overview.contains(node.id)) graph.overview[node.id] = { node.x, node.y };
        }
        WithFunctions(graph);
        _runtime->Set(std::move(graph));
        Changed();
    }
    void Document::SetGraph(kgraph::Graph graph) { Apply(std::move(graph)); }

    std::string Document::AddNode(std::string_view type, const double x, const double y, const std::string_view id, const std::string_view pass)
    {
        // A node of a component — `PointLight`, `PointLight Buffer` — is a node of a type there is, set up for it.
        std::optional<kgraph::Preset> preset;
        if (!kgraph::FindType(type)) {
            const auto presets = Presets();
            const auto found = std::ranges::find(presets, type, &kgraph::Preset::name);
            if (found == presets.end()) return {};
            preset = *found;
        }
        const std::string kind = preset ? preset->type : std::string(type);
        kgraph::Graph graph = Graph();
        std::string name(id);
        if (name.empty() || graph.Find(name)) {
            std::string base(id.empty() ? type : id);
            if (id.empty()) std::ranges::transform(base, base.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            name = base;
            for (int n = 2; graph.Find(name); ++n) name = std::format("{} {}", base, n);
        }
        auto& node = graph.Add(name, kind);
        node.x = x;
        node.y = y;
        node.pass = std::string(pass);
        if (preset) node.title = preset->name;
        if (!pass.empty() && std::ranges::find(graph.passes, pass) == graph.passes.end()) graph.passes.emplace_back(pass);
        // It starts with what a node of its kind needs, set to something that works: an image has a size, a format
        // and a usage from the first, and is an image, not a list of things to fill in before it is one.
        for (const auto& port : kgraph::PortsFor(*kgraph::FindType(kind), node))
            if (!port.optional && port.kind == kgraph::PortKind::eValue && !port.initial.IsNull()) node.Set(port.name, port.initial);
        if (preset)
            for (const auto& [input, value] : preset->inputs) node.Set(input, value);
        Apply(std::move(graph));
        return name;
    }

    namespace {
        /** The References something is wired from. */
        std::set<std::string> WiredReferences(const kgraph::Graph& graph)
        {
            std::set<std::string> wired;
            for (const auto& node : graph.nodes)
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link)
                        if (const kgraph::Node* from = graph.Find(input.link->node); from && from->type == "Reference") wired.insert(from->id);
            return wired;
        }

        /**
         * A Reference that loses the last wire from it goes with it: it is there to be wired from, and what it stands for
         * is still in the drawer to bring in again. One that was never wired — just brought in — stays.
         */
        void LetGoOfUnwired(kgraph::Graph& graph, const kgraph::Graph& was)
        {
            const auto before = WiredReferences(was), now = WiredReferences(graph);
            std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return node.type == "Reference" && before.contains(node.id) && !now.contains(node.id); });
        }
    }

    // ---- passes -----------------------------------------------------------------------------------------------

    namespace {
        /** Where a pass of a rate can be in the list: among the others of its rate, which are together — setup, fixed, every frame. */
        std::pair<std::ptrdiff_t, std::ptrdiff_t> RangeOf(const kgraph::Graph& graph, const kgraph::PassRate rate)
        {
            const auto count = [&](const kgraph::PassRate of) {
                return static_cast<std::ptrdiff_t>(std::ranges::count_if(graph.passes, [&](const std::string& pass) { return graph.RateOf(pass) == of; }));
            };
            const std::ptrdiff_t setups = count(kgraph::PassRate::eSetup), fixed = count(kgraph::PassRate::eFixed);
            switch (rate) {
            case kgraph::PassRate::eSetup: return { 0, setups };
            case kgraph::PassRate::eFixed: return { setups, setups + fixed };
            default: return { setups + fixed, static_cast<std::ptrdiff_t>(graph.passes.size()) };
            }
        }

        /** The setup and fixed pages as the list has them: the order they run in. */
        void RatesInOrder(kgraph::Graph& graph)
        {
            std::vector<std::string> setup, fixed;
            for (const auto& pass : graph.passes) {
                if (graph.IsSetup(pass)) setup.push_back(pass);
                else if (graph.IsFixed(pass)) fixed.push_back(pass);
            }
            graph.setup = std::move(setup);
            graph.fixed = std::move(fixed);
        }
    }

    bool Document::AddPass(const std::string_view name, const int index, const kgraph::PassRate rate)
    {
        kgraph::Graph graph = Graph();
        if (name.empty() || std::ranges::find(graph.passes, name) != graph.passes.end()) return false;
        const auto [first, end] = RangeOf(graph, rate);
        const std::ptrdiff_t at = index < 0 ? end : std::clamp<std::ptrdiff_t>(index, first, end);
        graph.passes.insert(graph.passes.begin() + at, std::string(name));
        if (rate == kgraph::PassRate::eSetup) graph.setup.emplace_back(name);
        if (rate == kgraph::PassRate::eFixed) graph.fixed.emplace_back(name);
        RatesInOrder(graph);
        Apply(std::move(graph));
        return true;
    }

    bool Document::SetPassRate(const std::string_view pass, const kgraph::PassRate rate)
    {
        kgraph::Graph graph = Graph();
        const auto at = std::ranges::find(graph.passes, pass);
        const kgraph::PassRate was = graph.RateOf(pass);
        if (at == graph.passes.end() || was == rate) return false;
        // Where it was, as near as it can be: at the end of its new rate's run coming from before it, at the start
        // coming from after it (setup, fixed, every frame).
        const std::string name = *at;
        graph.passes.erase(at);
        std::erase(graph.setup, name);
        std::erase(graph.fixed, name);
        const auto order = [](const kgraph::PassRate r) { return r == kgraph::PassRate::eSetup ? 0 : r == kgraph::PassRate::eFixed ? 1 : 2; };
        const auto [first, end] = RangeOf(graph, rate);
        graph.passes.insert(graph.passes.begin() + (order(was) < order(rate) ? first : end), name);
        if (rate == kgraph::PassRate::eSetup) graph.setup.push_back(name);
        if (rate == kgraph::PassRate::eFixed) graph.fixed.push_back(name);
        RatesInOrder(graph);
        Apply(std::move(graph));
        return true;
    }

    bool Document::RenamePass(const std::string_view pass, const std::string_view name)
    {
        kgraph::Graph graph = Graph();
        const auto at = std::ranges::find(graph.passes, pass);
        if (at == graph.passes.end() || name.empty() || std::ranges::find(graph.passes, name) != graph.passes.end()) return false;
        *at = std::string(name);
        std::ranges::replace(graph.setup, std::string(pass), std::string(name));
        std::ranges::replace(graph.fixed, std::string(pass), std::string(name));
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
        // Among those of its rate: setup before the fixed step, and that before anything done every frame.
        const auto [first, end] = RangeOf(graph, graph.RateOf(name));
        const auto to = std::clamp<std::ptrdiff_t>(index, first, end);
        graph.passes.insert(graph.passes.begin() + to, name);
        RatesInOrder(graph);
        Apply(std::move(graph));
        return true;
    }

    bool Document::RemovePass(const std::string_view pass, const bool keepNodes)
    {
        kgraph::Graph graph = Graph();
        if (std::erase(graph.passes, pass) == 0) return false;
        std::erase(graph.setup, pass);
        std::erase(graph.fixed, pass);
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

    namespace {
        /**
         * Wires the nodes of @p graph that @p touched names — and those wired from them — as passes are wired: a node
         * takes what another pass made through a Reference to it in its own pass, when that pass is before its own (or
         * the node is in none); straight, when the two are of one pass, or the other is a later one. A Reference made
         * for it is put beside the node that takes it. Those that stand for a node of their own pass, wired from by
         * nothing now, go.
         */
        void WireAcrossPasses(kgraph::Graph& graph, const std::set<std::string>& touched)
        {
            const auto order = [&](const std::string& pass) -> int {
                if (pass.empty()) return -1;
                const auto at = std::ranges::find(graph.passes, pass);
                return at == graph.passes.end() ? static_cast<int>(graph.passes.size()) : static_cast<int>(at - graph.passes.begin());
            };
            const auto standsFor = [&](const kgraph::Node& node) -> std::string {
                const auto target = node.inputs.find("node");
                return node.type == "Reference" && target != node.inputs.end() && target->second.value.String() ? *target->second.value.String() : std::string();
            };
            // What a node is, through the References between: the node that makes it.
            const auto origin = [&](std::string id) {
                for (int hops = 0; hops < 16; ++hops) {
                    const kgraph::Node* node = graph.Find(id);
                    const std::string target = node ? standsFor(*node) : std::string();
                    if (target.empty() || !graph.Find(target)) return id;
                    id = target;
                }
                return id;
            };
            const auto referenceIn = [&](const std::string& target, const std::string& pass, const double x, const double y) {
                for (const auto& node : graph.nodes)
                    if (node.pass == pass && standsFor(node) == target) return node.id;
                std::string name = std::format("{} (in {})", target, pass);
                for (int n = 2; graph.Find(name); ++n) name = std::format("{} (in {}) {}", target, pass, n);
                auto& made = graph.Add(name, "Reference");
                made.title = target;
                made.x = x;
                made.y = y;
                made.pass = pass;
                made.Set("node", target);
                return name;
            };
            // What was wired from before: a Reference that loses the last that took it, here, goes after.
            std::set<std::string> wiredBefore;
            for (const auto& node : graph.nodes)
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link) wiredBefore.insert(input.link->node);
            // Who is wired again: the nodes touched, and every node wired from one of them.
            std::vector<std::string> takers;
            for (const auto& node : graph.nodes) {
                if (node.type == "Reference") continue;
                bool wire = touched.contains(node.id);
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link && (touched.contains(input.link->node) || touched.contains(origin(input.link->node)))) wire = true;
                if (wire) takers.push_back(node.id);
            }
            for (const std::string& id : takers) {
                std::vector<std::pair<std::string, std::string>> rewired;     // input, the node it is wired from now
                const kgraph::Node* taker = graph.Find(id);
                if (!taker) continue;
                const std::string pass = taker->pass;
                const double x = taker->x, y = taker->y;
                // Its wires, copied: a Reference made below is a node more, and the nodes may move in memory for it.
                std::vector<std::pair<std::string, std::string>> wires;     // input, the node it is wired from
                for (const auto& [name, input] : taker->inputs)
                    if (input.link) wires.emplace_back(name, input.link->node);
                for (const auto& [name, wiredFrom] : wires) {
                    const std::string source = origin(wiredFrom);
                    const kgraph::Node* made = graph.Find(source);
                    if (!made) continue;
                    // Of an earlier pass (or of none) into one of a pass: brought in. Otherwise wired straight to it.
                    const bool bring = !pass.empty() && made->pass != pass && order(made->pass) < order(pass);
                    const std::string from = bring ? referenceIn(source, pass, x - 260.0, y) : source;
                    if (from != wiredFrom) rewired.emplace_back(name, from);
                }
                kgraph::Node* node = graph.Find(id);     // (found again: a Reference made may have moved the nodes)
                for (const auto& [name, from] : rewired) node->inputs.at(name).link->node = from;
            }
            // A Reference that nothing is wired from now — one that stands for a node of its own pass, or one whose last
            // taker this took from it: nothing to bring in. (One nothing was wired from before is left as it was.)
            std::set<std::string> wiredFrom;
            for (const auto& node : graph.nodes)
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link) wiredFrom.insert(input.link->node);
            std::erase_if(graph.nodes, [&](const kgraph::Node& node) {
                const std::string target = standsFor(node);
                const kgraph::Node* of = target.empty() ? nullptr : graph.Find(target);
                if (!of || wiredFrom.contains(node.id)) return false;
                return of->pass == node.pass || wiredBefore.contains(node.id);
            });
        }
    }

    bool Document::MoveNodesToPass(const std::vector<std::string>& ids, const std::string_view pass)
    {
        kgraph::Graph graph = Graph();
        std::set<std::string> moved;
        for (const auto& id : ids)
            if (kgraph::Node* node = graph.Find(id)) { node->pass = std::string(pass); moved.insert(node->id); }
        if (moved.empty()) return false;
        if (!pass.empty() && std::ranges::find(graph.passes, pass) == graph.passes.end()) graph.passes.emplace_back(pass);
        WireAcrossPasses(graph, moved);
        Apply(std::move(graph));
        return true;
    }

    std::string Document::MakeBufferFor(const std::string_view pipeline, std::string_view parameter, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return std::string(); };
        if (parameter.starts_with("param.")) parameter.remove_prefix(6);
        const kgraph::Node* found = Graph().Find(pipeline);
        if (!found) return fail(std::format("there is no node '{}'", pipeline));
        const kgraph::Node node = *found;       // (a copy: every step below makes the graph anew)
        // The shaders it names, a stage each: the module, and the entry point — the stage's name and Main when not said.
        std::vector<std::pair<std::string, std::string>> shaders;
        for (const auto& [name, input] : node.inputs) {
            if (!name.ends_with(".module") || !input.value.String() || input.value.String()->empty()) continue;
            const std::string stage = name.substr(0, name.size() - 7);
            const auto entry = node.inputs.find(stage + ".entry");
            shaders.emplace_back(*input.value.String(), entry != node.inputs.end() && entry->second.value.String() && !entry->second.value.String()->empty()
                                                           ? *entry->second.value.String() : stage.substr(0, stage.find_first_of(".[")) + "Main");
        }
        if (shaders.empty()) return fail(std::format("'{}' names no shader yet: the buffer is laid out as its shader declares it", pipeline));

        // The buffer is laid out by a shader that uses it: each stage's is tried, and the first that declares it is the one.
        const std::string id = AddNode("ShaderBuffer", node.x - 320.0, node.y + 40.0, std::format("{} data", parameter), node.pass);
        SetInput(id, "parameter", std::string(parameter));
        std::string why;
        for (const auto& [module, entry] : shaders) {
            SetInput(id, "module", module);
            SetInput(id, "entry", entry);
            const kgraph::NodeStatus* status = _runtime->Status(id);
            if (status && status->ok) {
                Connect(id, "out", node.id, std::format("param.{}", parameter));
                return id;
            }
            if (status) why = status->error;
        }
        RemoveNodes({ id });
        return fail(why.empty() ? std::format("no shader of '{}' declares a buffer '{}'", pipeline, parameter) : why);
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
        LetGoOfUnwired(graph, Graph());
        Apply(std::move(graph));
    }

    void Document::MoveNodes(const std::vector<std::string>& ids, const double dx, const double dy)
    {
        kgraph::Graph graph = Graph();
        for (const auto& id : ids)
            if (auto* node = graph.Find(id)) { node->x += dx; node->y += dy; }
        Apply(std::move(graph));
    }

    std::vector<std::string> Document::PasteNodes(std::vector<kgraph::Node> nodes, const double x, const double y, const std::string* pass)
    {
        std::vector<std::string> made;
        if (nodes.empty()) return made;
        kgraph::Graph graph = Graph();
        double left = nodes.front().x, top = nodes.front().y;
        for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }
        // A name of its own for each: what it was called, and the first number after it that is free.
        std::map<std::string, std::string> renamed;
        std::set<std::string> taken;
        for (const auto& node : graph.nodes) taken.insert(node.id);
        for (const auto& node : nodes) {
            std::string base = node.id;
            if (const auto space = base.rfind(' '); space != std::string::npos && space + 1 < base.size()
                && std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space) + 1, base.end(), [](const unsigned char c) { return std::isdigit(c) != 0; }))
                base.erase(space);
            std::string name = base;
            for (int n = 2; taken.contains(name); ++n) name = std::format("{} {}", base, n);
            taken.insert(name);
            renamed[node.id] = name;
        }
        // What a node is, through the References between: one that brings in a node copied with it is that node.
        const auto through = [&](std::string id) {
            for (int hops = 0; hops < 16 && !renamed.contains(id); ++hops) {
                const kgraph::Node* node = graph.Find(id);
                const auto target = node && node->type == "Reference" ? node->inputs.find("node") : decltype(node->inputs.end()) {};
                if (!node || node->type != "Reference" || target == node->inputs.end() || !target->second.value.String()) break;
                id = *target->second.value.String();
            }
            return id;
        };
        for (auto& node : nodes) {
            // Wired among themselves as they were — straight, or through a Reference to one of them; from the rest, as
            // far as the rest is still here.
            for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                if (auto& link = it->second.link) {
                    if (const auto within = renamed.find(through(link->node)); within != renamed.end()) link->node = within->second;
                    else if (!graph.Find(link->node)) { it = node.inputs.erase(it); continue; }
                }
                ++it;
            }
            if (node.title == node.id) node.title.clear();
            node.id = renamed[node.id];
            node.x += x - left;
            node.y += y - top;
            if (pass) node.pass = *pass;
            if (!node.pass.empty() && std::ranges::find(graph.passes, node.pass) == graph.passes.end()) graph.passes.push_back(node.pass);
            made.push_back(node.id);
            graph.nodes.push_back(std::move(node));
        }
        // Into a pass: what they take of the others is brought in, as a pass brings in what it uses.
        if (pass) {
            WireAcrossPasses(graph, std::set<std::string>(made.begin(), made.end()));
            std::erase_if(made, [&](const std::string& id) { return !graph.Find(id); });  // a Reference with nothing left to bring in
        }
        Apply(std::move(graph));
        return made;
    }

    std::vector<std::string> Document::DuplicateNodes(const std::vector<std::string>& ids, const double dx, const double dy)
    {
        std::vector<kgraph::Node> nodes;
        for (const auto& id : ids)
            if (const kgraph::Node* node = Graph().Find(id)) nodes.push_back(*node);
        if (nodes.empty()) return {};
        double left = nodes.front().x, top = nodes.front().y;
        for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }
        return PasteNodes(std::move(nodes), left + dx, top + dy);
    }

    void Document::PlaceNodes(const std::map<std::string, std::pair<double, double>>& where)
    {
        kgraph::Graph graph = Graph();
        bool moved = false;
        for (const auto& [id, at] : where)
            if (auto* node = graph.Find(id); node && (node->x != at.first || node->y != at.second)) { node->x = at.first; node->y = at.second; moved = true; }
        if (moved) Apply(std::move(graph));
    }

    std::map<std::string, std::pair<double, double>> Document::OverviewPlaces() const
    {
        const kgraph::Graph& graph = Graph();
        std::map<std::string, std::pair<double, double>> where;
        // Each pass's page, where it would be: beside the one before, by as much as that one is wide and a gap.
        std::vector<std::string> order { std::string() };
        order.insert(order.end(), graph.passes.begin(), graph.passes.end());
        constexpr double CardWidth = 320, Gap = 240;
        double x = 0;
        for (const auto& pass : order) {
            double left = 0, top = 0, right = 0;
            bool any = false;
            for (const auto& node : graph.nodes) {
                if (node.pass != pass) continue;
                if (!any) { left = right = node.x; top = node.y; any = true; }
                left = std::min(left, node.x); right = std::max(right, node.x); top = std::min(top, node.y);
            }
            if (!any) continue;
            for (const auto& node : graph.nodes)
                if (node.pass == pass) where[node.id] = { std::round(node.x - left + x), std::round(node.y - top) };
            x += right - left + CardWidth + Gap;
        }
        // What is in no pass the graph lists: where it is.
        for (const auto& node : graph.nodes)
            if (!where.contains(node.id)) where[node.id] = { node.x, node.y };
        for (const auto& [id, at] : graph.overview)
            if (where.contains(id)) where[id] = at;
        return where;
    }

    void Document::PlaceInOverview(const std::map<std::string, std::pair<double, double>>& where)
    {
        kgraph::Graph graph = Graph();
        // Put there once, every node is held where it is seen: none moves for a page's being changed after.
        auto placed = OverviewPlaces();
        for (const auto& [id, at] : where)
            if (placed.contains(id)) placed[id] = at;
        if (placed == graph.overview) return;
        graph.overview = std::move(placed);
        Apply(std::move(graph));
    }

    void Document::MoveInOverview(const std::vector<std::string>& ids, const double dx, const double dy)
    {
        const auto placed = OverviewPlaces();
        std::map<std::string, std::pair<double, double>> where;
        for (const auto& id : ids)
            if (const auto at = placed.find(id); at != placed.end()) where[id] = { at->second.first + dx, at->second.second + dy };
        PlaceInOverview(where);
    }

    void Document::SetMeshImport(const std::string& file, kgraph::MeshImport import)
    {
        kgraph::Graph graph = Graph();
        if (import.Default()) graph.meshImports.erase(file);
        else graph.meshImports[file] = std::move(import);
        if (graph.meshImports == Graph().meshImports) return;
        Apply(std::move(graph));
    }

    void Document::SetDefaultMeshTangents(const kgraph::Tangents tangents)
    {
        if (Graph().defaultMeshTangents == tangents) return;
        kgraph::Graph graph = Graph();
        graph.defaultMeshTangents = tangents;
        Apply(std::move(graph));
    }

    kgraph::MeshImport Document::MeshImportOf(const std::string_view file) const
    {
        const auto it = Graph().meshImports.find(std::string(file));
        return it == Graph().meshImports.end() ? kgraph::MeshImport {} : it->second;
    }

    bool Document::SetPlacement(const std::string_view id, const std::string_view input, const kgraph::Placement where)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        const kgraph::NodeType* type = node ? kgraph::FindType(node->type) : nullptr;
        if (!type || where == kgraph::Placement::eWire) return false;
        // What its type says of it — the listed input, for one given more than once.
        std::string listed(input);
        if (const auto open = listed.find('['); open != std::string::npos)
            if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
        const auto port = std::ranges::find(type->inputs, listed, &kgraph::Port::name);
        const kgraph::Placement own = port == type->inputs.end() ? kgraph::Placement::eInspector : port->place;
        if (own == kgraph::Placement::eWire) return false;
        if (where == own) node->placed.erase(std::string(input));
        else node->placed[std::string(input)] = where == kgraph::Placement::eNode ? "node" : "inspector";
        Apply(std::move(graph));
        return true;
    }

    std::string Document::MakeCompanion(const std::string_view id, const std::string_view input, std::string* error)
    {
        const kgraph::Node* node = Graph().Find(id);
        const kgraph::NodeType* type = node ? kgraph::FindType(node->type) : nullptr;
        std::string listed(input);
        if (const auto open = listed.find('['); open != std::string::npos)
            if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
        const auto port = type ? std::ranges::find(type->inputs, listed, &kgraph::Port::name) : decltype(type->inputs.end()) {};
        if (!type || port == type->inputs.end() || port->companion.empty()) {
            if (error) *error = std::format("'{}' of '{}' has no node of its own to set it up", input, id);
            return {};
        }
        const std::string pass = node->pass;     // (the node is another once a node is added)
        const double x = node->x - 300.0, y = node->y;
        const std::string made = AddNode(port->companion, x, y, {}, pass);
        if (made.empty() || !Connect(made, "out", id, input)) {
            if (error) *error = std::format("a {} could not be made and wired to '{}'", port->companion, input);
            return {};
        }
        return made;
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
        // A pipeline's parameter wired is the same for every draw: what its draws were given of it is theirs no more.
        if (node->type.ends_with("Pipeline") && input.starts_with("param."))
            for (auto& other : graph.nodes) {
                const auto pipeline = other.inputs.find("pipeline");
                if (pipeline != other.inputs.end() && pipeline->second.link && pipeline->second.link->node == id) other.inputs.erase(std::string(input));
            }
        LetGoOfUnwired(graph, Graph());
        Apply(std::move(graph));
        return true;
    }

    bool Document::ClearInput(const std::string_view id, const std::string_view input)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node || node->inputs.erase(std::string(input)) == 0) return false;
        LetGoOfUnwired(graph, Graph());
        Apply(std::move(graph));
        return true;
    }

    std::size_t Document::DisconnectOutput(const std::string_view id, const std::string_view output)
    {
        kgraph::Graph graph = Graph();
        std::size_t taken = 0;
        for (auto& node : graph.nodes)
            taken += std::erase_if(node.inputs, [&](const auto& input) { return input.second.link && input.second.link->node == id && input.second.link->output == output; });
        if (taken > 0) {
            LetGoOfUnwired(graph, Graph());
            Apply(std::move(graph));
        }
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
        Override(entity, "position");
        Shifted();
        return true;
    }

    bool Document::SetRotation(const kecs::Entity entity, const kecs::Vec3& degrees)
    {
        if (!_world.SetRotation(entity, degrees)) return false;
        Override(entity, "rotation");
        Shifted();
        return true;
    }

    bool Document::SetScale(const kecs::Entity entity, const kecs::Vec3& scale)
    {
        if (!_world.SetScale(entity, scale)) return false;
        Override(entity, "scale");
        Shifted();
        return true;
    }

    void Document::Moved(const kecs::Entity entity, const std::string_view what)
    {
        Override(entity, what);
        Shifted();
    }

    bool Document::DestroyEntity(const kecs::Entity entity)
    {
        // A prefab's entity taken from its instance stays taken: the instance made again does not have it.
        if (_world.Alive(entity)) Override(entity, "gone");
        if (!_world.Destroy(entity)) return false;
        Changed();
        return true;
    }

    std::vector<kecs::Entity> Document::Entities() const { return _world.All(); }

    std::string Document::NameOf(const kecs::Entity entity) const { return _world.NameOf(entity); }

    bool Document::Rename(const kecs::Entity entity, std::string name)
    {
        if (!_world.Rename(entity, std::move(name))) return false;
        Override(entity, "name");
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
        Override(entity, std::format("+{}", component));
        Changed();
        return true;
    }

    bool Document::RemoveComponent(const kecs::Entity entity, const std::string_view component)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.Remove(entity, id)) return false;
        Override(entity, std::format("-{}", component));
        Changed();
        return true;
    }

    bool Document::SetEnabled(const kecs::Entity entity, const std::string_view component, const bool enabled)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.SetEnabled(entity, id, enabled)) return false;
        Override(entity, std::format("~{}", component));
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
        if (found->type == "entity") return EntityValue(*reinterpret_cast<const kecs::Entity*>(at));
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
        } else if (found->type == "entity") {
            // Another entity of this scene, or none.
            const auto other = EntityOfValue(value);
            if (!other) return fail(std::format("'{}' is an entity: one of the scene's, or nothing", field));
            if (!other->IsNull() && !_world.Alive(*other)) return fail(std::format("'{}' is given an entity that is not in the scene", field));
            *reinterpret_cast<kecs::Entity*>(at) = *other;
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
                else if (const std::string* name = value.String(); name && !found->choices.empty()) {
                    // One of so many things, by its name — any case.
                    const auto same = [&](const std::string& choice) {
                        return std::ranges::equal(choice, *name, [](const char a, const char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
                    };
                    const auto it = std::ranges::find_if(found->choices, same);
                    if (it == found->choices.end()) {
                        std::string names;
                        for (const auto& choice : found->choices) names += (names.empty() ? "" : ", ") + choice;
                        return fail(std::format("'{}' is one of {}: not '{}'", field, names, *name));
                    }
                    WriteNumber(numbers, at, static_cast<double>(it - found->choices.begin()));
                }
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
        Override(entity, std::format("{}.{}", component, field));
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
        // A copy of a prefab's instance is another instance of it, with what the first made its own. A copy of an entity
        // that is part of an instance is one put under it: its own, no part of the prefab.
        const kecs::Entity outer = _world.Has<PrefabLink>(entity) ? kecs::NullEntity : PrefabRootOf(entity);
        const std::function<void(kecs::Entity, kecs::Entity)> carry = [&](const kecs::Entity from, const kecs::Entity to) {
            const bool outers = !outer.IsNull() && PrefabRootOf(from) == outer;
            if (const PrefabLink* link = _world.Get<PrefabLink>(from)) _world.Add<PrefabLink>(to, *link);
            if (const PrefabPart* part = _world.Get<PrefabPart>(from); part && !outers) _world.Add<PrefabPart>(to, *part);
            const auto& a = _world.ChildrenOf(from);
            const auto& b = _world.ChildrenOf(to);
            for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) carry(a[i], b[i]);
        };
        carry(entity, copy);
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
        WithFunctions(graph);
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
        // Each prefab's instance as its prefab is now: what was saved of it, and what has changed in the prefab since.
        UpdatePrefabs();
        Changed();
        _saved = _revision;
        return true;
    }

    bool Document::Save(const std::filesystem::path& path, std::string& error)
    {
        // A function open in place is closed first, keeping what was done to it: its page is no part of the scene.
        if (_opened && !CloseFunction(true, &error)) return false;
        if (_runtime->AssetDirectory().empty()) _runtime->SetAssetDirectory(std::filesystem::absolute(path).parent_path().parent_path() / "assets");
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { error = std::format("'{}' cannot be written", path.string()); return false; }
        out << ToJson();
        if (!out.flush()) { error = std::format("'{}' could not be written whole", path.string()); return false; }
        _saved = _revision;
        return true;
    }

    kecs::Entity Document::ImportModel(const std::filesystem::path& file, std::string* error)
    {
        std::error_code ec;
        const std::filesystem::path where = file.is_absolute() || _runtime->AssetDirectory().empty() ? file : _runtime->AssetDirectory() / file;
        if (!std::filesystem::is_regular_file(where, ec)) { if (error) *error = std::format("there is no file '{}'", where.generic_string()); return kecs::NullEntity; }
        // The file is read off the frame: the entity over what it holds is there now, and the rest when it is read.
        const kecs::Entity root = CreateEntity(file.stem().string());
        _importing.push_back({ file, root });
        (void)_runtime->ModelOf(file);
        return root;
    }

    void Document::Imported(const kecs::Entity root, const kgraph::Model& read)
    {
        using kgraph::Value;
        const kgraph::Model* model = &read;
        if (!_world.Alive(root)) return;    // taken out again while its file was being read

        // What a drawn thing is made of goes into every component that has a material's fields.
        std::vector<std::string> materials;
        for (const auto& name : ComponentNames())
            if (const kecs::ComponentId id = _world.Find(name); id != kecs::NoComponent && _world.Describe(id).FindField("albedo")) materials.push_back(name);
        AddTag("Masked");
        AddTag("Blended");
        const auto vec = [](const float* v) { return Value(Value::List { v[0], v[1], v[2] }); };
        const auto give = [&](const kecs::Entity entity, const std::string& component, const char* field, const Value& value) {
            if (_world.Describe(_world.Find(component)).FindField(field)) SetField(entity, component, field, value);
        };
        const auto drawn = [&](const kecs::Entity entity, const kgraph::ModelNode::Part& part) {
            AddComponent(entity, "Mesh");
            SetField(entity, "Mesh", "mesh", Value(part.mesh));
            if (part.material < 0 || part.material >= static_cast<int>(model->materials.size())) {
                // The file says nothing of what it is made of: something matte and pale, rather than nothing at all.
                const float pale[3] { 0.8f, 0.8f, 0.8f };
                for (const auto& component : materials) {
                    if (component.find("blend") != std::string::npos && materials.size() > 1) continue;
                    AddComponent(entity, component);
                    for (const char* map : { "albedoMap", "normalMap", "roughnessMap", "emissiveMap" }) give(entity, component, map, Value("$white"));
                    give(entity, component, "albedo", vec(pale));
                    give(entity, component, "roughness", Value(0.8));
                }
                return;
            }
            const kgraph::ModelMaterial& material = model->materials[static_cast<std::size_t>(part.material)];
            // A file's colour is light; a component's is as it is picked.
            const float picked[3] { std::pow(std::max(material.albedo[0], 0.f), 1.f / 2.2f), std::pow(std::max(material.albedo[1], 0.f), 1.f / 2.2f),
                                    std::pow(std::max(material.albedo[2], 0.f), 1.f / 2.2f) };
            // What is drawn over what is behind it is the blended pipeline's to draw, and the rest the others': each
            // entity has the material once, in the component of what draws it. A graph that has no pipeline named
            // for blending draws both with what it has.
            const auto blends = [](const std::string& component) { return component.find("blend") != std::string::npos; };
            const bool apart = std::ranges::any_of(materials, blends) && !std::ranges::all_of(materials, blends);
            for (const auto& component : materials) {
                if (apart && blends(component) != material.blended) continue;
                AddComponent(entity, component);
                give(entity, component, "albedo", vec(picked));
                give(entity, component, "metallic", Value(material.metallic));
                give(entity, component, "roughness", Value(material.roughness));
                give(entity, component, "emission", vec(material.emission));
                give(entity, component, "alphaCutoff", Value(material.alphaCutoff));
                // A picture the material has not is white, which leaves what it is multiplied by as it is.
                const auto picture = [](const std::string& map) { return Value(map.empty() ? std::string("$white") : map); };
                give(entity, component, "albedoMap", picture(material.albedoMap));
                give(entity, component, "normalMap", picture(material.normalMap));
                give(entity, component, "roughnessMap", picture(material.roughnessMap));
                give(entity, component, "emissiveMap", picture(material.emissiveMap));
            }
            if (material.blended) AddComponent(entity, "Blended");
            else if (material.alphaCutoff > 0.f) AddComponent(entity, "Masked");
        };

        std::vector<kecs::Entity> made(model->nodes.size(), root);
        for (std::size_t i = 0; i < model->nodes.size(); ++i) {
            const kgraph::ModelNode& node = model->nodes[i];
            const kecs::Entity parent = node.parent >= 0 ? made[static_cast<std::size_t>(node.parent)] : root;
            const kecs::Entity entity = made[i] = CreateEntity(node.name.empty() ? std::format("Node {}", i) : node.name, parent);
            SetPosition(entity, { node.position[0], node.position[1], node.position[2] });
            SetRotation(entity, { node.rotation[0], node.rotation[1], node.rotation[2] });
            SetScale(entity, { node.scale[0], node.scale[1], node.scale[2] });
            // One part is the thing itself; several are each a thing under it.
            if (node.parts.size() == 1) drawn(entity, node.parts.front());
            else for (std::size_t part = 0; part < node.parts.size(); ++part) drawn(CreateEntity(std::format("{} [{}]", NameOf(entity), part), entity), node.parts[part]);
        }
        for (const auto& light : model->lights) {
            const kecs::Entity entity = CreateEntity(light.name.empty() ? std::string("Light") : light.name, root);
            SetPosition(entity, { light.position[0], light.position[1], light.position[2] });
            // Forward is -Z: turned about Y to face the way it shines, then tipped up or down.
            static constexpr float Degrees = 180.f / 3.14159265358979323846f;
            SetRotation(entity, { std::asin(std::clamp(light.direction[1], -1.f, 1.f)) * Degrees, std::atan2(-light.direction[0], -light.direction[2]) * Degrees, 0.f });
            const char* component = light.kind == kgraph::ModelLight::Kind::eDirectional ? "DirectionalLight" : light.kind == kgraph::ModelLight::Kind::eSpot ? "SpotLight" : "PointLight";
            AddComponent(entity, component);
            SetField(entity, component, "color", vec(light.color));
            if (light.intensity > 0.f) SetField(entity, component, "intensity", Value(light.intensity));
            if (light.range > 0.f && light.kind != kgraph::ModelLight::Kind::eDirectional) SetField(entity, component, "range", Value(light.range));
            if (light.kind == kgraph::ModelLight::Kind::eSpot && light.outerAngle > 0.f) {
                SetField(entity, component, "innerAngle", Value(light.innerAngle));
                SetField(entity, component, "outerAngle", Value(light.outerAngle));
            }
        }
        Changed();
    }

    // ---- functions ----------------------------------------------------------------------------------------------

    namespace {
        constexpr const char* FunctionEnding = ".kfunc";

        bool ReadFunction(const std::filesystem::path& file, kgraph::Graph& out, std::string& error)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) { error = std::format("'{}' cannot be read", file.generic_string()); return false; }
            std::stringstream read;
            read << in.rdbuf();
            const std::string text = read.str();     // (kept: what the parse gives of it is views into it)
            kecs::json::Value parsed;
            if (!kecs::json::Parse(text, parsed, error)) return false;
            const auto* version = parsed.Find("koral-function");
            const auto* graph = parsed.Find("graph");
            if (!version || version->number != 1.0 || !graph) { error = std::format("'{}' is not a function: 'koral-function' 1, with a 'graph'", file.generic_string()); return false; }
            return kgraph::Graph::FromJson(graph->raw, out, error);
        }

        bool WriteFunction(const std::filesystem::path& file, const kgraph::Graph& graph, std::string& error)
        {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) { error = std::format("'{}' cannot be written", file.generic_string()); return false; }
            out << std::format("{{\n\"koral-function\": 1,\n\"graph\": {}\n}}\n", graph.ToJson());
            if (!out.flush()) { error = std::format("'{}' could not be written whole", file.generic_string()); return false; }
            return true;
        }

        bool IsFunctionName(const std::string& name)
        {
            return !name.empty() && name.front() != ' ' && name.back() != ' '
                && std::ranges::all_of(name, [](const unsigned char c) { return std::isalnum(c) || c == ' ' || c == '_' || c == '-'; });
        }

        const std::string* TextOf(const kgraph::Node& node, const char* input)
        {
            const auto it = node.inputs.find(input);
            const std::string* text = it == node.inputs.end() || it->second.link ? nullptr : it->second.value.String();
            return text && !text->empty() ? text : nullptr;
        }

        std::string Unique(const std::string& base, const std::function<bool(const std::string&)>& taken)
        {
            std::string name = base;
            for (int n = 2; taken(name); ++n) name = std::format("{} {}", base, n);
            return name;
        }

        /** What an edge is called for the port it went to or came from: its last part, as `param.instances` is `instances`. */
        std::string EdgeName(const std::string_view port)
        {
            std::string_view name = port;
            if (const auto dot = name.rfind('.'); dot != std::string_view::npos) name.remove_prefix(dot + 1);
            if (const auto open = name.find('['); open != std::string_view::npos) name = name.substr(0, open);
            return name.empty() ? std::string("value") : std::string(name);
        }

        bool IsEdgeType(const std::string_view type) { return type == "FunctionInput" || type == "FunctionOutput"; }

        std::string StandsForOf(const kgraph::Node& node)
        {
            const auto target = node.inputs.find("node");
            return node.type == "Reference" && target != node.inputs.end() && target->second.value.String() ? *target->second.value.String() : std::string();
        }
    }

    std::filesystem::path Document::FunctionsDirectory() const
    {
        const std::filesystem::path project = ProjectDirectory();
        return project.empty() ? std::filesystem::path() : project / "functions";
    }

    std::filesystem::path Document::EngineFunctionsDirectory() { return kscript::PackageDirectory() / "functions"; }

    std::vector<std::string> Document::Functions() const
    {
        std::set<std::string> names;
        std::error_code ignored;
        for (const auto& folder : { FunctionsDirectory(), EngineFunctionsDirectory() }) {
            if (folder.empty()) continue;
            for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
                if (entry.path().extension() == FunctionEnding) names.insert(entry.path().stem().string());
        }
        return { names.begin(), names.end() };
    }

    bool Document::IsEngineFunction(const std::string_view name) const
    {
        std::error_code ignored;
        const std::string file = std::string(name) + FunctionEnding;
        const std::filesystem::path project = FunctionsDirectory();
        if (!project.empty() && std::filesystem::exists(project / file, ignored)) return false;
        return std::filesystem::exists(EngineFunctionsDirectory() / file, ignored);
    }

    std::vector<kgraph::Preset> Document::Presets() const
    {
        std::vector<kgraph::Preset> presets = _runtime->Presets();
        for (const auto& name : Functions()) {
            kgraph::Preset call;
            call.name = kgraph::FindType(name) ? std::format("{} (function)", name) : name;
            call.type = "Function";
            call.doc = IsEngineFunction(name) ? std::format("A call of the engine's function {}: its nodes, as one. Pressed twice, it is opened; changed, it is saved as the project's own.", name)
                                              : std::format("A call of the project's function {}: its nodes, as one. Pressed twice, it is opened.", name);
            call.inputs = { { "function", kgraph::Value(name) } };
            presets.push_back(std::move(call));
        }
        return presets;
    }

    void Document::WithFunctions(kgraph::Graph& graph)
    {
        graph.functions.clear();
        const std::filesystem::path folder = FunctionsDirectory();
        std::vector<const kgraph::Graph*> look { &graph };
        while (!look.empty()) {
            const kgraph::Graph* of = look.back();
            look.pop_back();
            for (const auto& node : of->nodes) {
                if (node.type != "Function") continue;
                const std::string* name = TextOf(node, "function");
                if (!name || graph.functions.contains(*name)) continue;
                // The project's, or — when it has none so called — the engine's.
                const std::filesystem::path file = (IsEngineFunction(*name) ? EngineFunctionsDirectory() : folder) / (*name + FunctionEnding);
                std::error_code ec;
                const auto written = std::filesystem::last_write_time(file, ec);
                if (ec) continue;   // the call says there is no such function, on itself
                auto& cached = _functionFiles[*name];
                if (!cached.second || cached.first != written) {
                    kgraph::Graph read;
                    std::string error;
                    if (!ReadFunction(file, read, error)) { _notes.push_back(error); continue; }
                    cached = { written, std::make_shared<const kgraph::Graph>(std::move(read)) };
                }
                graph.functions[*name] = cached.second;
                look.push_back(cached.second.get());
            }
        }
    }

    void Document::ReloadFunctions()
    {
        _functionFiles.clear();
        Apply(kgraph::Graph(Graph()));
    }

    std::string Document::MakeFunction(const std::vector<std::string>& ids, const std::string& name, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return std::string(); };
        if (!IsFunctionName(name)) return fail("a function's name is letters, digits, spaces, '_' and '-'");
        const std::filesystem::path folder = FunctionsDirectory();
        if (folder.empty()) return fail("a function is kept in a project, and this scene is in none");
        const std::filesystem::path file = folder / (name + FunctionEnding);
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) return fail(std::format("there is a function called '{}' already", name));
        kgraph::Graph graph = Graph();
        // What it holds: the nodes picked — not a Reference, which is what another pass made, nor another's edge.
        std::set<std::string> chosen;
        for (const auto& id : ids)
            if (const kgraph::Node* node = graph.Find(id); node && node->type != "Reference" && !IsEdgeType(node->type)) chosen.insert(node->id);
        if (chosen.empty()) return fail("none of those can be a function's: pick the nodes it is to hold");
        const auto origin = [&](std::string id) {
            for (int hops = 0; hops < 16; ++hops) {
                const kgraph::Node* node = graph.Find(id);
                const std::string target = node ? StandsForOf(*node) : std::string();
                if (target.empty() || !graph.Find(target)) return id;
                id = target;
            }
            return id;
        };
        const auto kindOf = [&](const std::string& source, const std::string& output) -> std::string {
            for (const auto& port : _runtime->OutputsOf(origin(source)))
                if (port.name == output) {
                    if (port.kind == kgraph::PortKind::eResource)
                        return port.type == "Mesh" ? "eMesh" : port.type == "Buffer" || port.type == "BufferView" ? "eBuffer" : port.type == "Image" || port.type == "ImageView" ? "eImage" : "eResource";
                    return port.type == "entities" ? "eEntities" : "eValue";
                }
            return "eValue";
        };
        double left = 1e300, right = -1e300, top = 1e300, cx = 0, cy = 0;
        std::map<std::string, int> passes;
        for (const auto& node : graph.nodes)
            if (chosen.contains(node.id)) {
                left = std::min(left, node.x); right = std::max(right, node.x); top = std::min(top, node.y);
                cx += node.x; cy += node.y;
                ++passes[node.pass];
            }
        cx /= static_cast<double>(chosen.size());
        cy /= static_cast<double>(chosen.size());

        kgraph::Graph function;
        std::set<std::string> taken(chosen.begin(), chosen.end());
        // Its inputs: what its nodes take of the rest, an input for each output of something there.
        std::map<std::pair<std::string, std::string>, std::string> inputOf;     // what is taken, the edge it is taken through
        std::map<std::string, std::string> inputPort;                           // an edge, the call's port it is
        std::set<std::string> inputNames;
        for (const auto& node : graph.nodes) {
            if (!chosen.contains(node.id)) continue;
            kgraph::Node copy = node;
            copy.pass.clear();
            for (auto& [port, input] : copy.inputs) {
                if (!input.link || chosen.contains(input.link->node)) continue;
                const auto key = std::pair { input.link->node, input.link->output };
                auto it = inputOf.find(key);
                if (it == inputOf.end()) {
                    const std::string portName = Unique(EdgeName(port), [&](const std::string& n) { return inputNames.contains(n); });
                    inputNames.insert(portName);
                    const std::string edge = Unique("in " + portName, [&](const std::string& n) { return taken.contains(n); });
                    taken.insert(edge);
                    kgraph::Node& made = function.Add(edge, "FunctionInput");
                    made.Set("name", portName).Set("kind", kindOf(key.first, key.second));
                    made.x = left - 320.0;
                    made.y = top + 90.0 * static_cast<double>(inputOf.size());
                    it = inputOf.emplace(key, edge).first;
                    inputPort[edge] = portName;
                }
                input.link = kgraph::Link { it->second, "out" };
            }
            function.nodes.push_back(std::move(copy));
        }
        // Its outputs: what the rest takes of its nodes — straight, or through a Reference to one.
        std::map<std::pair<std::string, std::string>, std::string> outputOf;    // what is given, the call's port it is given at
        std::set<std::string> outputNames;
        for (const auto& node : graph.nodes) {
            if (chosen.contains(node.id)) continue;
            for (const auto& input : node.inputs | std::views::values) {
                if (!input.link) continue;
                const std::string source = origin(input.link->node);
                if (!chosen.contains(source)) continue;
                const auto key = std::pair { source, input.link->output };
                if (outputOf.contains(key)) continue;
                const std::string portName = Unique(EdgeName(key.second == "out" ? source : key.second), [&](const std::string& n) { return outputNames.contains(n); });
                outputNames.insert(portName);
                const std::string edge = Unique("out " + portName, [&](const std::string& n) { return taken.contains(n); });
                taken.insert(edge);
                kgraph::Node& made = function.Add(edge, "FunctionOutput");
                made.Set("name", portName).Set("kind", kindOf(key.first, key.second)).Wire("value", key.first, key.second);
                made.x = right + 320.0;
                made.y = top + 90.0 * static_cast<double>(outputOf.size());
                outputOf.emplace(key, portName);
            }
        }

        // The call, where they were: in the pass most of them were in, wired as they were.
        kgraph::Node call;
        call.id = Unique(name, [&](const std::string& n) { return graph.Find(n) && !chosen.contains(n); });
        call.type = "Function";
        call.pass = std::ranges::max_element(passes, {}, [](const auto& entry) { return entry.second; })->first;
        call.x = std::round(cx);
        call.y = std::round(cy);
        call.Set("function", name);
        for (const auto& [taken, edge] : inputOf) call.inputs[inputPort[edge]] = kgraph::Input { {}, kgraph::Link { taken.first, taken.second } };
        for (auto& node : graph.nodes) {
            if (chosen.contains(node.id)) continue;
            for (auto& input : node.inputs | std::views::values)
                if (input.link)
                    if (const auto at = outputOf.find({ origin(input.link->node), input.link->output }); at != outputOf.end()) input.link = kgraph::Link { call.id, at->second };
        }
        // What they were, and the References that brought them in: gone, the call in their place.
        std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return chosen.contains(node.id) || chosen.contains(StandsForOf(node)); });
        graph.nodes.push_back(std::move(call));
        const std::string made = graph.nodes.back().id;
        std::string why;
        if (!WriteFunction(file, function, why)) return fail(why);
        _functionFiles.erase(name);
        WireAcrossPasses(graph, { made });
        Apply(std::move(graph));
        return made;
    }

    std::string Document::OpenFunction(const std::string_view call, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return std::string(); };
        if (_opened) return fail(std::format("'{}' is open already: close it first", _opened->function));
        kgraph::Graph graph = Graph();
        const kgraph::Node* node = graph.Find(call);
        if (!node || node->type != "Function") return fail(std::format("'{}' is no call of a function", call));
        const std::string* name = TextOf(*node, "function");
        const auto found = name ? graph.functions.find(*name) : graph.functions.end();
        if (found == graph.functions.end() || !found->second) return fail(name ? std::format("there is no function '{}' among the project's functions", *name) : std::string("it calls no function yet"));
        _beforeOpened = graph;
        OpenedFunction opened { *node, *name, {}, std::string(call) + "/" };
        // Its page: a pass of its own, after the call's.
        opened.pass = Unique(std::format("{} (function)", *name), [&](const std::string& p) { return std::ranges::find(graph.passes, p) != graph.passes.end(); });
        const auto after = std::ranges::find(graph.passes, node->pass);
        graph.passes.insert(after == graph.passes.end() ? graph.passes.end() : after + 1, opened.pass);
        // Run as often as the call is.
        if (graph.IsSetup(node->pass)) graph.setup.push_back(opened.pass);
        else if (graph.IsFixed(node->pass)) graph.fixed.push_back(opened.pass);
        RatesInOrder(graph);
        std::map<std::string, std::string> outputEdge;      // a port of the call, the FunctionOutput it is
        for (const auto& inner : found->second->nodes) {
            kgraph::Node copy = inner;
            copy.id = opened.prefix + inner.id;
            copy.pass = opened.pass;
            for (auto& input : copy.inputs | std::views::values)
                if (input.link) input.link->node = opened.prefix + input.link->node;
            if (copy.type == "Reference")
                if (auto target = copy.inputs.find("node"); target != copy.inputs.end() && target->second.value.String())
                    target->second.value = kgraph::Value(opened.prefix + *target->second.value.String());
            const std::string port = TextOf(inner, "name") ? *TextOf(inner, "name") : inner.id;
            // An input is given what the call was given there; an output is what was wired from the call.
            if (inner.type == "FunctionInput")
                if (const auto given = node->inputs.find(port); given != node->inputs.end()) copy.inputs["value"] = given->second;
            if (inner.type == "FunctionOutput") outputEdge[port] = copy.id;
            graph.nodes.push_back(std::move(copy));
        }
        for (auto& other : graph.nodes)
            for (auto& input : other.inputs | std::views::values)
                if (input.link && input.link->node == call)
                    if (const auto edge = outputEdge.find(input.link->output); edge != outputEdge.end()) input.link = kgraph::Link { edge->second, "out" };
        std::erase_if(graph.nodes, [&](const kgraph::Node& n) { return n.id == call; });
        _opened = std::move(opened);
        Apply(std::move(graph));
        return _opened->pass;
    }

    bool Document::CloseFunction(const bool keep, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        if (!_opened) return fail("no function is open");
        if (!keep) {
            kgraph::Graph before = std::move(_beforeOpened);
            _opened.reset();
            Apply(std::move(before));
            return true;
        }
        const OpenedFunction opened = std::move(*_opened);
        _opened.reset();
        kgraph::Graph graph = Graph();
        std::set<std::string> page;
        for (const auto& node : graph.nodes) if (node.pass == opened.pass) page.insert(node.id);
        // What a node of the page is called in the function: without the call's name before it, where that names none other.
        std::set<std::string> names;
        std::map<std::string, std::string> inFunction;
        for (const auto& id : page) {
            std::string stripped = id.starts_with(opened.prefix) ? id.substr(opened.prefix.size()) : id;
            if (names.contains(stripped)) stripped = id;
            names.insert(stripped);
            inFunction[id] = stripped;
        }
        kgraph::Graph function;
        kgraph::Node call = opened.call;
        for (auto it = call.inputs.begin(); it != call.inputs.end();) it = it->first == "function" ? std::next(it) : call.inputs.erase(it);
        std::set<std::string> ports;
        double left = 1e300, right = -1e300;
        for (const auto& node : graph.nodes) if (page.contains(node.id)) { left = std::min(left, node.x); right = std::max(right, node.x); }
        const auto edgeFor = [&](const char* type, const std::string& port, const std::string& kind, const double x) -> kgraph::Node& {
            const std::string id = Unique(std::format("{} {}", type == std::string("FunctionInput") ? "in" : "out", port), [&](const std::string& n) { return names.contains(n); });
            names.insert(id);
            kgraph::Node& made = function.Add(id, type);
            made.Set("name", port).Set("kind", kind);
            made.x = x;
            made.y = 90.0 * static_cast<double>(function.nodes.size());
            return made;
        };
        for (const auto& node : graph.nodes) {
            if (!page.contains(node.id)) continue;
            kgraph::Node copy = node;
            copy.id = inFunction[node.id];
            copy.pass.clear();
            if (copy.type == "FunctionInput") {
                // What it was given is what the call is given there.
                const std::string port = TextOf(copy, "name") ? *TextOf(copy, "name") : copy.id;
                ports.insert(port);
                if (const auto given = copy.inputs.find("value"); given != copy.inputs.end()) {
                    if (given->second.link && !page.contains(given->second.link->node)) call.inputs[port] = given->second;
                    else if (!given->second.link) call.inputs[port] = given->second;
                    copy.inputs.erase(given);
                }
            }
            for (auto& [port, input] : copy.inputs) {
                if (!input.link) continue;
                if (page.contains(input.link->node)) { input.link->node = inFunction[input.link->node]; continue; }
                // Wired straight from outside the page: an input more, which the call is given.
                const std::string name = Unique(EdgeName(port), [&](const std::string& n) { return ports.contains(n); });
                ports.insert(name);
                std::string kind = "eValue";
                for (const auto& out : _runtime->OutputsOf(input.link->node))
                    if (out.name == input.link->output) kind = out.kind == kgraph::PortKind::eResource ? "eResource" : out.type == "entities" ? "eEntities" : "eValue";
                const std::string id = edgeFor("FunctionInput", name, kind, left - 320.0).id;
                call.inputs[name] = input;
                input.link = kgraph::Link { id, "out" };
            }
            if (copy.type == "Reference")
                if (auto target = copy.inputs.find("node"); target != copy.inputs.end() && target->second.value.String() && page.contains(*target->second.value.String()))
                    target->second.value = kgraph::Value(inFunction[*target->second.value.String()]);
            function.nodes.push_back(std::move(copy));
        }
        // What the rest takes of the page is what the call gives: at an output, or — taken straight — at one made for it.
        std::map<std::pair<std::string, std::string>, std::string> made;
        for (auto& node : graph.nodes) {
            if (page.contains(node.id)) continue;
            for (auto& [port, input] : node.inputs) {
                if (!input.link || !page.contains(input.link->node)) continue;
                const kgraph::Node* from = graph.Find(input.link->node);
                if (from && from->type == "FunctionOutput") {
                    input.link = kgraph::Link { call.id, TextOf(*from, "name") ? *TextOf(*from, "name") : inFunction[from->id] };
                    continue;
                }
                const auto key = std::pair { input.link->node, input.link->output };
                auto at = made.find(key);
                if (at == made.end()) {
                    const std::string name = Unique(EdgeName(key.second == "out" ? inFunction[key.first] : key.second), [&](const std::string& n) { return ports.contains(n); });
                    ports.insert(name);
                    edgeFor("FunctionOutput", name, "eValue", right + 320.0).Wire("value", inFunction[key.first], key.second);
                    at = made.emplace(key, name).first;
                }
                input.link = kgraph::Link { call.id, at->second };
            }
        }
        std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return page.contains(node.id); });
        std::erase(graph.passes, opened.pass);
        graph.nodes.push_back(call);
        std::string why;
        // Into the project, always: one of the engine's changed is the project's own from then on.
        if (FunctionsDirectory().empty()) return fail("a function is kept in a project, and this scene is in none");
        if (!WriteFunction(FunctionsDirectory() / (opened.function + FunctionEnding), function, why)) return fail(why);
        _functionFiles.erase(opened.function);
        WireAcrossPasses(graph, { call.id });
        Apply(std::move(graph));
        return true;
    }

    bool Document::WriteOut(const std::string_view call, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        kgraph::Graph graph = Graph();
        const kgraph::Node* node = graph.Find(call);
        if (!node || node->type != "Function") return fail(std::format("'{}' is no call of a function", call));
        const std::string* name = TextOf(*node, "function");
        const auto found = name ? graph.functions.find(*name) : graph.functions.end();
        if (found == graph.functions.end() || !found->second) return fail("there is no such function among the project's functions");
        const kgraph::Node at = *node;
        const std::string prefix = std::string(call) + "/";
        // What each edge stands for here: an input, what the call is given; an output, what it is wired from inside.
        std::map<std::string, kgraph::Input> stands;
        double left = 1e300, top = 1e300;
        for (const auto& inner : found->second->nodes) {
            if (!IsEdgeType(inner.type)) { left = std::min(left, inner.x); top = std::min(top, inner.y); }
            const std::string port = TextOf(inner, "name") ? *TextOf(inner, "name") : inner.id;
            if (inner.type == "FunctionInput") {
                const auto given = at.inputs.find(port);
                if (given != at.inputs.end()) stands[inner.id] = given->second;
                else if (const auto initial = inner.inputs.find("default"); initial != inner.inputs.end()) stands[inner.id] = kgraph::Input { initial->second.value, std::nullopt };
            }
        }
        std::set<std::string> written;
        for (const auto& inner : found->second->nodes) {
            if (IsEdgeType(inner.type)) continue;
            kgraph::Node copy = inner;
            copy.id = prefix + inner.id;
            copy.pass = at.pass;
            copy.x = at.x + (inner.x - left);
            copy.y = at.y + (inner.y - top);
            for (auto it = copy.inputs.begin(); it != copy.inputs.end();) {
                if (it->second.link) {
                    if (const auto edge = stands.find(it->second.link->node); edge != stands.end()) it->second = edge->second;
                    else if (const kgraph::Node* to = found->second->Find(it->second.link->node); to && IsEdgeType(to->type)) { it = copy.inputs.erase(it); continue; }
                    else it->second.link->node = prefix + it->second.link->node;
                }
                ++it;
            }
            if (copy.type == "Reference")
                if (auto target = copy.inputs.find("node"); target != copy.inputs.end() && target->second.value.String())
                    target->second.value = kgraph::Value(prefix + *target->second.value.String());
            written.insert(copy.id);
            graph.nodes.push_back(std::move(copy));
        }
        // What was wired from the call is wired from what its output was wired from inside.
        for (auto& other : graph.nodes)
            for (auto it = other.inputs.begin(); it != other.inputs.end();) {
                if (!it->second.link || it->second.link->node != call) { ++it; continue; }
                const kgraph::Node* edge = nullptr;
                for (const auto& inner : found->second->nodes)
                    if (inner.type == "FunctionOutput" && (TextOf(inner, "name") ? *TextOf(inner, "name") : inner.id) == it->second.link->output) edge = &inner;
                const auto value = edge ? edge->inputs.find("value") : decltype(edge->inputs.end()) {};
                if (!edge || value == edge->inputs.end() || !value->second.link) { it = other.inputs.erase(it); continue; }
                if (const auto through = stands.find(value->second.link->node); through != stands.end()) it->second = through->second;
                else it->second.link = kgraph::Link { prefix + value->second.link->node, value->second.link->output };
                ++it;
            }
        std::erase_if(graph.nodes, [&](const kgraph::Node& n) { return n.id == call; });
        WireAcrossPasses(graph, written);
        Apply(std::move(graph));
        return true;
    }

    // ---- prefabs ------------------------------------------------------------------------------------------------

    namespace {
        /** An instance's overrides, from the text its PrefabLink keeps them in: a key a line. */
        std::set<std::string> OverrideSet(const std::string& text)
        {
            std::set<std::string> keys;
            std::size_t at = 0;
            while (at < text.size()) {
                const std::size_t end = std::min(text.find('\n', at), text.size());
                if (end > at) keys.insert(text.substr(at, end - at));
                at = end + 1;
            }
            return keys;
        }
        std::string OverrideText(const std::set<std::string>& keys)
        {
            std::string text;
            for (const auto& key : keys) text += key + '\n';
            return text;
        }

        /** What a prefab's file holds: the entity over the rest, and all of it, each with the part it is. */
        bool ReadPrefab(const std::filesystem::path& file, Value& root, std::string& error)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) { error = std::format("'{}' cannot be read", file.generic_string()); return false; }
            std::stringstream read;
            read << in.rdbuf();
            const std::string text = read.str();     // (kept: what the parse gives of it is views into it)
            kecs::json::Value parsed;
            if (!kecs::json::Parse(text, parsed, error)) return false;
            const auto* version = parsed.Find("koral-prefab");
            const auto* top = parsed.Find("root");
            if (!version || version->number != 1.0 || !top) { error = std::format("'{}' is not a prefab: 'koral-prefab' 1, with a 'root'", file.generic_string()); return false; }
            root = Value::FromJson(*top);
            return true;
        }

        int PartOf(const Value& node)
        {
            const Value* part = node.Find("part");
            return part && part->Number() ? static_cast<int>(*part->Number()) : -1;
        }
    }

    std::filesystem::path Document::ProjectDirectory() const
    {
        const std::filesystem::path& assets = _runtime->AssetDirectory();
        return assets.empty() ? std::filesystem::path() : assets.parent_path();
    }

    kecs::Entity Document::PrefabRootOf(kecs::Entity entity) const
    {
        for (; _world.Alive(entity); entity = _world.ParentOf(entity))
            if (_world.Has<PrefabLink>(entity)) return entity;
        return kecs::NullEntity;
    }

    std::string Document::PrefabOf(const kecs::Entity root) const
    {
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        return link ? link->prefab : std::string();
    }

    std::vector<std::string> Document::OverridesOf(const kecs::Entity root) const
    {
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        if (!link) return {};
        const auto keys = OverrideSet(link->overrides);
        return { keys.begin(), keys.end() };
    }

    void Document::Keep(const kecs::Entity root, const int part, const std::string_view what)
    {
        PrefabLink& link = *_world.Mutable<PrefabLink>(root);
        auto keys = OverrideSet(link.overrides);
        // A component given back is no longer taken off, and the other way round.
        if (what.starts_with('+')) keys.erase(std::format("{}:-{}", part, what.substr(1)));
        if (what.starts_with('-')) keys.erase(std::format("{}:+{}", part, what.substr(1)));
        keys.insert(std::format("{}:{}", part, what));
        link.overrides = OverrideText(keys);
    }

    void Document::Override(const kecs::Entity entity, const std::string_view what)
    {
        if (_rebuilding > 0 || !_world.Alive(entity)) return;
        const kecs::Entity root = PrefabRootOf(entity);
        if (root.IsNull()) return;
        const PrefabLink& link = *_world.Get<PrefabLink>(root);
        const bool own = what == "position" || what == "rotation" || what == "scale" || what == "name" || what == "gone";
        // An instance another prefab placed: what is done to it is, to the instance it is in, a change of its own.
        if (link.outer >= 0)
            if (const kecs::Entity outer = PrefabRootOf(_world.ParentOf(root)); !outer.IsNull())
                Keep(outer, link.outer, entity == root && own ? what : std::string_view("nested"));
        // Where the instance is, what it is called and what it is under are its own: no override of anything.
        if (entity == root && own) return;
        if (const PrefabPart* part = _world.Get<PrefabPart>(entity)) Keep(root, part->part, what);
    }

    std::map<int, kecs::Entity> Document::PartsOf(const kecs::Entity root) const
    {
        std::map<int, kecs::Entity> parts;
        const std::function<void(kecs::Entity)> find = [&](const kecs::Entity entity) {
            if (entity != root)
                if (const PrefabLink* link = _world.Get<PrefabLink>(entity)) {
                    if (link->outer >= 0 && !parts.contains(link->outer)) parts[link->outer] = entity;
                    return;
                }
            if (const PrefabPart* part = _world.Get<PrefabPart>(entity); part && !parts.contains(part->part)) parts[part->part] = entity;
            for (const kecs::Entity child : _world.ChildrenOf(entity)) find(child);
        };
        find(root);
        return parts;
    }

    Value Document::WritePrefab(const kecs::Entity root)
    {
        // Which part each of the instance's own entities is — the one it was, or, for one put under it since, a new one
        // — and each instance of another prefab under it, which is one entity of this one.
        std::map<kecs::Entity, int> parts;
        std::set<int> taken;
        int next = 0;
        std::vector<kecs::Entity> fresh;
        const std::function<void(kecs::Entity)> number = [&](const kecs::Entity entity) {
            if (entity != root)
                if (const PrefabLink* link = _world.Get<PrefabLink>(entity)) {
                    if (link->outer >= 0 && taken.insert(link->outer).second) { parts[entity] = link->outer; next = std::max(next, link->outer + 1); }
                    else fresh.push_back(entity);
                    return;
                }
            const PrefabPart* part = _world.Get<PrefabPart>(entity);
            if (part && taken.insert(part->part).second) { parts[entity] = part->part; next = std::max(next, part->part + 1); }
            else fresh.push_back(entity);
            for (const kecs::Entity child : _world.ChildrenOf(entity)) number(child);
        };
        number(root);
        for (const kecs::Entity entity : fresh) {
            parts[entity] = next++;
            if (PrefabLink* link = entity != root ? _world.Mutable<PrefabLink>(entity) : nullptr) link->outer = parts[entity];
            else if (_world.Has<PrefabPart>(entity)) _world.Mutable<PrefabPart>(entity)->part = parts[entity];
            else _world.Add<PrefabPart>(entity, PrefabPart { parts[entity] });
        }
        const auto transform = [&](Value::Members& out, const kecs::Entity entity) {
            out.emplace_back("part", parts.at(entity));
            out.emplace_back("name", _world.NameOf(entity));
            out.emplace_back("position", Three(_world.PositionOf(entity)));
            out.emplace_back("rotation", Three(_world.RotationOf(entity)));
            out.emplace_back("scale", Three(_world.ScaleOf(entity)));
        };
        const std::function<Value(kecs::Entity)> write = [&](const kecs::Entity entity) {
            Value::Members out;
            transform(out, entity);
            // Another prefab's instance: which prefab, and the values of what it made its own — not its entities.
            if (entity != root)
                if (const PrefabLink* link = _world.Get<PrefabLink>(entity)) {
                    out.emplace_back("prefab", link->prefab);
                    const auto inner = PartsOf(entity);
                    Value::Members kept;
                    for (const auto& key : OverrideSet(link->overrides)) {
                        const auto colon = key.find(':');
                        const int part = std::atoi(key.substr(0, colon).c_str());
                        const std::string what = key.substr(colon + 1);
                        const auto at = inner.find(part);
                        Value value;
                        if (at != inner.end() && _world.Alive(at->second)) {
                            const kecs::Entity of = at->second;
                            if (what == "position") value = Three(_world.PositionOf(of));
                            else if (what == "rotation") value = Three(_world.RotationOf(of));
                            else if (what == "scale") value = Three(_world.ScaleOf(of));
                            else if (what == "name") value = _world.NameOf(of);
                            else if (what.starts_with('~')) value = IsEnabled(of, what.substr(1));
                            else if (const auto dot = what.find('.'); dot != std::string::npos && !what.starts_with('+') && !what.starts_with('-')) {
                                value = Tidied(Field(of, what.substr(0, dot), what.substr(dot + 1)));
                                if (EntityOfValue(value) && !value.IsNull()) value = {};     // an entity is not kept by number
                            }
                        }
                        kept.emplace_back(key, std::move(value));
                    }
                    out.emplace_back("overrides", Value(std::move(kept)));
                    return Value(std::move(out));
                }
            Value::Members components;
            Value::List disabled;
            for (const std::string& component : ComponentsOf(entity)) {
                Value::Members fields;
                for (const auto& field : _world.Describe(_world.Find(component)).fields) {
                    Value value = Tidied(Field(entity, component, field.name));
                    // Another of the prefab's entities, by which part it is; one outside it, none.
                    if (field.type == "entity") {
                        const auto other = EntityOfValue(value);
                        const auto it = other ? parts.find(*other) : parts.end();
                        value = it == parts.end() ? Value {} : Value(Value::Members { { "part", it->second } });
                    }
                    fields.emplace_back(field.name, std::move(value));
                }
                components.emplace_back(component, Value(std::move(fields)));
                if (!IsEnabled(entity, component)) disabled.emplace_back(component);
            }
            out.emplace_back("components", Value(std::move(components)));
            if (!disabled.empty()) out.emplace_back("disabled", Value(std::move(disabled)));
            Value::List children;
            for (const kecs::Entity child : _world.ChildrenOf(entity))
                if (parts.contains(child)) children.push_back(write(child));
            out.emplace_back("children", Value(std::move(children)));
            return Value(std::move(out));
        };
        return write(root);
    }

    namespace {
        bool WritePrefabFile(const std::filesystem::path& file, const Value& root, std::string& error)
        {
            std::error_code ec;
            if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) { error = std::format("'{}' cannot be written", file.generic_string()); return false; }
            out << std::format("{{\n\"koral-prefab\": 1,\n\"root\": {}\n}}\n", root.ToJson());
            if (!out.flush()) { error = std::format("'{}' could not be written whole", file.generic_string()); return false; }
            return true;
        }
    }

    bool Document::SaveAsPrefab(const kecs::Entity entity, const std::filesystem::path& file, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        if (!_world.Alive(entity)) return fail("there is no such entity");
        if (const kecs::Entity outer = PrefabRootOf(entity); !outer.IsNull() && outer != entity)
            return fail(std::format("{} is part of an instance of {}: Unlink that first, or make the prefab of it all", _world.NameOf(entity), PrefabOf(outer)));
        // What it was an instance of, it is not any more: it is the first of this one.
        if (_world.Has<PrefabLink>(entity)) Unlink(entity);
        const std::filesystem::path project = ProjectDirectory();
        std::error_code ec;
        const auto relative = project.empty() ? std::filesystem::path() : std::filesystem::relative(std::filesystem::absolute(file), project, ec);
        const std::string named = !ec && !relative.empty() && *relative.begin() != ".." ? relative.generic_string() : std::filesystem::absolute(file).generic_string();
        _world.Add<PrefabLink>(entity, PrefabLink { named, {}, -1 });
        const Value written = WritePrefab(entity);
        std::string why;
        if (!WritePrefabFile(file, written, why)) { Unlink(entity); return fail(why); }
        Changed();
        return true;
    }

    kecs::Entity Document::Instantiate(const std::filesystem::path& file, const kecs::Entity parent, std::string* error)
    {
        const std::filesystem::path whole = file.is_absolute() || ProjectDirectory().empty() ? file : ProjectDirectory() / file;
        Value prefab;
        std::string why;
        if (!ReadPrefab(whole, prefab, why)) { if (error) *error = why; return kecs::NullEntity; }
        const std::filesystem::path project = ProjectDirectory();
        std::error_code ec;
        const auto relative = project.empty() ? std::filesystem::path() : std::filesystem::relative(std::filesystem::absolute(whole), project, ec);
        const std::string named = !ec && !relative.empty() && *relative.begin() != ".." ? relative.generic_string() : std::filesystem::absolute(whole).generic_string();
        const auto& siblings = parent.IsNull() ? _world.Roots() : _world.ChildrenOf(parent);
        std::vector<std::string> names;
        for (const kecs::Entity sibling : siblings) names.push_back(_world.NameOf(sibling));
        const Value* name = prefab.Find("name");
        const kecs::Entity root = CreateEntity(UniqueName(name && name->String() ? *name->String() : whole.stem().string(), names), parent);
        kecs::Vec3 three;
        if (Three(prefab.Find("position"), three)) _world.SetPosition(root, three);
        if (Three(prefab.Find("rotation"), three)) _world.SetRotation(root, three);
        if (Three(prefab.Find("scale"), three)) _world.SetScale(root, three);
        _world.Add<PrefabLink>(root, PrefabLink { named, {}, -1 });
        _world.Add<PrefabPart>(root, PrefabPart { std::max(PartOf(prefab), 0) });
        Rebuild(root, prefab);
        Changed();
        return root;
    }

    void Document::Rebuild(const kecs::Entity root, const Value& prefab)
    {
        // A prefab that has an instance of itself in it, however far down, is made so far and no further.
        if (_rebuilding > 8) { _notes.push_back(std::format("{}: its prefab has itself in it: made eight deep, and no more", _world.NameOf(root))); return; }
        ++_rebuilding;
        const auto keys = OverrideSet(_world.Get<PrefabLink>(root)->overrides);
        const auto own = [&](const int part, const std::string& what) { return keys.contains(std::format("{}:{}", part, what)); };
        // Which of the instance's own entities is which part of the prefab. A part had twice (an entity of it copied
        // under it) is the first; the copy is the instance's own.
        std::map<int, kecs::Entity> parts;
        const std::function<void(kecs::Entity)> find = [&](const kecs::Entity entity) {
            if (entity != root)
                if (PrefabLink* link = _world.Mutable<PrefabLink>(entity)) {
                    if (link->outer >= 0 && !parts.contains(link->outer)) parts[link->outer] = entity;
                    else link->outer = -1;
                    return;
                }
            if (const PrefabPart* part = _world.Get<PrefabPart>(entity); part && entity != root) {
                if (!parts.contains(part->part)) parts[part->part] = entity;
                else _world.Remove<PrefabPart>(entity);
            }
            for (const kecs::Entity child : _world.ChildrenOf(entity)) find(child);
        };
        find(root);
        const int rootPart = std::max(PartOf(prefab), 0);
        _world.Mutable<PrefabPart>(root)->part = rootPart;
        parts[rootPart] = root;

        std::set<int> seen;
        struct Link { kecs::Entity entity; std::string component, field; int part; };
        std::vector<Link> links;
        const auto place = [&](const Value& node, const kecs::Entity entity, const int part) {
            const Value* name = node.Find("name");
            if (!own(part, "name") && name && name->String()) _world.Rename(entity, *name->String());
            kecs::Vec3 three;
            if (!own(part, "position") && Three(node.Find("position"), three)) _world.SetPosition(entity, three);
            if (!own(part, "rotation") && Three(node.Find("rotation"), three)) _world.SetRotation(entity, three);
            if (!own(part, "scale") && Three(node.Find("scale"), three)) _world.SetScale(entity, three);
        };
        // Another prefab's instance, placed by this one: made of its prefab, with what this one's says it made its own —
        // unless this instance changed something of it, which it keeps.
        const auto nested = [&](const Value& node, const kecs::Entity parent, const int part) {
            const std::string inner = node.Find("prefab")->String() ? *node.Find("prefab")->String() : std::string();
            const auto at = parts.find(part);
            kecs::Entity entity = at != parts.end() && _world.Alive(at->second) && PrefabOf(at->second) == inner ? at->second : kecs::NullEntity;
            if (!entity.IsNull() && at != parts.end() && at->second != entity) _world.Destroy(at->second);
            const bool made = entity.IsNull();
            if (made) {
                if (at != parts.end() && _world.Alive(at->second)) _world.Destroy(at->second);   // of another prefab, before
                const Value* name = node.Find("name");
                entity = CreateEntity(name && name->String() ? *name->String() : std::string("Entity"), parent);
                _world.Add<PrefabLink>(entity, PrefabLink { inner, {}, part });
                _world.Add<PrefabPart>(entity, PrefabPart { 0 });
                parts[part] = entity;
            }
            place(node, entity, part);
            const bool changedHere = own(part, "nested");
            const Value* kept = node.Find("overrides");
            if (!changedHere) {
                std::set<std::string> given;
                if (kept && kept->Named()) for (const auto& [key, value] : *kept->Named()) given.insert(key);
                _world.Mutable<PrefabLink>(entity)->overrides = OverrideText(given);
            }
            const std::filesystem::path file = std::filesystem::path(inner).is_absolute() || ProjectDirectory().empty() ? std::filesystem::path(inner) : ProjectDirectory() / inner;
            Value innerPrefab;
            std::string error;
            if (!ReadPrefab(file, innerPrefab, error)) { _notes.push_back(std::format("{}: {}", _world.NameOf(entity), error)); return; }
            Rebuild(entity, innerPrefab);
            if (changedHere || !kept || !kept->Named()) return;
            // What the outer prefab says the inner instance made its own, it has.
            const auto of = PartsOf(entity);
            for (const auto& [key, value] : *kept->Named()) {
                const auto colon = key.find(':');
                const auto it = of.find(std::atoi(key.substr(0, colon).c_str()));
                if (it == of.end() || !_world.Alive(it->second)) continue;
                const kecs::Entity target = it->second;
                const std::string what = key.substr(colon + 1);
                kecs::Vec3 three;
                if (what == "gone") { if (target != entity) _world.Destroy(target); }
                else if (what == "position" && Three(&value, three)) _world.SetPosition(target, three);
                else if (what == "rotation" && Three(&value, three)) _world.SetRotation(target, three);
                else if (what == "scale" && Three(&value, three)) _world.SetScale(target, three);
                else if (what == "name" && value.String()) _world.Rename(target, *value.String());
                else if (what.starts_with('+')) AddComponent(target, what.substr(1));
                else if (what.starts_with('-')) RemoveComponent(target, what.substr(1));
                else if (what.starts_with('~') && value.Bool()) SetEnabled(target, what.substr(1), *value.Bool());
                else if (const auto dot = what.find('.'); dot != std::string::npos) {
                    if (!std::ranges::contains(ComponentsOf(target), what.substr(0, dot))) AddComponent(target, what.substr(0, dot));
                    SetField(target, what.substr(0, dot), what.substr(dot + 1), value);
                }
            }
        };
        const std::function<void(const Value&, kecs::Entity)> make = [&](const Value& node, const kecs::Entity parent) {
            const int part = PartOf(node);
            if (part < 0) return;
            seen.insert(part);
            const auto at = parts.find(part);
            if (own(part, "gone")) {
                if (at != parts.end() && at->second != root) _world.Destroy(at->second);
                return;
            }
            if (const Value* inner = node.Find("prefab"); inner && inner->String() && part != rootPart) { nested(node, parent, part); return; }
            const Value* name = node.Find("name");
            kecs::Entity entity = at != parts.end() && _world.Alive(at->second) && !(at->second != root && _world.Has<PrefabLink>(at->second)) ? at->second : kecs::NullEntity;
            if (entity.IsNull()) {
                if (at != parts.end() && _world.Alive(at->second)) _world.Destroy(at->second);      // an instance, before
                entity = CreateEntity(name && name->String() ? *name->String() : std::string("Entity"), parent);
                _world.Add<PrefabPart>(entity, PrefabPart { part });
                parts[part] = entity;
            }
            if (entity != root) place(node, entity, part);
            std::set<std::string> has, off;
            if (const Value* disabled = node.Find("disabled"); disabled && disabled->Items())
                for (const auto& component : *disabled->Items()) if (component.String()) off.insert(*component.String());
            if (const Value* components = node.Find("components"); components && components->Named())
                for (const auto& [component, fields] : *components->Named()) {
                    has.insert(component);
                    if (own(part, "-" + component)) { RemoveComponent(entity, component); continue; }
                    const kecs::ComponentId id = _world.Find(component);
                    if (id == kecs::NoComponent) { _notes.push_back(std::format("{}: its prefab's '{}' is no component here: left out", _world.NameOf(root), component)); continue; }
                    if (!_world.Has(entity, id)) AddComponent(entity, component);
                    if (fields.Named())
                        for (const auto& [field, given] : *fields.Named()) {
                            if (own(part, component + "." + field)) continue;
                            if (const Value* other = given.Find("part"); other && other->Number() && given.Named() && given.Named()->size() == 1) {
                                links.push_back({ entity, component, field, static_cast<int>(*other->Number()) });
                                continue;
                            }
                            SetField(entity, component, field, given);
                        }
                    if (!own(part, "~" + component)) SetEnabled(entity, component, !off.contains(component));
                }
            // What the prefab does not have, the instance has not — unless it was given it.
            for (const std::string& component : ComponentsOf(entity))
                if (!has.contains(component) && !own(part, "+" + component)) RemoveComponent(entity, component);
            if (const Value* children = node.Find("children"); children && children->Items())
                for (const auto& child : *children->Items()) make(child, entity);
        };
        make(prefab, _world.ParentOf(root));
        // What the prefab no longer has, its instance no longer has either.
        for (const auto& [part, entity] : parts)
            if (!seen.contains(part) && entity != root && _world.Alive(entity)) _world.Destroy(entity);
        // And what one of its entities holds of another is that one, now that all of them are there.
        for (const auto& link : links) {
            const auto it = parts.find(link.part);
            SetField(link.entity, link.component, link.field, it != parts.end() && _world.Alive(it->second) ? EntityValue(it->second) : Value {});
        }
        --_rebuilding;
    }

    void Document::UpdatePrefabs(const std::filesystem::path& file)
    {
        const std::filesystem::path project = ProjectDirectory();
        const auto whole = [&](const std::filesystem::path& named) { return named.is_absolute() || project.empty() ? named : project / named; };
        std::vector<kecs::Entity> roots;
        for (const kecs::Entity entity : _world.All())
            if (_world.Has<PrefabLink>(entity)) roots.push_back(entity);
        std::map<std::string, std::optional<Value>> read;
        for (const kecs::Entity root : roots) {
            if (!_world.Alive(root)) continue;
            const std::filesystem::path of = whole(_world.Get<PrefabLink>(root)->prefab);
            std::error_code ec;
            if (!file.empty() && !std::filesystem::equivalent(of, whole(file), ec)) continue;
            auto& prefab = read[of.generic_string()];
            if (!prefab && !read.contains(of.generic_string() + "?")) {
                Value root_;
                std::string error;
                if (ReadPrefab(of, root_, error)) prefab = std::move(root_);
                else { read[of.generic_string() + "?"] = std::nullopt; _notes.push_back(std::format("{}: {}. It is as it was saved.", _world.NameOf(root), error)); }
            }
            if (prefab) Rebuild(root, *prefab);
        }
        Changed();
    }

    bool Document::ApplyPrefab(const kecs::Entity root, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        if (!link) return fail("it is no prefab's instance");
        const std::filesystem::path named = link->prefab;
        const std::filesystem::path file = named.is_absolute() || ProjectDirectory().empty() ? named : ProjectDirectory() / named;
        const Value written = WritePrefab(root);
        std::string why;
        if (!WritePrefabFile(file, written, why)) return fail(why);
        _world.Mutable<PrefabLink>(root)->overrides.clear();
        UpdatePrefabs(file);
        Changed();
        return true;
    }

    bool Document::RevertPrefab(const kecs::Entity root, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        if (!link) return fail("it is no prefab's instance");
        const std::filesystem::path named = link->prefab;
        const std::filesystem::path file = named.is_absolute() || ProjectDirectory().empty() ? named : ProjectDirectory() / named;
        Value prefab;
        std::string why;
        if (!ReadPrefab(file, prefab, why)) return fail(why);
        _world.Mutable<PrefabLink>(root)->overrides.clear();
        // What was put under it goes: entities no part of the prefab, and instances of others placed by hand. Those the
        // prefab places are its own, and made as it says.
        std::vector<kecs::Entity> extra;
        const std::function<void(kecs::Entity)> walk = [&](const kecs::Entity entity) {
            for (const kecs::Entity child : _world.ChildrenOf(entity)) {
                if (const PrefabLink* link = _world.Get<PrefabLink>(child)) {
                    if (link->outer < 0) extra.push_back(child);
                    else _world.Mutable<PrefabLink>(child)->overrides.clear();
                } else if (!_world.Has<PrefabPart>(child)) extra.push_back(child);
                else walk(child);
            }
        };
        walk(root);
        for (const kecs::Entity entity : extra) _world.Destroy(entity);
        Rebuild(root, prefab);
        Changed();
        return true;
    }

    bool Document::Unlink(const kecs::Entity root)
    {
        if (!_world.Alive(root) || !_world.Has<PrefabLink>(root)) return false;
        const std::function<void(kecs::Entity)> walk = [&](const kecs::Entity entity) {
            // An instance it placed stays one, of its own: placed by hand, now.
            if (entity != root) if (PrefabLink* link = _world.Mutable<PrefabLink>(entity)) { link->outer = -1; return; }
            _world.Remove<PrefabPart>(entity);
            for (const kecs::Entity child : _world.ChildrenOf(entity)) walk(child);
        };
        walk(root);
        // And itself, placed by another, is no longer that one's.
        if (const PrefabLink* link = _world.Get<PrefabLink>(root); link && link->outer >= 0)
            if (const kecs::Entity outer = PrefabRootOf(_world.ParentOf(root)); !outer.IsNull()) Keep(outer, link->outer, "gone");
        _world.Remove<PrefabLink>(root);
        Changed();
        return true;
    }

    bool Document::Load(const std::filesystem::path& path, std::string& error)
    {
        // A scene is in a project's `scenes` folder, beside its `assets`: where its entities' files are named from.
        const std::filesystem::path project = std::filesystem::absolute(path).parent_path().parent_path();
        _runtime->SetAssetDirectory(project / "assets");
        // Its scripts' components, as their sources declare them: whether or not anything runs them, they are what the file's are.
        DeclareScripts(project / "scripts");
        std::ifstream in(path, std::ios::binary);
        if (!in) { error = std::format("'{}' cannot be read", path.string()); return false; }
        std::stringstream text;
        text << in.rdbuf();
        return FromJson(text.str(), error);
    }
}
