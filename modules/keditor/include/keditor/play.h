//
// keditor: the scene a project's scene runs in. In the editor it is offscreen, shown in a panel; run on its own it
// is the window. Either way it is the same code running the same file.
//

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <scene.h>

#include <kgraph/runtime.h>

#include "document.h"
#include "overlay.h"

namespace keditor
{
    class PlayScene final : public kor::Scene {
    public:
        /** @brief Runs the scene file at @p file, or an empty scene when there is none. */
        explicit PlayScene(std::filesystem::path file = {});
        explicit PlayScene(const kor::SceneArgs& arguments);    ///< "scene": the file.

        void Initialize() override;
        void FixedUpdate() override;
        void Update() override;
        void LateUpdate() override;
        void Shutdown() override;

        [[nodiscard]] Document& Doc() { return *_document; }
        /** @brief Why the file it was opened with could not be loaded; empty when it was. */
        [[nodiscard]] const std::string& Error() const { return _error; }
        /** @brief Starts over with the scene in @p file, or with nothing in it. */
        bool Open(const std::filesystem::path& file, std::string& error);
        void New();

        /**
         * @brief Whether the scene plays: its scripts are updated. On, to begin
         *        with — a scene run on its own plays. An editor turns it off until it is told to play.
         */
        void SetPlaying(const bool playing) { _playing = playing; }
        [[nodiscard]] bool Playing() const { return _playing; }
        /**
         * @brief The scene starts playing from the beginning, or stops and is put back: its graph's setup pages run
         *        again (Runtime::RunSetupAgain), here and in any view that follows it, so that what they made starts from
         *        the world as it is. Not for a pause, or playing on from one.
         */
        void Restart();
        /** @brief One fixed step, as a frame's would be — the scripts' fixedUpdate, and the graph told of it — whether or not it plays. */
        void StepOnce();
        /** @brief How many times it has been restarted: what a view that follows it goes by. */
        [[nodiscard]] std::uint64_t Restarts() const { return _restarts; }
        /** @brief The scene running @p document, or null: how what only has the document reaches the scene. */
        [[nodiscard]] static PlayScene* Of(const Document& document);
        /**
         * @brief What is called each frame as the scene updates, before its passes are readied: where an editor
         *        moves its view and draws its gizmo, so that both are in the frame being made.
         */
        void OnFrame(std::function<void()> hook) { _onFrame = std::move(hook); }
        /**
         * @brief Has the scene picked from and drawn over as an editor does (Overlay), from here on and for every
         *        scene it opens: what a scene run on its own has no use for.
         */
        void EditAs();
        /** @brief What an editor draws over the scene and reads back of it; null unless EditAs was called. */
        [[nodiscard]] keditor::Overlay* Over() { return _overlay.get(); }
        /**
         * @brief Whose keys and mouse the scene's own scripts read (FlyCamera): the scene's window's, unless an
         *        editor showing the scene in a panel says its own.
         */
        void SetInput(const kor::Input* input, const kor::Window* window = nullptr) { _input = input; _inputWindow = window; }
        [[nodiscard]] const kor::Input& ScriptInput() { return _input ? *_input : SceneInput(); }
        /**
         * @brief How big, in pixels, the window is whose mouse the scripts read (ScriptInput): what its positions are
         *        from the top-left of — to make a ray of the cursor through the scene's camera.
         */
        [[nodiscard]] kor::UVec2 ScriptViewport() const { return _inputWindow ? _inputWindow->Extent() : SceneWindow().Extent(); }

    private:
        std::filesystem::path _file;
        std::unique_ptr<Document> _document;
        std::unique_ptr<Document> _replaced;    // the one before, until the frame it was drawn in is done
        std::string _error;
        bool _playing = true;
        std::uint64_t _restarts = 0;
        std::function<void()> _onFrame;
        const kor::Input* _input = nullptr;
        const kor::Window* _inputWindow = nullptr;      // whose that input is: the size its mouse is in
        bool _edited = false;
        std::unique_ptr<keditor::Overlay> _overlay, _overlayBefore;     // (the one before, as _replaced is kept)
    };

    /**
     * @brief The scene as it plays, beside the one an editor works on: what @p play runs, seen from the scene's own
     *        camera into this scene's window. Another view of the same graph and the same world (a kgraph::Runtime
     *        that follows the document's), with nothing picked and nothing of the editor's drawn over it: no outline,
     *        no gizmo, no camera's pyramid. Its window's keys and mouse are what an editor gives the scripts.
     *
     * Not shown (SetShown), it costs nothing: its passes are out of the frame and it follows no edit, until it is.
     */
    class GameScene final : public kor::Scene {
    public:
        explicit GameScene(PlayScene& play) : _play(play) {}

        void FixedUpdate() override;
        void Update() override;
        void LateUpdate() override;
        void Shutdown() override;

        /** @brief Whether its picture is seen: hidden, nothing of it is drawn. */
        void SetShown(bool shown) { _shown = shown; }
        [[nodiscard]] bool Shown() const { return _shown; }
        /** @brief Its view of the graph; null until it has first been shown a frame. */
        [[nodiscard]] kgraph::Runtime* Runtime() { return _runtime.get(); }
        /**
         * @brief Seen from @p view rather than the scene's camera, as an editor's view is — its debug views, what it
         *        hides — for a second view of the scene beside the editor's own. None, the scene's camera, as played.
         */
        void SetView(std::optional<kgraph::View> view, std::set<kecs::Entity> hidden = {})
        {
            _view = std::move(view);
            _hidden = std::move(hidden);
        }

    private:
        PlayScene& _play;
        const Document* _following = nullptr;
        std::unique_ptr<kgraph::Runtime> _runtime, _before;     // (the one before, until the frame it was drawn in is done)
        std::uint64_t _seen = 0, _restarts = 0;
        bool _shown = false, _enabled = true;     // not drawn until whoever shows it says it is seen
        std::optional<kgraph::View> _view;
        std::set<kecs::Entity> _hidden;
    };
}
