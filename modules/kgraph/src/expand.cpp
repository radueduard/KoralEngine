//
// kgraph: a graph's loops, written out. @see expand.h
//

#include "expand.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <set>

namespace kgraph
{
    std::string TurnId(const std::string_view id, const int turn) { return std::format("{} [{}]", id, turn); }

    namespace {
        const std::string* Text(const Node& node, const char* input)
        {
            const auto it = node.inputs.find(input);
            const std::string* text = it == node.inputs.end() || it->second.link ? nullptr : it->second.value.String();
            return text && !text->empty() ? text : nullptr;
        }
    }

    Port FunctionPort(std::string name, const std::string_view kind, std::string doc)
    {
        Port port { std::move(name), PortKind::eValue, "value", false, std::move(doc), nullptr };
        if (kind == "eEntities") port.type = "entities";
        else if (kind == "eMesh" || kind == "eBuffer" || kind == "eImage" || kind == "eResource") {
            port.kind = PortKind::eResource;
            port.type = kind == "eMesh" ? "Mesh" : kind == "eBuffer" ? "Buffer" : kind == "eImage" ? "Image" : "Resource";
        }
        return port;
    }

    namespace {
        bool IsEdge(const Node& node) { return node.type == "FunctionInput" || node.type == "FunctionOutput"; }

        /** What an edge of a function stands for where it is: what its `value` is given, or — an input given nothing — its default. */
        Input EdgeValue(const Node& node)
        {
            if (const auto value = node.inputs.find("value"); value != node.inputs.end() && (value->second.link || !value->second.value.IsNull())) return value->second;
            if (node.type == "FunctionInput")
                if (const auto initial = node.inputs.find("default"); initial != node.inputs.end()) return Input { initial->second.value, std::nullopt };
            return {};
        }

        /**
         * The graph with its calls written out: each Function node the nodes of what it calls, `<call>/<node>`, in the
         * call's pass; what is wired to a FunctionInput of it, what the call is given there; and what is wired from the
         * call, what its FunctionOutput is wired from. A function opened in place — its edges in the graph itself —
         * has them stand for what they are given in the same way.
         */
        Graph WriteCalls(const Graph& given, Expansion& out)
        {
            Graph graph;
            graph.passes = given.passes;
            graph.setup = given.setup;
            graph.fixed = given.fixed;
            graph.after = given.after;
            graph.orderSettled = given.orderSettled;
            graph.fixedStepsAtMost = given.fixedStepsAtMost;
            graph.meshImports = given.meshImports;
            graph.textureImports = given.textureImports;
            graph.texturePacks = given.texturePacks;
            graph.defaultMeshTangents = given.defaultMeshTangents;
            graph.nodes = given.nodes;
            // What each edge stands for, by its id: a wire to it is a wire to that.
            std::map<std::string, Input, std::less<>> stands;
            for (const auto& node : graph.nodes)
                if (IsEdge(node)) { stands[node.id] = EdgeValue(node); out.aliases[node.id] = stands[node.id]; }
            for (int depth = 0;; ++depth) {
                std::vector<Node> calls;
                for (const auto& node : graph.nodes) if (node.type == "Function") calls.push_back(node);
                if (calls.empty()) break;
                // Where each call is among the nodes: its nodes go there, so that its passes are where it is among the
                // others of its page — which, with nothing they use to say, run in the order of the nodes.
                std::vector<std::string> sequence;
                for (const auto& node : graph.nodes) sequence.push_back(node.id);
                std::erase_if(graph.nodes, [](const Node& node) { return node.type == "Function"; });
                if (depth >= 8) {
                    for (const auto& call : calls) out.errors[call.id] = "its function calls itself, further than eight deep: a function cannot have itself in it";
                    break;
                }
                for (const Node& call : calls) {
                    const std::string* name = Text(call, "function");
                    const auto found = name ? given.functions.find(*name) : given.functions.end();
                    if (!name) { out.errors[call.id] = "'function' is which function it calls: one of the project's functions"; continue; }
                    if (found == given.functions.end() || !found->second) { out.errors[call.id] = std::format("there is no function '{}' among the project's functions", *name); continue; }
                    const Graph& function = *found->second;
                    const std::string prefix = call.id + "/";
                    Expansion::Call& record = out.calls[call.id];
                    record.function = *name;
                    record.called = call.id;
                    // Its edges, in the order they say (`order`) — and those that say none after them, as they are on its page: top
                    // to bottom, then left to right.
                    std::vector<const Node*> ins, outs;
                    for (const auto& node : function.nodes) {
                        if (node.type == "FunctionInput") ins.push_back(&node);
                        if (node.type == "FunctionOutput") outs.push_back(&node);
                    }
                    const auto orderOf = [](const Node* edge) { const auto it = edge->inputs.find("order"); const double* n = it == edge->inputs.end() || it->second.link ? nullptr : it->second.value.Number(); return n ? *n : 1e300; };
                    const auto byPlace = [&](const Node* a, const Node* b) {
                        if (orderOf(a) != orderOf(b)) return orderOf(a) < orderOf(b);
                        return a->y != b->y ? a->y < b->y : a->x < b->x;
                    };
                    std::ranges::stable_sort(ins, byPlace);
                    std::ranges::stable_sort(outs, byPlace);
                    const auto portName = [](const Node& edge) { const std::string* named = Text(edge, "name"); return named ? *named : edge.id; };
                    for (const Node* edge : ins) {
                        const std::string port = portName(*edge);
                        const std::string* kind = Text(*edge, "kind");
                        record.inputs.push_back(FunctionPort(port, kind ? *kind : "eValue"));
                        // What the call is given there: a wire, a value typed on it — or the input's default.
                        const auto on = call.inputs.find(port);
                        stands[prefix + edge->id] = on != call.inputs.end() && (on->second.link || !on->second.value.IsNull()) ? on->second : EdgeValue(*edge);
                        // A buffer given nothing: one of nothing, made in the call's place — what the function reads of it,
                        // zeros; so that one may be left out (it is given, with its count, where it is wanted).
                        if (kind && *kind == "eBuffer" && !stands[prefix + edge->id].link && stands[prefix + edge->id].value.IsNull()) {
                            Node none;
                            none.id = prefix + edge->id + " (none)";
                            none.type = "Buffer";
                            none.pass = call.pass;
                            none.x = call.x;
                            none.y = call.y;
                            none.Set("instanceCount", Value(256)).Set("type", Value("eDeviceLocal"))
                                .Set("usage", Value(Value::List { Value("eStorage"), Value("eTransferSrc"), Value("eTransferDst") }));
                            record.nodes.push_back(none.id);
                            stands[prefix + edge->id] = Input { {}, Link { none.id, "out" } };
                            graph.nodes.push_back(std::move(none));
                        }
                    }
                    for (const Node* edge : outs) {
                        const std::string port = portName(*edge);
                        record.outputs.push_back(port);
                        Input value = EdgeValue(*edge);
                        if (value.link) value.link->node = prefix + value.link->node;
                        stands[prefix + edge->id] = value;
                        record.gives[port] = Link { prefix + edge->id, "out" };
                    }
                    for (const auto& node : function.nodes) {
                        if (IsEdge(node)) continue;
                        Node made = node;
                        made.id = prefix + node.id;
                        made.pass = call.pass;
                        for (auto& input : made.inputs | std::views::values)
                            if (input.link) input.link->node = prefix + input.link->node;
                        // What names another node of the function by its id names it as it is written out.
                        const auto named = [&](const char* input) {
                            if (auto target = made.inputs.find(input); target != made.inputs.end() && target->second.value.String() && !target->second.value.String()->empty())
                                target->second.value = Value(prefix + *target->second.value.String());
                        };
                        if (made.type == "Reference") named("node");
                        if (made.type == "Previous" || made.type == "Element") { named("of"); named("first"); }
                        record.nodes.push_back(made.id);
                        graph.nodes.push_back(std::move(made));
                    }
                    // What was wired from the call is wired from what its outputs are: straight from it, or from a
                    // Reference to it (another page's use of it) — once the call is written out, there is no node it names.
                    const auto ofCall = [&](std::string_view id) {
                        for (int hops = 0; hops < 16; ++hops) {
                            if (id == call.id) return true;
                            const Node* node = graph.Find(id);
                            const std::string* target = node && node->type == "Reference" ? Text(*node, "node") : nullptr;
                            if (!target) return false;
                            id = *target;
                        }
                        return false;
                    };
                    for (auto& node : graph.nodes)
                        for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                            if (!it->second.link || !ofCall(it->second.link->node)) { ++it; continue; }
                            const auto gives = record.gives.find(it->second.link->output);
                            if (gives == record.gives.end()) {
                                out.errors[call.id] = std::format("'{}' is wired from its '{}', and its function has no such output", node.id, it->second.link->output);
                                it = node.inputs.erase(it);
                                continue;
                            }
                            it->second.link = gives->second;
                            ++it;
                        }
                }
                // A call given what another of this round gives: the other was not in the graph to be wired again when it
                // was written out — it was waiting its turn — so what it is given is what that one's output stands for.
                for (auto& [edge, input] : stands) {
                    if (!input.link || std::ranges::none_of(calls, [&](const Node& call) { return call.id == input.link->node; })) continue;
                    const auto record = out.calls.find(input.link->node);
                    if (record == out.calls.end()) continue;    // it could not be written out: it said why
                    const auto gives = record->second.gives.find(input.link->output);
                    if (gives == record->second.gives.end()) {
                        out.errors[edge.substr(0, edge.rfind('/'))] = std::format("it is given '{}' of '{}', and that function has no such output", input.link->output, input.link->node);
                        input = {};
                        continue;
                    }
                    input.link = gives->second;
                }
                std::map<std::string, std::size_t, std::less<>> at;
                for (std::size_t i = 0; i < graph.nodes.size(); ++i) at.emplace(graph.nodes[i].id, i);
                std::vector<Node> placed;
                std::vector<bool> taken(graph.nodes.size(), false);
                const auto take = [&](const std::string& id) {
                    if (const auto it = at.find(id); it != at.end() && !taken[it->second]) { taken[it->second] = true; placed.push_back(std::move(graph.nodes[it->second])); }
                };
                for (const auto& id : sequence) {
                    if (const auto call = out.calls.find(id); call != out.calls.end() && std::ranges::any_of(calls, [&](const Node& c) { return c.id == id; }))
                        for (const auto& made : call->second.nodes) take(made);
                    else take(id);
                }
                for (std::size_t i = 0; i < graph.nodes.size(); ++i) if (!taken[i]) placed.push_back(std::move(graph.nodes[i]));
                graph.nodes = std::move(placed);
            }
            // Each wire to an edge is a wire to what the edge stands for — through as many as there are between.
            const auto follow = [&](Input input) {
                for (int hops = 0; hops < 64 && input.link && stands.contains(input.link->node); ++hops) input = stands.at(input.link->node);
                return input;
            };
            for (auto& node : graph.nodes)
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    if (it->second.link && stands.contains(it->second.link->node)) {
                        it->second = follow(it->second);
                        if (!it->second.link && it->second.value.IsNull()) { it = node.inputs.erase(it); continue; }
                    }
                    ++it;
                }
            std::erase_if(graph.nodes, IsEdge);
            for (auto& record : out.calls | std::views::values)
                for (auto& link : record.gives | std::views::values) {
                    const Input written = follow(Input { {}, link });
                    link = written.link ? *written.link : Link { {}, {} };
                }
            for (auto& [id, value] : out.aliases) value = follow(value);
            // A Reference to a call — another page's use of it — is that call where it is: what is wired from it was
            // wired from the call's outputs above, and what it gives and how it fares are the call's.
            const auto calledBy = [&](const Node& reference) -> const Expansion::Call* {
                const Node* node = &reference;
                for (int hops = 0; node && node->type == "Reference" && hops < 16; ++hops) {
                    const std::string* target = Text(*node, "node");
                    if (!target) return nullptr;
                    if (const auto call = out.calls.find(*target); call != out.calls.end()) return &call->second;
                    node = graph.Find(*target);
                }
                return nullptr;
            };
            std::vector<std::pair<std::string, Expansion::Call>> referred;
            for (const auto& node : graph.nodes)
                if (node.type == "Reference")
                    if (const Expansion::Call* call = calledBy(node)) referred.emplace_back(node.id, *call);
            for (auto& [id, call] : referred) {
                std::erase_if(graph.nodes, [&](const Node& node) { return node.id == id; });
                out.calls[id] = std::move(call);
            }
            // What was wired to one through a call's input — written out after the call it refers to was — is
            // wired from the call's outputs too.
            const auto through = [&](Link& link) {
                if (!std::ranges::contains(referred, link.node, &std::pair<std::string, Expansion::Call>::first)) return true;
                const auto& gives = out.calls.at(link.node).gives;
                const auto it = gives.find(link.output);
                if (it == gives.end()) return false;
                link = it->second;
                return true;
            };
            for (auto& node : graph.nodes)
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    if (it->second.link && !through(*it->second.link)) {
                        out.errors[node.id] = std::format("'{}' is wired from '{}' of '{}', and its function has no such output", it->first, it->second.link->output, it->second.link->node);
                        it = node.inputs.erase(it);
                        continue;
                    }
                    ++it;
                }
            for (auto& record : out.calls | std::views::values)
                for (auto& link : record.gives | std::views::values) (void)through(link);
            return graph;
        }
    }

    std::string BundleTaken(const std::string_view type, const std::string_view input)
    {
        if (input == "fields" && (type == "BufferBuilder" || type == "ShaderBuffer")) return "field.";
        if (input == "push" && (type == "Draw" || type == "Dispatch" || type == "TraceRays")) return "push.";
        return {};
    }

    namespace {
        bool IsEach(const std::string_view type) { return type == "ForEach" || type == "Entities" || type == "Filter" || type == "Entity"; }

        /** A BufferBuilder's fields, by name, as its layout lists them. */
        std::vector<std::string> LayoutNames(const Node& node)
        {
            std::vector<std::string> names;
            const auto layout = node.inputs.find("layout");
            if (layout == node.inputs.end() || !layout->second.value.Items()) return names;
            for (const auto& field : *layout->second.value.Items())
                if (const auto* members = field.Named())
                    for (const auto& [key, value] : *members)
                        if (key == "name" && value.String()) names.push_back(*value.String());
            return names;
        }

        /**
         * The graph with its Bundles written out: each input that takes one — a buffer's `fields`, a draw's `push` — is
         * given, for each member, the input of that name (`field.<name>`), unless it is given that already; and what is
         * wired from an Unbundle is wired from the member it names. So nothing new is worked out: a Bundle is a way of
         * wiring many at once. A Bundle or an Unbundle node is left in the graph, and builds nothing.
         */
        void WriteBundles(Graph& graph, Expansion& out, const ElementOf& elementOf)
        {
            std::map<std::string, const Node*, std::less<>> byId;
            for (const auto& node : graph.nodes) byId.emplace(node.id, &node);
            using Members = std::vector<std::pair<std::string, Input>>;
            // What a wire to a Bundle holds: a Bundle node's out, of what it is given; a ForEach's element. None, for anything else.
            std::function<std::optional<Members>(const Link&, int)> resolve = [&](const Link& link, const int depth) -> std::optional<Members> {
                const std::string key = link.node + "." + link.output;
                if (const auto known = out.bundles.find(key); known != out.bundles.end()) return known->second;
                const auto found = byId.find(link.node);
                if (found == byId.end() || depth > 32) return std::nullopt;
                const Node& node = *found->second;
                Members members;
                if (node.type == "Bundle" && link.output == "out") {
                    // What it is made on first, then its own: one of the same name takes the other's place.
                    if (const auto base = node.inputs.find("base"); base != node.inputs.end() && base->second.link)
                        if (auto under = resolve(*base->second.link, depth + 1)) members = std::move(*under);
                    // In the order of its names; then any given that its names have not.
                    std::vector<std::string> order;
                    if (const auto names = node.inputs.find("names"); names != node.inputs.end() && names->second.value.Items())
                        for (const auto& name : *names->second.value.Items())
                            if (name.String()) order.push_back(*name.String());
                    for (const auto& name : node.inputs | std::views::keys)
                        if (name.starts_with("field.") && !std::ranges::contains(order, name.substr(6))) order.push_back(name.substr(6));
                    for (const auto& member : order) {
                        const auto input = node.inputs.find("field." + member);
                        if (input == node.inputs.end() || (!input->second.link && input->second.value.IsNull())) continue;
                        if (const auto had = std::ranges::find(members, member, &Members::value_type::first); had != members.end()) had->second = input->second;
                        else members.emplace_back(member, input->second);
                    }
                } else if (IsEach(node.type) && link.output == "element" && elementOf) {
                    for (const auto& [member, output] : elementOf(node)) members.emplace_back(member, Input { {}, Link { node.id, output } });
                } else {
                    return std::nullopt;
                }
                out.bundles[key] = members;
                return members;
            };

            // Every Bundle there is, taken or not: what an editor offers a wire of one says how many of a node's it matches.
            for (const auto& node : graph.nodes) {
                if (node.type == "Bundle") (void)resolve(Link { node.id, "out" }, 0);
                else if (IsEach(node.type)) (void)resolve(Link { node.id, "element" }, 0);
            }
            for (auto& node : graph.nodes) {
                // What an Unbundle gives, by name, is what its Bundle has of that name.
                // One that is no Bundle here — a Switch of two, a Previous — is a struct as the graph runs: taken apart then.
                if (node.type == "Unbundle")
                    if (const auto of = node.inputs.find("of"); of != node.inputs.end() && of->second.link)
                        if (auto members = resolve(*of->second.link, 0)) out.bundles[node.id + ".of"] = std::move(*members);
                std::vector<std::string> taken;
                for (const auto& name : node.inputs | std::views::keys)
                    if (!BundleTaken(node.type, name).empty()) taken.push_back(name);
                for (const auto& name : taken) {
                    const Input given = node.inputs.at(name);
                    node.inputs.erase(name);
                    if (!given.link) continue;
                    const auto members = resolve(*given.link, 0);
                    if (!members) {
                        // A buffer's fields of a struct only as the graph runs (a Switch of two): filled from it then, by name.
                        if (name == "fields" && (node.type == "ShaderBuffer" || node.type == "BufferBuilder")) { node.inputs[name] = given; continue; }
                        out.errors[node.id] = std::format("'{}' takes a Bundle — a Bundle node's, a ForEach's element, or a struct as the graph runs, for a buffer's fields — "
                                                          "and is wired from '{}' of '{}'", name, given.link->output, given.link->node);
                        continue;
                    }
                    const std::string prefix = BundleTaken(node.type, name);
                    // A BufferBuilder's fields are on the node: what matches none of them is said, and so is what of them it has none for.
                    const std::vector<std::string> layout = node.type == "BufferBuilder" ? LayoutNames(node) : std::vector<std::string> {};
                    std::vector<std::string> unmatched;
                    for (const auto& [member, input] : *members) {
                        if (node.type == "BufferBuilder" && !std::ranges::contains(layout, member)) { unmatched.push_back(member); continue; }
                        if (node.inputs.contains(prefix + member)) continue;     // given by itself: that wins
                        node.inputs[prefix + member] = input;
                        out.fromBundle[node.id].insert(prefix + member);
                    }
                    std::vector<std::string> missing;
                    for (const auto& field : layout)
                        if (!node.inputs.contains(prefix + field)) missing.push_back(field);
                    const auto list = [](const std::vector<std::string>& names) {
                        std::string text;
                        for (const auto& each : names) text += (text.empty() ? "'" : ", '") + each + "'";
                        return text;
                    };
                    std::string note;
                    if (!unmatched.empty()) note = std::format("the bundle has {}, which the layout has not", list(unmatched));
                    if (!missing.empty()) note += std::format("{}the layout has {}, which the bundle has not", note.empty() ? "" : "; ", list(missing));
                    if (!note.empty()) out.notes[node.id] = std::move(note);
                }
            }
            // What a Bundle is made on, and what an Unbundle takes apart, is written out: no wire of it is left to follow. A
            // Bundle has what it is made on as inputs of its own, for its value as the graph runs. What is a struct only as it
            // runs is left wired, for then.
            for (auto& node : graph.nodes) {
                if (node.type == "Bundle")
                    if (const auto base = node.inputs.find("base"); base != node.inputs.end() && base->second.link)
                        if (const auto under = resolve(*base->second.link, 0)) {
                            for (const auto& [member, input] : *under)
                                if (!node.inputs.contains("field." + member)) node.inputs["field." + member] = input;
                            node.inputs.erase("base");
                        }
                if (node.type == "Unbundle" && out.bundles.contains(node.id + ".of")) node.inputs.erase("of");
            }
            // What is wired from an Unbundle is wired from its member of that name.
            for (auto& node : graph.nodes)
                for (auto& [name, input] : node.inputs) {
                    if (!input.link) continue;
                    const auto from = byId.find(input.link->node);
                    if (from == byId.end() || from->second->type != "Unbundle") continue;
                    const auto bundle = out.bundles.find(input.link->node + ".of");
                    if (bundle == out.bundles.end()) continue;     // it said why
                    const auto member = std::ranges::find(bundle->second, input.link->output, &Members::value_type::first);
                    if (member == bundle->second.end()) {
                        out.errors[node.id] = std::format("'{}' is wired from '{}' of '{}', and its Bundle has no '{}'", name, input.link->output, input.link->node, input.link->output);
                        input = {};
                        continue;
                    }
                    input = member->second;
                }
        }
    }

    /**
     * A Draw `batched`, its mesh each element's of a Filter or a ForEach, written out as one indirect draw of all of them:
     * a Filter of the same entities grouped by mesh (`<draw>/batch`), their meshes packed as one (`<draw>/pack`), and a
     * command for each mesh (`<draw>/commands`). What each element is to the shader is then its [entity("PER_INSTANCE")]
     * buffer's, filled in the batch's order. A Draw wired a value of each element otherwise — a `push.model` from the
     * element's matrix — cannot be one draw: it is left a draw each, and says why.
     */
    /**
     * A ComponentBuffer written out as what it is: a ForEach of its component (`<buffer>/each`), and the node itself a
     * BufferBuilder laid out as the fields it holds — the component's, in the order it declares them, then where the
     * entity is — each wired from the ForEach's output of it, unless the node is wired another for it.
     */
    static void WriteComponentBuffers(Graph& graph, Expansion& out, const FieldsOf& fieldsOf)
    {
        std::vector<std::pair<std::string, Node>> made;     // each ForEach, and the buffer it goes before
        for (Node& node : graph.nodes) {
            if (node.type != "ComponentBuffer") continue;
            const std::string* component = Text(node, "component");
            if (!component || component->empty()) { out.errors[node.id] = "'component' is the component whose entities it holds: name one"; continue; }
            const auto fields = fieldsOf ? fieldsOf(*component) : std::nullopt;
            if (!fields) { out.errors[node.id] = std::format("there is no component '{}'", *component); continue; }
            // Which it holds: those ticked; told nothing, every field of its own and where the entity is.
            std::optional<std::set<std::string, std::less<>>> ticked;
            if (const auto given = node.inputs.find("fields"); given != node.inputs.end() && given->second.value.Items()) {
                ticked.emplace();
                for (const auto& name : *given->second.value.Items())
                    if (name.String()) ticked->insert(*name.String());
            }
            const auto holds = [&](const std::string_view name, const bool unticked) { return ticked ? ticked->contains(name) : unticked; };

            Node each;
            each.id = node.id + "/each";
            each.type = "ForEach";
            each.pass = node.pass;
            each.Set("component", *component);
            for (const char* name : { "with", "without", "where" })
                if (const auto given = node.inputs.find(name); given != node.inputs.end()) each.inputs[name] = given->second;
            Value::List layout;
            std::set<std::string, std::less<>> has;
            const auto add = [&](const std::string& name, const std::string_view type, std::string output) {
                layout.emplace_back(Value::Members { { "name", name }, { "type", std::string(type) } });
                if (!node.inputs.contains("field." + name)) node.Wire("field." + name, each.id, std::move(output));
            };
            for (const auto& [name, type] : *fields) {
                has.insert(name);
                if (holds(name, true)) add(name, type, "field." + name);
            }
            // Where the entity is, unless a field of its own has the name: that one is the component's.
            for (const auto& [name, type] : PlaceFields()) {
                if (!has.insert(std::string(name)).second) continue;
                if (holds(name, PlacedByDefault(name))) add(std::string(name), type, std::string(name));
            }
            // What it does not hold is left out, said: a name the component has not, a field wired that is not ticked.
            std::string unknown, unticked;
            if (ticked)
                for (const auto& name : *ticked)
                    if (!has.contains(name)) unknown += std::format("{}'{}'", unknown.empty() ? "" : ", ", name);
            std::vector<std::string> wired;
            for (const auto& name : node.inputs | std::views::keys)
                if (name.starts_with("field.") && !std::ranges::contains(layout, Value(name.substr(6)), [](const Value& field) { return *field.Find("name"); }))
                    wired.push_back(name);
            for (const auto& name : wired) {
                node.inputs.erase(name);
                unticked += std::format("{}'{}'", unticked.empty() ? "" : ", ", name.substr(6));
            }
            std::string note;
            if (!unknown.empty()) note = std::format("{} has no {}: it is left out", *component, unknown);
            if (!unticked.empty()) note += std::format("{}{} is wired and not ticked: it is left out", note.empty() ? "" : "; ", unticked);
            if (!note.empty()) out.notes[node.id] = std::move(note);
            if (layout.empty()) { out.errors[node.id] = "it holds no field: tick one in 'fields'"; continue; }

            node.type = "BufferBuilder";
            for (const char* name : { "component", "fields", "with", "without", "where" }) node.inputs.erase(name);
            node.Set("layout", Value(std::move(layout)));
            made.emplace_back(node.id, std::move(each));
        }
        for (auto& [before, each] : made)
            graph.nodes.insert(std::ranges::find(graph.nodes, before, &Node::id), std::move(each));
    }

    /**
     * What is one node for the one who writes the graph, and several for what runs it, written out as those:
     *
     * - a ComputePass is a ComputePipeline (`<pass>/pipeline`) of its module, entry, parameters and constants, and the
     *   node itself a Dispatch of it, with the rest;
     * - a RenderTarget is an Image, sized by the Screen (`<target>/screen`) or the image it is sized by, times its scale,
     *   rounded up and at least one (`<target>/extent`); what is wired from its `extent` is wired from that;
     * - a History is a Keep on a page of its own before every other (`@history`): copied when the frame begins, before
     *   anything writes what it copies, so that what reads it during a frame reads it as the frame before left it.
     */
    static void WriteShorthands(Graph& graph, Expansion& out)
    {
        // Each node made, and the one it goes before: a deque, so that a node `add` gave stays where it is while more are
        // added — `add(...).Wire("a", made.id)` evaluates `made.id` after the add.
        std::deque<std::pair<std::string, Node>> made;
        std::map<std::string, std::string, std::less<>> extents;    // a RenderTarget's `extent`, by its id: what gives it now
        const auto add = [&](const Node& at, std::string id, std::string type) -> Node& {
            Node node;
            node.id = std::move(id);
            node.type = std::move(type);
            node.pass = at.pass;
            made.emplace_back(at.id, std::move(node));
            return made.back().second;
        };
        bool history = false;
        for (Node& node : graph.nodes) {
            if (node.type == "ComputePass") {
                Node& pipeline = add(node, node.id + "/pipeline", "ComputePipeline");
                for (const auto& [from, to] : { std::pair { "module", "compute.module" }, { "entry", "compute.entry" } })
                    if (const auto given = node.inputs.find(from); given != node.inputs.end()) pipeline.inputs[to] = given->second;
                // A parameter or a constant is the pipeline's, the same for every dispatch; what else it is given, the Dispatch's.
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    if (it->first.starts_with("param.") || it->first.starts_with("constant.")) {
                        pipeline.inputs[it->first] = it->second;
                        it = node.inputs.erase(it);
                    } else if (it->first == "module" || it->first == "entry") {
                        it = node.inputs.erase(it);
                    } else {
                        ++it;
                    }
                }
                node.type = "Dispatch";
                node.Wire("pipeline", pipeline.id);
            } else if (node.type == "RenderTarget") {
                const auto sizeOf = node.inputs.find("sizeOf");
                const auto extent = node.inputs.find("extent");
                Input size;
                if (extent != node.inputs.end()) {
                    size = extent->second;
                } else if (sizeOf != node.inputs.end() && sizeOf->second.link) {
                    Node& of = add(node, node.id + "/size", "ImageExtent");
                    of.inputs["image"] = sizeOf->second;
                    size = Input { {}, Link { of.id, "out" } };
                } else {
                    add(node, node.id + "/screen", "Screen");
                    size = Input { {}, Link { node.id + "/screen", "extent" } };
                }
                // Times the scale — a number the same each way, or one for each — rounded up, and at least a pixel.
                Input scale { Value(1), std::nullopt };
                if (const auto given = node.inputs.find("scale"); given != node.inputs.end()) {
                    scale = given->second;
                    if (const auto* each = scale.value.Items(); each && each->size() == 2) scale.value = Value(Value::List { (*each)[0], (*each)[1], 1 });
                    else if (const double* n = scale.value.Number()) scale.value = Value(Value::List { *n, *n, 1 });
                }
                Node& scaled = add(node, node.id + "/scaled", "Multiply");
                scaled.inputs["a"] = size;
                scaled.inputs["b"] = scale;
                add(node, node.id + "/rounded", "Ceil").Wire("a", scaled.id);
                add(node, node.id + "/extent", "Max").Wire("a", node.id + "/rounded").Set("b", 1);
                extents[node.id] = node.id + "/extent";
                // As many mips as there are down to a pixel, for 0: one more than the log2 of its larger side, rounded down.
                if (const auto mips = node.inputs.find("mipLevels"); mips != node.inputs.end() && mips->second.value.Number() && *mips->second.value.Number() == 0) {
                    add(node, node.id + "/sides", "Split").Wire("vector", node.id + "/extent");
                    add(node, node.id + "/side", "Max").Wire("a", node.id + "/sides", "x").Wire("b", node.id + "/sides", "y");
                    add(node, node.id + "/halvings", "Log2").Wire("a", node.id + "/side");
                    add(node, node.id + "/whole halvings", "Floor").Wire("a", node.id + "/halvings");
                    add(node, node.id + "/mips", "Add").Wire("a", node.id + "/whole halvings").Set("b", 1);
                    node.Wire("mipLevels", node.id + "/mips");
                }
                for (const char* name : { "sizeOf", "scale" }) node.inputs.erase(name);
                node.type = "Image";
                node.Wire("extent", node.id + "/extent");
                if (!node.inputs.contains("format")) node.Set("format", "eRGBA16_SFLOAT");
            } else if (node.type == "History") {
                node.type = "Keep";
                node.pass = "@history";
                history = true;
            }
        }
        for (auto& [before, node] : made)
            graph.nodes.insert(std::ranges::find(graph.nodes, before, &Node::id), std::move(node));
        if (history && !std::ranges::contains(graph.passes, std::string("@history"))) graph.passes.insert(graph.passes.begin(), "@history");
        // What is wired from a RenderTarget's extent is wired from what works it out.
        for (Node& node : graph.nodes)
            for (auto& input : node.inputs | std::views::values)
                if (input.link && input.link->output == "extent")
                    if (const auto found = extents.find(input.link->node); found != extents.end()) input.link = Link { found->second, "out" };
    }

    /**
     * The loops of @p in that are in no other, written out: each node of one — of a loop in it too — a node a turn, and
     * what it takes from the Repeat that turn's numbers. A loop in a loop is a Repeat of it, written out a Repeat a turn
     * (`inner [2]`), whose own turns the next call writes out. False when there was no loop to write out.
     */
    static bool WriteLevel(const Graph& in, std::vector<Node>& written, Expansion& out, const std::map<std::string, int, std::less<>>& counts)
    {
        std::map<std::string, const Node*, std::less<>> repeats;
        for (const auto& node : in.nodes) if (node.type == "Repeat") repeats[node.id] = &node;
        if (repeats.empty()) {
            // No loop: each node as it is — but one that could not be written out, by what came before (a Bundle, a buffer).
            for (const auto& node : in.nodes) if (!out.errors.contains(node.id)) written.push_back(node);
            return false;
        }

        // Which loop each node is in, the innermost: the Repeat it is wired from, through whatever is between — a Repeat
        // too, which is then a loop in that one. A Previous is in the loop of what it is the turn before of; an Element is
        // in none for naming a node of one, which is its use.
        std::map<std::string, std::string, std::less<>> loopOf, parent;
        std::set<std::string, std::less<>> visiting;
        const auto within = [&](std::string inner, const std::string& outer) {
            while (!inner.empty()) {
                if (inner == outer) return true;
                const auto up = parent.find(inner);
                inner = up == parent.end() ? std::string() : up->second;
            }
            return false;
        };
        const std::function<std::string(const std::string&)> find = [&](const std::string& id) -> std::string {
            if (const auto known = loopOf.find(id); known != loopOf.end()) return known->second;
            const Node* node = in.Find(id);
            if (!node || !visiting.insert(id).second) return {};
            std::set<std::string> loops;
            const auto through = [&](const std::string& other) {
                if (repeats.contains(other)) loops.insert(other);
                else if (std::string loop = find(other); !loop.empty()) loops.insert(std::move(loop));
            };
            for (const auto& input : node->inputs | std::views::values)
                if (input.link) through(input.link->node);
            if (node->type == "Reference")
                if (const std::string* target = Text(*node, "node")) through(*target);
            if (node->type == "Previous")
                if (const std::string* of = Text(*node, "of")) through(*of);
            visiting.erase(id);
            // Of several, the one in all the others: what is wired from a loop in a loop is in both.
            std::string innermost;
            for (const auto& loop : loops) {
                if (innermost.empty() || within(loop, innermost)) innermost = loop;
                else if (!within(innermost, loop)) {
                    out.errors[id] = std::format("it is wired from two loops, '{}' and '{}', neither in the other: a node is in one, or in one in another", innermost, loop);
                    return loopOf[id] = std::string();
                }
            }
            // A Repeat wired from a loop is a loop in that one.
            if (node->type == "Repeat" && !innermost.empty()) parent[id] = innermost;
            return loopOf[id] = innermost;
        };
        for (const auto& node : in.nodes) (void)find(node.id);
        // The loop in no other that each node is of: what is written out now.
        const auto rootOf = [&](const std::string& id) {
            const auto known = loopOf.find(id);
            std::string loop = known == loopOf.end() ? std::string() : known->second;
            while (!loop.empty()) {
                const auto up = parent.find(loop);
                if (up == parent.end()) break;
                loop = up->second;
            }
            return loop;
        };

        // How many turns each of those has: given on it, or — wired — what that gives as the graph last ran, one until then.
        std::map<std::string, int, std::less<>> turnsOf;
        for (const auto& [id, node] : repeats) {
            if (parent.contains(id)) continue;
            const auto count = node->inputs.find("count");
            if (count != node->inputs.end() && count->second.link) {
                out.wiredCounts[id] = *count->second.link;
                const auto said = counts.find(id);
                turnsOf[id] = said == counts.end() ? 1 : said->second;
                if (turnsOf[id] < 0 || turnsOf[id] > 256) {
                    out.errors[id] = std::format("'count' is wired from what gives {}: a loop has from 0 to 256 turns", turnsOf[id]);
                    turnsOf[id] = 0;
                }
                continue;
            }
            const double* turns = count == node->inputs.end() ? nullptr : count->second.value.Number();
            if (!turns || *turns < 1 || *turns > 256 || std::floor(*turns) != *turns) {
                out.errors[id] = "'count' is how many turns it has: a whole number from 1 to 256, or wired from what gives one";
                turnsOf[id] = 0;
            } else {
                turnsOf[id] = static_cast<int>(*turns);
            }
        }

        std::vector<Node> made;
        for (const auto& node : in.nodes) {
            if (out.errors.contains(node.id)) continue;
            if (node.type == "Repeat" && !parent.contains(node.id)) continue;      // written out now: what is wired from it has its numbers
            const std::string root = rootOf(node.id);
            const bool innermost = !root.empty() && loopOf[node.id] == root;       // of this loop alone: its Previous, its Element, are worked out now
            const int count = root.empty() ? 1 : turnsOf[root];
            const auto ofTheLoop = [&](const std::string& id) {
                return !root.empty() && rootOf(id) == root && !(repeats.contains(id) && !parent.contains(id));
            };
            for (int turn = 0; turn < count; ++turn) {
                Node one = node;
                if (!root.empty()) {
                    one.id = TurnId(node.id, turn);
                    out.turns[node.id].push_back(one.id);
                }
                // What it is wired to of its loop is that turn's: the Repeat's numbers as they are on it, and the other
                // nodes of the loop — of a loop in it too — as they are on it.
                for (auto& [name, input] : one.inputs) {
                    if (!input.link) continue;
                    if (input.link->node == root) {
                        const std::string& output = input.link->output;
                        const Value value = output == "reverse" ? Value(count - 1 - turn) : output == "count" ? Value(count) : output == "first" ? Value(turn == 0)
                                          : output == "last" ? Value(turn == count - 1) : Value(turn);
                        input = Input { value, std::nullopt };
                    } else if (ofTheLoop(input.link->node)) {
                        input.link->node = TurnId(input.link->node, turn);
                    }
                }
                const auto stands = [&](std::string target) {
                    one.type = "Reference";
                    one.inputs.clear();
                    one.inputs["node"] = Input { Value(std::move(target)), std::nullopt };
                };
                const auto renamed = [&](const char* name) {
                    if (const std::string* named = Text(one, name); named && ofTheLoop(*named)) one.inputs[name] = Input { Value(TurnId(*named, turn)), std::nullopt };
                };
                if (node.type == "Reference") {
                    renamed("node");
                } else if (node.type == "Previous") {
                    const std::string* of = Text(node, "of");
                    const std::string* first = Text(node, "first");
                    if (!of || root.empty()) { out.errors[node.id] = "'of' is a node of a loop, by its id: this is what that node made the turn before"; break; }
                    if (!innermost) { renamed("of"); renamed("first"); }      // of a loop in this one: its own turns say which
                    else if (turn > 0) stands(TurnId(*of, turn - 1));
                    else if (first) stands(*first);
                    else { out.errors[node.id] = "'first' is the node it is on the first turn, when there is no turn before"; break; }
                } else if (node.type == "Element") {
                    const std::string* of = Text(node, "of");
                    // Of a call: of the node its output comes from — the first, or the one `output` names.
                    std::string ofCall;
                    if (of && !in.Find(*of))
                        if (const auto call = out.calls.find(*of); call != out.calls.end() && !call->second.outputs.empty()) {
                            const std::string* named = Text(node, "output");
                            const std::string& output = named ? *named : call->second.outputs.front();
                            const auto gives = call->second.gives.find(output);
                            if (gives == call->second.gives.end()) { out.errors[node.id] = std::format("'{}' calls a function with no output '{}'", *of, output); break; }
                            if (gives->second.output != "out") {
                                out.errors[node.id] = std::format("'{}' of '{}' is '{}' of what is in it: take the Element of a node of the loop whose 'out' it is", output, *of, gives->second.output);
                                break;
                            }
                            ofCall = gives->second.node;
                            of = &ofCall;
                        }
                    if (!of || !in.Find(*of)) { out.errors[node.id] = "'of' is a node of a loop, by its id: this is what that node made on one turn"; break; }
                    const std::string theirs = rootOf(*of);
                    if (theirs.empty()) {
                        // Of a node no loop has now — of none, or one written out a level before, by its turn: that one.
                        stands(*of);
                        made.push_back(std::move(one));
                        continue;
                    }
                    if (loopOf[*of] != theirs) {
                        // Of a loop in a loop: the outer's last turn now, and the inner's own `index` when that one is written out.
                        one.inputs["of"] = Input { Value(TurnId(*of, turnsOf[theirs] - 1)), std::nullopt };
                        made.push_back(std::move(one));
                        continue;
                    }
                    const int turns = turnsOf[theirs];
                    const auto index = one.inputs.find("index");
                    const double* which = index == one.inputs.end() ? nullptr : index->second.value.Number();
                    if (index != one.inputs.end() && (index->second.link || (!which && !index->second.value.IsNull()))) {
                        out.errors[node.id] = "'index' is a turn's number: given on the node, or wired straight from a Repeat";
                        break;
                    }
                    const int at = which ? static_cast<int>(std::llround(*which)) : turns - 1;
                    if (at < 0 || at >= turns) { out.errors[node.id] = std::format("'{}' has {} turns, from 0: there is no turn {}", theirs, turns, at); break; }
                    stands(TurnId(*of, at));
                }
                made.push_back(std::move(one));
            }
        }
        // A node that could not be written out takes its turns with it.
        for (const auto& id : out.errors | std::views::keys) {
            std::erase_if(made, [&](const Node& node) { return node.id == id || node.id.starts_with(id + " ["); });
            out.turns.erase(id);
        }
        // A loop's turns one after another, each with every node of the loop — where its first node is among the rest:
        // so that its passes, which with nothing they use to say run in the order of the nodes, run a turn at a time
        // (count, then scan, then scatter; and then the next turn's), not each node's turns together.
        std::map<std::string, std::size_t, std::less<>> at;
        for (std::size_t i = 0; i < made.size(); ++i) at.emplace(made[i].id, i);
        std::vector<bool> taken(made.size(), false);
        const auto take = [&](const std::string& id) {
            if (const auto it = at.find(id); it != at.end() && !taken[it->second]) { taken[it->second] = true; written.push_back(std::move(made[it->second])); }
        };
        std::set<std::string> done;
        for (const auto& node : in.nodes) {
            const std::string root = rootOf(node.id);
            if (root.empty()) { take(node.id); continue; }
            if (!done.insert(root).second) continue;
            for (int turn = 0; turn < turnsOf[root]; ++turn)
                for (const auto& member : in.nodes)
                    if (rootOf(member.id) == root) take(TurnId(member.id, turn));
        }
        for (std::size_t i = 0; i < made.size(); ++i) if (!taken[i]) written.push_back(std::move(made[i]));
        return true;
    }

    /** What a wire of @p graph comes from, past References. */
    static const Node* OriginOf(const Graph& graph, const std::string_view id)
    {
        const Node* node = graph.Find(id);
        for (int hops = 0; node && node->type == "Reference" && hops < 16; ++hops) {
            const auto named = node->inputs.find("node");
            node = named != node->inputs.end() && named->second.value.String() ? graph.Find(*named->second.value.String()) : nullptr;
        }
        return node;
    }

    std::string WhyNotBatched(const Graph& graph, const Node& draw, std::vector<std::string>* each)
    {
        // Whether what a wire carries is each element's: an element's output of a node that is entities an element at a
        // time, or what is worked out of one. What is of them all — how many, their groups — is not.
        std::map<std::string, bool, std::less<>> eachOf;
        std::function<bool(const Link&)> ofEach = [&](const Link& link) -> bool {
            const Node* node = OriginOf(graph, link.node);
            if (!node) return false;
            if (node->type == "Entities" || node->type == "Filter" || node->type == "ForEach")
                return !std::ranges::contains(std::array<std::string_view, 5> { "out", "entities", "count", "groups", "groupCount" }, link.output);
            // What gathers every element into one — a list of them, a buffer of them, their meshes packed — is of them all: the
            // lights' buffer a pipeline reads, a PointShadows' idle worked out of a Collect of the lights, a pack's layout.
            static constexpr std::array<std::string_view, 7> gathering { "Collect", "BufferBuilder", "ShaderBuffer", "MeshPack", "IndirectDraws", "Meshlets", "RayScene" };
            if (std::ranges::contains(gathering, node->type)) return false;
            if (const auto known = eachOf.find(node->id); known != eachOf.end()) return known->second;
            eachOf[node->id] = false;      // a loop of wires: not this way
            bool found = false;
            for (const auto& input : node->inputs | std::views::values)
                if (input.link && ofEach(*input.link)) { found = true; break; }
            return eachOf[node->id] = found;
        };
        const auto mesh = draw.inputs.find("mesh");
        const Node* source = mesh != draw.inputs.end() && mesh->second.link ? OriginOf(graph, mesh->second.link->node) : nullptr;
        if (!source || (source->type != "Filter" && source->type != "ForEach")) return "'mesh' is not each element's of a Filter or a ForEach";
        const std::string& output = mesh->second.link->output;
        const auto component = source->inputs.find("component");
        const bool ofMesh = component != source->inputs.end() && component->second.value.String() && *component->second.value.String() == "Mesh";
        if (output != "Mesh.mesh" && !(ofMesh && output == "field.mesh")) return std::format("'mesh' is '{}' of '{}', not its Mesh's", output, source->id);
        if (const auto command = draw.inputs.find("command"); command != draw.inputs.end() && command->second.value.String()
            && *command->second.value.String() != "eAuto" && *command->second.value.String() != "eDrawIndexed")
            return std::format("'command' is {}: a batch is drawn indexed and indirect", *command->second.value.String());
        std::vector<std::string> theirs;
        for (const auto& [name, input] : draw.inputs)
            if (name != "mesh" && input.link && ofEach(*input.link)) theirs.push_back(name);
        if (each) *each = theirs;
        if (!theirs.empty()) {
            std::string list;
            for (const auto& name : theirs) list += std::format("{}'{}'", list.empty() ? "" : ", ", name);
            return std::format("{} {} each element's — what each is to the shader is its [entity(\"PER_INSTANCE\")] buffer's when it is one draw",
                               list, theirs.size() == 1 ? "is" : "are");
        }
        if (source->type == "ForEach")
            for (const char* name : { "where", "select", "entityId", "groupBy" })
                if (source->inputs.contains(name)) return std::format("'{}' says '{}', which a batch of it does not: make it a Filter", source->id, name);
        return {};
    }

    static void WriteBatches(Expansion& out)
    {
        Graph& graph = out.graph;
        const auto origin = [&](const std::string& id) { return OriginOf(graph, id); };
        // The batch and the pack of each mesh source a batched Draw is of, by its wire (`node.output`): one for all of them.
        std::map<std::string, std::pair<std::string, std::string>, std::less<>> packs;
        std::vector<std::string> draws;
        for (const Node& node : graph.nodes)
            if (node.type == "Draw")
                if (const auto batched = node.inputs.find("batched"); batched != node.inputs.end() && batched->second.value.Bool() && *batched->second.value.Bool())
                    draws.push_back(node.id);
        for (const std::string& id : draws) {
            Node draw = *graph.Find(id);
            if (const std::string why = WhyNotBatched(graph, draw); !why.empty()) {
                out.unbatched[id] = std::format("batched: {}, so it draws one at a time", why);
                continue;
            }
            const auto mesh = draw.inputs.find("mesh");
            const Node* source = origin(mesh->second.link->node);
            const auto component = source->inputs.find("component");
            const bool ofMesh = component != source->inputs.end() && component->second.value.String() && *component->second.value.String() == "Mesh";

            // The same entities, grouped by mesh: of a Filter, those it keeps; of a ForEach, those with what it says. Draws of
            // the meshes of one — a shadow's cascades, a turn each — share the batch and the pack, each with commands of its own.
            std::vector<Node> made;
            const std::string of = mesh->second.link->node + "." + mesh->second.link->output;
            const bool sharing = packs.contains(of);
            if (!sharing) packs[of] = { id + "/batch", id + "/pack" };
            const std::string batch = packs[of].first, pack = packs[of].second, commands = id + "/commands";
            Node grouped;
            grouped.id = batch;
            grouped.type = "Filter";
            grouped.pass = draw.pass;
            Value::List with { "Mesh" };
            if (source->type == "Filter") {
                grouped.Wire("entities", mesh->second.link->node, "out");
            } else {
                Node all;
                all.id = id + "/all";
                all.type = "Entities";
                all.pass = draw.pass;
                made.push_back(std::move(all));
                grouped.Wire("entities", id + "/all", "out");
                if (!ofMesh && component != source->inputs.end()) with.push_back(component->second.value);
                if (const auto also = source->inputs.find("with"); also != source->inputs.end() && also->second.value.Items())
                    for (const auto& name : *also->second.value.Items()) if (name.String() && *name.String() != "Mesh") with.push_back(name);
                if (const auto without = source->inputs.find("without"); without != source->inputs.end()) grouped.inputs["without"] = without->second;
            }
            grouped.Set("with", Value(std::move(with))).Set("groupBy", "Mesh");
            Node packed;
            packed.id = pack;
            packed.type = "MeshPack";
            packed.pass = draw.pass;
            packed.Wire("meshes", batch, "Mesh.mesh");
            if (sharing) made.clear();      // the Entities a ForEach's batch is of, too: the first's
            else {
                made.push_back(std::move(grouped));
                made.push_back(std::move(packed));
            }
            Node commanded;
            commanded.id = commands;
            commanded.type = "IndirectDraws";
            commanded.pass = draw.pass;
            commanded.Wire("ranges", pack, "ranges").Wire("groups", batch, "groups");
            // Given `cull`: what is out of its sight left out on the GPU — CullInSight's passes, its commands starting with
            // no instances each frame, and what its shader reads compacted, the elements in sight one after another as they
            // are drawn, so that it reads its instance at first + index as it does unculled. Of several matrices one after
            // another — a ShadowCascades' — the `cullIndex`th, sixteen numbers from sixteen times it (`<draw>/sight`).
            const auto cull = draw.inputs.find("cull");
            const bool sighted = cull != draw.inputs.end() && cull->second.link;
            if (sighted) commanded.Set("mode", "eGPU");
            made.push_back(std::move(commanded));
            if (sighted) {
                const std::string groups = id + "/cull groups", sizer = id + "/cull sizer", culler = id + "/culler";
                const auto add = [&](const std::string& made_, const char* type) -> Node& {
                    Node node;
                    node.id = made_;
                    node.type = type;
                    node.pass = draw.pass;
                    made.push_back(std::move(node));
                    return made.back();
                };
                add(groups, "Buffer").Set("instanceCount", 12).Set("usage", Value::List { "eStorage", "eIndirect", "eTransferSrc", "eTransferDst" }).Set("type", "eDeviceLocal");
                add(sizer, "ComputePipeline").Set("compute.module", "koral_cull").Set("compute.entry", "size").Wire("param.visible", commands, "visible").Wire("param.args", groups);
                add(id + "/cull size", "Dispatch").Wire("pipeline", sizer).Set("groups", Value::List { 1, 1, 1 });
                add(culler, "ComputePipeline").Set("compute.module", "koral_cull").Set("compute.entry", "cull").Wire("param.instances", id, "instances")
                    .Wire("param.ranges", pack, "rangeBuffer").Wire("param.commands", commands, "commands").Wire("param.visible", commands, "visible")
                    .Wire("param.drawn", id, "drawnInstances");
                Node& culls = add(id + "/cull", "Dispatch").Wire("pipeline", culler).Wire("indirect", groups).Wire("push.drawCount", commands, "drawCount")
                    .Wire("push.stride", id, "instanceStride").Wire("push.matrixOffset", id, "instanceMatrixOffset");
                if (const auto index = draw.inputs.find("cullIndex"); index != draw.inputs.end()) {
                    // The `cullIndex`th of several: given, sixteen times it; wired, worked out of what it is wired from.
                    Node& sight = add(id + "/sight", "Slice");
                    sight.inputs["of"] = cull->second;
                    sight.Set("count", 16);
                    if (index->second.link) {
                        add(id + "/sight first", "Multiply").Set("b", 16).inputs["a"] = index->second;
                        Node& slice = *std::ranges::find(made, id + "/sight", &Node::id);
                        slice.Wire("first", id + "/sight first");
                    } else {
                        sight.Set("first", 16.0 * (index->second.value.Number() ? *index->second.value.Number() : 0.0));
                    }
                    std::ranges::find(made, id + "/cull", &Node::id)->Wire("push.viewProjection", id + "/sight");
                } else {
                    culls.inputs["push.viewProjection"] = cull->second;
                }
            }

            Node& written = *graph.Find(id);
            written.Wire("mesh", pack, "mesh").Set("command", "eDrawIndexedIndirect").Wire("indirect", commands, "commands").Wire("drawCount", commands, "drawCount");
            if (sighted) written.Set("compacted", true);
            // Left out of sight a draw at a time, on the CPU: no more of a batch.
            written.inputs.erase("cull");
            const auto at = std::ranges::find(graph.nodes, id, &Node::id);
            graph.nodes.insert(at, std::make_move_iterator(made.begin()), std::make_move_iterator(made.end()));
            out.batched.insert(id);
        }
    }

    Expansion Expand(const Graph& given, const ElementOf& elementOf, const FieldsOf& fieldsOf, const std::map<std::string, int, std::less<>>& counts)
    {
        Expansion out;
        // The calls first: what a function holds may be a loop, and a call may be in one. Then the Bundles, which a
        // function may give or take.
        Graph written = WriteCalls(given, out);
        WriteBundles(written, out, elementOf);
        WriteComponentBuffers(written, out, fieldsOf);
        WriteShorthands(written, out);
        out.graph.passes = written.passes;
        out.graph.setup = written.setup;
        out.graph.fixed = written.fixed;
        out.graph.after = written.after;
        out.graph.orderSettled = written.orderSettled;
        out.graph.fixedStepsAtMost = written.fixedStepsAtMost;
        out.graph.meshImports = written.meshImports;
        out.graph.textureImports = written.textureImports;
        out.graph.texturePacks = written.texturePacks;
        out.graph.defaultMeshTangents = written.defaultMeshTangents;

        // The loops, a level at a time: those in no other first — a loop in one written out a Repeat a turn — then those.
        Graph level;
        level.nodes = std::move(written.nodes);
        for (int depth = 0; depth < 8; ++depth) {
            std::vector<Node> next;
            const bool any = WriteLevel(level, next, out, counts);
            level.nodes = std::move(next);
            if (!any) break;
        }
        out.graph.nodes = std::move(level.nodes);
        // A node of a loop in a loop is, by its id, each turn of the innermost one: `x` is `x [0] [0]`, `x [0] [1]`...
        {
            const std::function<void(const std::string&, std::vector<std::string>&, int)> leaves = [&](const std::string& id, std::vector<std::string>& into, const int depth) {
                const auto it = out.turns.find(id);
                if (it == out.turns.end() || depth > 8) { into.push_back(id); return; }
                for (const auto& turn : it->second) leaves(turn, into, depth + 1);
            };
            std::map<std::string, std::vector<std::string>, std::less<>> flat;
            for (const auto& [id, list] : out.turns) {
                std::vector<std::string> all;
                for (const auto& turn : list) leaves(turn, all, 0);
                flat[id] = std::move(all);
            }
            out.turns = std::move(flat);
        }
        WriteBatches(out);
        return out;
    }
}
