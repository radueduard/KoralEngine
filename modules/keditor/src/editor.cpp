#include <keditor/editor.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <set>

#include <app.h>
#include <builderDescriptions.h>
#include <koralUI.h>
#include <kui/dock.h>
#include <kui/nodes.h>

#include <kgui/layout.h>
#include <koralFileBrowser.h>
#include <koralSceneView.h>

#include <kecs/json.h>
#include <kgraph/catalog.h>

namespace keditor
{
    using kgraph::Value;
    namespace kb = kor::builders;

    // ---- the session ------------------------------------------------------------------------------------------

    bool Session::NewProject(const std::filesystem::path& directory)
    {
        std::string error;
        Project project;
        if (!Project::Create(directory, directory.filename().string(), project, error)) { Say(error, true); return false; }
        _project = std::move(project);
        _hasProject = true;
        _scene = _project.start;
        _project.Use();
        return Save();
    }

    bool Session::OpenProject(const std::filesystem::path& directory)
    {
        std::string error;
        Project project;
        if (!Project::Open(directory, project, error)) { Say(error, true); return false; }
        project.Use();
        if (!_play.Open(project.ScenePath(project.start), error)) { Say(error, true); return false; }
        _project = std::move(project);
        _hasProject = true;
        _scene = _project.start;
        selectedNode.clear();
        selectedEntity = kecs::NullEntity;
        Say(std::format("Opened {}.", _project.name));
        return true;
    }

    bool Session::Save()
    {
        if (!_hasProject) { Say("There is no project to save it in yet: File, New project.", true); return false; }
        std::string error;
        if (!Doc().Save(_project.ScenePath(_scene), error)) { Say(error, true); return false; }
        Say(std::format("Saved {}.", _project.ScenePath(_scene).filename().string()));
        return true;
    }

    std::string Session::RunCommand() const
    {
        if (!_hasProject) return {};
        // The player that was built with this editor: beside it. Failing that, whichever is found by name.
        std::filesystem::path player = "koral-play";
        std::error_code ec;
        if (const auto self = std::filesystem::read_symlink("/proc/self/exe", ec); !ec && std::filesystem::exists(self.parent_path() / "koral-play", ec))
            player = self.parent_path() / "koral-play";
        return std::format("\"{}\" \"{}\"", player.string(), _project.directory.string());
    }

    bool Session::Run()
    {
        if (!_hasProject) { Say("There is no project to run yet: File, New project.", true); return false; }
        if (!Save()) return false;
#ifdef _WIN32
        const std::string command = "start \"\" " + RunCommand();
#else
        const std::string command = RunCommand() + " &";
#endif
        if (std::system(command.c_str()) != 0) { Say("The player could not be started.", true); return false; }
        Say(std::format("Running {}.", _project.name));
        return true;
    }

    namespace {
        constexpr float LabelWidth = 132.f;

        kui::Widget Small(std::string label, std::function<void()> onPressed)
        {
            return kui::Button(std::move(label), std::move(onPressed),
                               kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(8.f, 2.f)));
        }
        kui::Widget Heading(std::string text)
        {
            kui::TextStyle style;
            style.size = 15.f;
            style.weight = 700;
            return kui::Text(std::move(text), style);
        }
        kui::Widget Problem(std::string text)
        {
            return kui::Text(std::move(text), kui::TextStyle {}.SetSize(13.f).SetColor(kui::Color::Hex(0xFF6B6B)));
        }
        kui::Widget Rows(std::vector<kui::Widget> children, const float gap = 6.f)
        {
            return kui::Column(std::move(children), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(gap));
        }
        kui::Widget Panel(std::vector<kui::Widget> children)
        {
            return kui::ScrollView(kui::Padding(kui::EdgeInsets::All(10.f), Rows(std::move(children), 8.f)));
        }

        float NumberOf(const Value& value, const float fallback = 0.f)
        {
            if (const double* number = value.Number()) return static_cast<float>(*number);
            if (const bool* flag = value.Bool()) return *flag ? 1.f : 0.f;
            return fallback;
        }

        /** A value typed as JSON: for what has no editor of its own. Nothing typed is no value. */
        kui::Widget JsonField(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            const std::string text = value.IsNull() ? std::string() : value.ToJson();
            return kui::TextField(kui::TextFieldOptions { .text = text, .placeholder = "as JSON",
                .onSubmitted = [set](const std::string& typed) {
                    if (typed.empty()) { set(Value {}); return; }
                    kecs::json::Value parsed;
                    std::string error;
                    if (kecs::json::Parse(typed, parsed, error)) set(Value::FromJson(parsed));
                } }).Key(key + "=" + text);
        }

        /** An editor for a value of a builder's type, whatever it is: what a node's inspector is made of. */
        kui::Widget TypeEditor(const kb::Type& type, const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            using kb::Kind;
            switch (type.kind) {
            case Kind::eBool:
                return kui::Checkbox(value.Bool() && *value.Bool(), [set](const bool on) { set(Value(on)); });
            case Kind::eInt:
            case Kind::eUInt:
                return kui::DragValue(NumberOf(value), [set](const float v) { set(Value(std::round(v))); },
                                      kui::DragValueOptions {}.SetSpeed(1.f).SetDecimals(0).SetRange(type.kind == Kind::eUInt ? 0.f : -kui::Infinity, kui::Infinity));
            case Kind::eFloat:
                return kui::DragValue(NumberOf(value), [set](const float v) { set(Value(v)); });
            case Kind::eString: {
                const std::string text = value.String() ? *value.String() : std::string();
                return kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
            }
            case Kind::eEnum:
            case Kind::eChoice: {
                std::vector<std::string> names;
                int selected = -1;
                if (type.enumeration)
                    for (const auto& entry : type.enumeration->values) {
                        if ((value.String() && *value.String() == entry.name) || (value.Number() && static_cast<std::uint64_t>(*value.Number()) == entry.value))
                            selected = static_cast<int>(names.size());
                        names.emplace_back(entry.name);
                    }
                return kui::Dropdown(names, selected, [set, names](const int index) { set(Value(names[static_cast<std::size_t>(index)])); },
                                     kui::DropdownOptions {}.SetPlaceholder("(as it is by default)"));
            }
            case Kind::eFlags: {
                std::set<std::string> on;
                if (const auto* items = value.Items()) { for (const auto& item : *items) if (item.String()) on.insert(*item.String()); }
                else if (value.String()) on.insert(*value.String());
                std::vector<kui::Widget> boxes;
                if (type.enumeration)
                    for (const auto& entry : type.enumeration->values) {
                        const std::string name(entry.name);
                        boxes.push_back(kui::Checkbox(on.contains(name), [set, on, name](const bool checked) {
                            auto changed = on;
                            if (checked) changed.insert(name); else changed.erase(name);
                            set(Value(Value::List(changed.begin(), changed.end())));
                        }, name));
                    }
                return kui::Column(std::move(boxes), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(2.f));
            }
            case Kind::eArray: {
                if (!type.element || (type.element->kind != Kind::eFloat && type.element->kind != Kind::eInt && type.element->kind != Kind::eUInt)) break;
                std::vector<float> numbers(type.length, 0.f);
                if (const auto* items = value.Items())
                    for (std::size_t i = 0; i < numbers.size() && i < items->size(); ++i) numbers[i] = NumberOf((*items)[i]);
                return kgui::DragFloats(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); });
            }
            case Kind::eStruct: {
                std::vector<kui::Widget> rows;
                for (const auto& field : type.fields) {
                    const std::string name(field.name);
                    const Value* member = value.Find(name);
                    const auto setMember = [set, value, name](Value changed) {
                        Value::Members members = value.Named() ? *value.Named() : Value::Members {};
                        const auto known = std::ranges::find_if(members, [&](const auto& m) { return m.first == name; });
                        if (known != members.end()) known->second = std::move(changed);
                        else members.emplace_back(name, std::move(changed));
                        set(Value(std::move(members)));
                    };
                    rows.push_back(kgui::Labeled(name, TypeEditor(*field.type, member ? *member : Value {}, setMember, key + "." + name), 110.f));
                }
                return Rows(std::move(rows), 4.f);
            }
            case Kind::eList: {
                if (!type.element) break;
                const Value::List items = value.Items() ? *value.Items() : Value::List {};
                std::vector<kui::Widget> rows;
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const auto setItem = [set, items, i](Value changed) { auto list = items; list[i] = std::move(changed); set(Value(std::move(list))); };
                    rows.push_back(kui::Row({ kui::Expanded(TypeEditor(*type.element, items[i], setItem, std::format("{}[{}]", key, i))),
                                              Small("-", [set, items, i] { auto list = items; list.erase(list.begin() + static_cast<std::ptrdiff_t>(i)); set(Value(std::move(list))); }) },
                                            kui::FlexOptions {}.SetGap(4.f)));
                }
                rows.push_back(Small("+ one more", [set, items, element = type.element] {
                    auto list = items;
                    list.push_back(element->kind == Kind::eStruct ? Value(Value::Members {}) : element->kind == Kind::eString ? Value(std::string()) : Value(0));
                    set(Value(std::move(list)));
                }));
                return Rows(std::move(rows), 4.f);
            }
            default: break;
            }
            return JsonField(value, set, key);
        }

        /** An editor for an input of one of the engine's own nodes, which says only what its type is called. */
        kui::Widget NamedEditor(const std::string& type, const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            if (type == "number") return kui::DragValue(NumberOf(value), [set](const float v) { set(Value(v)); });
            if (type == "bool") return kui::Checkbox(value.Bool() && *value.Bool(), [set](const bool on) { set(Value(on)); });
            if (type == "string") {
                const std::string text = value.String() ? *value.String() : std::string();
                return kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
            }
            return JsonField(value, set, key);
        }

        /** A node's inputs as they stand: one given more than once is there once for each it has, and once more. */
        std::vector<kgraph::Port> PortsNow(const kgraph::Runtime& runtime, const kgraph::Node& node)
        {
            std::vector<kgraph::Port> ports;
            for (const auto& port : runtime.PortsOf(node.id)) {
                const auto at = port.name.find("[]");
                if (at == std::string::npos) { ports.push_back(port); continue; }
                const std::string prefix = port.name.substr(0, at + 1);
                int count = 0;
                for (const auto& name : node.inputs | std::views::keys)
                    if (name.starts_with(prefix)) count = std::max(count, std::atoi(name.c_str() + prefix.size()) + 1);
                for (int i = 0; i <= count; ++i) {
                    kgraph::Port each = port;
                    each.name = std::format("{}{}]{}", prefix, i, port.name.substr(at + 2));
                    each.repeatable = false;
                    ports.push_back(std::move(each));
                }
            }
            return ports;
        }

        /** Whether an input shows as a port on its node: what takes a resource, and a value that is wired or was asked to. */
        bool ShowsAsPort(const kgraph::Port& port, const kgraph::Node& node)
        {
            if (port.kind == kgraph::PortKind::eResource) return true;
            const auto input = node.inputs.find(port.name);
            return input != node.inputs.end() && (input->second.link || input->second.value.IsNull());
        }

        kui::Color CategoryColor(const std::string& category)
        {
            if (category == "Pipelines") return kui::Color::Hex(0x7A4BC2);
            if (category == "Passes") return kui::Color::Hex(0xC2643B);
            if (category == "Values") return kui::Color::Hex(0x3B7FC2);
            return kui::Color::Hex(0x3B9C7A);
        }

        // ---- the root -------------------------------------------------------------------------------------------

        class Root final : public kui::StatefulWidget {
        public:
            explicit Root(std::shared_ptr<Session> session) : _session(std::move(session))
            {
                _layout = std::make_shared<kui::DockLayout>();
                _layout->Dock("graph")
                        .Dock("scene", kui::DockSide::eRight, "graph", 0.34f)
                        .Dock("inspector", kui::DockSide::eBottom, "scene", 0.55f)
                        .Dock("outliner", kui::DockSide::eLeft, "graph", 0.16f)
                        .Dock("problems", kui::DockSide::eBottom, "graph", 0.22f);
            }

            void InitState() override
            {
                // The document changes under the panels — an edit here, a shader saved elsewhere: built again when it has.
                Animate([this](float) {
                    if (const auto version = _session->Version(); version != _shown) SetState([&] { _shown = version; });
                    return true;
                });
            }

            kui::Widget Build() override
            {
                _shown = _session->Version();
                auto body = kui::Column({
                    Menus(),
                    kui::Expanded(kui::DockSpace(_layout, {
                        { "graph", "Graph", Graph(), false },
                        { "scene", "Scene", kgui::SceneView(_session->Play()), false },
                        { "outliner", "Outliner", Outliner(), false },
                        { "inspector", "Inspector", Inspector(), false },
                        { "problems", "Problems", Problems(), false },
                    })),
                    kui::StatusBar(_session->Status(), _session->Failed() ? kui::StatusLevel::eError : kui::StatusLevel::eInfo),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetMainAxisSize(kui::MainAxisSize::eMax));
                return kui::Modal(_dialog != Dialog::eNone, std::move(body), DialogBody(), [this] { SetState([&] { _dialog = Dialog::eNone; }); });
            }

        private:
            enum class Dialog { eNone, eAddNode, eNewProject, eOpenProject };

            Document& Doc() { return _session->Doc(); }

            // ---- menus and dialogs ------------------------------------------------------------------------------

            kui::Widget Menus()
            {
                return kui::MenuBar({
                    { "File", {
                        { "New project...", [this] { SetState([&] { _dialog = Dialog::eNewProject; }); } },
                        { "Open project...", [this] { SetState([&] { _dialog = Dialog::eOpenProject; }); } },
                        { .separator = true },
                        { "Save scene", [this] { if (!_session->HasProject()) SetState([&] { _dialog = Dialog::eNewProject; }); else _session->Save(); } },
                    } },
                    { "Project", {
                        { "Run", [this] { if (!_session->HasProject()) SetState([&] { _dialog = Dialog::eNewProject; }); else _session->Run(); } },
                    } },
                    { "Graph", {
                        { "Add node...", [this] { SetState([&] { _dialog = Dialog::eAddNode; _addAt = { 80.f, 80.f }; _filter.clear(); _newId.clear(); }); } },
                    } },
                    { "Entity", {
                        { "New entity", [this] { _session->Select(Doc().CreateEntity()); } },
                        { "New tag...", [this] { SetState([&] { _dialog = Dialog::eNone; _session->Say("Give the tag a name in the inspector of an entity."); }); } },
                    } },
                });
            }

            kui::Widget DialogBody()
            {
                switch (_dialog) {
                case Dialog::eAddNode: return AddNodeDialog();
                case Dialog::eNewProject:
                    return kgui::FileBrowser({ .directories = true, .createDirectories = true, .confirm = "Make the project here" },
                        [this](const std::filesystem::path& chosen) { _session->NewProject(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                case Dialog::eOpenProject:
                    return kgui::FileBrowser({ .directories = true, .confirm = "Open" },
                        [this](const std::filesystem::path& chosen) { _session->OpenProject(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                default: return {};
                }
            }

            /** Every kind of node, under its category, narrowed by what is typed. */
            kui::Widget AddNodeDialog()
            {
                std::string lowered = _filter;
                std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::map<std::string, std::vector<kui::Widget>> byCategory;
                for (const auto& type : kgraph::Catalog()) {
                    std::string name = type.name;
                    std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (!lowered.empty() && name.find(lowered) == std::string::npos) continue;
                    byCategory[type.category].push_back(kui::Tooltip(type.doc, kui::Selectable(type.name, false, [this, name = type.name] {
                        const std::string id = Doc().AddNode(name, _addAt.x, _addAt.y, _newId);
                        _session->Select(id);
                        _session->Say(std::format("Added {}.", id));
                        SetState([&] { _dialog = Dialog::eNone; });
                    })));
                }
                std::vector<kui::Widget> columns;
                for (auto& [category, items] : byCategory) {
                    items.insert(items.begin(), kgui::Muted(category));
                    columns.push_back(kui::Column(std::move(items), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(2.f)).Width(190.f));
                }
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading("Add a node"),
                    kui::Row({
                        kui::TextField(kui::TextFieldOptions { .text = _filter, .placeholder = "Find a kind of node",
                                                               .onChanged = [this](const std::string& typed) { SetState([&] { _filter = typed; }); }, .focus = 1 }),
                        kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name (a pipeline's names its component)",
                                                               .onChanged = [this](const std::string& typed) { _newId = typed; }, .width = 320.f }),
                    }, kui::FlexOptions {}.SetGap(8.f)),
                    kui::Row(std::move(columns), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(12.f)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(12.f)));
            }

            // ---- the graph --------------------------------------------------------------------------------------

            kui::Widget Graph()
            {
                const kgraph::Graph& graph = Doc().Graph();
                const kgraph::Runtime& runtime = Doc().Runtime();
                kui::NodeGraph shown;
                for (const auto& node : graph.nodes) {
                    const kgraph::NodeType* type = kgraph::FindType(node.type);
                    kui::GraphNode card;
                    card.id = node.id;
                    card.title = node.title.empty() ? node.id : node.title;
                    card.position = { static_cast<float>(node.x), static_cast<float>(node.y) };
                    card.accent = CategoryColor(type ? type->category : std::string());
                    for (const auto& port : PortsNow(runtime, node))
                        if (ShowsAsPort(port, node))
                            card.inputs.push_back({ port.name, port.name, (port.kind == kgraph::PortKind::eResource ? "r:" : "v:") + port.type });
                    if (type)
                        for (const auto& output : type->outputs)
                            card.outputs.push_back({ output.name, output.name == "out" ? output.type : output.name,
                                                     (output.kind == kgraph::PortKind::eResource ? "r:" : "v:") + output.type });
                    if (const auto* status = runtime.Status(node.id); status && !status->ok) card.error = status->error;
                    card.body = kui::Padding(kui::EdgeInsets::Symmetric(8.f, 2.f), kgui::Muted(node.type, 11.f));
                    shown.nodes.push_back(std::move(card));
                    for (const auto& [name, input] : node.inputs)
                        if (input.link) shown.wires.push_back({ { input.link->node, input.link->output }, { node.id, name } });
                }
                return kui::NodeEditor(std::move(shown), kui::NodeEditorOptions {}
                    .CanConnect([](const kui::NodePort& from, const kui::NodePort& to) {
                        // A resource to what takes one of its kind (a view of an image where an image goes), a value to a value.
                        if (from.type.substr(0, 2) != to.type.substr(0, 2)) return false;
                        if (from.type.starts_with("v:")) return true;
                        return from.type == to.type || from.type == "r:Resource" || to.type == "r:Resource" || (from.type == "r:ImageView" && to.type == "r:Image");
                    })
                    .OnConnect([this](const kui::GraphWire& wire) { Doc().Connect(wire.from.node, wire.from.port, wire.to.node, wire.to.port); })
                    .OnDisconnect([this](const kui::GraphWire& wire) { Doc().ClearInput(wire.to.node, wire.to.port); })
                    .OnMove([this](const std::vector<std::string>& nodes, const kor::Vec2 by) { Doc().MoveNodes(nodes, by.x, by.y); })
                    .OnDelete([this](const std::vector<std::string>& nodes, const std::vector<kui::GraphWire>& wires, const std::vector<std::string>&) {
                        for (const auto& wire : wires) Doc().ClearInput(wire.to.node, wire.to.port);
                        if (!nodes.empty()) Doc().RemoveNodes(nodes);
                        if (std::ranges::find(nodes, _session->selectedNode) != nodes.end()) _session->Select(std::string());
                    })
                    .OnSelectionChanged([this](const std::vector<std::string>& nodes) { if (!nodes.empty()) _session->Select(nodes.front()); })
                    .OnContextMenu([this](const kor::Vec2 at, kor::Vec2) { SetState([&] { _dialog = Dialog::eAddNode; _addAt = at; _filter.clear(); _newId.clear(); }); }));
            }

            // ---- the outliner -----------------------------------------------------------------------------------

            kui::Widget Outliner()
            {
                std::vector<kui::Widget> rows;
                rows.push_back(Small("+ Entity", [this] { _session->Select(Doc().CreateEntity()); }));
                for (const kecs::Entity entity : Doc().Entities())
                    rows.push_back(kui::Selectable(Doc().NameOf(entity), entity == _session->selectedEntity, [this, entity] { _session->Select(entity); }));
                if (rows.size() == 1) rows.push_back(kgui::Muted("No entities yet."));
                return Panel(std::move(rows));
            }

            // ---- the inspector ----------------------------------------------------------------------------------

            kui::Widget Inspector()
            {
                if (Doc().World().Alive(_session->selectedEntity)) return EntityInspector(_session->selectedEntity);
                if (const kgraph::Node* node = Doc().Graph().Find(_session->selectedNode)) return NodeInspector(*node);
                return Panel({ kgui::Muted("Pick a node or an entity to see what it is made of.") });
            }

            kui::Widget NodeInspector(const kgraph::Node& node)
            {
                const kgraph::Runtime& runtime = Doc().Runtime();
                const std::string id = node.id;
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(id));
                rows.push_back(kgui::Muted(node.type));
                if (const auto* status = runtime.Status(id); status && !status->ok) rows.push_back(Problem(status->error));
                if (const auto* report = runtime.ReportOf(id)) {
                    rows.push_back(kgui::Muted(std::format("Runs for {} {}.", report->ran, report->ran == 1 ? "entity" : "entities")));
                    if (report->skipped > 0) rows.push_back(Problem(std::format("{} passed by. {}", report->skipped, report->why)));
                    else if (!report->why.empty()) rows.push_back(Problem(report->why));
                }
                if (const kecs::ComponentId component = runtime.ComponentOf(id); component != kecs::NoComponent)
                    rows.push_back(kgui::Muted(std::format("Entities run it by having the component '{}'.", Doc().World().Describe(component).name)));
                rows.push_back(kui::Separator());

                for (const auto& port : PortsNow(runtime, node)) {
                    const auto input = node.inputs.find(port.name);
                    const bool given = input != node.inputs.end();
                    const std::string name = port.name;
                    kui::Widget editor;
                    if (given && input->second.link) {
                        editor = kgui::Line(std::format("from {}{}", input->second.link->node, input->second.link->output == "out" ? "" : "." + input->second.link->output));
                    } else if (port.kind == kgraph::PortKind::eResource) {
                        editor = kgui::Muted("wire one in");
                    } else if (given && input->second.value.IsNull()) {
                        editor = kgui::Muted("a port: wire a value in");
                    } else {
                        const Value value = given ? input->second.value : Value {};
                        const auto set = [this, id, name](Value changed) {
                            if (changed.IsNull()) Doc().ClearInput(id, name);
                            else Doc().SetInput(id, name, std::move(changed));
                        };
                        const std::string key = id + "/" + name;
                        editor = port.builder ? TypeEditor(*port.builder, value, set, key) : NamedEditor(port.type, value, set, key);
                    }
                    std::vector<kui::Widget> line { kui::Expanded(std::move(editor)) };
                    if (given) line.push_back(kui::Tooltip("Back to nothing given", Small("x", [this, id, name] { Doc().ClearInput(id, name); })));
                    else if (port.kind == kgraph::PortKind::eValue)
                        line.push_back(kui::Tooltip("Show it as a port, to wire a value to", Small("o", [this, id, name] { Doc().SetInput(id, name, Value {}); })));
                    rows.push_back(kui::Tooltip(port.doc.empty() ? port.type : port.doc + " (" + port.type + ")",
                                                kgui::Labeled(name, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)), LabelWidth)));
                }
                rows.push_back(kui::Separator());
                rows.push_back(Small("Remove the node", [this, id] { Doc().RemoveNodes({ id }); _session->Select(std::string()); }));
                return Panel(std::move(rows));
            }

            kui::Widget EntityInspector(const kecs::Entity entity)
            {
                Document& doc = Doc();
                std::vector<kui::Widget> rows;
                const std::string name = doc.NameOf(entity);
                rows.push_back(kgui::Labeled("Name", kui::TextField(kui::TextFieldOptions { .text = name,
                    .onSubmitted = [this, entity](const std::string& typed) { Doc().Rename(entity, typed); } }).Key("name=" + name), LabelWidth));

                const auto has = doc.ComponentsOf(entity);
                for (const auto& component : has) {
                    rows.push_back(kui::Separator());
                    rows.push_back(kui::Row({
                        kui::Tooltip("Switched off, it has the component still and nothing runs for it",
                                     kui::Checkbox(doc.IsEnabled(entity, component), [this, entity, component](const bool on) { Doc().SetEnabled(entity, component, on); })),
                        kui::Expanded(Heading(component)),
                        Small("Remove", [this, entity, component] { Doc().RemoveComponent(entity, component); }),
                    }, kui::FlexOptions {}.SetGap(6.f)));
                    const auto& type = doc.World().Describe(doc.World().Find(component));
                    for (const auto& field : type.fields) {
                        const std::string fieldName = field.name;
                        const Value value = doc.Field(entity, component, fieldName);
                        const auto set = [this, entity, component, fieldName](const Value& changed) {
                            std::string error;
                            if (!Doc().SetField(entity, component, fieldName, changed, &error)) _session->Say(error, true);
                        };
                        kui::Widget editor;
                        if (HoldsResource(field.type)) {
                            std::vector<std::string> choices { "(nothing)" };
                            int selected = 0;
                            for (const auto& node : doc.NodesFor(field.type)) {
                                if (value.String() && *value.String() == node) selected = static_cast<int>(choices.size());
                                choices.push_back(node);
                            }
                            editor = kui::Dropdown(choices, selected, [set, choices](const int index) {
                                set(index <= 0 ? Value {} : Value(choices[static_cast<std::size_t>(index)]));
                            });
                        } else if (!Editable(field.type)) {
                            editor = kgui::Muted(field.type);
                        } else if (value.Bool()) {
                            editor = kui::Checkbox(*value.Bool(), [set](const bool on) { set(Value(on)); });
                        } else if (value.String()) {
                            const std::string text = *value.String();
                            editor = kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } })
                                .Key(component + "." + fieldName + "=" + text);
                        } else if (const auto* items = value.Items()) {
                            std::vector<float> numbers;
                            for (const auto& item : *items) numbers.push_back(NumberOf(item));
                            const bool whole = field.type.starts_with("i") || field.type.starts_with("u") || field.type.starts_with("b");
                            editor = kgui::DragFloats(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); },
                                                      whole ? 1.f : 0.01f, whole ? 0 : 2);
                        } else {
                            const bool whole = field.type.starts_with("i") || field.type.starts_with("u");
                            editor = kui::DragValue(NumberOf(value), [set](const float v) { set(Value(v)); },
                                                    kui::DragValueOptions {}.SetSpeed(whole ? 1.f : 0.01f).SetDecimals(whole ? 0 : 2));
                        }
                        rows.push_back(kui::Tooltip(field.type, kgui::Labeled(fieldName, std::move(editor), LabelWidth)));
                    }
                }

                rows.push_back(kui::Separator());
                std::vector<std::string> more;
                for (const auto& component : doc.ComponentNames())
                    if (std::ranges::find(has, component) == has.end()) more.push_back(component);
                rows.push_back(kgui::Labeled("Add", kui::Dropdown(more, -1, [this, entity, more](const int index) {
                    Doc().AddComponent(entity, more[static_cast<std::size_t>(index)]);
                }, kui::DropdownOptions {}.SetPlaceholder("a component, or a tag")), LabelWidth));
                rows.push_back(kgui::Labeled("New tag", kui::TextField(kui::TextFieldOptions { .placeholder = "its name, then Enter",
                    .onSubmitted = [this, entity](const std::string& typed) {
                        if (typed.empty()) return;
                        Doc().AddTag(typed);
                        Doc().AddComponent(entity, typed);
                    } }).Key(std::format("tag{}", has.size())), LabelWidth));
                rows.push_back(Small("Destroy the entity", [this, entity] { Doc().DestroyEntity(entity); _session->Select(kecs::NullEntity); }));
                return Panel(std::move(rows));
            }

            // ---- what is wrong ----------------------------------------------------------------------------------

            kui::Widget Problems()
            {
                const kgraph::Runtime& runtime = Doc().Runtime();
                std::vector<kui::Widget> rows;
                for (const auto& problem : runtime.Problems()) {
                    const std::string node = problem.substr(0, problem.find(':'));
                    rows.push_back(Problem(problem).OnTap([this, node] { _session->Select(node); }));
                }
                for (const auto& node : Doc().Graph().nodes)
                    if (const auto* report = runtime.ReportOf(node.id); report && (report->skipped > 0 || (report->ran == 0 && !report->why.empty())))
                        rows.push_back(Problem(report->skipped > 0 ? std::format("{}: {} passed by. {}", node.id, report->skipped, report->why)
                                                                   : std::format("{}: {}", node.id, report->why)).OnTap([this, id = node.id] { _session->Select(id); }));
                for (const auto& note : Doc().Notes()) rows.push_back(kgui::Muted(note));
                if (rows.empty()) rows.push_back(kgui::Muted("Nothing is wrong."));
                return Panel(std::move(rows));
            }

            std::shared_ptr<Session> _session;
            std::shared_ptr<kui::DockLayout> _layout;
            std::uint64_t _shown = 0;
            Dialog _dialog = Dialog::eNone;
            kor::Vec2 _addAt {};
            std::string _filter, _newId;
        };
    }

    kui::Widget EditorRoot(std::shared_ptr<Session> session) { return kui::Make<Root>(std::move(session)); }

    // ---- the scene ----------------------------------------------------------------------------------------------

    EditorScene::EditorScene(std::filesystem::path project) : _projectPath(std::move(project)) {}
    EditorScene::EditorScene(const kor::SceneArgs& arguments) : _projectPath(arguments.String("project")) {}
    EditorScene::~EditorScene() = default;

    void EditorScene::Initialize()
    {
        // The scene being worked on runs as it would on its own, into an image a panel shows.
        _play = kor::App::Current().OpenOffscreen<PlayScene>({ .title = "Scene", .extent = { 640, 360 } });
        _session = std::make_shared<Session>(*_play);
        Document::Alias(&Graph(), &_play->Graph());     // whoever is handed this scene is after the one it edits
        if (!_projectPath.empty()) _session->OpenProject(_projectPath);
        _ui = std::make_unique<kui::Ui>(EditorRoot(_session));
        Graph().Add<kui::UiPass>(*_ui);
    }

    void EditorScene::Update()
    {
        _ui->Update();
        // Control+S saves, Control+R runs — unless a field is being typed into.
        const kor::Input& input = SceneInput();
        if (!input.InterfaceWantsKeyboard() && (input.IsKeyHeld(kor::Key::eLeftControl) || input.IsKeyHeld(kor::Key::eRightControl))) {
            if (input.IsKeyPressed(kor::Key::eS)) _session->Save();
            if (input.IsKeyPressed(kor::Key::eR)) _session->Run();
        }
    }

    void EditorScene::Shutdown()
    {
        _ui.reset();
        _session.reset();
        Document::Alias(&Graph(), nullptr);
        if (_play) kor::App::Current().Close(*_play);
        _play = nullptr;
    }
}
