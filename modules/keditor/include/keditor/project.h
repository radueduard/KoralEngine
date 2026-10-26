//
// keditor: a project on disk. A folder with `project.json`, its scenes under `scenes/` and its shaders under
// `shaders/`.
//

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace keditor
{
    struct Project {
        std::filesystem::path directory;
        std::string name;
        std::vector<std::string> scenes;    ///< By name: `scenes/<name>.scene.json`.
        std::string start;                  ///< The scene a run opens.
        /**
         * The language its scripts are in, all of them: "kotlin" or "csharp" — said by its first script, and kept in
         * `project.json`. Empty until then: one with scripts from before is taken to be in theirs (Language).
         */
        std::string scripts;
        /**
         * What the scene's pictures are offered as when they are compressed, by kind (kgraph::TextureKindName → kgraph::
         * TextureFormatName): `"normal": "BC5"`. A kind not here is offered kgraph::DefaultTextureFormat. In `project.json`.
         */
        std::map<std::string, std::string> textureFormats;
        /** Whether a scene opened with pictures not compressed asks to compress them. In `project.json` when it does not. */
        bool askToCompress = true;

        /** @brief Makes a project in @p directory, which is made too: one empty scene, "main", and a `shaders` folder. */
        static bool Create(const std::filesystem::path& directory, std::string name, Project& out, std::string& error);
        /** @brief Opens the project in @p directory, or the one a `project.json` given directly is of. */
        static bool Open(const std::filesystem::path& directory, Project& out, std::string& error);
        /** @brief Writes `project.json`. */
        bool Save(std::string& error) const;

        [[nodiscard]] std::filesystem::path ScenePath(std::string_view scene) const;
        /**
         * @brief Which language its scripts are in: 'k' for Kotlin, 'c' for C#, 0 for none yet. What `scripts` says,
         *        or — unsaid — what the files in its `scripts` folder are (Kotlin, when there are both).
         */
        [[nodiscard]] char Language() const;
        /** @brief Scripts in its folder that are not in its language: what is wrong, and why they do not run. Empty when none are. */
        [[nodiscard]] std::vector<std::filesystem::path> ScriptsInAnotherLanguage() const;
        /** @brief "kotlin" or "csharp", from 'k' or 'c'; empty for anything else. */
        [[nodiscard]] static std::string LanguageName(char language);
        [[nodiscard]] std::filesystem::path ShaderDirectory() const { return directory / "shaders"; }
        /** @brief Makes its shaders the ones found first: before a scene of it is loaded. */
        void Use() const;
    };
}
