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
#include <scene.h>

#include <kgraph/catalog.h>

#include "document.h"
#include "play.h"
#include "project.h"

namespace keditor
{
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

        std::string selectedNode;                       ///< The node the inspector shows, when no entity is picked.
        kecs::Entity selectedEntity = kecs::NullEntity; ///< The entity it shows.
        void Select(std::string node) { selectedNode = std::move(node); selectedEntity = kecs::NullEntity; ++_changes; }
        void Select(const kecs::Entity entity) { selectedEntity = entity; selectedNode.clear(); ++_changes; }

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
