//
// keditor: the scene a project's scene runs in. In the editor it is offscreen, shown in a panel; run on its own it
// is the window. Either way it is the same code running the same file.
//

#pragma once

#include <filesystem>
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
        void Update() override;
        void Shutdown() override;

        [[nodiscard]] Document& Doc() { return *_document; }
        /** @brief Why the file it was opened with could not be loaded; empty when it was. */
        [[nodiscard]] const std::string& Error() const { return _error; }
        /** @brief Starts over with the scene in @p file, or with nothing in it. */
        bool Open(const std::filesystem::path& file, std::string& error);
        void New();

    private:
        std::filesystem::path _file;
        std::unique_ptr<Document> _document;
        std::string _error;
    };
}
