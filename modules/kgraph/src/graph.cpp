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
        std::string out = "{\n  \"kgraph\": 1,\n  \"nodes\": [";
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const Node& node = nodes[i];
            out += std::format("{}\n    {{\n      \"id\": {},\n      \"type\": {}", i ? "," : "", json::Quote(node.id), json::Quote(node.type));
            if (!node.title.empty()) out += std::format(",\n      \"title\": {}", json::Quote(node.title));
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
            if (const json::Value* at = item.Find("at"); at && at->items.size() == 2) { node.x = at->items[0].number; node.y = at->items[1].number; }
            if (const json::Value* inputs = item.Find("inputs"))
                for (const auto& [name, value] : inputs->members) {
                    const json::Value* link = value.kind == json::Value::Kind::eObject ? value.Find("$link") : nullptr;
                    if (link && link->items.size() == 2) node.Wire(name, link->items[0].string, link->items[1].string);
                    else node.Set(name, Value::FromJson(value));
                }
        }
        out = std::move(graph);
        return true;
    }
}
