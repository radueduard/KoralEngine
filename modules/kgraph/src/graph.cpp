//
// kgraph: the graph as data, and as JSON.
//

#include <kgraph/graph.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

#include <kecs/json.h>

namespace kgraph
{
    namespace json = kecs::json;

    const Value* Value::Find(const std::string_view name) const
    {
        if (const auto* members = Named())
            for (const auto& [key, value] : *members) if (key == name) return &value;
        return nullptr;
    }

    std::string Value::ToJson() const
    {
        struct Writer {
            std::string operator()(std::monostate) const { return "null"; }
            std::string operator()(const bool v) const { return v ? "true" : "false"; }
            std::string operator()(const double v) const
            {
                if (!std::isfinite(v)) return "null";
                char buffer[32];
                return std::string(buffer, std::to_chars(buffer, buffer + sizeof buffer, v).ptr);
            }
            std::string operator()(const std::string& v) const { return json::Quote(v); }
            std::string operator()(const List& v) const
            {
                std::string out = "[";
                for (std::size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + v[i].ToJson();
                return out + "]";
            }
            std::string operator()(const Members& v) const
            {
                std::string out = "{";
                for (std::size_t i = 0; i < v.size(); ++i) out += std::format("{} {}: {}", i ? "," : "", json::Quote(v[i].first), v[i].second.ToJson());
                return out + (v.empty() ? "}" : " }");
            }
        };
        return std::visit(Writer {}, data);
    }

    Value Value::FromJson(const json::Value& value)
    {
        switch (value.kind) {
        case json::Value::Kind::eBool: return Value(value.boolean);
        case json::Value::Kind::eNumber: return Value(value.number);
        case json::Value::Kind::eString: return Value(value.string);
        case json::Value::Kind::eArray: {
            List list;
            for (const auto& item : value.items) list.push_back(FromJson(item));
            return Value(std::move(list));
        }
        case json::Value::Kind::eObject: {
            Members members;
            for (const auto& [key, member] : value.members) members.emplace_back(key, FromJson(member));
            return Value(std::move(members));
        }
        default: return {};
        }
    }

    const Node* Graph::Find(const std::string_view id) const
    {
        const auto it = std::ranges::find(nodes, id, &Node::id);
        return it == nodes.end() ? nullptr : &*it;
    }

    Node* Graph::Find(const std::string_view id) { return const_cast<Node*>(std::as_const(*this).Find(id)); }

    std::string_view PassRateName(const PassRate rate)
    {
        switch (rate) {
        case PassRate::eSetup: return "setup";
        case PassRate::eFixed: return "fixed";
        default: return "frame";
        }
    }

    bool Graph::IsSetup(const std::string_view pass) const { return !pass.empty() && std::ranges::find(setup, pass) != setup.end(); }
    bool Graph::IsFixed(const std::string_view pass) const { return !pass.empty() && !IsSetup(pass) && std::ranges::find(fixed, pass) != fixed.end(); }
    PassRate Graph::RateOf(const std::string_view pass) const { return IsSetup(pass) ? PassRate::eSetup : IsFixed(pass) ? PassRate::eFixed : PassRate::eFrame; }

    bool Graph::SetRate(const std::string_view pass, const PassRate rate)
    {
        if (pass.empty() || std::ranges::find(passes, pass) == passes.end()) return false;
        std::erase(setup, pass);
        std::erase(fixed, pass);
        if (rate == PassRate::eSetup) setup.emplace_back(pass);
        if (rate == PassRate::eFixed) fixed.emplace_back(pass);
        OrderByRate();
        return true;
    }

    void Graph::OrderByRate()
    {
        const auto gone = [&](const std::string& pass) { return std::ranges::find(passes, pass) == passes.end(); };
        std::erase_if(setup, gone);
        std::erase_if(fixed, [&](const std::string& pass) { return gone(pass) || IsSetup(pass); });
        std::vector<std::string> ordered = setup;
        ordered.insert(ordered.end(), fixed.begin(), fixed.end());
        for (const auto& pass : passes)
            if (RateOf(pass) == PassRate::eFrame) ordered.push_back(pass);
        passes = std::move(ordered);
    }

    Node& Graph::Add(std::string id, std::string type)
    {
        Node node;
        node.id = std::move(id);
        node.type = std::move(type);
        nodes.push_back(std::move(node));
        return nodes.back();
    }

    // A wire is written where a value would be, as { "$link": [node, output] }: no value a node takes is that.
    std::string Graph::ToJson() const
    {
        std::string out = "{\n  \"kgraph\": 1,";
        // The pages by how often they run: what is made once, what is done on the fixed step, then every frame.
        const auto list = [&](const char* key, const PassRate rate) {
            std::string names;
            for (const auto& pass : passes)
                if (RateOf(pass) == rate) names += std::format("{}{}", names.empty() ? "" : ", ", json::Quote(pass));
            if (!names.empty()) out += std::format("\n  \"{}\": [{}],", key, names);
        };
        list("setup", PassRate::eSetup);
        list("fixed", PassRate::eFixed);
        list("passes", PassRate::eFrame);
        if (fixedStepsAtMost != Graph().fixedStepsAtMost) out += std::format("\n  \"fixedStepsAtMost\": {},", fixedStepsAtMost);
        if (!meshImports.empty()) {
            // How model files are read: each file's attributes left out, and its buffers.
            out += "\n  \"meshImports\": {";
            bool first = true;
            for (const auto& [file, import] : meshImports) {
                Value::List leaveOut(import.leaveOut.begin(), import.leaveOut.end()), buffers;
                for (const auto& buffer : import.buffers) buffers.emplace_back(Value::List(buffer.begin(), buffer.end()));
                out += std::format("{}\n    {}: {}", first ? "" : ",", json::Quote(file),
                                   Value(Value::Members { { "leaveOut", Value(std::move(leaveOut)) }, { "buffers", Value(std::move(buffers)) } }).ToJson());
                first = false;
            }
            out += "\n  },";
        }
        if (defaultMeshTangents != Graph().defaultMeshTangents)
            out += std::format("\n  \"defaultMeshTangents\": {},", json::Quote(TangentsName(defaultMeshTangents)));
        if (!overview.empty()) {
            // Where each node is when every node is seen at once.
            out += "\n  \"overview\": {";
            bool first = true;
            for (const auto& [id, at] : overview) {
                out += std::format("{}\n    {}: {}", first ? "" : ",", json::Quote(id), Value(Value::List { at.first, at.second }).ToJson());
                first = false;
            }
            out += "\n  },";
        }
        out += "\n  \"nodes\": [";
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const Node& node = nodes[i];
            out += std::format("{}\n    {{\n      \"id\": {},\n      \"type\": {}", i ? "," : "", json::Quote(node.id), json::Quote(node.type));
            if (!node.title.empty()) out += std::format(",\n      \"title\": {}", json::Quote(node.title));
            if (!node.pass.empty()) out += std::format(",\n      \"pass\": {}", json::Quote(node.pass));
            if (!node.placed.empty()) {
                Value::Members placed;
                for (const auto& [input, where] : node.placed) placed.emplace_back(input, Value(where));
                out += std::format(",\n      \"placed\": {}", Value(std::move(placed)).ToJson());
            }
            out += std::format(",\n      \"at\": {}", Value(Value::List { node.x, node.y }).ToJson());
            out += ",\n      \"inputs\": {";
            bool first = true;
            for (const auto& [name, input] : node.inputs) {
                const std::string value = input.link
                    ? std::format("{{ \"$link\": [{}, {}] }}", json::Quote(input.link->node), json::Quote(input.link->output))
                    : input.value.ToJson();
                out += std::format("{}\n        {}: {}", first ? "" : ",", json::Quote(name), value);
                first = false;
            }
            out += first ? "}\n    }" : "\n      }\n    }";
        }
        return out + (nodes.empty() ? "]\n}\n" : "\n  ]\n}\n");
    }

    bool Graph::FromJson(const std::string_view text, Graph& out, std::string& error)
    {
        json::Value root;
        if (!json::Parse(text, root, error)) return false;
        const json::Value* version = root.Find("kgraph");
        const json::Value* nodes = root.Find("nodes");
        if (!version || !nodes || nodes->kind != json::Value::Kind::eArray) {
            error = "not a saved graph: it needs \"kgraph\" and \"nodes\"";
            return false;
        }
        if (version->number != 1.0) {
            error = std::format("a saved graph of version {}, which this does not read", version->number);
            return false;
        }
        Graph graph;
        for (const auto& item : nodes->items) {
            const json::Value* id = item.Find("id");
            const json::Value* type = item.Find("type");
            if (!id || !type || id->kind != json::Value::Kind::eString || type->kind != json::Value::Kind::eString) {
                error = "a node needs an \"id\" and a \"type\"";
                return false;
            }
            if (graph.Find(id->string)) {
                error = std::format("two nodes are called '{}'", id->string);
                return false;
            }
            Node& node = graph.Add(id->string, type->string);
            if (const json::Value* title = item.Find("title")) node.title = title->string;
            if (const json::Value* pass = item.Find("pass"); pass && pass->kind == json::Value::Kind::eString) node.pass = pass->string;
            if (const json::Value* at = item.Find("at"); at && at->items.size() == 2) { node.x = at->items[0].number; node.y = at->items[1].number; }
            if (const json::Value* placed = item.Find("placed"); placed && placed->kind == json::Value::Kind::eObject)
                for (const auto& [input, where] : placed->members)
                    if (where.kind == json::Value::Kind::eString && (where.string == "node" || where.string == "inspector")) node.placed[input] = where.string;
            if (const json::Value* inputs = item.Find("inputs"))
                for (const auto& [name, value] : inputs->members) {
                    const json::Value* link = value.kind == json::Value::Kind::eObject ? value.Find("$link") : nullptr;
                    if (link && link->items.size() == 2) node.Wire(name, link->items[0].string, link->items[1].string);
                    else node.Set(name, Value::FromJson(value));
                }
        }
        if (const json::Value* overview = root.Find("overview"); overview && overview->kind == json::Value::Kind::eObject)
            for (const auto& [id, at] : overview->members)
                if (at.items.size() == 2) graph.overview[id] = { at.items[0].number, at.items[1].number };
        if (const json::Value* imports = root.Find("meshImports"); imports && imports->kind == json::Value::Kind::eObject)
            for (const auto& [file, entry] : imports->members) {
                MeshImport import;
                if (const json::Value* leave = entry.Find("leaveOut"))
                    for (const auto& semantic : leave->items) if (semantic.kind == json::Value::Kind::eString) import.leaveOut.push_back(semantic.string);
                if (const json::Value* buffers = entry.Find("buffers"))
                    for (const auto& buffer : buffers->items) {
                        std::vector<std::string> semantics;
                        for (const auto& semantic : buffer.items) if (semantic.kind == json::Value::Kind::eString) semantics.push_back(semantic.string);
                        if (!semantics.empty()) import.buffers.push_back(std::move(semantics));
                    }
                if (!import.Default()) graph.meshImports[file] = std::move(import);
            }
        if (const json::Value* tangents = root.Find("defaultMeshTangents"); tangents && tangents->kind == json::Value::Kind::eString)
            if (const auto named = TangentsNamed(tangents->string)) graph.defaultMeshTangents = *named;
        if (const json::Value* setup = root.Find("setup"))
            for (const auto& pass : setup->items)
                if (pass.kind == json::Value::Kind::eString && std::ranges::find(graph.passes, pass.string) == graph.passes.end()) {
                    graph.passes.push_back(pass.string);
                    graph.setup.push_back(pass.string);
                }
        if (const json::Value* most = root.Find("fixedStepsAtMost"); most && most->kind == json::Value::Kind::eNumber)
            graph.fixedStepsAtMost = std::clamp(static_cast<int>(most->number), 1, 16);
        if (const json::Value* fixed = root.Find("fixed"))
            for (const auto& pass : fixed->items)
                if (pass.kind == json::Value::Kind::eString && std::ranges::find(graph.passes, pass.string) == graph.passes.end()) {
                    graph.passes.push_back(pass.string);
                    graph.fixed.push_back(pass.string);
                }
        if (const json::Value* passes = root.Find("passes"))
            for (const auto& pass : passes->items)
                if (pass.kind == json::Value::Kind::eString && std::ranges::find(graph.passes, pass.string) == graph.passes.end()) graph.passes.push_back(pass.string);
        // A node of a pass the list does not have: the pass is there all the same, after those it does.
        for (const auto& node : graph.nodes)
            if (!node.pass.empty() && std::ranges::find(graph.passes, node.pass) == graph.passes.end()) graph.passes.push_back(node.pass);
        out = std::move(graph);
        return true;
    }
}
