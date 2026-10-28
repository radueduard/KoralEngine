//
// keditor: the editor. A window of panels over one scene: its pass graph as nodes, the scene itself as it runs,
// its entities, and whatever is picked, to change. Everything it does, it does through a Document — and so does
// keeping it honest in a test.
//

#pragma once

#include <kui/nodes.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <kui/widgets.h>
#include <debugDraw.h>
#include <koralViewport.h>
#include <scene.h>

#include <kgraph/catalog.h>

#include "document.h"
#include "look.h"
#include "play.h"
#include "project.h"

namespace keditor
{
    /**
     * @brief The editor's own view of the scene: it circles a target from a distance, turned about the up axis (yaw)
     *        and tilted (pitch; negative looks down), in degrees. Not an entity: it is how the scene is looked at.
     */
    struct EditorCamera {
        kor::Vec3 target { 0.f };
        float yaw = 34.f, pitch = -22.f, distance = 4.f;
        float fieldOfView = 60.f, near = 0.05f, far = 1000.f;
        /// Seen straight on: no nearer thing bigger, as tall as the target is far — a plan, an elevation.
        bool orthographic = false;

        [[nodiscard]] kor::Vec3 Position() const;
        [[nodiscard]] kgraph::View View() const;
        /** @brief Turns round the target as a drag of so many pixels says. */
        void Orbit(float dx, float dy);
        /** @brief Moves sideways and up with the drag — what is under the pointer stays under it — in a view so many pixels tall. */
        void Pan(float dx, float dy, float height);
        /** @brief Closer for a scroll up, further for one down. */
        void Zoom(float steps);
        /** @brief Looks at what @p world is the transform of, from far enough to see all of a unit box of it. */
        void Frame(const kecs::Mat4& world);
    };

    /** @brief Something to make entities from: an entity as Document::WriteEntity writes it, under a name. */
    struct ObjectTemplate {
        std::string name, description;
        kgraph::Value data;
        std::filesystem::path file;
    };

    /**
     * @brief What runs @p project with its scripts — in the editor, with @p edit; on its own otherwise: its own Gradle
     *        build for scripts in Kotlin, the C# runner for scripts in C#. Empty for a project with none.
     */
    [[nodiscard]] std::string ScriptRunner(const Project& project, bool edit);
    /**
     * @brief Before any window is opened: the editor on @p project through its scripts' runner, when it has scripts —
     *        in a process forked for it, in this one's terminal, waited for. What it exited with; nothing for a project
     *        with no scripts, or one this program was itself started for by that runner.
     */
    [[nodiscard]] std::optional<int> RunThroughScripts(const Project& project);

    /** @brief What a new shader of a kind has besides what every one of its kind has. Each is for the kinds it names. */
    struct ShaderChoice {
        bool wholePicture = false;      ///< Vertex: one triangle over the whole picture, reading another — no mesh — in place of each entity's mesh.
        bool tessellation = false;      ///< Vertex: tessellation control and evaluation shaders, each triangle cut finer.
        bool geometry = false;          ///< Vertex: a geometry shader, each triangle passed on.
        bool task = false;              ///< MeshShading: a task shader, saying how many mesh shader groups there are.
        bool fragment = true;           ///< Vertex, MeshShading: a fragment shader. Without, only depth is drawn: a shadow map, a depth prepass.
        bool anyHit = false;            ///< RayTracing: an any hit shader in the hit group, that can let a ray pass on.
        bool intersection = false;      ///< RayTracing: an intersection shader, for what is no triangles: a sphere in each box.
    };

    /** @brief What the editor's panels share: the project, the scene being worked on, and what is picked. */
    class Session {
    public:
        /** @brief Works on the scene @p play runs. It must outlive the session. */
        explicit Session(PlayScene& play) : _play(play) {}

        [[nodiscard]] Document& Doc() { return _play.Doc(); }
        [[nodiscard]] PlayScene& Play() { return _play; }
        /** @brief The scene as it plays, seen from its own camera: the Game panel's. Null in an editor with none. */
        [[nodiscard]] GameScene* Game() { return _game; }
        void SetGame(GameScene* game) { _game = game; }
        /** @brief The scene's second view, beside the first while the view is split: a GameScene seen from secondCamera. */
        [[nodiscard]] GameScene* SecondView() { return _second; }
        void SetSecondView(GameScene* second) { _second = second; }
        [[nodiscard]] bool HasProject() const { return _hasProject; }
        [[nodiscard]] const Project& CurrentProject() const { return _project; }
        [[nodiscard]] const std::string& SceneName() const { return _scene; }

        /** @brief Makes a project in @p directory and saves the scene being worked on as its first. */
        bool NewProject(const std::filesystem::path& directory);
        /** @brief Opens the project in @p directory, and the scene it starts with. */
        bool OpenProject(const std::filesystem::path& directory);
        /** @brief Saves the scene to the project. False without one: a project is made first. */
        bool Save();
        /**
         * @brief Saves, and runs the project on its own: `koral-play`, from beside this program, in a window of its
         *        own. False without a project, or when the scene could not be saved.
         */
        bool Run();
        /**
         * @brief What Run runs: whatever runs the project with its scripts — its own Gradle build for scripts in
         *        Kotlin, the C# runner for scripts in C# — or the player, for a project with none. Empty without a project.
         */
        [[nodiscard]] std::string RunCommand() const;
        /** @brief The same, for the project in the editor: empty when it has no scripts, and so needs nothing but this editor. */
        [[nodiscard]] std::string EditCommand() const;
        /**
         * @brief The project's scripts that nothing is running: files in its `scripts` folder whose class is no
         *        component with its functions in place. What an editor started without the scripts' host has —
         *        the scene plays, and they do nothing.
         */
        [[nodiscard]] std::vector<std::string> ScriptsNotRunning();
        /** @brief Saves, starts the editor again on this project with its scripts, and closes this one. */
        bool ReopenWithScripts();
        /** @brief One thing wrong now: what is said of it, the node it is of (when it is a node's), and whether it is only a warning. */
        struct Problem { std::string text, node; bool warning = false; };
        /**
         * @brief What is wrong now: the graph's problems and what its passes could not run for, the project's scripts
         *        that nothing runs, and what was noted reading the scene. The editor logs each as it comes and as it goes.
         */
        [[nodiscard]] std::vector<Problem> Problems();
        /**
         * @brief The project whose scripts the program this editor runs in runs: what a scripts' runner opened the
         *        editor for. Empty in `koral-editor`, which runs none.
         */
        void HostScripts(std::filesystem::path project) { _scriptsHost = std::move(project); }
        /** @brief Whether this editor runs the scripts of the project that is open. */
        [[nodiscard]] bool HostsScripts() const;
        /**
         * @brief Whether a project with scripts this editor does not run is opened again by their runner as soon as
         *        it is opened (ReopenWithScripts), rather than edited here with its scripts doing nothing. The
         *        editor's program says so; a test does not.
         */
        bool handOver = false;
        /** @brief Between frames: hands the project over to its scripts' runner when opening it asked to. */
        void UpdateHandOver();

        /**
         * @brief Makes an input a node can do without one the node shows: asked for, and not yet given. What a node
         *        shows of its own accord is what it cannot do without, and whatever it has been given.
         */
        void Show(const std::string& node, const std::string& input) { _asked[node].insert(input); ++_changes; }
        void Hide(const std::string& node, const std::string& input) { _asked[node].erase(input); ++_changes; }
        /** @brief Whether a part of a node's inspector (Port::group) is open: as it was left, or — never opened nor closed — @p otherwise. */
        [[nodiscard]] bool SectionOpen(const std::string& node, const std::string& group, const bool otherwise) const
        {
            const auto it = _sections.find(node);
            if (it == _sections.end()) return otherwise;
            const auto open = it->second.find(group);
            return open == it->second.end() ? otherwise : open->second;
        }
        void OpenSection(const std::string& node, const std::string& group, const bool open) { _sections[node][group] = open; ++_changes; }
        [[nodiscard]] bool Asked(const std::string& node, const std::string& input) const
        {
            const auto it = _asked.find(node);
            return it != _asked.end() && it->second.contains(input);
        }
        /** @brief The project's shader modules, by name: the .slang files of its shaders folder. Empty without a project. */
        [[nodiscard]] std::vector<std::string> ShaderModules() const;
        /** @brief The entry points a module of the project declares, in the order it does. */
        [[nodiscard]] std::vector<std::string> EntryPoints(const std::string& module) const;
        /** @brief An entry point of a module, and the stage its `[shader("...")]` declares it for. */
        struct ShaderEntry {
            std::string name;
            std::string stage;
            int line = 0;           ///< Where its function is declared, from 1.
        };
        /** @brief The entry points a module of the project declares, each with its stage, in the order it does. */
        [[nodiscard]] std::vector<ShaderEntry> ShaderEntries(const std::string& module) const;

        // ---- the scene view ---------------------------------------------------------------------------------

        EditorCamera camera;
        /**
         * Whether the scene view shows what the scene's camera sees, rather than the editor's own view. Playing or not:
         * what it looks like played is the Game panel's.
         */
        bool lookThroughCamera = false;
        [[nodiscard]] bool ThroughSceneCamera() const { return lookThroughCamera; }
        bool gizmoEnabled = true;
        kor::GizmoMode gizmo = kor::GizmoMode::eTranslate;
        void SetGizmo(const bool enabled, const kor::GizmoMode mode) { gizmoEnabled = enabled; gizmo = mode; ++_changes; }
        /**
         * The picked entity's Collider, as its inspector asks: drawn in the scene's view as the physics will have it —
         * and, being changed, sized by a gizmo of its own in place of the one that moves the entity. Only ever the
         * picked one's.
         */
        bool colliderShown = false, colliderEditing = false;
        void ShowCollider(const bool shown, const bool editing) { colliderShown = shown || editing; colliderEditing = editing; ++_changes; }
        void SetLookThroughCamera(const bool through) { lookThroughCamera = through; ++_changes; }
        /** What the scene's panel shows the scene through: its pointer, for the gizmo and for picking. */
        std::shared_ptr<kgui::ViewportState> viewport = std::make_shared<kgui::ViewportState>();
        /** @brief Points the editor's view at @p entity, and stops looking through the scene's camera to show it. */
        void Frame(kecs::Entity entity);
        // ---- the scene view's tools ------------------------------------------------------------------------------

        /** @brief How the gizmo snaps: in steps of so many units moved, degrees turned, and of a scale's factor. */
        struct Snap {
            bool on = false;    ///< Snapped; Ctrl held does the other while dragging.
            float move = 0.25f, turn = 15.f, scale = 0.1f;
            bool operator==(const Snap&) const = default;
        };
        Snap snap;
        /** Whether the gizmo is along the world's axes rather than what it is on's own. */
        bool gizmoWorld = false;
        /** How fast the view flies, as a factor: the wheel while the right button is held changes it. */
        float flySpeed = 1.f;
        /** Lights, cameras and what draws nothing, shown by an icon in the scene where they are: a click on one picks it. */
        bool iconsShown = true;
        /** The scene's numbers in a corner of its view: what was drawn, dispatched, traced, and what it took. */
        bool statsShown = false;
        /** Dragged with the gizmo, put on the surface under the pointer rather than moved along an axis. */
        bool surfaceSnap = false;

        /** @brief Which way the view looks straight along an axis: from above, below, the front... */
        enum class Axis : std::uint8_t { eTop, eBottom, eFront, eBack, eRight, eLeft };
        /**
         * @brief Looks along @p axis at what it looks at now, and seen straight on (orthographic) unless @p perspective:
         *        the editor's view, or @p view (the second one).
         */
        void LookAlong(Axis axis, bool perspective = false, EditorCamera* view = nullptr);
        /** @brief Seen straight on, or in perspective. */
        void SetOrthographic(bool orthographic);

        /**
         * @brief What is picked besides selectedEntity — Ctrl or Shift and a click, or a box dragged round them. The
         *        gizmo moves them all as one, round the first.
         */
        std::vector<kecs::Entity> alsoSelected;
        /** @brief Every picked entity, the first first. */
        [[nodiscard]] std::vector<kecs::Entity> Selection() const;
        /** @brief Adds @p entity to what is picked, or takes it away when it is picked already. */
        void SelectAlso(kecs::Entity entity);
        /** @brief Picks what of the scene's entities with a Mesh is inside the box, in the view's pixels; @p adding to what is picked. */
        void SelectInBox(kor::Vec2 from, kor::Vec2 to, bool adding);

        /** @brief Saves where the view is as bookmark @p slot (0 to 8: Ctrl and a digit), for the scene. */
        void SaveBookmark(int slot);
        /** @brief Goes to bookmark @p slot (a digit); false when there is none. */
        bool GoToBookmark(int slot);
        [[nodiscard]] bool HasBookmark(int slot) const;

        /** @brief What the pointer is over in the scene view, a few frames late: the entity, and where on its surface. */
        struct Probe {
            bool hit = false;           ///< A surface is there.
            kecs::Entity entity = kecs::NullEntity;
            kor::Vec3 position { 0.f }, normal { 0.f, 1.f, 0.f };
            float distance = 0.f;       ///< From where the view is.
        };
        /** @brief What measuring does with a click: nothing, a ruler between two points, or saying what is under the pointer. */
        enum class Measure : std::uint8_t { eOff, eRuler, eProbe };
        Measure measure = Measure::eOff;
        void SetMeasure(Measure m) { measure = m; ruler.clear(); ++_changes; }
        [[nodiscard]] const Probe& Probed() const { return _probe; }
        /** The ruler's ends, as they are clicked: none, one, or both. */
        std::vector<kor::Vec3> ruler;

        /**
         * @brief Something dropped on the scene view at @p u across and @p v down (0 to 1): what surface is there is
         *        asked for, and @p then runs once it is known — what it places (a prefab, a model imported) is put on it.
         */
        void DropAt(float u, float v, std::function<void()> then);
        /** Where the next prefab placed or model imported goes: on the surface it was dropped on. None, where it would. */
        std::optional<kor::Vec3> placeAt;

        /** @brief Hides what is picked in the scene view (not in the game, not in the saved scene). */
        void HideSelected();
        /** @brief Shows what is picked by itself in the scene view: everything else drawn is hidden. */
        void Isolate();
        /** @brief Shows everything hidden in the scene view again. */
        void ShowAll();
        [[nodiscard]] const std::set<kecs::Entity>& ViewHidden() const { return _viewHidden; }
        [[nodiscard]] bool Isolated() const { return _isolated; }

        /**
         * A second view of the scene beside the first, from its own place: the Scene panel split in two. It is drawn as
         * the first is — its view mode, what is hidden — without the editor's lines over it: it is looked through, and
         * turned, moved and zoomed as the first is; what is picked and moved is the first's.
         */
        bool splitView = false;
        EditorCamera secondCamera;
        std::shared_ptr<kgui::ViewportState> secondViewport = std::make_shared<kgui::ViewportState>();
        /** @brief Splits the view in two, the second from where the first is; or back to one. */
        void SetSplit(bool split);

        /** @brief Frames a second, averaged over the last quarter of one. */
        [[nodiscard]] int Fps() const { return _fps; }
        /** @brief How long a frame takes, in ms, averaged over the same quarter of a second. */
        [[nodiscard]] double FrameMs() const { return _frameMs; }

        /**
         * @brief How the Scene panel shows the scene: as its graph draws it (lit); a channel of its materials, which
         *        the shaders that import koral_debug show; one of the editor's own ways — its edges, clay, normals —
         *        from the editor's graphs (Overlay::Views); or a DebugView of the scene's graph. Only the Scene panel's:
         *        the Game panel, and the project run, show the scene lit.
         */
        struct ViewMode {
            enum class Kind : std::uint8_t { eLit, eMaterial, eEditor, eGraph };
            Kind kind = Kind::eLit;
            kgraph::ShaderDebugChannel channel = kgraph::ShaderDebugChannel::eLit;     ///< eMaterial's.
            std::filesystem::path file;     ///< eEditor's: the editor graph its DebugView is in.
            std::string id;                 ///< eEditor's and eGraph's: the DebugView.
            bool operator==(const ViewMode&) const = default;
        };
        /** @brief One of the ways there are to show the scene: what it is called, and under which heading. */
        struct ViewModeChoice {
            ViewMode mode;
            std::string name;
            std::string group;      ///< "", "Material", "Editor" or "Scene graph".
        };
        /** @brief Every way there is now: lit, the material's channels, the editor graphs' DebugViews, then the scene graph's. */
        [[nodiscard]] std::vector<ViewModeChoice> ViewModes() const;
        [[nodiscard]] const ViewMode& CurrentViewMode() const { return _viewMode; }
        /** @brief Shows the scene so. A DebugView that is gone — the graph changed — is lit again. */
        void SetViewMode(ViewMode mode);
        /** @brief The graphics pipelines of the scene whose shaders show no channel of their material: drawn lit whatever is chosen. */
        [[nodiscard]] std::vector<std::string> WithoutMaterialViews() const;
        /** @brief The picture shown in place of the scene's, for a DebugView; empty for the scene lit or a channel of its materials. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> ViewModePicture() const;
        /** @brief The grid in the scene, as this user set it last: kept from one run to the next. */
        [[nodiscard]] const Grid& SceneGrid() const { return _grid; }
        void SetGrid(const Grid& grid);
        /** @brief The grid, on nothing, to go over the scene's picture; empty while it is not shown. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> GridPicture() const;
        /**
         * @brief Each frame, as the scene being edited updates: the view moved by the mouse over the scene's panel
         *        (right-drag turns, middle-drag moves, the wheel zooms), what is picked outlined and given its
         *        gizmo, the cameras and the picked light drawn, and a click on the picture picking what is under it.
         *        @p input is the editor window's.
         */
        void Tick(const kor::Input& input, float seconds);

        // ---- playing ----------------------------------------------------------------------------------------

        /** @brief Starts, or goes on, playing: scripts and the scene's systems run. The scene as it was is kept, for Stop. */
        void StartPlaying();
        void Pause();
        /**
         * @brief Paused, or not yet playing: one fixed step and no more — the scripts' fixedUpdate and the graph's fixed
         *        pages, once — and paused after it. Started from the beginning, as Play is, when it was not playing.
         */
        void Step();
        /** @brief Ends play, and puts the scene back as it was when it began. */
        void Stop();
        [[nodiscard]] bool InPlayMode() const { return !_snapshot.empty(); }

        // ---- a prefab, opened ----------------------------------------------------------------------------------
        //
        // A prefab is edited by itself, as Unity's are: opened, the scene's entities are put aside — with what can be
        // taken back of them — and the outliner and the scene view have the prefab's alone, entities like any. Back to
        // the scene, it is as it was, and every instance of the prefab follows what was saved of it.

        /**
         * @brief Makes a prefab of one empty entity, @p name — `prefabs/<name>.prefab` — and opens it. False, saying why,
         *        for a name that is not one, one that is there, or no project.
         */
        bool NewPrefab(const std::string& name);
        /** @brief Opens the prefab whose asset id is @p id (`prefab:Crate.prefab`) by itself. One open already is saved first. */
        bool OpenPrefab(const std::string& id);
        /** @brief Writes the prefab that is open as it is now: its first entity at the top, and what is under it. */
        bool SavePrefab();
        /** @brief Back to the scene, as it was — the prefab saved first, when @p save — and its instances as the prefab is. */
        bool ClosePrefab(bool save = true);
        /** @brief The prefab that is open, as it names it (`prefabs/Crate.prefab`); empty when the scene is. */
        [[nodiscard]] std::string PrefabOpen() const { return _prefab ? _prefab->name : std::string(); }
        /** @brief Whether the prefab that is open has changed since it was opened or saved. */
        [[nodiscard]] bool PrefabChanged();
        [[nodiscard]] bool IsPlaying() const { return _play.Playing(); }

        // ---- entities ---------------------------------------------------------------------------------------

        kecs::Entity Duplicate(kecs::Entity entity);
        void Delete(kecs::Entity entity);
        /** @brief The objects there are to make: the files of the project's `objects` folder, by name. Read again by LoadObjects. */
        [[nodiscard]] const std::vector<ObjectTemplate>& Objects() const { return _objects; }
        void LoadObjects();
        kecs::Entity Create(const ObjectTemplate& object, kecs::Entity parent = kecs::NullEntity);
        /** @brief Saves @p entity, and what is under it, as an object of its name: a file among the project's objects. */
        bool SaveAsObject(kecs::Entity entity);
        /**
         * @brief Saves @p entity, and what is under it, as a prefab of its name — `prefabs/<name>.prefab` — and makes it
         *        the prefab's first instance. False, saying why, when there is one of that name already.
         */
        bool SaveAsPrefab(kecs::Entity entity);
        /** @brief An instance of the prefab whose asset id is @p id (`prefab:Crate.prefab`), last under @p parent, and picked. */
        kecs::Entity PlacePrefab(const std::string& id, kecs::Entity parent = kecs::NullEntity);
        // ---- files ------------------------------------------------------------------------------------------

        /**
         * @brief The kinds of shader there are to start from, by name, each with what it is: "Vertex" (vertices and their
         *        fragments), "MeshShading" (mesh shaders and theirs), "Compute", "RayTracing" (a ray generation shader, a
         *        miss shader and a hit group).
         */
        [[nodiscard]] static const std::vector<std::pair<std::string, std::string>>& ShaderKinds();
        using ShaderChoice = keditor::ShaderChoice;
        /**
         * @brief Why a shader of @p kind — with @p option of its ShaderChoice, by the field's name, when given — cannot be run
         *        on this device; empty when it can. "This GPU has no mesh shaders."
         */
        [[nodiscard]] static std::string ShaderMissing(std::string_view kind, std::string_view option = {});
        /** @brief The languages a script is written in, the same way. */
        [[nodiscard]] static const std::vector<std::pair<std::string, std::string>>& ScriptKinds();
        /** @brief The languages a script can be made in here: the project's, once it has one; either, until then. */
        [[nodiscard]] std::vector<std::pair<std::string, std::string>> ScriptKindsHere() const;
        /**
         * @brief Makes a shader of @p kind, one of ShaderKinds, with what @p choice says, called @p name:
         *        `shaders/<name>.slang`, a module a pipeline node then names — each entry point called as a pipeline looks
         *        for it when it is not named: vertexMain, fragmentMain, closestHitMain. False, saying why, for a name that is
         *        not one, a file that is there, or what this device cannot run.
         */
        bool NewShader(const std::string& name, const std::string& kind, const ShaderChoice& choice = {});
        /**
         * @brief Makes a script called @p name of @p kind, one of ScriptKinds — its language, and what it is: "C#" or "Kotlin"
         *        for a script of entities, "C# ResourceScript" or "Kotlin ResourceScript" for one that makes resources and fills
         *        them as they are made — as `scripts/<name>.cs`, or `.kt`.
         */
        bool NewScript(const std::string& name, const std::string& kind);

        /**
         * @brief Starts the scene over as the standard one: the graph that draws meshes lit under a sky, and a sky,
         *        a camera, a sun and a sphere. Needs a project, for its shaders to be in.
         */
        /** @brief Makes entities of a model file (Document::ImportModel), and picks the one over them all. */
        bool ImportModel(const std::filesystem::path& file);
        /**
         * @brief Brings a picture or a model into the project, as one of its assets — and nothing into the scene: the Assets
         *        panel has it, to be dragged onto the scene or the outliner when it is wanted there. A picture is copied to
         *        `assets/textures`, a model to a folder of its own in `assets/models`, with what it names beside it — a
         *        glTF's buffers and pictures, an OBJ's materials and theirs. One in the project already is only picked.
         * @return Its asset's id; empty, with why said, when there is no project or it is no picture or model.
         */
        std::string ImportAsset(const std::filesystem::path& file);
        /** @brief The scene made anew from the standard graph (keditor::AddStandardGraph), its shader written into the project. */
        bool NewStandardScene();
        /** @brief The scene made anew from the lit graph (keditor::AddLitGraph): every light, under a sky. */
        bool NewLitScene();
        /** @brief Reads the scene again from its file. */
        bool Revert();

        // ---- the graph's pages ------------------------------------------------------------------------------

        /** @brief The pass whose page the graph panel shows; none while it shows every node at once. */
        [[nodiscard]] const std::string& Pass() const { return _pass; }
        [[nodiscard]] bool ShowsEveryPass() const { return _everyPass; }
        void ShowPass(std::string pass) { _pass = std::move(pass); _everyPass = false; ++_changes; }
        void ShowEveryPass() { _everyPass = true; ++_changes; }
        /**
         * @brief Whether the Here to use panel — what the page shown can use of the passes before it — is in sight: its
         *        images are pictured while it is. The editor says so as its panels are laid out.
         */
        void ShowDrawer(const bool open) { _drawer = open; ++_changes; }
        [[nodiscard]] bool DrawerShown() const { return _drawer; }
        /** @brief Picks node @p id, and turns to the page it is on: the graph panel brings it into view. */
        void GoTo(const std::string& id);
        /**
         * @brief Makes Draw @p id one draw of all its elements (`batched`): what the inspector offers a Draw whose shader
         *        reads each element from its [entity("PER_INSTANCE")] buffer already. @see kgraph::Runtime::BatchingOf
         */
        void Batch(const std::string& id);
        /** @brief Where the graph panel looks: shared with it, so that whoever asks knows where a node is on the screen. */
        std::shared_ptr<kui::NodeEditorView> graphView = std::make_shared<kui::NodeEditorView>();
        /** @brief Goes up each time a node is gone to. */
        [[nodiscard]] std::uint32_t Went() const { return _went; }

        /** @brief Keeps nodes @p ids as they are now, to be pasted — here, or in another scene of this run. */
        void CopyNodes(const std::vector<std::string>& ids);
        [[nodiscard]] bool HasCopiedNodes() const { return !_copied.empty(); }
        /** @brief Puts what was copied on the page that is shown, at (@p x, @p y), and picks the first of it. */
        std::vector<std::string> PasteNodes(double x, double y);
        /** @brief Copies of nodes @p ids beside them; the first is picked. */
        std::vector<std::string> DuplicateNodes(const std::vector<std::string>& ids);
        /**
         * @brief Nodes @p ids put in @p pass (Document::MoveNodesToPass), and that pass's page shown with them picked.
         *        An empty @p pass is a new one, named for what it holds first.
         */
        bool MoveNodesToPass(const std::vector<std::string>& ids, std::string pass);
        /** @brief Nodes @p ids made a function called @p name (Document::MakeFunction): the call picked, on its page. */
        std::string MakeFunction(const std::vector<std::string>& ids, const std::string& name);
        /** @brief Call @p call's function opened in place (Document::OpenFunction), its page shown. */
        bool OpenFunction(const std::string& call);
        /** @brief The function opened closed: kept (written to its file, every call following), or put back as it was. */
        bool CloseFunction(bool keep);
        /** @brief Call @p call written out into its nodes, in its place. */
        bool WriteOut(const std::string& call);
        /** @brief Copies of nodes @p ids in @p pass, where they are on their own page: that page shown, the first of them picked. */
        std::vector<std::string> CopyNodesToPass(const std::vector<std::string>& ids, const std::string& pass);
        /** @brief The file of the first shader node @p id names, when it names one of the project's; empty otherwise. */
        [[nodiscard]] std::filesystem::path ShaderOf(std::string_view id);

        std::string selectedNode;                       ///< The node the inspector shows, when no entity is picked.
        kecs::Entity selectedEntity = kecs::NullEntity; ///< The entity it shows.
        void Select(std::string node) { selectedNode = std::move(node); selectedEntity = kecs::NullEntity; alsoSelected.clear(); selectedAsset.clear(); ++_changes; }
        void Select(const kecs::Entity entity) { selectedEntity = entity; alsoSelected.clear(); selectedNode.clear(); selectedAsset.clear(); ++_changes; }

        // ---- what an entity is, and where it goes ------------------------------------------------------------

        /**
         * @brief What an entity is, for whoever shows it by an icon: "camera", "sun", "light", "mesh" (it has one),
         *        "script" (a component that is none of the scene's own or a pipeline's), "group" (it has entities
         *        under it and nothing else), or "empty".
         */
        [[nodiscard]] std::string EntityKind(kecs::Entity entity);
        /** @brief The pipeline node each generated component is of: `forward` for the component `forward`. By component name. */
        [[nodiscard]] std::map<std::string, std::string> PipelineComponents();
        /** @brief The passes' nodes that run for @p entity: each Draw or Dispatch whose pipeline's component it has, switched on. */
        [[nodiscard]] std::vector<std::string> RunsFor(kecs::Entity entity);
        /** @brief Whether anything draws @p entity or what is under it: there is something to show and to hide. */
        [[nodiscard]] bool CanShow(kecs::Entity entity);
        /** @brief Whether @p entity is drawn: any of its pipelines' components is switched on. */
        [[nodiscard]] bool IsShown(kecs::Entity entity);
        /** @brief Switches every pipeline's component of @p entity, and of everything under it, on or off. */
        void SetShown(kecs::Entity entity, bool shown);
        enum class Beside : std::uint8_t { eBefore, eUnder, eAfter };
        /**
         * @brief Moves @p entity beside @p target: before or after it among what it is under, or under it, last.
         *        False, with Status saying why, when it would go under itself.
         */
        bool MoveEntity(kecs::Entity entity, kecs::Entity target, Beside where);
        /**
         * @brief What a field of @p component that holds a @p fieldType can be given, by id: for a mesh, the
         *        meshes there are without a file, those the scene has of files, and the graph's; for a picture, a
         *        plain white, black or flat normal, the textures the scene and the project have, and the images the
         *        graph makes in a pass before the one that component's pipeline is run in; for anything else, the
         *        graph's nodes of that kind up to that pass.
         */
        [[nodiscard]] std::vector<std::string> FieldChoices(std::string_view component, std::string_view fieldType);
        /**
         * @brief Gives @p entity something of the Assets panel, dropped on it: a mesh is its Mesh (which it is given,
         *        when it has none), a texture its first picture field with nothing in it — its albedo's before any
         *        other — and a script the component of that name. False, with Status saying why, when it cannot.
         */
        bool GiveAsset(const std::string& asset, kecs::Entity entity);
        /**
         * @brief Makes the node an asset dropped on the graph is: an ImageFile for a picture, a pipeline for a
         *        shader — a compute one when the module has a compute entry point, a graphics one otherwise — on
         *        the page that is shown, at (@p x, @p y). The node's id; empty, with Status saying why, for
         *        anything else.
         */
        std::string AssetToNode(const std::string& asset, double x, double y);
        /**
         * @brief The nodes input @p input of @p node can be wired from, by id: those whose main output is of a kind
         *        the input takes, that are on its page, in no pass, or made by a pass before it — and that are not
         *        wired from @p node themselves.
         */
        [[nodiscard]] std::vector<std::string> WireChoices(std::string_view node, std::string_view input);
        /** @brief Wires input @p input of @p node from @p from: brought into the node's pass first, when it is of another. */
        bool WireFrom(std::string_view node, std::string_view input, const std::string& from);

        // ---- taking back, and what the scene can be switched between -----------------------------------------

        /**
         * @brief Puts the scene back as it was before the last thing done to it — its graph and its entities — and
         *        Redo does the thing again. What is done in one go (a drag, a field typed into) is taken back in
         *        one; what a scene does while it plays is no edit, and is not kept.
         */
        bool Undo();
        bool Redo();
        [[nodiscard]] bool CanUndo() const { return !_undo.empty() || _undoPending; }
        [[nodiscard]] bool CanRedo() const { return !_redo.empty(); }

        /** @brief One of the graph's Switch nodes whose `index` a Number or a Bool node gives: what there is to choose, by name. */
        struct Choice {
            std::string driver;                 ///< The node that says which: its id is what the choice is called.
            std::vector<std::string> options;   ///< What each number shows: the node wired to that option.
            int chosen = 0;
        };
        /** @brief Every such choice of the graph: a picture or a debug view of it, shown as a switch in the Scene panel. */
        [[nodiscard]] std::vector<Choice> Choices() const;
        void Choose(const std::string& driver, int option);

        /**
         * @brief The graph as its pages run: each page a step, wired from every page it uses something of — a resource
         *        or a value one of its nodes made, brought in by a Reference or wired straight — and each Switch that
         *        chooses between branches (a Choice) a step of its own, between the pages its options come from and
         *        those that use what it chose. What the Pass flow panel shows; none of it is edited there but the choice.
         */
        struct PassFlow {
            struct Step {
                std::string id;                     ///< The page's name, or the Switch's id.
                bool isSwitch = false;
                std::string rate;                   ///< A page's: "setup", "fixed" or "frame".
                std::size_t nodes = 0;              ///< How many nodes the page has.
                bool ran = false;                   ///< Whether a pass of it ran in the last frame.
                double gpuMs = 0.0;                 ///< What its passes cost, on the GPU.
                std::string driver;                 ///< A Switch's: the node that says which (Choice::driver).
                std::vector<std::string> options;   ///< A Switch's: the page each option comes from.
                int chosen = 0;
                int order = 0;                      ///< Where it runs among the pages: a Switch, where the page it is on does.
                /// A page that runs only while a choice is one of its options — its passes idle unless `driver` is
                /// `option` (or are enabled only then) — is of that branch: the driver, and the option. Otherwise -1.
                std::string branchDriver;
                int branchOption = -1;
            };
            struct Link {
                std::string from, to;               ///< Steps, by id.
                std::vector<std::string> carries;   ///< What of `from` `to` uses: nodes, by id.
                int option = -1;                    ///< Into a Switch: which of its options; -1 otherwise.
            };
            std::vector<Step> steps;                ///< The pages in the order they run, then the Switches.
            std::vector<Link> links;
        };
        [[nodiscard]] PassFlow Flow() const;

        /**
         * @brief The pages of what the choices do not choose now: each of whose passes idles (or is switched off) by a
         *        choice's value — a branch of a Switch not chosen, shadow maps rays need not — and each page of no pass of
         *        its own that only such pages use. A pass idle for a reason of its own (a shadow drawn only when it
         *        changed) does not count. What the Graph panel's page list leaves out.
         */
        [[nodiscard]] std::set<std::string> PagesNotChosen() const;
        /**
         * @brief Whether a pipeline's component — a material — is read by a pass the choices leave running: one that runs
         *        its pipeline, or one whose pipeline reads its Draw's buffer of instances (the meshlets read the forward
         *        Draw's). True for a component of no pipeline, and for one no pass uses.
         */
        [[nodiscard]] bool MaterialInUse(const std::string& component);

        /**
         * @brief Where each step of a flow goes, and the branches it is drawn in. Columns from left to right in the order
         *        the steps need each other — each step in the column, between those it takes from and those that take
         *        from it, that keeps its wires shortest — and in each column the steps at the heights of those they are
         *        wired to, above or below, kept apart. The steps only one option of a Switch leads to are a branch: a
         *        band of their own, its steps kept together, apart from the others'.
         */
        struct FlowLayout {
            std::map<std::string, std::pair<float, float>> at;      ///< Each step's top-left.
            struct Branch {
                std::string switchId;
                int option = 0;
                std::string title;
                std::vector<std::string> steps;
            };
            std::vector<Branch> branches;
        };
        /** @param sizes Each step's card, as big as it was drawn; one not drawn yet is guessed. */
        [[nodiscard]] static FlowLayout LayOut(const PassFlow& flow, const std::map<std::string, std::pair<float, float>>& sizes);

        // ---- what a project is made of, besides its scenes ----------------------------------------------------

        /** @brief Something of the project's that is in a file: what the Assets panel lists. */
        struct Asset {
            enum class Kind : std::uint8_t { eModel, eMesh, eTexture, eShader, eScript, eScene, ePrefab, eMaterial };
            Kind kind = Kind::eTexture;
            /// What it is picked by, and — for a mesh or a texture — what an entity's field holds to have it:
            /// `@textures/wall.png`, `@sponza.gltf#3`. A shader module is `shader:<module>` and each entry point of
            /// it `shader:<module>#<entry>`; a script `script:<file>` and a prefab `prefab:<file>`.
            std::string id;
            std::string label;              ///< What it is called: a mesh by the name its model gives it, a shader by its entry's, anything else by its file's.
            std::string group;              ///< A mesh's: the model it is of. A shader's: its module.
            std::string stage;              ///< A shader's: the stage its entry is declared for (`vertex`, `compute`); empty for a module of none.
            std::filesystem::path file;
            bool pending = false;           ///< Still being read.
            bool loaded = false;            ///< The scene has it: something of it asked for it.
            /// The folder it is shown in, as the Assets panel has them: the one its file is in, from the project's
            /// own (`assets/textures`, `shaders`) — or, for a file that is not in the project, under OutsideFolder.
            std::string folder;
            std::string parent;             ///< A mesh's: the model it is of, by that model's id. A shader entry's: its module's.
            /// The editor's own, of no file: a default mesh (`$cube`, under Meshes/Primitives) or a default material
            /// (`material:Gold`, under Materials/Defaults).
            bool builtIn = false;
        };
        /**
         * @brief What a thing drawn is made of, as the fields of the component that draws it (`lit`'s, or any with an
         *        `albedo`): its colour as it is picked, how metallic and how rough, the light it gives. Dropped on an
         *        entity, these are what that component holds — its pictures white, so that the colour is the colour.
         */
        struct Material {
            std::string name;
            float albedo[3] { 0.8f, 0.8f, 0.8f };
            float metallic = 0.f;
            float roughness = 0.5f;
            float emission[3] { 0.f, 0.f, 0.f };
        };
        /** @brief The editor's materials, listed under Materials/Defaults: `material:<name>` each. */
        [[nodiscard]] static const std::vector<Material>& DefaultMaterials();
        /** @brief A default material, by its asset's id (`material:Gold`); null for none. */
        [[nodiscard]] static const Material* MaterialOf(std::string_view id);
        /** @brief The folder files that are not in the project are shown under, in the Assets panel. */
        static constexpr const char* OutsideFolder = "Outside the project";
        /**
         * @brief Every mesh and picture the scene has of a file, the models and pictures in the project's `assets`
         *        folder that it has not, and the project's shaders and scripts. Models first, then meshes, textures,
         *        shaders and scripts, each by name.
         */
        [[nodiscard]] std::vector<Asset> Assets();
        /** @brief What a listed mesh or picture is, held while the interface shows it; null for one not loaded. */
        [[nodiscard]] std::shared_ptr<kgraph::Resource> AssetHeld(std::string_view id) const;
        std::string selectedAsset;          ///< The asset the inspector shows, when it is picked: by its id.
        /** @brief A small picture of a listed mesh, once it is drawn; empty until then. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> MeshPicture(std::string_view id) const;
        /**
         * @brief How many small pictures of meshes have been drawn: what shows one follows this, and is built again alone
         *        when a new one is made — one every few frames while the Assets panel's are drawn, each was the whole interface.
         */
        [[nodiscard]] std::uint64_t MeshPicturesMade() const { return _meshPicturesMade; }

        // ---- pictures kept block-compressed ---------------------------------------------------------------------

        /** @brief A picture file the scene reads, as the Compress textures dialog lists it. */
        struct SceneTexture {
            std::string id;                     ///< `@main_sponza/textures/wall_Normal.png`
            std::string field;                  ///< A field of an entity's that holds it — `normalMap` — or empty.
            kgraph::TextureKind kind = kgraph::TextureKind::eColor;    ///< What it is for, as its name and field tell.
            kgraph::TextureImport import;       ///< How the scene's graph keeps it.
            std::string pack;                   ///< The pack it is a channel of; empty for none.
            bool cooked = false;                ///< Read from a file cooked of it — or of its pack — as it is now.
            bool compressible = true;           ///< Not an HDR picture, nor one compressed already: kgraph::TextureCompressible.
        };
        /** @brief Every picture file the scene reads, by id; not the packs (the graph's texturePacks). */
        [[nodiscard]] std::vector<SceneTexture> SceneTextures();
        /** @brief What a picture of @p kind is offered as, as the project says; DefaultTextureFormat when it says nothing. */
        [[nodiscard]] kgraph::TextureFormat OfferedFormat(kgraph::TextureKind kind) const;
        /** @brief What pictures of @p kind are offered as from now on: kept in the project. */
        void SetOfferedFormat(kgraph::TextureKind kind, kgraph::TextureFormat format);
        /**
         * @brief How the scene's pictures are kept — each file's import, and the packs — an edit of the graph like any;
         *        with @p cook, every picture kept in a block format that has no file cooked of it as it is now is cooked,
         *        off the frame, and read from it as it is. The project's `cache/` is kept out of git, where it has one.
         */
        void KeepTextures(std::map<std::string, kgraph::TextureImport> imports, std::map<std::string, kgraph::TexturePack> packs, bool cook);
        [[nodiscard]] const kgraph::Runtime::CookProgress& Cooking();
        /** @brief Once a scene opened has been read: true, once, when it has pictures that are not compressed and the project asks. */
        [[nodiscard]] bool TakeAskToCompress() { return std::exchange(_askToCompress, false); }
        /** @brief Not to ask again, for this project. */
        void DontAskToCompress();
        std::string assetFilter;            ///< What the Assets panel lists is what has this in its name, wherever it is; a folder's own, when empty.
        std::string assetFolder;            ///< The folder the Assets panel shows: empty, the project's own.
        int assetKind = -1;                 ///< An Asset::Kind the panel shows nothing but, wherever it is; -1, every kind.
        float assetTileSize = 72.f;         ///< How big the panel shows each thing; at its least, a line each.
        bool drawerList = false;            ///< The drawer beside a pass's page shows a line a thing, not a picture over its name.
        const kor::Window* window = nullptr;    ///< The editor's own window: what its title bar's buttons make smaller, bigger, or close.
        void SetDrawerList(const bool lines) { drawerList = lines; ++_changes; }
        std::set<std::string> assetsOpened; ///< The models whose meshes are shown after them, by id.
        /**
         * @brief Every folder the Assets panel has: the project's own, empty ones too, and those the things in
         *        @p assets are in. By path, sorted; a folder's parents are there before it.
         */
        [[nodiscard]] std::vector<std::string> AssetFolders(const std::vector<Asset>& assets) const;
        /** @brief Where a folder of the panel is on disk; empty for one that is not a folder of the project's. */
        [[nodiscard]] std::filesystem::path AssetFolderPath(std::string_view folder) const;
        /** @brief Makes a folder called @p name in the folder that is shown. False, with Status saying why, when it cannot. */
        bool NewAssetFolder(const std::string& name);
        /** @brief What pressing a listed thing twice does: a shader or a script is opened to be written, a model shows or hides its meshes. */
        void OpenAsset(const Asset& asset);
        /** @brief Shows @p path in the system's file manager: a folder itself, a file in the folder it is in. */
        bool ShowInFiles(const std::filesystem::path& path);
        /** @brief Moves a file or a folder of the project's to the system's trash. False, with Status saying why, when it cannot. */
        bool TrashAsset(const std::filesystem::path& path);
        /** @brief What a field that holds something of a file is shown as: the mesh's or the file's own name, not where it is. */
        [[nodiscard]] std::string AssetLabel(const std::string& id) const;
        void SelectAsset(std::string id) { selectedAsset = std::move(id); selectedNode.clear(); selectedEntity = kecs::NullEntity; alsoSelected.clear(); ++_changes; }

        /**
         * @brief Opens the project's folder in an editor of code — Visual Studio Code, or what the KORAL_ENGINE_CODE
         *        environment variable names — at @p file when one is given (a shader, a script), at its @p line
         *        when that is given too (from 1). The project is
         *        given what that editor needs to know it first (WriteEditorSetup), and its Kotlin build when it has
         *        Kotlin scripts, so that a script is written with the engine's names offered.
         */
        bool OpenInCode(const std::filesystem::path& file = {}, int line = 0);
        /** @brief How a command is run: the system's shell, unless something else is put here (a test). */
        static inline std::function<bool(const std::string& command)> launch;
        /** @brief How the editor closes once it has handed over to another: App::Quit, unless something else is put here (a test). */
        static inline std::function<void()> quit;
        /**
         * @brief Whether a hand-over (ReopenWithScripts) is the program's to make once its window is closed — in its own
         *        terminal, where it can see the runner fail — rather than a runner started from here. `koral-editor` says so.
         */
        static inline bool handOverOnExit = false;
        /** @brief The project the program is to go to the runner of, once its window is closed; nothing for none. */
        static inline std::optional<std::filesystem::path> handOverPending;
        /**
         * @brief The project the editor opens when it is given none: this user's choice, kept in their config
         *        folder (SetStartupProject). Empty when there is none, or it is no project any more.
         */
        [[nodiscard]] static std::filesystem::path StartupProject();
        /** @brief Makes @p project the one the editor opens when given none; empty, the engine's own again. */
        static bool SetStartupProject(const std::filesystem::path& project);
        /**
         * @brief The editor's theme: this user's choice of design and of dark or light, kept in their config folder
         *        (SetSavedTheme). Koral's dark when none was chosen.
         */
        [[nodiscard]] static kui::Theme SavedTheme();
        /** @brief Keeps @p design, dark or light, as the theme the editor opens in. */
        static bool SetSavedTheme(kui::ThemeDesign design, bool dark);
        /** @brief A theme chosen (Settings, Theme): the editor puts it on between frames, and keeps it. */
        void ChooseTheme(kui::ThemeDesign design, bool dark);
        std::optional<kui::Theme> themeChosen;  ///< What ChooseTheme chose, until the editor puts it on.
        /** @brief Where a new project is offered to be made: KORAL_ENGINE_PROJECTS, or the folder the build names. */
        [[nodiscard]] static std::filesystem::path ProjectsDirectory();
        /** @brief Whether what is under @p entity is folded away in the outliner. Open, to begin with. */
        [[nodiscard]] bool Folded(const kecs::Entity entity) const { return _folded.contains(entity); }
        void Fold(const kecs::Entity entity, const bool folded) { if (folded) _folded.insert(entity); else _folded.erase(entity); ++_changes; }

        /** @brief The last thing done, for the foot of the window; and whether it went wrong. */
        [[nodiscard]] const std::string& Status() const { return _status; }
        [[nodiscard]] bool Failed() const { return _failed; }
        /** @brief Says @p status at the foot of the window, and in the log: as an error when it @p failed. */
        void Say(std::string status, bool failed = false);

        /** @brief Asks for the list of nodes to add, as a right click on the graph does: the interface opens it. */
        // ---- looking at what a node holds -------------------------------------------------------------------

        /**
         * @brief Looks at what node @p id makes — its image, as a picture, or its buffer, as numbers — which the
         *        inspector then shows under the node, a few times a second. An empty id stops looking.
         */
        void Look(std::string id);
        /** @brief Stops looking at one node; the others looked at go on being. */
        void Unlook(const std::string& id);
        [[nodiscard]] bool IsLooking(const std::string_view id) const { return _looks.contains(id); }
        /** @brief The node last looked at: what the inspector's controls are of. Any number are looked at at once, each shown on its node. */
        [[nodiscard]] const std::string& Looking() const { return _looking; }
        LookMode lookMode = LookMode::eAuto;
        std::uint32_t lookSlice = 0;                    ///< Which slice, of an image with depth.
        std::uint32_t lookMip = 0, lookLayer = 0;       ///< Which level and layer of the image looked at last: its first, to begin with.
        /// How a picture looked at is drawn bigger or smaller than it is: blended, or a texel a square — a height map's cells.
        kui::ImageFilter lookFilter = kui::ImageFilter::eLinear;
        /** @brief What is looked at, as last made: an image's picture (and what its values run from and to), or a buffer's lines. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> LookPicture() const { return LookPictureOf(_looking); }
        [[nodiscard]] const std::string& LookSays() const { return LookSaysOf(_looking); }
        [[nodiscard]] const std::vector<std::string>& LookLines() const;
        [[nodiscard]] kor::ResourceRef<const kor::Image> LookPictureOf(std::string_view id) const;
        [[nodiscard]] const std::string& LookSaysOf(std::string_view id) const;
        /** @brief What the editor draws over the scene, as the interface shows it: the picture it was last told of. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> OverPicture() const;
        /** @brief Each frame: makes what is looked at again, when it is time. */
        void UpdateLook(float seconds);
        /**
         * @brief The small picture an image node shows of what it holds: copied out once in so many frames, each image
         *        on frames of its own, so that every image on the page shown is seen on its node without each costing a
         *        copy a frame. Null until the first is made.
         */
        [[nodiscard]] kor::ResourceRef<const kor::Image> ThumbnailOf(std::string_view id) const;
        /**
         * @brief A small picture of what node @p id makes, for the drawer beside a page: an image's, as it was when its turn
         *        last came to be copied out (UpdateThumbnails copies the drawer's while it is open); a mesh drawn by itself.
         *        Empty for anything else, and until it is made.
         */
        kor::ResourceRef<const kor::Image> NodePicture(std::string_view id);
        /** @brief What a thumbnail cannot be, when it cannot: an image of a format that does not read back. Empty otherwise. */
        [[nodiscard]] const std::string& ThumbnailSaysOf(std::string_view id) const;
        /**
         * @brief How many pictures have been made of node @p id, looked at closely or as its thumbnail: what shows one
         *        follows this, and is built again alone when only its picture is new.
         */
        [[nodiscard]] std::uint64_t PictureVersionOf(std::string_view id, bool looking) const;
        /** @brief Each frame: keeps the page's image nodes watched, copied out now and then, and makes their pictures, the stalest first. */
        void UpdateThumbnails(float seconds);

        // ---- the viewer: one texture, buffer or mesh, looked at closely ---------------------------------------

        enum class ViewKind : std::uint8_t { eNone, eImage, eBuffer, eMesh };
        /**
         * @brief Opens what @p id is in the viewer: a node's image, buffer or mesh, or a file's texture or mesh
         *        (`@wall.png`, `@model.gltf#3`). An image is every level and layer of it, its histogram and its texels;
         *        a buffer a table of its elements; a mesh drawn by itself, to turn about. False, and said, for
         *        anything else.
         */
        bool View(std::string id);
        void StopViewing();
        [[nodiscard]] const std::string& Viewing() const { return _viewing; }
        [[nodiscard]] ViewKind Viewed() const { return _viewKind; }
        /** @brief Goes up each time something is opened in the viewer: when the interface brings the viewer forward. */
        [[nodiscard]] std::uint32_t ViewAsked() const { return _viewAsked; }
        /** @brief Goes up each time play starts: when the Game panel is brought forward. */
        [[nodiscard]] std::uint32_t PlayStarted() const { return _playStarted; }

        /** @brief What of an image is viewed, and how. Changed, it is copied out again. */
        struct ImageView {
            std::uint32_t mip = 0, layer = 0;
            LookOptions look;
            kui::ImageFilter filter = kui::ImageFilter::eLinear;    ///< How it is drawn zoomed in or out: blended, or a texel a square.
            bool logHistogram = false;
        };
        ImageView imageView;
        /** @brief What the viewed image or buffer held, as last copied out: none until it has been. */
        [[nodiscard]] const std::optional<kgraph::Peek>& ViewedPeek() const { return _viewPeek; }
        [[nodiscard]] kor::ResourceRef<const kor::Image> ViewedPicture() const;
        [[nodiscard]] const Picture& ViewedPictureMade() const { return _viewPicture; }
        [[nodiscard]] const Histogram& ViewedHistogram() const { return _viewHistogram; }
        /** @brief Why there is nothing to show yet, or what went wrong; empty while there is. */
        [[nodiscard]] const std::string& ViewSays() const { return _viewSays; }

        /** @brief The layout a viewed buffer is read by, as typed: empty, the one its shader gives it, or eight words a row. */
        /** @brief What of the viewed image is shown was changed (imageView, bufferFirst, bufferHex): made again now. */
        void ViewChanged() { _viewTimer = 0.f; ++_changes; }
        void SetBufferLayout(std::string text);
        [[nodiscard]] const std::string& BufferLayoutText() const { return _bufferLayoutText; }
        [[nodiscard]] const std::optional<BufferLayout>& ViewedBufferLayout() const { return _bufferLayout; }
        [[nodiscard]] const std::string& BufferLayoutError() const { return _bufferLayoutError; }
        std::size_t bufferFirst = 0;        ///< The first row of the table shown.
        bool bufferHex = false;

        /** @brief The viewed mesh, drawn: null when no mesh is viewed. */
        [[nodiscard]] MeshViewer* ViewedMesh() const { return _viewKind == ViewKind::eMesh ? _meshViewer.get() : nullptr; }
        /** @brief The viewed mesh's own, as the device has it: its vertices' layout, its index count. */
        [[nodiscard]] std::shared_ptr<kgraph::Resource> ViewedMeshHeld() const;
        /** @brief Each frame: copies out and makes again what is viewed, when it is time; draws a viewed mesh. */
        void UpdateView(float seconds);

        void AskForNodes() { _wantsNodes = true; ++_changes; }
        [[nodiscard]] bool TakeAskForNodes() { return std::exchange(_wantsNodes, false); }
        /** @brief Asks for the New shader dialog: its kinds, and what each can have. */
        void AskForNewShader() { _wantsShader = true; ++_changes; }
        [[nodiscard]] bool TakeAskForNewShader() { return std::exchange(_wantsShader, false); }
        /** @brief Asks for the page the graph shows to be put in order: its nodes left to right, by what is wired to what. */
        void AskToArrange() { _wantsArrange = true; ++_changes; }
        [[nodiscard]] bool TakeAskToArrange() { return std::exchange(_wantsArrange, false); }
        /**
         * @brief Asks for every page to be put in order, one after another: each shown, and put in order once it has been
         *        drawn — its nodes as big as they come out. The page shown before is shown again after.
         */
        void ArrangeEveryPage();
        /** @brief The pages still to be put in order, the next first: none when none is. */
        [[nodiscard]] const std::vector<std::string>& PagesToArrange() const { return _toArrange; }
        /** @brief The page shown has been put in order: the next is shown, or — none left — the one shown before. */
        void ArrangedPage();

        /** @brief Goes up when anything the panels show changed: the document, what is picked, the status. */
        [[nodiscard]] std::uint64_t Version() { return Doc().Version() + _changes; }

    private:
        PlayScene& _play;
        GameScene* _game = nullptr;
        GameScene* _second = nullptr;
        Project _project;
        bool _hasProject = false;
        std::filesystem::path _scriptsHost;
        bool _handingOver = false;
        std::string _scene = "main";
        std::string _status = "A scene with nothing in it. Right-click the graph to add a node.";
        bool _failed = false;
        std::uint64_t _changes = 0;
        std::map<std::string, std::set<std::string>> _asked;
        std::map<std::string, std::map<std::string, bool>> _sections;   // which parts of a node's inspector were opened or closed
        std::set<kecs::Entity> _folded;
        std::string _pass;
        bool _everyPass = false;
        bool _drawer = false;      // the Here to use panel is in sight
        std::string _snapshot;              // the entities as they were when play began
        std::vector<ObjectTemplate> _objects;
        int _fps = 0, _framesCounted = 0;
        double _frameMs = 0.0;
        float _secondsCounted = 0.f;
        std::optional<kor::Vec2> _lastMouse;
        std::optional<kor::MouseButton> _viewDrag;
        std::optional<kor::Vec2> _pressAt;
        bool _gizmoMoved = false, _wasDown = false;
        Probe _probe;
        int _probing = 0;                       // frames since the pointer's surface was last asked for
        void Probed(const kgraph::Runtime::PickHit& hit);
        std::vector<std::pair<kecs::Entity, kor::Vec3>> _icons;     // this frame's, where each is: a click near one picks it
        std::set<kecs::Entity> _viewHidden;
        bool _isolated = false;
        std::array<std::optional<EditorCamera>, 9> _bookmarks;
        std::string _bookmarksOf;               // the scene they are of: read for it once, written when one is saved
        void ReadBookmarks();
        std::optional<kor::Mat4> _groupBefore;  // the first picked one's matrix as the gizmo found it, moving the rest with it
        std::optional<kor::Vec2> _boxFrom;      // where a box being dragged round entities began
        std::optional<kor::MouseButton> _secondDrag;    // the button turning or moving the second view
        /** Turns, moves and zooms @p view as the mouse over @p port says, with @p drag the button doing it. */
        void Steer(EditorCamera& view, kgui::ViewportState& port, std::optional<kor::MouseButton>& drag, const kor::Input& input, kor::Vec2 delta, float seconds);
        bool _wantsNodes = false, _wantsArrange = false;
        std::vector<std::string> _toArrange;
        std::string _arrangedFrom;
        bool _wantsShader = false;
        std::uint32_t _went = 0;
        // What the outliner asks of every entity, worked out once for the scene as it is.
        std::map<std::string, std::string> _pipelines;
        std::uint64_t _pipelinesAt = 0, _drawnAt = 0;
        std::map<kecs::Entity, std::pair<bool, bool>> _drawn;      // can be shown, and is
        const std::pair<bool, bool>& Drawn(kecs::Entity entity);
        std::vector<kgraph::Node> _copied;
        std::shared_ptr<kgraph::Resource> _overHeld;    // the overlay's picture the interface shows
        std::shared_ptr<kgraph::Resource> _modeHeld, _gridHeld;     // the DebugView's picture, and the grid's
        ViewMode _viewMode;
        Grid _grid;
        bool _gridRead = false;
        std::vector<std::pair<std::shared_ptr<kgraph::Resource>, int>> _overBefore;    // those it showed, and for how many frames more each is kept
        std::vector<std::pair<std::string, std::shared_ptr<kgraph::Resource>>> _assetsHeld, _assetsHeldBefore;   // what the Assets panel shows, and showed
        std::size_t _reading = 0;
        std::unique_ptr<Thumbnails> _thumbnails;
        std::uint64_t _meshPicturesMade = 0;
        std::string _compressAskedFor;          // the scene it was last asked of, once it was read: asked once a scene
        bool _askToCompress = false;
        std::size_t _cookedShown = 0;           // how many cooked, as the status bar last said
        // The scene as it was, a state a thing done: its graph and its entities, each as text.
        struct Was { std::string graph, world; };
        std::vector<Was> _undo, _redo;
        Was _now;                               // as it is, by the last count
        // The prefab that is open, and the scene put aside for it: its entities, whether it had changed, what could be taken back.
        struct OpenPrefabState {
            std::string name;                   // `prefabs/Crate.prefab`
            std::filesystem::path file;
            std::string scene;
            bool sceneDirty = false;
            std::vector<Was> undo, redo;
            Was now;
            std::uint64_t saved = 0;            // the document's revision when the prefab was last saved or opened
        };
        std::optional<OpenPrefabState> _prefab;
        const Document* _undoOf = nullptr;      // the document those are states of: another one starts over
        std::uint64_t _undoRevision = 0;
        float _undoQuiet = 0.f;
        bool _undoPending = false;              // something was done since _now was taken
        bool NewScene(bool lit);
        void TrackUndo(float seconds, bool busy);
        void Commit();
        void PutBack(const Was& state);
        std::string _looking;
        float _lookTimer = 0.f;
        struct Looked {
            kor::Resource<kor::Image> image;
            std::string says;
            std::vector<std::string> lines;
            std::uint64_t version = 0;  // counts its pictures: what shows it is built again, and nothing else
        };
        std::map<std::string, Looked, std::less<>> _looks;     // every node looked at, and what was last made of it
        std::vector<kor::Resource<kor::Image>> _lookBefore;    // the pictures before, until the frames that show them are done
        // Each image node's own small picture, made again a node at a time.
        struct Thumbnail {
            kor::Resource<kor::Image> image;
            std::string says;
            std::uint64_t made = 0;     // the frame it was last made in: the stalest is made next
            std::uint64_t version = 0;  // counts its pictures: what shows it is built again, and nothing else
        };
        std::map<std::string, Thumbnail, std::less<>> _thumbs;
        std::map<std::string, std::uint64_t, std::less<>> _thumbWatched;   // each image node watched for its thumbnail, since which frame
        float _thumbTimer = 0.f;
        std::uint64_t _thumbFrame = 0;
        std::vector<std::pair<std::uint64_t, kor::Resource<kor::Image>>> _thumbBefore;     // replaced, kept while a frame may draw them
        // The viewer's: what is viewed, what was last copied out of it and made of that.
        std::string _viewing, _viewSays;
        ViewKind _viewKind = ViewKind::eNone;
        std::uint32_t _viewAsked = 0, _playStarted = 0;
        float _viewTimer = 0.f;
        std::optional<kgraph::Peek> _viewPeek;
        Picture _viewPicture;
        kor::Resource<kor::Image> _viewImage;
        std::vector<std::pair<kor::Resource<kor::Image>, int>> _viewBefore;     // pictures it showed, and for how many frames more each is kept
        Histogram _viewHistogram;
        std::string _bufferLayoutText, _bufferLayoutError;
        std::optional<BufferLayout> _bufferLayout;
        std::unique_ptr<MeshViewer> _meshViewer;
        std::shared_ptr<kgraph::Resource> _viewMeshHeld, _viewPictureHeld;
    };

    /**
     * @brief The kinds of node there are to add, for what someone typed looking for one: best match first. Each word
     *        typed has to be found — in the name, in another word for it ("texture" finds Image), in its category or
     *        in what it says it does — and a name that starts with it counts for most. Everything, when nothing is typed.
     *        The nodes of the world's components (@p presets: `PointLight`, `PointLight Buffer`) are among them, under
     *        "Components", each a node type with its own name and the ports of the one it is made of.
     */
    [[nodiscard]] std::vector<const kgraph::NodeType*> FindNodeTypes(std::string_view typed, const std::vector<kgraph::Preset>& presets = {});
    /**
     * @brief The sections of the list of nodes, in order, of the categories in @p counts (how many each has): `#Functions`,
     *        each function's purpose under it — on a page of @p rate eFixed, Physics first — and those that say none as
     *        Functions; `#Scene and values`; `#Vulkan`, what is made on the device and what runs. A heading is `#` and its
     *        name, and is there only when something is under it.
     */
    [[nodiscard]] std::vector<std::string> NodeListSections(const std::map<std::string, std::size_t>& counts, kgraph::PassRate rate = kgraph::PassRate::eFrame);

    /** @brief A node as the graph's view has it, for putting a page in order: where it is, and about how big its card is. */
    struct PlacedNode {
        std::string id;
        float x = 0.f, y = 0.f, width = 160.f, height = 80.f;
        std::string group;      ///< The frame it is in — the outermost, of those one inside another — laid out as a block of its own. Empty: none.
        int depth = 0;          ///< How many frames are inside that one, one in another: how far out it is drawn from its nodes.
    };
    /** @brief A wire, for putting a page in order: from which node to which, and where on each — its ports' — it is. */
    struct PlacedWire {
        std::string from, to;
        float fromY = -1.f, toY = -1.f;     ///< How far down each node its port is, from the node's top. Negative: its middle.
        bool operator==(const PlacedWire&) const = default;
    };
    /**
     * @brief Where a page's nodes go to be read from left to right: each in the column after the last thing it is
     *        wired from — and no further from what it is wired to than it need be — and down its column in the order
     *        that crosses the fewest wires, port by port. Then each is put level with what it is wired to — its ports
     *        with theirs, as near as the others in its column leave room for — so that wires run as straight as they
     *        can, and one that runs past a column has a way kept clear through it. What nothing is wired to or from is
     *        put in rows under the rest.
     * @param wires From which node to which; a wire to or from a node not in @p nodes is not counted.
     * @param aspect How wide, for how tall, the page is to come out — the view's own shape: when the page laid out so
     *               is far from that, a column taller than it leaves is wrapped into more, side by side, and what is
     *               wired to nothing is in rows as wide as the rest. 0: each column as tall as it is, the unwired in one row.
     * @return Each node's place, by id: Document::PlaceNodes puts them there.
     */
    [[nodiscard]] std::map<std::string, std::pair<double, double>> ArrangeNodes(const std::vector<PlacedNode>& nodes, const std::vector<PlacedWire>& wires,
                                                                               float aspect = 0.f);
    /** @brief As ArrangeNodes, each wire at the middle of the nodes it is between. */
    [[nodiscard]] std::map<std::string, std::pair<double, double>> ArrangeNodes(const std::vector<PlacedNode>& nodes, const std::vector<std::pair<std::string, std::string>>& wires,
                                                                               float aspect = 0.f);

    /**
     * @brief What node @p id costs a frame, of the runtime's @p timings: a pass node's own, or — a Function's call, a node
     *        of a loop — every pass it stands for (`call/node`, `node [n]`) added up. None, for a node that runs nothing.
     */
    [[nodiscard]] std::optional<kgraph::Runtime::Timing> CostOf(const std::map<std::string, kgraph::Runtime::Timing, std::less<>>& timings, const std::string& id);

    /** @brief A port of a node that a wire let go on it could go to, or come from: what the menu that opens lists. */
    struct PortChoice {
        std::string port;        ///< Its name: an input's, for a wire from an output; an output's, for a wire from an input.
        std::string label;       ///< As the node calls it.
        bool fits = false;       ///< Whether it takes what the wire carries.
        std::string why;         ///< Not fitting: what it is of, against what the wire is of.
        std::string wiredFrom;   ///< An input wired already: from what, in words. Chosen, the new wire takes its place.
        bool byName = false;     ///< A Bundle into what it fills by name (`fields`, `push`): offered first, saying how many it fills.
    };
    /**
     * @brief The ports of @p node a wire from @p fromPort of @p fromNode (an output's, or with @p fromOutput false an
     *        input's) could be wired to, let go on it: every input of it — shown or not, set on the node or in the
     *        inspector — or every output, what it gives of itself among them. Those that fit come first, each in the
     *        node's order; those that don't say why. None, for a wire onto its own node.
     */
    [[nodiscard]] std::vector<PortChoice> PortChoices(const Document& doc, const std::string& fromNode, const std::string& fromPort, bool fromOutput,
                                                      const std::string& node);

    /** @brief The whole interface, over @p session. */
    [[nodiscard]] kui::Widget EditorRoot(std::shared_ptr<Session> session);

    /** @brief The editor's own scene: its window is the interface, and the scene it edits runs offscreen inside it. */
    class EditorScene final : public kor::Scene {
    public:
        /**
         * @brief Opens on the project in @p project, or on an empty scene when there is none. With @p handOver, a
         *        project with scripts is opened again by their runner (Session::handOver).
         */
        explicit EditorScene(std::filesystem::path project = {}, bool handOver = false);
        /** "project": its folder. "scripts": the project whose scripts the runner that opened this runs. */
        explicit EditorScene(const kor::SceneArgs& arguments);
        ~EditorScene() override;

        void Initialize() override;
        void Update() override;
        void Shutdown() override;

        [[nodiscard]] Session& CurrentSession() { return *_session; }
        [[nodiscard]] kui::Ui& Interface() { return *_ui; }

    private:
        std::filesystem::path _projectPath, _scriptsHost;
        bool _handOver = false;
        PlayScene* _play = nullptr;
        GameScene* _game = nullptr;
        GameScene* _second = nullptr;      // the Scene panel's second view, while it is split
        std::shared_ptr<Session> _session;
        std::unique_ptr<kui::Ui> _ui;
    };
}
