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

#include "document.h"

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
         * @brief What is called each frame as the scene updates, before its passes are readied: where an editor
         *        moves its view and draws its gizmo, so that both are in the frame being made.
         */
        void OnFrame(std::function<void()> hook) { _onFrame = std::move(hook); }

    private:
        std::filesystem::path _file;
        std::unique_ptr<Document> _document;
        std::unique_ptr<Document> _replaced;    // the one before, until the frame it was drawn in is done
        std::string _error;
        bool _playing = true;
        std::function<void()> _onFrame;
    };
}
