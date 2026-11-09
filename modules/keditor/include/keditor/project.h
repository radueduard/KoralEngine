//
// keditor: a project on disk. A folder with `project.json`, its scenes under `scenes/` and its shaders under
// `shaders/`. A project is a set of scenes, scripts, shaders, functions, objects, prefabs and assets; a module is the
// same, minus the scenes — what it has is for the projects that import it, and its one scene, `test`, is only for
// trying it out. A project, or a module, imports modules whose scripts are in its language: what they have is its too.
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
        /**
         * What a model file's materials are imported as, by what the file reports them as (Document::SetImportKind):
         * `"pbr"` and `"classic"` to a material kind's name, for every file (`""`) or for one (`@models/old.obj`). In
         * `project.json` when it is not as it starts.
         */
        std::map<std::string, std::map<std::string, std::string>> importKinds;
        /** A module: what it has is for the projects that import it, and its one scene is `test`. In `project.json` as `"kind": "module"`. */
        bool module = false;
        /**
         * The modules it imports, each its folder from this one's (`../physics`), whole, or one of the engine's by its name
         * (`@modules/filters`: in OfficialModules). In `project.json` as `"modules"`.
         */
        std::vector<std::string> modules;
        /**
         * A module's: the version of the engine's shader library it was made for (LibraryVersion), what its shaders import and
         * build on. 0 for one made before modules said so. In `project.json` as `"library"`.
         */
        int library = 0;

        /** @brief The version of the engine's shader library (packages/koral/shaders): one more each time what a module builds on changes. */
        [[nodiscard]] static int LibraryVersion();
        /** @brief Where the engine's own modules are: packages/modules, beside its package. */
        [[nodiscard]] static std::filesystem::path OfficialModules();

        /**
         * @brief Makes a project in @p directory, which is made too: one empty scene, "main" — "test", for a module —
         *        and a `shaders` folder. Its scripts are in @p language from the start (Language): 0 leaves that to its
         *        first script. It imports @p modules (their folders: kept from this one's where they can be).
         */
        static bool Create(const std::filesystem::path& directory, std::string name, Project& out, std::string& error, char language = 0,
                           bool module = false, const std::vector<std::filesystem::path>& modules = {});
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
        /** @brief Makes its shaders the ones found first, then its modules': before a scene of it is loaded. */
        void Use() const;

        /**
         * @brief The modules it imports, and those they import, each once: the nearest first, in the order each says them.
         *        One that is not there, is no module, or has its scripts in another language than this one is left out
         *        and said in @p problems — as is one that imports, round about, what imports it. Said too, and kept: one
         *        made for another version of the shader library, and a shader or a function two of them have by one name
         *        (the nearest's is found first).
         */
        [[nodiscard]] std::vector<Project> Imported(std::vector<std::string>* problems = nullptr) const;
        /** @brief The folders of Imported: where what the modules have is. */
        [[nodiscard]] std::vector<std::filesystem::path> ImportedDirectories(std::vector<std::string>* problems = nullptr) const;
        /** @brief The modules found in @p folder, one level down: those a project made there is offered to import. */
        [[nodiscard]] static std::vector<Project> ModulesIn(const std::filesystem::path& folder);
        /**
         * @brief @p module 's folder as this one keeps it: one of the engine's by its name (`@modules/filters`), else from its own
         *        folder where the two share a drive, or whole.
         */
        [[nodiscard]] std::string Reference(const std::filesystem::path& module) const;
    };
}
