// keditor: the editor's session — its assets, its scripts, what it says.

#include "editor_internal.h"

namespace keditor
{

    // What is done is counted by the document; a state is kept once the doing stops — the mouse let go, nothing changed
    // for a moment — so that a drag, or a number dragged through fifty values, is one thing to take back.
    void Session::TrackUndo(const float seconds, const bool busy)
    {
        Document& doc = Doc();
        if (&doc != _undoOf) {
            _undoOf = &doc;
            _undo.clear();
            _redo.clear();
            _now = { doc.Graph().ToJson(), doc.WorldJson() };
            _undoRevision = doc.Revision();
            _undoPending = false;
            return;
        }
        // Playing changes the scene and edits nothing; files being read make entities as they arrive, which is no edit
        // either. But nothing a file brings changes the graph: what does, meanwhile, is an edit — one that reads a file
        // again itself, how a model's meshes are read — and is kept once the files are read.
        if (InPlayMode()) { _undoRevision = doc.Revision(); return; }
        if (doc.Reading() > 0) {
            if (doc.Revision() != _undoRevision && doc.Graph().ToJson() != _now.graph) _undoPending = true;
            _undoRevision = doc.Revision();
            return;
        }
        if (doc.Revision() != _undoRevision) {
            _undoRevision = doc.Revision();
            _undoPending = true;
            _undoQuiet = 0.f;
            return;
        }
        if (!_undoPending || busy || (_undoQuiet += seconds) < 0.3f) return;
        Commit();
    }

    void Session::Commit()
    {
        if (!_undoPending) return;
        _undoPending = false;
        Was is { Doc().Graph().ToJson(), Doc().WorldJson() };
        if (is.graph == _now.graph && is.world == _now.world) return;
        _undo.push_back(std::move(_now));
        if (_undo.size() > 64) _undo.erase(_undo.begin());
        _redo.clear();
        _now = std::move(is);
        ++_changes;
    }

    void Session::PutBack(const Was& state)
    {
        Document& doc = Doc();
        const std::string picked = doc.World().Alive(selectedEntity) ? doc.NameOf(selectedEntity) : std::string();
        std::string error;
        if (state.graph != _now.graph) {
            kgraph::Graph graph;
            if (kgraph::Graph::FromJson(state.graph, graph, error)) doc.SetGraph(std::move(graph));
        }
        if (state.world != _now.world && !doc.RestoreWorld(state.world, error)) Say(error, true);
        _now = state;
        _undoRevision = doc.Revision();
        _undoPending = false;
        // What was picked is found again by its name: the entities are made anew.
        selectedEntity = picked.empty() ? kecs::NullEntity : doc.World().FindEntity(picked);
        if (!doc.Graph().Find(selectedNode)) selectedNode.clear();
        _folded.clear();
        ++_changes;
    }

    bool Session::Undo()
    {
        if (InPlayMode()) { Say("Stop playing to take something back: what a scene does as it plays is not an edit.", true); return false; }
        Commit();
        if (_undo.empty()) { Say("Nothing to take back."); return false; }
        Was before = std::move(_undo.back());
        _undo.pop_back();
        _redo.push_back(_now);
        PutBack(before);
        Say("Taken back.");
        return true;
    }

    bool Session::Redo()
    {
        if (InPlayMode() || _undoPending || _redo.empty()) { Say("Nothing to do again."); return false; }
        Was after = std::move(_redo.back());
        _redo.pop_back();
        _undo.push_back(_now);
        PutBack(after);
        Say("Done again.");
        return true;
    }

    std::vector<Session::Choice> Session::Choices() const
    {
        std::vector<Choice> choices;
        const kgraph::Graph& graph = _play.Doc().Graph();
        for (const auto& node : graph.nodes) {
            if (node.type != "Switch") continue;
            const auto index = node.inputs.find("index");
            const kgraph::Node* driver = index != node.inputs.end() && index->second.link ? graph.Find(index->second.link->node) : nullptr;
            // Brought in from another page: the node it stands for says which.
            for (int hops = 0; driver && driver->type == "Reference" && hops < 16; ++hops) {
                const auto target = driver->inputs.find("node");
                driver = target != driver->inputs.end() && target->second.value.String() ? graph.Find(*target->second.value.String()) : nullptr;
            }
            if (!driver || (driver->type != "Int" && driver->type != "UInt" && driver->type != "Bool")) continue;
            if (std::ranges::find(choices, driver->id, &Choice::driver) != choices.end()) continue;
            Choice choice;
            choice.driver = driver->id;
            for (int option = 0; option < 16; ++option) {
                const auto wired = node.inputs.find(std::format("option[{}]", option));
                if (wired == node.inputs.end() || !wired->second.link) break;
                // What a pass brought in is called what it stands for.
                const kgraph::Node* shown = graph.Find(wired->second.link->node);
                const auto target = shown && shown->type == "Reference" ? shown->inputs.find("node") : std::map<std::string, kgraph::Input>::const_iterator {};
                choice.options.push_back(shown && shown->type == "Reference" && target != shown->inputs.end() && target->second.value.String() ? *target->second.value.String() : wired->second.link->node);
            }
            if (choice.options.size() < 2) continue;
            const auto value = driver->inputs.find("value");
            if (value != driver->inputs.end()) choice.chosen = value->second.value.Bool() ? (*value->second.value.Bool() ? 1 : 0) : value->second.value.Number() ? static_cast<int>(*value->second.value.Number()) : 0;
            choices.push_back(std::move(choice));
        }
        return choices;
    }

    void Session::Choose(const std::string& driver, const int option)
    {
        const kgraph::Node* node = Doc().Graph().Find(driver);
        if (!node) return;
        Doc().SetInput(driver, "value", node->type == "Bool" ? Value(option != 0) : Value(option));
    }

    Session::PassFlow Session::Flow() const
    {
        PassFlow flow;
        const kgraph::Graph& graph = _play.Doc().Graph();
        // What a node stands for: a Reference is the node it brings in.
        const auto origin = [&](std::string id) {
            for (int hops = 0; hops < 16; ++hops) {
                const kgraph::Node* node = graph.Find(id);
                if (!node || node->type != "Reference") break;
                const auto target = node->inputs.find("node");
                if (target == node->inputs.end() || !target->second.value.String()) break;
                id = *target->second.value.String();
            }
            return id;
        };
        // The Switches that choose between branches are steps of their own; every other node is of its page.
        std::set<std::string> switches;
        for (const auto& choice : Choices())
            for (const auto& node : graph.nodes)
                if (node.type == "Switch")
                    if (const auto index = node.inputs.find("index"); index != node.inputs.end() && index->second.link && origin(index->second.link->node) == choice.driver)
                        switches.insert(node.id);
        const auto stepOf = [&](const std::string& id) -> std::string {
            if (switches.contains(id)) return id;
            const kgraph::Node* node = graph.Find(id);
            return node ? node->pass : std::string();
        };
        const auto at = [&](const std::string& page) { return static_cast<int>(std::ranges::find(graph.passes, page) - graph.passes.begin()); };
        // Whose work a page reading @p source (its @p output) has: the last page before it to write it — not the one it
        // was made on, when another drew into it since. When none before it did, the page it is made on, if that is
        // before it too; else nobody's: what it reads is as the frame before left it, or a node set down on a later page
        // that nothing has worked on yet. The flow is of what comes before, so that it has no way round.
        const auto producer = [&](const std::string& source, const std::string& output, const std::string& readerPage) {
            std::string from = stepOf(source);
            if (switches.contains(source)) return from;
            const int reader = at(readerPage);
            int latest = -1;
            for (const auto& use : _play.Doc().Runtime().UsesOf(source, output))
                if (use.write && at(use.pass) < reader && at(use.pass) > latest) { latest = at(use.pass); from = use.pass; }
            if (latest < 0 && at(from) >= reader) return std::string();
            return from;
        };
        for (const auto& page : graph.passes) {
            // A page of nothing but what it brings in (one that holds the Switches, which are steps of their own) is no step.
            if (std::ranges::none_of(graph.nodes, [&](const kgraph::Node& node) { return node.pass == page && node.type != "Reference" && node.type != "Note" && !switches.contains(node.id); }))
                continue;
            PassFlow::Step step;
            step.id = page;
            step.order = static_cast<int>(std::ranges::find(graph.passes, page) - graph.passes.begin());
            step.rate = std::string(kgraph::PassRateName(graph.RateOf(page)));
            step.nodes = static_cast<std::size_t>(std::ranges::count(graph.nodes, page, &kgraph::Node::pass));
            // A page of nothing but values and resources has no pass to leave out: it is as good as run.
            step.ran = std::ranges::none_of(graph.nodes, [&](const kgraph::Node& node) {
                const kgraph::NodeType* type = node.pass == page ? kgraph::FindType(node.type) : nullptr;
                return type && type->category == "Passes";
            });
            // Which branch it is of, as its passes say: each idles unless a choice is one option (or is on only then).
            const auto only = [&](const std::string& wired, const bool idling, auto&& self, int depth) -> std::optional<std::pair<std::string, int>> {
                const kgraph::Node* by = graph.Find(origin(wired));
                if (!by || depth > 4) return std::nullopt;
                if (by->type == "Max" || by->type == "Or")      // idle for this or that: whichever names a choice
                    for (const char* side : { "a", "b" })
                        if (const auto it = by->inputs.find(side); it != by->inputs.end() && it->second.link)
                            if (auto found = self(it->second.link->node, idling, self, depth + 1)) return found;
                if (by->type != (idling ? "NotEqual" : "Equal")) return std::nullopt;
                const auto a = by->inputs.find("a"), b = by->inputs.find("b");
                if (a == by->inputs.end() || !a->second.link || b == by->inputs.end() || !b->second.value.Number()) return std::nullopt;
                return std::pair { origin(a->second.link->node), static_cast<int>(*b->second.value.Number()) };
            };
            for (const auto& node : graph.nodes) {
                if (node.pass != page) continue;
                for (const auto& [input, idling] : { std::pair { "idle", true }, std::pair { "enabled", false } })
                    if (const auto it = node.inputs.find(input); it != node.inputs.end() && it->second.link)
                        if (const auto found = only(it->second.link->node, idling, only, 0)) { step.branchDriver = found->first; step.branchOption = found->second; }
                if (step.branchOption >= 0) break;
            }
            flow.steps.push_back(std::move(step));
        }
        for (const auto& id : switches) {
            PassFlow::Step step;
            step.id = id;
            step.isSwitch = true;
            const kgraph::Node* node = graph.Find(id);
            step.order = static_cast<int>(std::ranges::find(graph.passes, node->pass) - graph.passes.begin());
            step.driver = origin(node->inputs.at("index").link->node);
            for (int option = 0; option < 16; ++option) {
                const auto wired = node->inputs.find(std::format("option[{}]", option));
                if (wired == node->inputs.end() || !wired->second.link) break;
                step.options.push_back(producer(origin(wired->second.link->node), wired->second.link->output, node->pass));
            }
            // A Switch chooses once what it chooses between is there: just after the last of its options' pages, wherever
            // the node itself was set down — a page after one that reads what it chooses, as often as not.
            int last = -1;
            for (const auto& option : step.options) if (!option.empty()) last = std::max(last, at(option));
            if (last >= 0) step.order = last;
            if (const kgraph::Node* driver = graph.Find(step.driver))
                if (const auto value = driver->inputs.find("value"); value != driver->inputs.end())
                    step.chosen = value->second.value.Bool() ? (*value->second.value.Bool() ? 1 : 0) : value->second.value.Number() ? static_cast<int>(*value->second.value.Number()) : 0;
            flow.steps.push_back(std::move(step));
        }
        // A link for each step a step takes something from, saying what.
        std::map<std::tuple<std::string, std::string, int>, std::set<std::string>> carried;
        for (const auto& node : graph.nodes) {
            const std::string to = stepOf(node.id);
            if (to.empty()) continue;
            for (const auto& [input, given] : node.inputs) {
                if (!given.link) continue;
                if (switches.contains(node.id) && input == "index") continue;     // which: the choice, not something used
                const std::string source = origin(given.link->node);
                const std::string from = producer(source, given.link->output, node.pass);
                if (from.empty() || from == to) continue;
                int option = -1;
                if (switches.contains(node.id) && input.starts_with("option[")) option = std::stoi(input.substr(7));
                carried[{ from, to, option }].insert(source);
            }
        }
        // Only from what comes before: a page by its place, a Switch just after the last page it chooses from. What is
        // left is a flow with no way round, whatever else the graph wires.
        const auto rank = [&](const std::string& id) {
            const auto step = std::ranges::find(flow.steps, id, &PassFlow::Step::id);
            if (step == flow.steps.end()) return static_cast<double>(at(id));
            return static_cast<double>(step->order) + (step->isSwitch ? 0.5 : 0.0);
        };
        for (auto& [key, sources] : carried)
            if (rank(std::get<0>(key)) < rank(std::get<1>(key)))
                flow.links.push_back({ std::get<0>(key), std::get<1>(key), std::vector<std::string>(sources.begin(), sources.end()), std::get<2>(key) });
        // What ran in the last frame, and what it cost: a pass of the frame is a node of the graph — a call's by its call,
        // a loop's turn by its node.
        for (const auto& pass : _play.Doc().Runtime().Frame().passes) {
            std::string id = pass.node;
            if (const auto slash = id.find('/'); slash != std::string::npos) id.resize(slash);
            if (const auto turn = id.find(" ["); turn != std::string::npos) id.resize(turn);
            const kgraph::Node* node = graph.Find(id);
            if (!node) continue;
            // One that idles (a branch not chosen) is there, and does nothing.
            const kgraph::Value idle = _play.Doc().Runtime().ValueOf(pass.node, "idle");
            if (idle.Bool() ? *idle.Bool() : idle.Number() && *idle.Number() != 0.0) continue;
            if (const auto step = std::ranges::find(flow.steps, node->pass, &PassFlow::Step::id); step != flow.steps.end()) {
                step->ran = true;
                step->gpuMs += pass.gpuMs;
            }
        }
        return flow;
    }

    std::set<std::string> Session::PagesNotChosen() const
    {
        const kgraph::Graph& graph = _play.Doc().Graph();
        std::map<std::string, int> chosen;
        for (const auto& choice : Choices()) chosen[choice.driver] = choice.chosen;
        std::set<std::string> hidden;
        if (chosen.empty()) return hidden;
        // A page of passes: left out when every one of them is off by a choice.
        std::set<std::string> passless;
        for (const auto& page : graph.passes) {
            bool any = false, on = false;
            for (const auto& node : graph.nodes) {
                if (node.pass != page || !IsPass(node)) continue;
                any = true;
                if (!OffByChoice(graph, node, chosen).value_or(false)) { on = true; break; }
            }
            if (!any) passless.insert(page);
            else if (!on) hidden.insert(page);
        }
        // A page of no passes: left out when what uses it is all left out — the scene the rays are traced through, when
        // they are not — till nothing more is.
        std::map<std::string, std::set<std::string>> usedBy;
        for (const auto& node : graph.nodes)
            for (const auto& input : node.inputs | std::views::values)
                if (input.link)
                    if (const kgraph::Node* from = graph.Find(OriginOf(graph, input.link->node)); from && from->pass != node.pass && !node.pass.empty())
                        usedBy[from->pass].insert(node.pass);
        for (bool more = true; more;) {
            more = false;
            for (const auto& page : passless) {
                if (hidden.contains(page)) continue;
                const auto users = usedBy.find(page);
                if (users == usedBy.end() || users->second.empty()) continue;
                if (std::ranges::all_of(users->second, [&](const std::string& user) { return hidden.contains(user); })) { hidden.insert(page); more = true; }
            }
        }
        return hidden;
    }

    bool Session::MaterialInUse(const std::string& component)
    {
        const auto pipelines = PipelineComponents();
        const auto of = pipelines.find(component);
        if (of == pipelines.end()) return true;
        const kgraph::Graph& graph = _play.Doc().Graph();
        std::map<std::string, int> chosen;
        for (const auto& choice : Choices()) chosen[choice.driver] = choice.chosen;
        if (chosen.empty()) return true;
        const std::string& pipeline = of->second;
        const auto pipelineOf = [&](const kgraph::Node& pass) {
            const auto it = pass.inputs.find("pipeline");
            return it != pass.inputs.end() && it->second.link ? OriginOf(graph, it->second.link->node) : std::string();
        };
        // The Draws of the pipeline, whose buffers of instances hold the component's fields: what reads one reads it.
        std::set<std::string> draws;
        for (const auto& node : graph.nodes)
            if (node.type == "Draw" && pipelineOf(node) == pipeline) draws.insert(node.id);
        const auto readsInstances = [&](const kgraph::Node& node) {
            return std::ranges::any_of(node.inputs | std::views::values, [&](const kgraph::Input& input) {
                return input.link && input.link->output == "instances" && draws.contains(OriginOf(graph, input.link->node));
            });
        };
        std::set<std::string> readers { pipeline };
        for (const auto& node : graph.nodes)
            if (node.type.ends_with("Pipeline") && readsInstances(node)) readers.insert(node.id);
        bool used = false;
        for (const auto& node : graph.nodes) {
            if (!IsPass(node) || !(readers.contains(pipelineOf(node)) || readsInstances(node))) continue;
            used = true;
            if (!OffByChoice(graph, node, chosen).value_or(false)) return true;
        }
        return !used;
    }

    Session::FlowLayout Session::LayOut(const PassFlow& flow, const std::map<std::string, std::pair<float, float>>& sizes)
    {
        FlowLayout layout;
        const std::size_t count = flow.steps.size();
        if (count == 0) return layout;
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < count; ++i) index[flow.steps[i].id] = i;
        std::vector<std::set<std::size_t>> from(count), to(count);
        for (const auto& link : flow.links) {
            const auto a = index.find(link.from), b = index.find(link.to);
            if (a == index.end() || b == index.end() || a->second == b->second) continue;
            // What a page reads of one that runs after it is what that one left the frame before (a history): drawn, and
            // no say in where either goes. A Switch counts as running with the page it is on, after what it chooses from.
            const auto& one = flow.steps[a->second];
            const auto& other = flow.steps[b->second];
            const bool forward = one.order < other.order || (one.order == other.order && !one.isSwitch && other.isSwitch);
            if (!forward) continue;
            to[a->second].insert(b->second);
            from[b->second].insert(a->second);
        }
        const auto sizeOf = [&](const std::size_t i) {
            const auto it = sizes.find(flow.steps[i].id);
            if (it != sizes.end() && it->second.first > 1.f) return it->second;
            return flow.steps[i].isSwitch ? std::pair { 220.f, 150.f } : std::pair { 200.f, 90.f };
        };

        // ---- columns: as early as what it takes from lets it, then where its wires are shortest -----------------------
        std::vector<int> column(count, 0);
        for (std::size_t round = 0; round < count; ++round) {
            bool moved = false;
            for (std::size_t i = 0; i < count; ++i)
                for (const std::size_t j : from[i])
                    if (column[i] < column[j] + 1 && column[j] + 1 < static_cast<int>(count)) { column[i] = column[j] + 1; moved = true; }
            if (!moved) break;
        }
        for (int round = 0; round < 6; ++round)
            for (std::size_t k = 0; k < count; ++k) {
                const std::size_t i = round % 2 == 0 ? count - 1 - k : k;
                if (from[i].empty() || to[i].empty()) continue;    // the first and the last stay where they are
                int low = 0, high = std::numeric_limits<int>::max();
                // Each wire's pull, and how much it counts: one from a step that feeds many (the frame's values) is
                // long wherever this goes, so it counts the less for it.
                std::vector<std::pair<int, float>> near;
                for (const std::size_t j : from[i]) { low = std::max(low, column[j] + 1); near.push_back({ column[j] + 1, 1.f / static_cast<float>(to[j].size()) }); }
                for (const std::size_t j : to[i]) { high = std::min(high, column[j] - 1); near.push_back({ column[j] - 1, 1.f }); }
                if (low > high) continue;
                std::ranges::sort(near);
                float total = 0.f, reached = 0.f;
                for (const auto& [at, weight] : near) total += weight;
                int median = near.front().first;
                for (const auto& [at, weight] : near) { reached += weight; median = at; if (reached >= total * 0.5f) break; }
                column[i] = std::clamp(median, low, high);      // the weighted median: the least wire, weighed and summed
            }

        // ---- branches: a choice's options, each a band — the pages that run only for it, as their passes say, and what
        // only it is reached from ---------------------------------------------------------------------------------------
        const auto ancestors = [&](const std::size_t start) {
            std::set<std::size_t> seen { start };
            std::vector<std::size_t> todo { start };
            while (!todo.empty()) {
                const std::size_t i = todo.back();
                todo.pop_back();
                for (const std::size_t j : from[i]) if (seen.insert(j).second) todo.push_back(j);
            }
            return seen;
        };
        std::vector<int> band(count, 0);       // 0: shared; then a branch each, from 1
        std::map<std::pair<std::string, int>, int> bandOf;
        const auto bandFor = [&](const std::string& driver, const int option, const std::string& last) {
            const auto key = std::pair { driver, option };
            if (const auto it = bandOf.find(key); it != bandOf.end()) return it->second;
            FlowLayout::Branch branch;
            branch.option = option;
            branch.title = std::format("{} = {}", driver, option);
            (void)last;
            layout.branches.push_back(std::move(branch));
            return bandOf[key] = static_cast<int>(layout.branches.size());
        };
        for (const auto& step : flow.steps)
            if (step.isSwitch)
                for (std::size_t k = 0; k < step.options.size(); ++k) {
                    const int b = bandFor(step.driver.empty() ? step.id : step.driver, static_cast<int>(k), step.options[k]);
                    layout.branches[static_cast<std::size_t>(b - 1)].switchId = step.id;
                }
        // What the graph says of a page: it runs only for this option.
        for (std::size_t i = 0; i < count; ++i)
            if (flow.steps[i].branchOption >= 0) band[i] = bandFor(flow.steps[i].branchDriver, flow.steps[i].branchOption, {});
        // What else only one option is reached from.
        for (const auto& step : flow.steps) {
            if (!step.isSwitch) continue;
            std::vector<std::set<std::size_t>> reached;
            for (const auto& option : step.options) reached.push_back(index.contains(option) ? ancestors(index.at(option)) : std::set<std::size_t>());
            for (std::size_t k = 0; k < reached.size(); ++k)
                for (const std::size_t i : reached[k]) {
                    if (band[i] != 0) continue;
                    bool only = true;
                    for (std::size_t other = 0; other < reached.size() && only; ++other) if (other != k && reached[other].contains(i)) only = false;
                    if (only) band[i] = bandFor(step.driver.empty() ? step.id : step.driver, static_cast<int>(k), step.options[k]);
                }
        }
        for (std::size_t i = 0; i < count; ++i)
            if (band[i] != 0) layout.branches[static_cast<std::size_t>(band[i] - 1)].steps.push_back(flow.steps[i].id);
        std::erase_if(layout.branches, [](const FlowLayout::Branch& branch) { return branch.steps.empty(); });
        // (The bands numbered again, as the branches are now.)
        {
            std::map<int, int> renumber;
            int next = 0;
            std::vector<std::string> kept;
            for (const auto& branch : layout.branches) kept.push_back(branch.steps.front());
            for (std::size_t i = 0; i < count; ++i)
                if (band[i] != 0 && !renumber.contains(band[i]))
                    for (std::size_t b = 0; b < layout.branches.size(); ++b)
                        if (std::ranges::find(layout.branches[b].steps, flow.steps[i].id) != layout.branches[b].steps.end()) { renumber[band[i]] = static_cast<int>(b) + 1; break; }
            for (auto& each : band) if (each != 0) each = renumber[each];
            (void)next;
        }

        // ---- heights: each at those it is wired to, its column's kept in order and apart --------------------------------
        int columns = 0;
        for (const int c : column) columns = std::max(columns, c + 1);
        std::vector<std::vector<std::size_t>> stacked(static_cast<std::size_t>(columns));
        for (std::size_t i = 0; i < count; ++i) stacked[static_cast<std::size_t>(column[i])].push_back(i);
        std::vector<float> y(count, 0.f);
        constexpr float Gap = 28.f, BandGap = 96.f;
        // Each column's steps put as near as they can be to where they would be, in order, none over the next: the
        // nearest such heights, summed over the squares (the pool-adjacent-violators way).
        const auto settle = [&](std::vector<std::size_t>& steps, const std::vector<float>& wanted) {
            std::vector<float> offset(steps.size(), 0.f);
            for (std::size_t k = 1; k < steps.size(); ++k)
                offset[k] = offset[k - 1] + sizeOf(steps[k - 1]).second + (band[steps[k]] != band[steps[k - 1]] ? BandGap : Gap);
            struct Pool { float sum; int n; std::size_t first; };
            std::vector<Pool> pools;
            for (std::size_t k = 0; k < steps.size(); ++k) {
                pools.push_back({ wanted[k] - offset[k], 1, k });
                while (pools.size() > 1 && pools[pools.size() - 2].sum / pools[pools.size() - 2].n > pools.back().sum / pools.back().n) {
                    pools[pools.size() - 2].sum += pools.back().sum;
                    pools[pools.size() - 2].n += pools.back().n;
                    pools.pop_back();
                }
            }
            for (std::size_t p = 0; p < pools.size(); ++p) {
                const std::size_t end = p + 1 < pools.size() ? pools[p + 1].first : steps.size();
                for (std::size_t k = pools[p].first; k < end; ++k) y[steps[k]] = pools[p].sum / pools[p].n + offset[k];
            }
        };
        // To begin with: shared first, then each branch in its order, each in the order of the flow.
        for (auto& steps : stacked) {
            std::ranges::stable_sort(steps, {}, [&](const std::size_t i) { return band[i]; });
            std::vector<float> wanted(steps.size(), 0.f);
            settle(steps, wanted);
        }
        const auto middle = [&](const std::size_t i) { return y[i] + sizeOf(i).second * 0.5f; };
        for (int round = 0; round < 12; ++round)
            for (int c = 0; c < columns; ++c) {
                auto& steps = stacked[static_cast<std::size_t>(round % 2 == 0 ? c : columns - 1 - c)];
                // Where each would be: level with the middle of those it is wired to — its branch's own counting double,
                // so that a branch keeps together rather than reaching for what it shares.
                std::vector<std::pair<float, std::size_t>> wanted;
                for (const std::size_t i : steps) {
                    float sum = 0.f, weight = 0.f;
                    for (const auto* near : { &from[i], &to[i] })
                        for (const std::size_t j : *near) {
                            const float w = band[j] == band[i] ? 2.f : 1.f;
                            sum += middle(j) * w;
                            weight += w;
                        }
                    wanted.push_back({ weight > 0.f ? sum / weight - sizeOf(i).second * 0.5f : y[i], i });
                }
                // In order of their bands, and within one of where they would be.
                std::ranges::stable_sort(wanted, [&](const auto& a, const auto& b) {
                    return band[a.second] != band[b.second] ? band[a.second] < band[b.second] : a.first < b.first;
                });
                std::vector<float> heights;
                for (std::size_t k = 0; k < wanted.size(); ++k) { steps[k] = wanted[k].second; heights.push_back(wanted[k].first); }
                settle(steps, heights);
            }

        // ---- the branches kept apart: each a block of heights none of another has, in their order, moved as little as can be.
        // What is shared, in the columns the branches are in, is a block of its own before them: out of their frames.
        const int branchBands = static_cast<int>(layout.branches.size());
        std::vector<bool> amongBranches(static_cast<std::size_t>(columns), false);
        for (std::size_t i = 0; i < count; ++i) if (band[i] != 0) amongBranches[static_cast<std::size_t>(column[i])] = true;
        const auto blockOf = [&](const std::size_t i) { return band[i] != 0 ? band[i] : amongBranches[static_cast<std::size_t>(column[i])] ? 0 : -1; };
        const int bands = branchBands + 1;      // block 0: what is shared among the branches
        if (branchBands > 1) {
            std::vector<float> tops(static_cast<std::size_t>(bands), std::numeric_limits<float>::max()), bottoms(static_cast<std::size_t>(bands), std::numeric_limits<float>::lowest());
            for (std::size_t i = 0; i < count; ++i) {
                if (blockOf(i) < 0) continue;
                const auto b = static_cast<std::size_t>(blockOf(i));
                tops[b] = std::min(tops[b], y[i]);
                bottoms[b] = std::max(bottoms[b], y[i] + sizeOf(i).second);
            }
            // Where each block's top goes: as near where it is as it can be, each below the one before it.
            struct Pool { float sum; int n; std::size_t first; };
            // (A block with nothing in it takes no room.)
            for (std::size_t b = 0; b < tops.size(); ++b) if (tops[b] > bottoms[b]) { tops[b] = 0.f; bottoms[b] = -BandGap; }
            std::vector<float> offset(static_cast<std::size_t>(bands), 0.f);
            for (std::size_t b = 1; b < offset.size(); ++b) offset[b] = offset[b - 1] + (bottoms[b - 1] - tops[b - 1]) + BandGap;
            std::vector<Pool> pools;
            for (std::size_t b = 0; b < offset.size(); ++b) {
                pools.push_back({ tops[b] - offset[b], 1, b });
                while (pools.size() > 1 && pools[pools.size() - 2].sum / pools[pools.size() - 2].n > pools.back().sum / pools.back().n) {
                    pools[pools.size() - 2].sum += pools.back().sum;
                    pools[pools.size() - 2].n += pools.back().n;
                    pools.pop_back();
                }
            }
            std::vector<float> shift(static_cast<std::size_t>(bands), 0.f);
            for (std::size_t p = 0; p < pools.size(); ++p) {
                const std::size_t end = p + 1 < pools.size() ? pools[p + 1].first : offset.size();
                for (std::size_t b = pools[p].first; b < end; ++b) shift[b] = pools[p].sum / pools[p].n + offset[b] - tops[b];
            }
            for (std::size_t i = 0; i < count; ++i)
                if (blockOf(i) >= 0) y[i] += shift[static_cast<std::size_t>(blockOf(i))];
            // What is shared, moved off the blocks where they now cross it: each column kept in order, apart.
            for (auto& steps : stacked) {
                std::ranges::stable_sort(steps, {}, [&](const std::size_t i) { return y[i]; });
                std::vector<float> wanted;
                for (const std::size_t i : steps) wanted.push_back(y[i]);
                settle(steps, wanted);
            }
        }

        // ---- across: each column as wide as its widest card, and a space between -----------------------------------------
        std::vector<float> x(static_cast<std::size_t>(columns) + 1, 0.f);
        for (int c = 0; c < columns; ++c) {
            float widest = 0.f;
            for (const std::size_t i : stacked[static_cast<std::size_t>(c)]) widest = std::max(widest, sizeOf(i).first);
            x[static_cast<std::size_t>(c) + 1] = x[static_cast<std::size_t>(c)] + widest + 90.f;
        }
        float top = std::numeric_limits<float>::max();
        for (const float each : y) top = std::min(top, each);
        for (std::size_t i = 0; i < count; ++i) layout.at[flow.steps[i].id] = { x[static_cast<std::size_t>(column[i])], y[i] - top };
        return layout;
    }

    std::filesystem::path Session::ProjectsDirectory()
    {
        const char* said = std::getenv("KORAL_ENGINE_PROJECTS");
        std::filesystem::path directory = said && *said ? said : KEDITOR_PROJECTS;
        std::error_code ignored;
        // Where it was built for, on the machine it was built on; on another — a release's — KoralProjects in this user's home.
        if (!(said && *said) && !std::filesystem::is_directory(directory, ignored))
#ifdef _WIN32
            if (const char* home = std::getenv("USERPROFILE"); home && *home) directory = std::filesystem::path(home) / "KoralProjects";
#else
            if (const char* home = std::getenv("HOME"); home && *home) directory = std::filesystem::path(home) / "KoralProjects";
#endif
        std::filesystem::create_directories(directory, ignored);
        return directory;
    }

    // ---- what an entity is, and where it goes ------------------------------------------------------------------

    std::map<std::string, std::string> Session::PipelineComponents()
    {
        // Asked for by every line of the outliner: worked out once for the scene as it is, not once a line.
        if (_pipelinesAt == Doc().Version() + 1) return _pipelines;
        std::map<std::string, std::string> of;
        const kgraph::Runtime& runtime = Doc().Runtime();
        for (const auto& node : Doc().Graph().nodes)
            if (const kecs::ComponentId component = runtime.ComponentOf(node.id); component != kecs::NoComponent) of[Doc().World().Describe(component).name] = node.id;
        _pipelines = of;
        _pipelinesAt = Doc().Version() + 1;
        return of;
    }

    const std::pair<bool, bool>& Session::Drawn(const kecs::Entity entity)
    {
        // Whether each entity can be shown, and is: for every one of them at once, what is under each counted with
        // it, when the scene has changed — and looked up after.
        if (_drawnAt != Doc().Version() + 1) {
            _drawnAt = Doc().Version() + 1;
            _drawn.clear();
            const auto pipelines = PipelineComponents();
            const std::function<std::pair<bool, bool>(kecs::Entity)> each = [&](const kecs::Entity one) {
                std::pair<bool, bool> is { false, false };
                for (const auto& component : Doc().ComponentsOf(one))
                    if (pipelines.contains(component)) { is.first = true; is.second = is.second || Doc().IsEnabled(one, component); }
                for (const kecs::Entity child : Doc().World().ChildrenOf(one)) {
                    const auto under = each(child);
                    is.first = is.first || under.first;
                    is.second = is.second || under.second;
                }
                return _drawn[one] = is;
            };
            for (const kecs::Entity root : Doc().World().Roots()) each(root);
        }
        static const std::pair<bool, bool> none { false, false };
        const auto found = _drawn.find(entity);
        return found == _drawn.end() ? none : found->second;
    }

    std::string Session::EntityKind(const kecs::Entity entity)
    {
        const auto has = Doc().ComponentsOf(entity);
        const auto with = [&](const std::string_view name) { return std::ranges::find(has, name) != has.end(); };
        if (std::ranges::any_of(has, [&](const std::string& component) { return IsCamera(Doc().World(), component); })) return "camera";
        if (with("DirectionalLight")) return "sun";
        for (const auto& component : has)
            if (component.ends_with("Light")) return "light";
        if (with("Mesh")) return "mesh";
        const auto pipelines = PipelineComponents();
        static const std::set<std::string> own { "Mesh", "Camera", "Label" };
        for (const auto& component : has)
            if (!own.contains(component) && !IsCamera(Doc().World(), component) && !pipelines.contains(component) && !Doc().World().Describe(Doc().World().Find(component)).fields.empty()) return "script";
        return Doc().World().ChildrenOf(entity).empty() ? "empty" : "group";
    }

    std::vector<std::string> Session::RunsFor(const kecs::Entity entity)
    {
        std::vector<std::string> passes;
        const auto pipelines = PipelineComponents();
        for (const auto& component : Doc().ComponentsOf(entity)) {
            const auto pipeline = pipelines.find(component);
            if (pipeline == pipelines.end() || !Doc().IsEnabled(entity, component)) continue;
            for (const auto& node : Doc().Graph().nodes)
                if (const auto input = node.inputs.find("pipeline"); input != node.inputs.end() && input->second.link && input->second.link->node == pipeline->second) passes.push_back(node.id);
        }
        return passes;
    }

    bool Session::CanShow(const kecs::Entity entity) { return Drawn(entity).first; }
    bool Session::IsShown(const kecs::Entity entity) { return Drawn(entity).second; }

    void Session::SetShown(const kecs::Entity entity, const bool shown)
    {
        const auto pipelines = PipelineComponents();
        const std::function<void(kecs::Entity)> each = [&](const kecs::Entity one) {
            for (const auto& component : Doc().ComponentsOf(one))
                if (pipelines.contains(component) && Doc().IsEnabled(one, component) != shown) Doc().SetEnabled(one, component, shown);
            // (A copy: switching a component moves nothing, but what is under it is asked for again each time.)
            const std::vector<kecs::Entity> under(Doc().World().ChildrenOf(one).begin(), Doc().World().ChildrenOf(one).end());
            for (const kecs::Entity child : under) each(child);
        };
        each(entity);
        ++_changes;
    }

    bool Session::MoveEntity(const kecs::Entity entity, const kecs::Entity target, const Beside where)
    {
        Document& doc = Doc();
        if (entity == target || !doc.World().Alive(entity) || !doc.World().Alive(target)) return false;
        bool moved = false;
        if (where == Beside::eUnder) moved = doc.SetParent(entity, target);
        else {
            const kecs::Entity parent = doc.World().ParentOf(target);
            // Where the target is among what it is with, as they are now: the world counts what is being moved, when
            // that is with it too, until it has moved.
            std::vector<kecs::Entity> beside;
            if (parent.IsNull()) { const auto roots = doc.World().Roots(); beside.assign(roots.begin(), roots.end()); }
            else beside.assign(doc.World().ChildrenOf(parent).begin(), doc.World().ChildrenOf(parent).end());
            const auto at = std::ranges::find(beside, target);
            const int index = static_cast<int>(at - beside.begin()) + (where == Beside::eAfter ? 1 : 0);
            moved = doc.SetParent(entity, parent, index);
        }
        if (!moved) Say(std::format("'{}' is above '{}': it cannot go under it", doc.NameOf(entity), doc.NameOf(target)), true);
        return moved;
    }

    std::vector<std::string> Session::FieldChoices(const std::string_view component, const std::string_view fieldType)
    {
        Document& doc = Doc();
        const auto& passes = doc.Passes();
        const auto passOf = [&](const std::string& pass) { return pass.empty() ? -1 : static_cast<int>(std::ranges::find(passes, pass) - passes.begin()); };
        // The pass the component's pipeline is run in: what a later pass makes is not there to be given to it.
        int last = static_cast<int>(passes.size());
        const auto pipelines = PipelineComponents();
        if (const auto pipeline = pipelines.find(std::string(component)); pipeline != pipelines.end()) {
            int ran = -1;
            for (const auto& node : doc.Graph().nodes)
                if (const auto input = node.inputs.find("pipeline"); input != node.inputs.end() && input->second.link && input->second.link->node == pipeline->second) ran = std::max(ran, passOf(node.pass));
            if (const kgraph::Node* node = doc.Graph().Find(pipeline->second); ran < 0 && node) ran = passOf(node->pass);
            if (ran >= 0) last = ran;
        } else if (doc.Runtime().MaterialKindOf(component)) {
            // A material's: the first pass that draws with a pipeline whose shaders take something of the kind.
            for (const auto& node : doc.Graph().nodes) {
                const auto input = node.inputs.find("pipeline");
                if (input == node.inputs.end() || !input->second.link) continue;
                if (std::ranges::contains(doc.Runtime().MaterialsOf(input->second.link->node), std::string(component))) last = std::min(last, passOf(node.pass));
            }
        }
        const bool mesh = fieldType == "Mesh", picture = fieldType == "Image";
        std::vector<std::string> choices;
        if (mesh)
            for (const auto& name : kgraph::DefaultMeshNames()) choices.push_back(kgraph::DefaultMeshId(name));
        if (picture) choices.insert(choices.end(), { "$white", "$black", "$normal" });
        if (mesh || picture) {
            for (const auto& asset : Assets())
                if (asset.kind == (mesh ? Asset::Kind::eMesh : Asset::Kind::eTexture) && !asset.builtIn) choices.push_back(asset.id);
        }
        for (const auto& id : doc.NodesFor(fieldType)) {
            const kgraph::Node* node = doc.Graph().Find(id);
            if (!node) continue;        // (a mesh there is without a node: listed already)
            const int pass = passOf(node->pass);
            // A picture a pass draws into is not given to what that pass draws: one from a file is, wherever it is.
            if (picture ? (pass < last || node->type == "ImageFile") : pass <= last) choices.push_back(id);
        }
        return choices;
    }

    bool Session::GiveAsset(const std::string& id, const kecs::Entity entity)
    {
        Document& doc = Doc();
        if (!doc.World().Alive(entity)) return false;
        const auto assets = Assets();
        const auto asset = std::ranges::find(assets, id, &Asset::id);
        if (asset == assets.end()) { Say("That is nothing an entity can be given.", true); return false; }
        const std::string name = doc.NameOf(entity);
        std::string error;
        switch (asset->kind) {
        case Asset::Kind::eGenerator:
            if (!doc.GiveGenerator(entity, asset->file, &error)) { Say(error, true); return false; }
            Say(std::format("{} is made by {}.", name, asset->label));
            return true;
        case Asset::Kind::eMesh:
            if (!std::ranges::contains(doc.ComponentsOf(entity), std::string("Mesh"))) doc.AddComponent(entity, "Mesh");
            if (!doc.SetField(entity, "Mesh", "mesh", kgraph::Value(id), &error)) { Say(error, true); return false; }
            Say(std::format("{} is {}'s mesh.", asset->label, name));
            return true;
        case Asset::Kind::eTexture: {
            // A material's base colour (or diffuse), before any other; one with nothing in it, before one that has something.
            std::optional<std::pair<std::string, std::string>> best;
            int bestScore = -1;
            for (const auto& component : doc.ComponentsOf(entity))
                for (const auto& field : doc.World().Describe(doc.World().Find(component)).fields) {
                    if (field.type != "Image") continue;
                    const kgraph::Value held = doc.Field(entity, component, field.name);
                    const bool free = !held.String() || held.String()->empty() || held.String()->starts_with("$");
                    const bool colour = field.name.find("albedo") != std::string::npos || field.name == "baseColorMap" || field.name == "diffuseMap";
                    const int score = (colour ? 2 : 0) + (free ? 1 : 0);
                    if (score > bestScore) { bestScore = score; best = { component, field.name }; }
                }
            if (!best) { Say(std::format("{} has no field that holds a picture: a component of a pipeline that samples one has.", name), true); return false; }
            // And every other material of it that has a picture of that name: what they share is set in each, as the
            // inspector's shared material sets it.
            const auto pipelines = PipelineComponents();
            std::vector<std::string> into { best->first };
            if (pipelines.contains(best->first))
                for (const auto& component : doc.ComponentsOf(entity))
                    if (component != best->first && pipelines.contains(component))
                        if (const auto* field = doc.World().Describe(doc.World().Find(component)).FindField(best->second); field && field->type == "Image") into.push_back(component);
            for (const auto& component : into)
                if (!doc.SetField(entity, component, best->second, kgraph::Value(id), &error)) { Say(error, true); return false; }
            Say(into.size() > 1 ? std::format("{} is {}'s {}, in each of its {} materials.", asset->label, name, best->second, into.size())
                                : std::format("{} is {}'s {}.", asset->label, name, best->second));
            return true;
        }
        case Asset::Kind::ePrefab:
            // Dropped on an entity, a prefab's instance is made under it.
            return !PlacePrefab(id, entity).IsNull();
        case Asset::Kind::eMaterial: {
            // A Material asset: the entity follows it from now on — one of the editor's, a model's, a project's file.
            if (const Material* material = MaterialOf(id); material && !doc.MaterialAssetOf(id)) {
                doc.AddMaterialAsset({ .id = id, .name = material->label, .kind = material->kind, .values = ValuesOf(*material) });
            }
            std::string error;
            if (!doc.GiveMaterial(entity, id, &error)) { Say(std::format("{} could not be given {}: {}.", name, asset->label, error), true); return false; }
            Say(std::format("{} is made of {}.", name, asset->label));
            return true;
        }
        case Asset::Kind::eScript: {
            const std::string& component = asset->label;   // the component itself, not its file: a file may declare several
            if (asset->group == "kind") { Say(std::format("{} is a kind: nothing has one itself. Give {} one of what derives from it.", component, name), true); return false; }
            if (asset->group == "resources") { Say(std::format("{} makes resources for a Script node of it: it is no component an entity has.", component), true); return false; }
            if (asset->group == "conditions") { Say(std::format("{} holds conditions a ForEach or a Filter keeps entities by: it is no component an entity has.", component), true); return false; }
            if (std::ranges::contains(doc.ComponentsOf(entity), component)) { Say(std::format("{} has {} already.", name, component)); return true; }
            if (!doc.AddComponent(entity, component)) {
                Say(std::format("There is no component '{}' yet: a script's is there once the editor runs the project's scripts.", component), true);
                return false;
            }
            Say(std::format("{} has {}.", name, component));
            return true;
        }
        default:
            Say(std::format("{} is not something an entity is given: a mesh, a texture or a script is.", asset->label), true);
            return false;
        }
    }

    const std::vector<Session::Material>& Session::DefaultMaterials()
    {
        // Colours as they are picked; metals by what they reflect (their colour), and smooth to rough. Blinn-Phong's are
        // the classic table's (McReynolds and Blythe), each by its name in that list.
        const auto pbr = [](const char* name, const std::array<float, 3> colour, const float metallic, const float roughness, const std::array<float, 3> emission = {}) {
            Material material;
            material.name = material.label = name;
            std::ranges::copy(colour, material.albedo);
            material.metallic = metallic;
            material.roughness = roughness;
            std::ranges::copy(emission, material.emission);
            return material;
        };
        const auto classic = [](const char* name, const std::array<float, 3> diffuse, const std::array<float, 3> specular, const float shininess) {
            Material material;
            material.name = std::format("Blinn-Phong {}", name);
            material.label = name;
            material.kind = "BlinnPhongMaterial";
            std::ranges::copy(diffuse, material.albedo);
            std::ranges::copy(specular, material.specular);
            material.shininess = shininess;
            return material;
        };
        static const std::vector<Material> materials {
            pbr("White", { 0.92f, 0.92f, 0.92f }, 0.f, 0.5f),
            pbr("Grey", { 0.5f, 0.5f, 0.52f }, 0.f, 0.6f),
            pbr("Black", { 0.05f, 0.05f, 0.05f }, 0.f, 0.5f),
            pbr("Rubber", { 0.08f, 0.08f, 0.09f }, 0.f, 0.95f),
            pbr("Clay", { 0.72f, 0.45f, 0.33f }, 0.f, 0.9f),
            pbr("Wood", { 0.55f, 0.36f, 0.2f }, 0.f, 0.7f),
            pbr("Concrete", { 0.55f, 0.54f, 0.52f }, 0.f, 0.92f),
            pbr("Ceramic", { 0.9f, 0.88f, 0.84f }, 0.f, 0.15f),
            pbr("Red plastic", { 0.8f, 0.1f, 0.08f }, 0.f, 0.35f),
            pbr("Orange plastic", { 0.95f, 0.45f, 0.08f }, 0.f, 0.35f),
            pbr("Yellow plastic", { 0.95f, 0.82f, 0.15f }, 0.f, 0.35f),
            pbr("Green plastic", { 0.15f, 0.65f, 0.2f }, 0.f, 0.35f),
            pbr("Blue plastic", { 0.12f, 0.3f, 0.85f }, 0.f, 0.35f),
            pbr("Purple plastic", { 0.5f, 0.2f, 0.75f }, 0.f, 0.35f),
            pbr("Gold", { 1.f, 0.78f, 0.34f }, 1.f, 0.25f),
            pbr("Silver", { 0.97f, 0.96f, 0.91f }, 1.f, 0.15f),
            pbr("Copper", { 0.95f, 0.64f, 0.54f }, 1.f, 0.3f),
            pbr("Brass", { 0.91f, 0.78f, 0.42f }, 1.f, 0.35f),
            pbr("Chrome", { 0.95f, 0.95f, 0.95f }, 1.f, 0.05f),
            pbr("Steel", { 0.6f, 0.6f, 0.62f }, 1.f, 0.45f),
            pbr("Aluminium", { 0.91f, 0.92f, 0.92f }, 1.f, 0.55f),
            pbr("Glowing white", { 0.9f, 0.9f, 0.9f }, 0.f, 0.5f, { 4.f, 4.f, 4.f }),
            pbr("Glowing orange", { 1.f, 0.5f, 0.15f }, 0.f, 0.5f, { 4.f, 1.6f, 0.4f }),

            classic("Emerald", { 0.076f, 0.614f, 0.076f }, { 0.633f, 0.728f, 0.633f }, 77.f),
            classic("Jade", { 0.54f, 0.89f, 0.63f }, { 0.316f, 0.316f, 0.316f }, 13.f),
            classic("Obsidian", { 0.183f, 0.17f, 0.225f }, { 0.333f, 0.329f, 0.346f }, 38.f),
            classic("Pearl", { 1.f, 0.829f, 0.829f }, { 0.297f, 0.297f, 0.297f }, 11.f),
            classic("Ruby", { 0.614f, 0.041f, 0.041f }, { 0.728f, 0.627f, 0.627f }, 77.f),
            classic("Turquoise", { 0.396f, 0.742f, 0.691f }, { 0.297f, 0.308f, 0.307f }, 13.f),
            classic("Brass", { 0.78f, 0.569f, 0.114f }, { 0.992f, 0.942f, 0.808f }, 28.f),
            classic("Bronze", { 0.714f, 0.428f, 0.181f }, { 0.394f, 0.272f, 0.167f }, 26.f),
            classic("Chrome", { 0.4f, 0.4f, 0.4f }, { 0.775f, 0.775f, 0.775f }, 77.f),
            classic("Copper", { 0.704f, 0.27f, 0.083f }, { 0.257f, 0.138f, 0.086f }, 13.f),
            classic("Gold", { 0.752f, 0.606f, 0.226f }, { 0.628f, 0.556f, 0.366f }, 51.f),
            classic("Silver", { 0.508f, 0.508f, 0.508f }, { 0.508f, 0.508f, 0.508f }, 51.f),
            classic("Black plastic", { 0.01f, 0.01f, 0.01f }, { 0.5f, 0.5f, 0.5f }, 32.f),
            classic("Cyan plastic", { 0.f, 0.51f, 0.51f }, { 0.502f, 0.502f, 0.502f }, 32.f),
            classic("Green plastic", { 0.1f, 0.35f, 0.1f }, { 0.45f, 0.55f, 0.45f }, 32.f),
            classic("Red plastic", { 0.5f, 0.f, 0.f }, { 0.7f, 0.6f, 0.6f }, 32.f),
            classic("White plastic", { 0.55f, 0.55f, 0.55f }, { 0.7f, 0.7f, 0.7f }, 32.f),
            classic("Yellow plastic", { 0.5f, 0.5f, 0.f }, { 0.6f, 0.6f, 0.5f }, 32.f),
            classic("Black rubber", { 0.01f, 0.01f, 0.01f }, { 0.4f, 0.4f, 0.4f }, 10.f),
            classic("Green rubber", { 0.4f, 0.5f, 0.4f }, { 0.04f, 0.7f, 0.04f }, 10.f),
            classic("Red rubber", { 0.5f, 0.4f, 0.4f }, { 0.7f, 0.04f, 0.04f }, 10.f),
            classic("White rubber", { 0.5f, 0.5f, 0.5f }, { 0.7f, 0.7f, 0.7f }, 10.f),
        };
        return materials;
    }

    std::vector<std::pair<std::string, kgraph::Value>> Session::ValuesOf(const Material& material)
    {
        using kgraph::Value;
        const auto three = [](const float* v) { return Value(Value::List { v[0], v[1], v[2] }); };
        const auto four = [](const float* v) { return Value(Value::List { v[0], v[1], v[2], 1.0 }); };
        if (material.kind == "BlinnPhongMaterial")
            return { { "diffuse", four(material.albedo) }, { "specular", three(material.specular) }, { "shininess", Value(material.shininess) }, { "emission", three(material.emission) } };
        return { { "baseColor", four(material.albedo) }, { "metallic", Value(material.metallic) }, { "roughness", Value(material.roughness) }, { "emission", three(material.emission) } };
    }

    std::optional<MaterialShown> Session::MaterialToShow(const std::string& id)
    {
        std::string kind;
        std::vector<std::pair<std::string, kgraph::Value>> values;
        if (const MaterialAsset* asset = Doc().MaterialAssetOf(id)) { kind = asset->kind; values = asset->values; }
        else if (const Material* material = MaterialOf(id)) { kind = material->kind; values = ValuesOf(*material); }
        else return std::nullopt;
        const kgraph::MaterialKind* declared = Doc().Runtime().MaterialKindOf(kind);
        if (!declared) return std::nullopt;
        return MaterialShown { *declared, std::move(values), Doc().Runtime().GeneratedShaderDirectory() };
    }

    void Session::SetPreviewMesh(std::string id)
    {
        _previewMesh = std::move(id);
        ++_changes;
    }

    std::filesystem::path Session::ShadingModuleOf(const std::string& kind) const
    {
        return _hasProject ? _project.ShaderDirectory() / (kind + "Shading.slang") : std::filesystem::path();
    }

    bool Session::HasShading(const std::string& kind) const
    {
        std::error_code ignored;
        return std::filesystem::exists(kscript::PackageDirectory() / "shaders" / (kind + "Shading.slang"), ignored)
            || (_hasProject && std::filesystem::exists(ShadingModuleOf(kind), ignored));
    }

    bool Session::WriteShadingModule(const std::string& kind)
    {
        const std::filesystem::path file = ShadingModuleOf(kind);
        if (file.empty()) { Say("A shading is written to a project: File, New project.", true); return false; }
        if (std::error_code ignored; std::filesystem::exists(file, ignored)) { Say(std::format("{} has its shading already.", kind), true); return false; }
        std::ofstream out(file, std::ios::binary);
        out << std::format(R"slang(// How a {0} is lit. The material inspector and the Assets panel draw every {0} through this; a shader of the
// project's does too, with KoralLit (see koral_shading in the engine's package, and docs/shaders.md).

module {0}Shading;

__exported import koral_shadings;
__exported import {0};

/** What is at a point of the surface, read, and how it answers light. */
public struct {0}Lit : IKoralShading {{
    public float3 normal;
    public float3 colour;

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {{
        const KoralAngles a = KoralAnglesOf(normal, toEye, toLight);     // a lobe of koral_brdf's: KoralBurleyLobe, KoralGgxLobe...
        return colour * KoralLambertLobe.Reflected(a, 1.0) * light * a.nl;
    }}
    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye) {{ return colour * environment.Diffuse(normal); }}
    public float3 Emitted() {{ return 0.0; }}
    public float Opacity() {{ return 1.0; }}
}};

/** How a {0}'s fields are read at a point. */
public extension {0} : IKoralMaterial {{
    public typealias Shading = {0}Lit;

    public {0}Lit Read(KoralSurface at)
    {{
        {0}Lit lit;
        lit.normal = at.normal;
        lit.colour = float3(0.8);     // from the fields of the {0}
        return lit;
    }}
}};
)slang", kind);
        if (!out) { Say(std::format("{} could not be written.", file.generic_string()), true); return false; }
        out.close();
        Say(std::format("{} is written: its {} is what is drawn.", file.filename().string(), kind));
        ++_changes;
        return true;
    }

    void Session::TryMaterialField(const std::string& field, kgraph::Value value)
    {
        const auto it = std::ranges::find(_materialTried, field, &std::pair<std::string, kgraph::Value>::first);
        if (it != _materialTried.end()) it->second = std::move(value);
        else _materialTried.emplace_back(field, std::move(value));
        ++_changes;
    }

    void Session::ForgetMaterialTried()
    {
        _materialTried.clear();
        ++_changes;
    }

    void Session::UpdateMaterialPreview()
    {
        if (_viewKind != ViewKind::eMaterial) {
            if (_materialPreview) {
                if (!_previewedAs.empty()) { _previewedAs.clear(); _materialPreview->Show(std::nullopt); }
                _materialPreview->Update();
            }
            return;
        }
        if (!_materialPreview) {
            _materialPreview = std::make_unique<MaterialPreview>(_play.Graph(), kor::UVec2(512, 384), true);
            _materialPreview->SetFind([this](const std::string& name) { return Doc().Runtime().ResourceOf(name); });
        }
        _materialPreview->SetAssetDirectory(Doc().Runtime().AssetDirectory());
        // Drawn again only when what it is drawn with is another: the material as it is, and over it what is tried out.
        if (auto shown = MaterialToShow(_viewing)) {
            for (const auto& [field, value] : _materialTried) {
                const auto it = std::ranges::find(shown->values, field, &std::pair<std::string, kgraph::Value>::first);
                if (it != shown->values.end()) it->second = value;
                else shown->values.emplace_back(field, value);
            }
            std::string as = shown->kind.name;
            for (const auto& [field, value] : shown->values) as += std::format("\n{}={}", field, value.ToJson());
            if (as != _previewedAs) {
                _previewedAs = std::move(as);
                _materialPreview->Show(std::move(shown));
            }
        }
        _materialPreview->SetMesh(Doc().Runtime().ResourceOf(_previewMesh));
        _materialPreview->Update();
        if (auto picture = _materialPreview->PictureHeld(); picture != _previewPictureHeld) {
            if (_previewPictureHeld) _overBefore.emplace_back(std::move(_previewPictureHeld), 4);
            _previewPictureHeld = std::move(picture);
            ++_changes;
        }
    }

    const Session::Material* Session::MaterialOf(const std::string_view id)
    {
        if (!id.starts_with("material:")) return nullptr;
        const auto& all = DefaultMaterials();
        const auto it = std::ranges::find(all, id.substr(9), &Material::name);
        return it == all.end() ? nullptr : &*it;
    }

    std::string Session::AssetToNode(const std::string& id, const double x, const double y)
    {
        const auto assets = Assets();
        const auto asset = std::ranges::find(assets, id, &Asset::id);
        if (asset == assets.end()) return {};
        Document& doc = Doc();
        const std::string pass = _everyPass ? std::string() : _pass;
        std::string made;
        if (asset->kind == Asset::Kind::eTexture) {
            made = doc.AddNode("ImageFile", x, y, asset->file.stem().string(), pass);
            if (!made.empty()) doc.SetInput(made, "file", kgraph::Value(id.substr(1)));
        } else if (asset->kind == Asset::Kind::eShader) {
            const std::string& module = asset->group;
            const auto entries = ShaderEntries(module);
            if (entries.empty()) { Say(std::format("{} has no entry point: it is a module others import, and no pipeline's.", asset->label), true); return {}; }
            // The entry dropped is its stage's; the module's first of each other stage the pipeline has, beside it.
            const auto entry = [&](const std::string_view stage) -> const std::string* {
                if (asset->stage == stage) return &asset->label;
                const auto found = std::ranges::find(entries, stage, &ShaderEntry::stage);
                return found == entries.end() ? nullptr : &found->name;
            };
            // A module dropped is a pipeline of its entries: a compute one when it draws nothing, of its first compute entry.
            const auto compute = std::ranges::find(entries, std::string("compute"), &ShaderEntry::stage);
            const bool draws = entry("vertex") || entry("fragment");
            if (asset->stage == "compute" || (asset->stage.empty() && !draws && compute != entries.end())) {
                const std::string& name = asset->stage.empty() ? compute->name : asset->label;
                made = doc.AddNode("ComputePipeline", x, y, module, pass);
                if (!made.empty()) { doc.SetInput(made, "compute.module", kgraph::Value(module)); doc.SetInput(made, "compute.entry", kgraph::Value(name)); }
            } else {
                made = doc.AddNode("GraphicsPipeline", x, y, module, pass);
                if (!made.empty())
                    for (const char* stage : { "vertex", "fragment" })
                        if (const std::string* found = entry(stage)) {
                            doc.SetInput(made, std::string(stage) + ".module", kgraph::Value(module));
                            doc.SetInput(made, std::string(stage) + ".entry", kgraph::Value(*found));
                        }
            }
        } else if (asset->kind == Asset::Kind::eFunction) {
            // A function dropped is a call of it.
            const std::string name = asset->file.stem().string();
            made = doc.AddNode("Function", x, y, name, pass);
            if (!made.empty()) doc.SetInput(made, "function", kgraph::Value(name));
        } else {
            Say(std::format("{} is not a node: a texture is an ImageFile, a shader a pipeline, a function a call of it.", asset->label), true);
            return {};
        }
        if (!made.empty()) { Select(made); Say(std::format("Made {} of {}.", made, asset->label)); }
        return made;
    }

    std::vector<std::string> Session::WireChoices(const std::string_view id, const std::string_view input)
    {
        Document& doc = Doc();
        const kgraph::Runtime& runtime = doc.Runtime();
        const kgraph::Node* node = doc.Graph().Find(id);
        std::vector<std::string> choices;
        if (!node) return choices;
        // What the input takes: by the name it has on the node, or the one its type lists it by (`color[].view`).
        std::string wanted;
        for (const auto& port : runtime.PortsOf(id)) {
            std::string listed = port.name;
            if (const auto at = listed.find("[]"); at != std::string::npos) {
                const auto open = input.find('[');
                const auto close = input.find(']');
                if (open == at && close != std::string_view::npos) listed = std::string(input.substr(0, close)) + listed.substr(at + 1);
            }
            if (listed == input && port.kind == kgraph::PortKind::eResource) wanted = port.type;
        }
        if (wanted.empty()) return choices;
        const auto& passes = doc.Passes();
        const auto passOf = [&](const std::string& pass) { return pass.empty() ? -1 : static_cast<int>(std::ranges::find(passes, pass) - passes.begin()); };
        const int here = passOf(node->pass);
        // Whatever is wired from this node, straight or through others, cannot be wired back into it.
        std::set<std::string> after { node->id };
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& other : doc.Graph().nodes)
                if (!after.contains(other.id) && std::ranges::any_of(other.inputs | std::views::values, [&](const kgraph::Input& in) { return in.link && after.contains(in.link->node); })) { after.insert(other.id); grew = true; }
        }
        for (const auto& other : doc.Graph().nodes) {
            if (after.contains(other.id)) continue;
            if (other.type == "Reference" && other.pass != node->pass) continue;       // another page's stand-in for something
            if (const int pass = passOf(other.pass); pass > here && here >= 0) continue;
            // What stands for another node gives what that node gives.
            const kgraph::Node* stood = &other;
            for (int hops = 0; stood && stood->type == "Reference" && hops < 8; ++hops) {
                const auto target = stood->inputs.find("node");
                stood = target != stood->inputs.end() && target->second.value.String() ? doc.Graph().Find(*target->second.value.String()) : nullptr;
            }
            if (!stood) continue;
            // (And a node this page already has a stand-in for is offered as that, once.)
            if (other.type != "Reference" && other.pass != node->pass && !other.pass.empty()
                && std::ranges::any_of(doc.Graph().nodes, [&](const kgraph::Node& each) {
                       const auto target = each.inputs.find("node");
                       return each.type == "Reference" && each.pass == node->pass && target != each.inputs.end() && target->second.value.String() && *target->second.value.String() == other.id;
                   })) continue;
            std::string gives;
            for (const auto& output : runtime.OutputsOf(stood->id))
                if (output.name == "out" && output.kind == kgraph::PortKind::eResource) gives = output.type;
            if (gives.empty()) continue;
            const bool fits = wanted == "Resource" || wanted == "value" || gives == wanted || gives == "Resource" || (gives == "ImageView" && wanted == "Image");
            if (fits) choices.push_back(other.id);
        }
        return choices;
    }

    bool Session::WireFrom(const std::string_view id, const std::string_view input, const std::string& from)
    {
        Document& doc = Doc();
        const kgraph::Node* node = doc.Graph().Find(id);
        const kgraph::Node* source = doc.Graph().Find(from);
        if (!node || !source) return false;
        std::string wired = from;
        // Of another pass, it is brought into this one: a node here that stands for it.
        if (!node->pass.empty() && !source->pass.empty() && source->pass != node->pass) wired = doc.Import(from, node->pass, node->x - 260., node->y);
        if (wired.empty() || !doc.Connect(wired, "out", id, input)) { Say(std::format("{} could not be wired to {}.", from, input), true); return false; }
        return true;
    }

    bool Session::ShowInFiles(const std::filesystem::path& path)
    {
        std::error_code ignored;
        const std::filesystem::path folder = std::filesystem::is_directory(path, ignored) ? path : path.parent_path();
        const std::string command = std::format("xdg-open \"{}\"", folder.string());
        const bool started = launch ? launch(command) : std::system((command + " >/dev/null 2>&1 &").c_str()) == 0;
        if (!started) Say("The file manager could not be started.", true);
        return started;
    }

    kor::ResourceRef<const kor::Image> Session::NodePicture(const std::string_view id)
    {
        kgraph::Runtime& runtime = Doc().Runtime();
        if (runtime.ImageOf(id).Get()) return ThumbnailOf(id);
        const auto held = runtime.ResourceOf(id);
        if (!held || held->kind != "Mesh" || held->pending) return {};
        // By the mesh it is now: a node built again is another mesh, and another picture.
        const std::string key = std::format("node {}#{}", id, static_cast<const void*>(held.get()));
        ThumbnailsHere().Want(key, held);
        return _thumbnails->Of(key);
    }

    kor::ResourceRef<const kor::Image> Session::MeshPicture(const std::string_view id) const
    {
        return _thumbnails ? _thumbnails->Of(id) : kor::ResourceRef<const kor::Image>();
    }

    std::vector<Session::SceneTexture> Session::SceneTextures()
    {
        Document& doc = Doc();
        const kgraph::Graph& graph = doc.Graph();
        // Which field holds each: the first found, by walking the entities as they are saved.
        std::map<std::string, std::string, std::less<>> fields;
        for (const kecs::Entity entity : doc.World().All()) {
            const kgraph::Value written = doc.WriteEntity(entity);
            const kgraph::Value* components = written.Find("components");
            if (!components || !components->Named()) continue;
            for (const auto& values : *components->Named() | std::views::values)
                if (values.Named())
                    for (const auto& [field, value] : *values.Named())
                        if (const std::string* id = value.String(); id && id->starts_with('@') && id->find('#') == std::string::npos) fields.try_emplace(*id, field);
        }
        std::map<std::string, std::string, std::less<>> packOf;
        for (const auto& [pack, made] : graph.texturePacks)
            for (const auto& channel : made.channels) if (!channel.from.empty()) packOf.try_emplace(channel.from, pack);
        std::vector<SceneTexture> textures;
        for (const auto& id : doc.Runtime().Pictures()) {
            if (kgraph::IsTexturePack(id)) continue;
            SceneTexture texture;
            texture.id = id;
            if (const auto it = fields.find(id); it != fields.end()) texture.field = it->second;
            texture.kind = kgraph::TextureKindOf(texture.field, id);
            if (const auto it = graph.textureImports.find(id); it != graph.textureImports.end()) texture.import = it->second;
            if (const auto it = packOf.find(id); it != packOf.end()) texture.pack = it->second;
            texture.cooked = doc.Runtime().Cooked(texture.pack.empty() ? id : texture.pack);
            texture.compressible = kgraph::TextureCompressible(id);
            textures.push_back(std::move(texture));
        }
        std::ranges::sort(textures, {}, &SceneTexture::id);
        return textures;
    }

    kgraph::TextureFormat Session::OfferedFormat(const kgraph::TextureKind kind) const
    {
        if (const auto it = _project.textureFormats.find(std::string(kgraph::TextureKindName(kind))); it != _project.textureFormats.end())
            if (const auto format = kgraph::TextureFormatNamed(it->second)) return *format;
        return kgraph::DefaultTextureFormat(kind);
    }

    void Session::SetOfferedFormat(const kgraph::TextureKind kind, const kgraph::TextureFormat format)
    {
        if (OfferedFormat(kind) == format) return;
        _project.textureFormats[std::string(kgraph::TextureKindName(kind))] = std::string(kgraph::TextureFormatName(format));
        std::string error;
        if (_hasProject && !_project.Save(error)) Say(error, true);
    }

    std::string Session::NewMaterial(const std::string& kind)
    {
        if (!_hasProject) { Say("A material of the project's own goes in a project: File, New project.", true); return {}; }
        if (!Doc().Runtime().MaterialKindOf(kind)) { Say(std::format("There is no material kind '{}'.", kind), true); return {}; }
        const std::filesystem::path folder = _project.directory / "assets" / "materials";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::filesystem::path file = folder / "Material.kmat";
        for (int n = 2; std::filesystem::exists(file, ec); ++n) file = folder / std::format("Material {}.kmat", n);
        std::ofstream(file, std::ios::binary) << std::format("{{\n  \"koral-material\": 1,\n  \"kind\": {},\n  \"values\": {{}}\n}}\n", kecs::json::Quote(kind));
        Doc().SetAssetDirectory(_project.directory / "assets");
        const std::string id = Doc().Runtime().AssetId(file);
        if (!Doc().MaterialAssetOf(id)) { Say(std::format("{} could not be made.", file.filename().string()), true); return {}; }
        SelectAsset(id);
        Say(std::format("A {} of the project's: {}. Drag it onto an entity, which then follows it.", kind, file.filename().string()));
        return id;
    }

    std::string Session::MaterialAssetFrom(const kecs::Entity entity, const std::string& kind)
    {
        Document& doc = Doc();
        const kecs::ComponentId id = doc.World().Find(kind);
        if (id == kecs::NoComponent || !doc.World().Has(entity, id)) return {};
        const std::string made = NewMaterial(kind);
        if (made.empty()) return {};
        // What the entity's is, every field: the asset's now, and the entity follows it.
        for (const auto& field : doc.World().Describe(id).fields)
            if (const kgraph::Value value = doc.Field(entity, kind, field.name); !value.IsNull()) doc.SetMaterialAssetField(made, field.name, value);
        doc.GiveMaterial(entity, made);
        SelectAsset(made);
        Say(std::format("{}'s {} is an asset of the project's now, which it follows.", doc.NameOf(entity), kind));
        return made;
    }

    const kgraph::Runtime::CookProgress& Session::Cooking() { return Doc().Runtime().Cooking(); }

    bool Session::OpenInCode(const std::filesystem::path& file, const int line)
    {
        if (!_hasProject) { Say("There is a folder to open once there is a project: File, New project.", true); return false; }
        WriteEditorSetup(_project.directory, _project.name);
        const char* said = std::getenv("KORAL_ENGINE_CODE");
        std::string command = std::format("{} \"{}\"", said && *said ? said : "code", _project.directory.string());
        if (!file.empty()) command += line > 0 ? std::format(" --goto \"{}:{}\"", file.string(), line) : std::format(" --goto \"{}\"", file.string());
        // The shell's, and not waited for: the editor of code is a program of its own.
#ifdef _WIN32
        // `code` is a code.cmd, so a cmd runs it. /b is no window of its own; /s keeps the command's quotes.
        const std::string detached = "start \"\" /b cmd /s /c \"" + command + " >NUL 2>&1\"";
#else
        const std::string detached = command + " >/dev/null 2>&1 &";
#endif
        const bool started = launch ? launch(command) : std::system(detached.c_str()) == 0;
        if (started) Say(file.empty() ? std::string("Opened the project in the editor of code.") : std::format("Opened {} in the editor of code.", file.filename().string()));
        else Say(std::format("'{}' could not be run: set KORAL_ENGINE_CODE to the editor's command.", command), true);
        return started;
    }

    kor::ResourceRef<const kor::Image> Session::OverPicture() const
    {
        return _overHeld && _overHeld->handle ? kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(_overHeld->handle)) : kor::ResourceRef<const kor::Image>();
    }

    void Session::Look(std::string id)
    {
        // Nothing named: none is looked at any more.
        if (id.empty()) {
            for (auto& [looked, made] : _looks) {
                if (looked != _viewing) Doc().Runtime().Unwatch(looked);    // what the viewer has, it goes on copying out
                if (made.image.Valid()) _lookBefore.push_back(std::move(made.image));
            }
            _looks.clear();
            _looking.clear();
            ++_changes;
            return;
        }
        _looking = id;
        if (_looks.try_emplace(id).second) {
            Doc().Runtime().Watch(id);
            Doc().Runtime().WatchEvery(id, 1);      // closely: every frame, whatever its thumbnail asked
            _looks[id].says = "Not copied out yet.";
            _lookTimer = 0.f;
        }
        lookSlice = lookMip = lookLayer = 0;
        ++_changes;
    }

    void Session::Unlook(const std::string& id)
    {
        const auto it = _looks.find(id);
        if (it == _looks.end()) return;
        if (id != _viewing) Doc().Runtime().Unwatch(id);
        if (it->second.image.Valid()) _lookBefore.push_back(std::move(it->second.image));
        _looks.erase(it);
        if (_looking == id) _looking = _looks.empty() ? std::string() : _looks.begin()->first;
        ++_changes;
    }

    kor::ResourceRef<const kor::Image> Session::LookPictureOf(const std::string_view id) const
    {
        const auto it = _looks.find(id);
        return it != _looks.end() && it->second.image.Valid() ? kor::ResourceRef<const kor::Image>(it->second.image) : kor::ResourceRef<const kor::Image>();
    }
    const std::string& Session::LookSaysOf(const std::string_view id) const
    {
        static const std::string nothing;
        const auto it = _looks.find(id);
        return it == _looks.end() ? nothing : it->second.says;
    }
    const std::vector<std::string>& Session::LookLines() const
    {
        static const std::vector<std::string> none;
        const auto it = _looks.find(_looking);
        return it == _looks.end() ? none : it->second.lines;
    }

    bool Session::View(std::string id)
    {
        if (id.empty()) { StopViewing(); return false; }
        // A material is no resource: it is viewed on a mesh, and what is tried out there is forgotten with another.
        if (MaterialToShow(id)) {
            if (id != _viewing) { StopViewing(); _materialTried.clear(); }
            _viewing = std::move(id);
            _viewKind = ViewKind::eMaterial;
            ++_viewAsked;
            ++_changes;
            return true;
        }
        kgraph::Runtime& runtime = Doc().Runtime();
        std::shared_ptr<kgraph::Resource> held = runtime.ResourceOf(id);
        if (!held && id.starts_with('@')) held = AssetHeld(id);
        ViewKind kind = ViewKind::eNone;
        if (held) {
            if (held->kind == "Mesh") kind = ViewKind::eMesh;
            else if (held->kind == "Image" || held->kind == "ImageView" || runtime.ImageOf(id).Get()) kind = ViewKind::eImage;
            else if (held->kind == "Buffer" || held->kind == "BufferView") kind = ViewKind::eBuffer;
        }
        if (kind == ViewKind::eNone) { Say(std::format("'{}' holds no image, buffer or mesh to view.", id), true); return false; }
        if (id != _viewing) {
            // Another thing: looked at from its first level and layer, its first row, and seen whole.
            StopViewing();
            imageView.mip = imageView.layer = 0;
            imageView.look.slice = 0;
            imageView.look.black.reset();
            imageView.look.white.reset();
            bufferFirst = 0;
            if (_meshViewer) _meshViewer->Frame();
        }
        _viewing = std::move(id);
        _viewKind = kind;
        _viewSays = kind == ViewKind::eMesh ? std::string() : std::string("Not copied out yet.");
        _viewTimer = 0.f;
        if (kind != ViewKind::eMesh) runtime.Watch(_viewing, imageView.mip, imageView.layer);
        ++_viewAsked;
        ++_changes;
        return true;
    }

    void Session::StopViewing()
    {
        if (_viewing.empty()) return;
        if (_viewKind != ViewKind::eMesh && _viewKind != ViewKind::eMaterial && !_looks.contains(_viewing)) Doc().Runtime().Unwatch(_viewing);
        _materialTried.clear();
        if (_previewPictureHeld) _overBefore.emplace_back(std::move(_previewPictureHeld), 4);
        _previewPictureHeld.reset();
        if (_viewImage.Valid()) _viewBefore.emplace_back(std::move(_viewImage), 4);
        if (_viewPictureHeld) _overBefore.emplace_back(std::move(_viewPictureHeld), 4);
        _viewPictureHeld.reset();
        if (_meshViewer) _meshViewer->Show(nullptr);
        _viewPeek.reset();
        _viewPicture = {};
        _viewHistogram = {};
        _viewSays.clear();
        _viewing.clear();
        _viewKind = ViewKind::eNone;
        ++_changes;
    }

    kor::ResourceRef<const kor::Image> Session::ViewedPicture() const
    {
        if (_viewKind == ViewKind::eMesh) return _meshViewer ? _meshViewer->Picture() : kor::ResourceRef<const kor::Image>();
        return _viewImage.Valid() ? kor::ResourceRef<const kor::Image>(_viewImage) : kor::ResourceRef<const kor::Image>();
    }

    std::shared_ptr<kgraph::Resource> Session::ViewedMeshHeld() const
    {
        if (_viewKind != ViewKind::eMesh) return nullptr;
        if (auto held = _play.Doc().Runtime().ResourceOf(_viewing)) return held;
        return AssetHeld(_viewing);
    }

    void Session::SetBufferLayout(std::string text)
    {
        _bufferLayoutText = std::move(text);
        _bufferLayoutError.clear();
        _bufferLayout.reset();
        const bool blank = std::ranges::all_of(_bufferLayoutText, [](const char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
        if (!blank) _bufferLayout = ParseBufferLayout(_bufferLayoutText, &_bufferLayoutError);
        bufferFirst = 0;
        ++_changes;
    }

    void Session::UpdateView(const float seconds)
    {
        std::erase_if(_viewBefore, [](auto& kept) { return --kept.second <= 0; });
        UpdateMaterialPreview();
        // A mesh no longer viewed: its passes go here, at the start of a frame (StopViewing may have been pressed in one).
        if (_meshViewer && _viewKind != ViewKind::eMesh) _meshViewer->Update();
        if (_viewKind == ViewKind::eNone || _viewKind == ViewKind::eMaterial) return;
        kgraph::Runtime& runtime = Doc().Runtime();
        if (_viewKind == ViewKind::eMesh) {
            const auto held = ViewedMeshHeld();
            if (!_meshViewer) _meshViewer = std::make_unique<MeshViewer>(_play.Graph());
            _meshViewer->Show(held && !held->pending ? held : nullptr);
            _meshViewer->Update();
            const std::string says = !held ? std::string("It is not there any more.") : held->pending ? std::string("Still being read.") : _meshViewer->Problem();
            if (says != _viewSays) { _viewSays = says; ++_changes; }
            // Its picture is made again when it is another size: held until the interface has stopped showing the last.
            if (auto picture = _meshViewer->PictureHeld(); picture != _viewPictureHeld) {
                if (_viewPictureHeld) _overBefore.emplace_back(std::move(_viewPictureHeld), 4);
                _viewPictureHeld = std::move(picture);
                ++_changes;
            }
            return;
        }
        runtime.Watch(_viewing, imageView.mip, imageView.layer);
        if ((_viewTimer -= seconds) > 0.f) return;
        _viewTimer = 0.25f;
        auto peek = runtime.Watched(_viewing);
        if (!peek) {
            std::string says = runtime.WatchProblem(_viewing);
            if (says.empty()) says = "Not copied out yet.";
            if (says != _viewSays) { _viewSays = std::move(says); ++_changes; }
            return;
        }
        _viewSays.clear();
        if (peek->image) {
            LookOptions options = imageView.look;
            options.most = 2048;
            options.slice = std::min(options.slice, std::max(peek->extent.z, 1u) - 1);
            _viewPicture = ToPicture(*peek, options);
            _viewHistogram = HistogramOf(*peek, options.slice);
            if (_viewPicture.pixels.empty()) {
                _viewSays = std::format("{} has no such channel to show.", peek->format);
            } else {
                auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(_viewPicture.size)
                    .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                    .SetData(std::span<const kor::U8Vec4>(_viewPicture.pixels)).Build();
                if (image.Valid()) {
                    if (_viewImage.Valid()) _viewBefore.emplace_back(std::move(_viewImage), 4);
                    _viewImage = std::move(image);
                }
            }
        }
        _viewPeek = std::move(peek);
        ++_changes;
    }

    void Session::UpdateLook(const float seconds)
    {
        if (_looks.empty()) { _lookBefore.clear(); return; }
        kgraph::Runtime& runtime = Doc().Runtime();
        // A scene opened or made anew is another runtime, which was not asked to watch.
        for (const auto& id : _looks | std::views::keys) {
            if (id == _looking) runtime.Watch(id, lookMip, lookLayer);
            else runtime.Watch(id);
        }
        if ((_lookTimer -= seconds) > 0.f) return;
        _lookTimer = 0.25f;
        _lookBefore.clear();    // those are several frames old by now
        for (auto& [id, looked] : _looks) {
            const auto peek = runtime.Watched(id);
            std::string says;
            std::vector<std::string> lines;
            if (!peek) {
                says = runtime.WatchProblem(id);
                if (says.empty()) says = "Not copied out yet.";
            } else if (peek->image) {
                const Picture picture = ToPicture(*peek, lookMode, id == _looking ? lookSlice : 0);
                if (picture.pixels.empty()) {
                    says = std::format("{} has no such channel to show.", peek->format);
                } else {
                    auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(picture.size)
                        .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                        .SetData(std::span<const kor::U8Vec4>(picture.pixels)).Build();
                    if (image.Valid()) {
                        if (looked.image.Valid()) _lookBefore.push_back(std::move(looked.image));
                        looked.image = std::move(image);
                    }
                    says = std::format("{}, {} x {}{}. Its values run from {:.4g} to {:.4g}.", peek->format, peek->extent.x, peek->extent.y,
                                       peek->extent.z > 1 ? std::format(" x {}", peek->extent.z) : std::string(), picture.least, picture.greatest);
                }
            } else {
                lines = ToLines(*peek);
                says = peek->stride > 0 ? std::format("{} bytes: {} of {} each.", peek->bytes.size(), peek->count == 1 ? std::string("1 element") : std::format("{} elements", peek->count), peek->stride)
                                        : std::format("{} bytes, as whole numbers of 32 bits.", peek->bytes.size());
            }
            // A picture made again, and nothing else said of it: only what shows the picture is built again. What is
            // said of it is the interface's, which is.
            if (says != looked.says || lines != looked.lines) ++_changes;
            else if (peek && peek->image) ++looked.version;
            looked.says = std::move(says);
            looked.lines = std::move(lines);
        }
    }

    std::uint64_t Session::PictureVersionOf(const std::string_view id, const bool looking) const
    {
        if (looking) { const auto it = _looks.find(id); return it == _looks.end() ? 0 : it->second.version; }
        const auto it = _thumbs.find(id);
        return it == _thumbs.end() ? 0 : it->second.version;
    }

    kor::ResourceRef<const kor::Image> Session::ThumbnailOf(const std::string_view id) const
    {
        const auto it = _thumbs.find(id);
        return it != _thumbs.end() && it->second.image.Valid() ? kor::ResourceRef<const kor::Image>(it->second.image) : kor::ResourceRef<const kor::Image>();
    }

    const std::string& Session::ThumbnailSaysOf(const std::string_view id) const
    {
        static const std::string nothing;
        const auto it = _thumbs.find(id);
        return it == _thumbs.end() ? nothing : it->second.says;
    }

    void Session::UpdateThumbnails(const float seconds)
    {
        ++_thumbFrame;
        std::erase_if(_thumbBefore, [&](const auto& old) { return old.first + 4 < _thumbFrame; });
        kgraph::Runtime& runtime = Doc().Runtime();
        // The image nodes of the page shown, but those looked at closely: their pictures are made every few frames already.
        const std::string shown = ShowsEveryPass() ? std::string() : Pass();
        std::set<std::string, std::less<>> images;
        for (const auto& node : Doc().Graph().nodes)
            if ((shown.empty() || node.pass == shown) && node.type != "Screen" && !_looks.contains(node.id) && runtime.ImageOf(node.id).Get()) images.insert(node.id);
        // And, while it is open, those of the drawer beside the page: what the passes before it made. A few dozen at most.
        if (_drawer && !shown.empty()) {
            std::size_t more = 0;
            for (const auto& id : Doc().Importable(shown)) {
                if (more >= 48) break;
                const kgraph::Node* node = Doc().Graph().Find(id);
                if (!node || node->type == "Screen" || _looks.contains(id) || !runtime.ImageOf(id).Get()) continue;
                if (images.insert(id).second) ++more;
            }
        }
        // Each is watched for as long as it is on the page, and copied out once in so many frames — each on frames of its
        // own — so that a page of images costs about a copy a frame, and nothing is built again while it is shown.
        for (auto it = _thumbWatched.begin(); it != _thumbWatched.end();) {
            if (images.contains(it->first)) { ++it; continue; }
            if (!_looks.contains(it->first) && it->first != _viewing) runtime.Unwatch(it->first);
            it = _thumbWatched.erase(it);
        }
        for (const auto& id : images) {
            if (!_thumbWatched.contains(id)) {
                // Of an image with smaller levels, the first no wider than a thumbnail needs: much less to copy.
                const auto image = runtime.ImageOf(id);
                std::uint32_t mip = 0;
                for (kor::u32 width = image->Extent().x; mip + 1 < image->MipLevels() && width > 512; width /= 2) ++mip;
                _thumbWatched[id] = _thumbFrame;
                if (mip > 0) runtime.Watch(id, mip, 0);
            }
            runtime.Watch(id);              // (another runtime — a scene opened anew — was not asked to)
            runtime.WatchEvery(id, 30);
        }
        // Pictures made now and then, a few at a time — each makes the interface be built again — of those made longest
        // ago whose first copy has had time to come.
        // One at a time, every eighth of a second: as many a second as four every half, without four in one frame.
        if ((_thumbTimer -= seconds) > 0.f) return;
        _thumbTimer = 0.125f;
        std::vector<std::pair<std::uint64_t, std::string>> due;
        for (const auto& [id, since] : _thumbWatched) {
            if (_thumbFrame - since < 4) continue;
            const auto it = _thumbs.find(id);
            due.emplace_back(it == _thumbs.end() ? 0 : it->second.made, id);
        }
        std::ranges::sort(due);
        bool changed = false;
        for (std::size_t n = 0; n < due.size() && n < 1; ++n) {
            const std::string& id = due[n].second;
            Thumbnail& thumb = _thumbs[id];
            thumb.made = _thumbFrame;
            const auto peek = runtime.Watched(id);
            if (peek && peek->image) {
                const Picture picture = ToPicture(*peek, LookMode::eAuto, 0, 256);
                if (picture.pixels.empty()) { thumb.says = std::format("{} does not read back as a picture.", peek->format); changed = true; continue; }
                auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(picture.size)
                    .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                    .SetData(std::span<const kor::U8Vec4>(picture.pixels)).Build();
                if (!image.Valid()) continue;
                // A picture where there was one, and nothing said: the card is as big as it was, and only its picture is
                // built again. A first one, or one in place of what was said, makes the card another size.
                if (!thumb.image.Valid() || !thumb.says.empty()) changed = true;
                else ++thumb.version;
                if (thumb.image.Valid()) _thumbBefore.emplace_back(_thumbFrame, std::move(thumb.image));
                thumb.image = std::move(image);
                thumb.says.clear();
            } else if (_thumbFrame - _thumbWatched[id] > 90 && !thumb.image.Valid() && thumb.says.empty()) {
                // Nothing comes of it: an image that cannot be copied, or that no pass draws into.
                thumb.says = runtime.WatchProblem(id);
                if (thumb.says.empty()) thumb.says = "Nothing has drawn into it yet.";
                changed = true;
            }
        }
        if (changed) ++_changes;
    }

    std::vector<std::string> Session::EntryPoints(const std::string& module) const
    {
        std::vector<std::string> entries;
        for (auto& entry : ShaderEntries(module)) entries.push_back(std::move(entry.name));
        return entries;
    }

    std::vector<Session::ShaderEntry> Session::ShaderEntries(const std::string& module) const
    {
        std::vector<ShaderEntry> entries;
        if (!_hasProject) return entries;
        std::ifstream in(_project.ShaderDirectory() / (module + ".slang"));
        if (!in) return entries;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // [shader("stage")], whatever other attributes follow it, a return type, and the function's name.
        static const std::regex declared(R"re(\[\s*shader\s*\(\s*"(\w+)"\s*\)\s*\]\s*(?:\[[^\]]*\]\s*)*[\w:<>,\s]+?\s+(\w+)\s*\()re");
        for (std::sregex_iterator it(text.begin(), text.end(), declared), end; it != end; ++it) {
            const auto name = text.begin() + (*it).position(2);
            entries.push_back({ (*it)[2].str(), (*it)[1].str(), 1 + static_cast<int>(std::count(text.begin(), name, '\n')) });
        }
        return entries;
    }

    std::vector<const kgraph::NodeType*> FindNodeTypes(const std::string_view typed, const std::vector<kgraph::Preset>& presets)
    {
        // A component's node as a type of its own: kept while the editor runs, so what is found stays where it is.
        static std::map<std::string, std::unique_ptr<kgraph::NodeType>, std::less<>> components;
        std::vector<const kgraph::NodeType*> offered;
        for (const auto& preset : presets) {
            const kgraph::NodeType* base = kgraph::FindType(preset.type);
            if (!base) continue;
            auto& type = components[preset.name];
            if (!type) type = std::make_unique<kgraph::NodeType>();
            *type = *base;
            type->name = preset.name;
            // A call of a function is offered under what it is for, among the functions; a component's node, among the components.
            const bool call = preset.type == "Function";
            type->category = call ? (preset.group.empty() ? "Functions" : preset.group) : "Components";
            type->doc = preset.doc;
            type->keywords = call ? std::format("function call {} {} {}", preset.group, preset.keywords, base->keywords) : std::format("{} {}", preset.component, base->keywords);
            offered.push_back(type.get());
        }
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        std::vector<std::string> words;
        for (std::size_t at = 0; at < typed.size();) {
            const auto end = typed.find(' ', at);
            const auto word = typed.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
            if (!word.empty()) words.push_back(lower(std::string(word)));
            if (end == std::string_view::npos) break;
            at = end + 1;
        }
        // Whether a word starts one of the words of a text: of "vertex fragment", of "GraphicsPipeline" at its capitals.
        const auto startsAWord = [&](const std::string& text, const std::string& word) {
            for (std::size_t at = 0; at < text.size(); ++at) {
                const bool start = at == 0 || text[at - 1] == ' ' || (std::isupper(static_cast<unsigned char>(text[at])) && !std::isupper(static_cast<unsigned char>(text[at - 1])));
                if (start && lower(text.substr(at, word.size())) == word) return true;
            }
            return false;
        };
        std::vector<std::pair<int, const kgraph::NodeType*>> found;
        std::vector<const kgraph::NodeType*> all;
        for (const auto& type : kgraph::Catalog()) all.push_back(&type);
        all.insert(all.end(), offered.begin(), offered.end());
        for (const kgraph::NodeType* each : all) {
            const kgraph::NodeType& type = *each;
            if (!type.listed) continue;
            const std::string name = lower(type.name), doc = lower(type.doc), category = lower(type.category);
            int score = 0;
            bool all = true;
            for (const auto& word : words) {
                int best = 0;
                if (name == word) best = 100;
                else if (name.starts_with(word)) best = 60;
                else if (startsAWord(type.name, word)) best = 45;
                else if (startsAWord(type.keywords, word)) best = 25;
                else if (name.find(word) != std::string::npos) best = 20;     // inside a word of it: "if" in RandomUniform is no if
                else if (category.starts_with(word)) best = 10;
                else if (doc.find(word) != std::string::npos) best = 5;
                if (best == 0) { all = false; break; }
                score += best;
            }
            if (all) found.emplace_back(score, &type);
        }
        // Best first; among equals, as the catalog has them — which with nothing typed is all of them, in its order.
        std::ranges::stable_sort(found, [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<const kgraph::NodeType*> types;
        for (const auto& [score, type] : found) types.push_back(type);
        return types;
    }

    std::vector<std::string> NodeListSections(const std::map<std::string, std::size_t>& counts, const kgraph::PassRate rate)
    {
        static const std::vector<std::string> scene { "Components", "Scene", "Values", "Math", "Random", "Vectors", "Cameras", "Logic", "Flow" };
        static const std::vector<std::string> vulkan { "Resources", "Pipelines", "Passes" };
        // What is neither is what a function is for: those first, by name, the functions that say nothing last — and on a
        // page of the fixed step, what simulates before the rest.
        std::vector<std::string> purposes;
        for (const auto& category : counts | std::views::keys)
            if (!std::ranges::contains(scene, category) && !std::ranges::contains(vulkan, category) && category != "Functions") purposes.push_back(category);
        if (rate == kgraph::PassRate::eFixed)
            std::ranges::stable_partition(purposes, [](const std::string& purpose) { return purpose == "Physics"; });
        if (counts.contains("Functions")) purposes.push_back("Functions");
        std::vector<std::string> sections;
        const auto section = [&](const std::string& heading, const std::vector<std::string>& categories) {
            const auto first = sections.size();
            for (const auto& category : categories)
                if (counts.contains(category)) sections.push_back(category);
            if (sections.size() > first) sections.insert(sections.begin() + static_cast<std::ptrdiff_t>(first), "#" + heading);
        };
        section("Functions", purposes);
        section("Scene and values", scene);
        section("Vulkan", vulkan);
        return sections;
    }
}
