//
// keditor: the editor. A window of panels over one scene: its pass graph as nodes, the scene itself as it runs,
// its entities, and whatever is picked, to change. Everything it does, it does through a Document — and so does
// keeping it honest in a test.
//

#pragma once

#include <filesystem>
#include <map>
#include <memory>
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

    /** @brief What the editor's panels share: the project, the scene being worked on, and what is picked. */
    class Session {
    public:
        /** @brief Works on the scene @p play runs. It must outlive the session. */
        explicit Session(PlayScene& play) : _play(play) {}

        [[nodiscard]] Document& Doc() { return _play.Doc(); }
        [[nodiscard]] PlayScene& Play() { return _play; }
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
        /** @brief What Run runs: the player, then the project's folder. Empty without a project. */
        [[nodiscard]] std::string RunCommand() const;

        /**
         * @brief Makes an input a node can do without one the node shows: asked for, and not yet given. What a node
         *        shows of its own accord is what it cannot do without, and whatever it has been given.
         */
        void Show(const std::string& node, const std::string& input) { _asked[node].insert(input); ++_changes; }
        void Hide(const std::string& node, const std::string& input) { _asked[node].erase(input); ++_changes; }
        [[nodiscard]] bool Asked(const std::string& node, const std::string& input) const
        {
            const auto it = _asked.find(node);
            return it != _asked.end() && it->second.contains(input);
        }
        /** @brief The project's shader modules, by name: the .slang files of its shaders folder. Empty without a project. */
        [[nodiscard]] std::vector<std::string> ShaderModules() const;
        /** @brief The entry points a module of the project declares, in the order it does. */
        [[nodiscard]] std::vector<std::string> EntryPoints(const std::string& module) const;

        // ---- the scene view ---------------------------------------------------------------------------------

        EditorCamera camera;
        /** Whether the scene view shows what the scene's camera sees, rather than the editor's own view. */
        bool lookThroughCamera = false;
        [[nodiscard]] bool ThroughSceneCamera() const { return lookThroughCamera || InPlayMode(); }
        bool gizmoEnabled = true;
        kor::GizmoMode gizmo = kor::GizmoMode::eTranslate;
        void SetGizmo(const bool enabled, const kor::GizmoMode mode) { gizmoEnabled = enabled; gizmo = mode; ++_changes; }
        void SetLookThroughCamera(const bool through) { lookThroughCamera = through; ++_changes; }
        /** What the scene's panel shows the scene through: its pointer, for the gizmo and for picking. */
        std::shared_ptr<kgui::ViewportState> viewport = std::make_shared<kgui::ViewportState>();
        /** @brief Points the editor's view at @p entity, and stops looking through the scene's camera to show it. */
        void Frame(kecs::Entity entity);
        /** @brief Frames a second, averaged over the last quarter of one. */
        [[nodiscard]] int Fps() const { return _fps; }
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
        /** @brief Ends play, and puts the scene back as it was when it began. */
        void Stop();
        [[nodiscard]] bool InPlayMode() const { return !_snapshot.empty(); }
        [[nodiscard]] bool IsPlaying() const { return _play.Playing(); }

        // ---- entities ---------------------------------------------------------------------------------------

        kecs::Entity Duplicate(kecs::Entity entity);
        void Delete(kecs::Entity entity);
        /** @brief The objects there are to make: the project's `objects/*.json`, by name. Read again by LoadObjects. */
        [[nodiscard]] const std::vector<ObjectTemplate>& Objects() const { return _objects; }
        void LoadObjects();
        kecs::Entity Create(const ObjectTemplate& object, kecs::Entity parent = kecs::NullEntity);
        /** @brief Saves @p entity, and what is under it, as an object of its name: a file among the project's objects. */
        bool SaveAsObject(kecs::Entity entity);
        /**
         * @brief Starts the scene over as the standard one: the graph that draws meshes lit under a sky, and a sky,
         *        a camera, a sun and a sphere. Needs a project, for its shaders to be in.
         */
        bool NewStandardScene();
        /** @brief Reads the scene again from its file. */
        bool Revert();

        std::string selectedNode;                       ///< The node the inspector shows, when no entity is picked.
        kecs::Entity selectedEntity = kecs::NullEntity; ///< The entity it shows.
        void Select(std::string node) { selectedNode = std::move(node); selectedEntity = kecs::NullEntity; ++_changes; }
        void Select(const kecs::Entity entity) { selectedEntity = entity; selectedNode.clear(); ++_changes; }
        /** @brief Whether what is under @p entity is folded away in the outliner. Open, to begin with. */
        [[nodiscard]] bool Folded(const kecs::Entity entity) const { return _folded.contains(entity); }
        void Fold(const kecs::Entity entity, const bool folded) { if (folded) _folded.insert(entity); else _folded.erase(entity); ++_changes; }

        /** @brief The last thing done, for the foot of the window; and whether it went wrong. */
        [[nodiscard]] const std::string& Status() const { return _status; }
        [[nodiscard]] bool Failed() const { return _failed; }
        void Say(std::string status, bool failed = false) { _status = std::move(status); _failed = failed; ++_changes; }

        /** @brief Asks for the list of nodes to add, as a right click on the graph does: the interface opens it. */
        void AskForNodes() { _wantsNodes = true; ++_changes; }
        [[nodiscard]] bool TakeAskForNodes() { return std::exchange(_wantsNodes, false); }

        /** @brief Goes up when anything the panels show changed: the document, what is picked, the status. */
        [[nodiscard]] std::uint64_t Version() { return Doc().Version() + _changes; }

    private:
        PlayScene& _play;
        Project _project;
        bool _hasProject = false;
        std::string _scene = "main";
        std::string _status = "A scene with nothing in it. Right-click the graph to add a node.";
        bool _failed = false;
        std::uint64_t _changes = 0;
        std::map<std::string, std::set<std::string>> _asked;
        std::set<kecs::Entity> _folded;
        std::string _snapshot;              // the entities as they were when play began
        std::vector<ObjectTemplate> _objects;
        int _fps = 0, _framesCounted = 0;
        float _secondsCounted = 0.f;
        std::optional<kor::Vec2> _lastMouse;
        std::optional<kor::MouseButton> _viewDrag;
        std::optional<kor::Vec2> _pressAt;
        bool _gizmoMoved = false, _wasDown = false;
        bool _wantsNodes = false;
    };

    /**
     * @brief The kinds of node there are to add, for what someone typed looking for one: best match first. Each word
     *        typed has to be found — in the name, in another word for it ("texture" finds Image), in its category or
     *        in what it says it does — and a name that starts with it counts for most. Everything, when nothing is typed.
     */
    [[nodiscard]] std::vector<const kgraph::NodeType*> FindNodeTypes(std::string_view typed);

    /** @brief The whole interface, over @p session. */
    [[nodiscard]] kui::Widget EditorRoot(std::shared_ptr<Session> session);

    /** @brief The editor's own scene: its window is the interface, and the scene it edits runs offscreen inside it. */
    class EditorScene final : public kor::Scene {
    public:
        /** @brief Opens on the project in @p project, or on an empty scene when there is none. */
        explicit EditorScene(std::filesystem::path project = {});
        explicit EditorScene(const kor::SceneArgs& arguments);  ///< "project": its folder.
        ~EditorScene() override;

        void Initialize() override;
        void Update() override;
        void Shutdown() override;

        [[nodiscard]] Session& CurrentSession() { return *_session; }
        [[nodiscard]] kui::Ui& Interface() { return *_ui; }

    private:
        std::filesystem::path _projectPath;
        PlayScene* _play = nullptr;
        std::shared_ptr<Session> _session;
        std::unique_ptr<kui::Ui> _ui;
    };
}
