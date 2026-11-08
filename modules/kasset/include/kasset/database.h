#pragma once

/**
 * @file database.h
 * @brief Every asset of a project, by ID and by file: what Scan finds of the project's folders and their metas. Gives a
 *        file without a meta one, and keeps a model's parts' IDs as the file is read again.
 *
 * Touches nothing of the device, and keeps nothing but metas and where they are: run it anywhere, a build's tools
 * among them. Not safe to use from two threads at once.
 */

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "id.h"
#include "meta.h"

namespace kasset
{
    /** @brief An asset of the project's: its meta, and the file it is beside. */
    struct Entry {
        Meta meta;
        std::filesystem::path file;     ///< From the project's folder, `/` between folders: `assets/textures/wall.png` — a module's `../physics/assets/wall.png`.
    };

    /** @brief What happened to a kind of part when a file was read again (Database::RecordParts). */
    struct PartsRecorded {
        std::vector<Id> ids;            ///< Each part's, in the order they were given.
        std::vector<Part> gone;         ///< Parts it had that the file no longer has: what held them has nothing now.
    };

    class Database {
    public:
        /** @brief A project's, at @p root: nothing in it until Scan. */
        explicit Database(std::filesystem::path root);

        [[nodiscard]] const std::filesystem::path& Root() const { return _root; }
        /**
         * @brief The folders of the modules the project imports: their assets are the project's too, found by Scan after
         *        its own and kept by the IDs their metas give them. A module's scenes are its own, and not among them. An
         *        Entry of a module's has its file from the project's folder (`../physics/assets/wall.png`), or whole when
         *        the two are on different drives. Taken from the next Scan on.
         */
        void SetModules(std::vector<std::filesystem::path> modules) { _modules = std::move(modules); }
        [[nodiscard]] const std::vector<std::filesystem::path>& Modules() const { return _modules; }
        /** @brief The folder of the module @p file is in, when it is in one of them rather than in the project. */
        [[nodiscard]] std::optional<std::filesystem::path> ModuleOf(const std::filesystem::path& file) const;
        /**
         * @brief Where @p file 's meta is, whole: in the MetaFolder of the project — or of the module @p file is in — in
         *        the folders @p file is in there (MetaPathOf). @p file is whole or from the project's folder.
         */
        [[nodiscard]] std::filesystem::path MetaOf(const std::filesystem::path& file) const;
        /** @brief Where a file named by an old `@` reference is: the project's `assets` folder. */
        [[nodiscard]] std::filesystem::path AssetDirectory() const { return _root / "assets"; }

        /**
         * @brief Finds every asset in the project's folders, and reads its meta (MetaOf). A file without one is given
         *        one, with a new ID. A meta whose file is gone is given to the one file of that name without a meta,
         *        wherever it is (a file moved without its meta), or else left and said of. A file copied with its meta
         *        is given a new ID. A meta beside its file, where they were kept before MetaFolder, is moved there.
         *        Hidden folders, `cache`, and what scripts are built into (`bin`, `obj`, `build`) are passed by.
         *
         * @return What was wrong, a sentence each: a meta that could not be read or written, one left of no file.
         */
        std::vector<std::string> Scan();

        [[nodiscard]] const Entry* Find(Id id) const;
        /** @brief The asset @p file is: a path from the project's folder, or a whole one. */
        [[nodiscard]] const Entry* FindFile(const std::filesystem::path& file) const;
        /** @brief Every asset, in no order. */
        [[nodiscard]] std::vector<const Entry*> Entries() const;
        [[nodiscard]] std::size_t Size() const { return _entries.size(); }

        /**
         * @brief Makes @p file an asset when it is not one yet, as Scan would: its meta read, or written new. Nothing,
         *        with why, for a file that is no kind of asset, or is not in the project.
         */
        std::expected<const Entry*, std::string> Add(const std::filesystem::path& file);

        /**
         * @brief Gives the parts of @p kind of asset @p asset IDs, as its file names them now (@p names, in the file's
         *        order): each the ID the part of that name had — the second of a name the second's — and one whose name
         *        is new the ID of the one of no name found in its place, or a new one. What it had that is not found
         *        again is gone. The meta is written when anything changed.
         */
        std::expected<PartsRecorded, std::string> RecordParts(Id asset, Kind kind, const std::vector<std::string>& names);
        /**
         * @brief The ID of part @p index of @p kind of @p asset: the one it has, or — before its file has been read for
         *        its names — a new one of no name, which RecordParts then names. What an old `@model.gltf#3` becomes.
         */
        std::expected<Id, std::string> PartAt(Id asset, Kind kind, int index);
        /** @brief The ID of @p asset 's part of @p kind called @p name: the one it has, or a new one. A shader's entry point. */
        std::expected<Id, std::string> PartNamed(Id asset, Kind kind, std::string_view name);

        /**
         * @brief Sets parameter @p key of asset @p id 's meta to @p json — empty takes it away — and writes the meta
         *        when that changed it. True when it did.
         */
        std::expected<bool, std::string> SetParameter(Id id, std::string_view key, std::string_view json);

        /**
         * @brief An asset of the engine's own, of no file: a default mesh or material. Known by its old name (`$cube`,
         *        `material:Gold`) as well as by Id::Named(@p name).
         */
        Id AddBuiltIn(Kind kind, std::string_view name, std::string legacy);
        /** @brief Whether @p id is one of the engine's own, and which kind. */
        [[nodiscard]] std::optional<Kind> BuiltInKind(Id id) const;

        /**
         * @brief What an old reference names, as a Reference: `@textures/wall.png`, `@sponza/Sponza.gltf#3`,
         *        `@sponza/Sponza.gltf#material:2`, `$cube`, `material:Gold`, `shader:lit`, `shader:lit#vertex`,
         *        `script:Spin.cs`, `prefab:Tree.prefab`, `scene:main`. Nothing for one that names no asset of the
         *        project's (a file outside it, a mesh a script made) or is no old reference. A part it names is
         *        given an ID when it has none.
         */
        std::optional<Reference> FromLegacy(std::string_view legacy);
        /** @brief The old reference @p reference was: what the runtime still finds things by. Empty for none. */
        [[nodiscard]] std::string ToLegacy(const Reference& reference) const;
        /**
         * @brief What @p text names — a reference (`<id>/<part>`) or an old one (`@ship.gltf#3`) — changing nothing: a
         *        part not given an ID yet is its asset's. Nothing for what names no asset of the project's.
         */
        [[nodiscard]] std::optional<Reference> Lookup(std::string_view text) const;

        // ---- which uses which ---------------------------------------------------------------------------------------

        /**
         * @brief Reads every asset that names others — scenes, prefabs, functions, materials, generators, and what was
         *        changed of a model's materials — for what it names. What Users and Uses then say.
         */
        void IndexReferences();
        /** @brief Reads @p id again for what it names: after it was saved. */
        void Reindex(Id id);
        /** @brief The assets that name @p id, or a part of it: what has nothing once it is gone. */
        [[nodiscard]] std::vector<Id> Users(Id id) const;
        /** @brief What asset @p id names, each once. */
        [[nodiscard]] std::vector<Reference> Uses(Id id) const;
        /**
         * @brief Writes each asset that names @p id by where it is (`@textures/wall.png`) — a scene, a prefab, a material
         *        saved before it was named by its ID — naming it by its ID instead, so that it still does once @p id is
         *        moved. What was written, a path each.
         */
        std::expected<std::vector<std::filesystem::path>, std::string> KeepUsersByIds(Id id);
        /**
         * @brief Moves asset @p id 's file — and its meta, and for a model what it names beside it — to @p to (from the
         *        project's folder, or whole). What names it by its ID still does. Not over a file that is there.
         */
        std::expected<void, std::string> Move(Id id, const std::filesystem::path& to);

    private:
        Entry* Mutable(Id id);
        std::expected<void, std::string> Store(Entry& entry);
        void Index(Entry entry);
        [[nodiscard]] std::string Relative(const std::filesystem::path& file) const;

        std::filesystem::path _root;
        std::vector<std::filesystem::path> _modules;
        std::unordered_map<Id, Entry> _entries;
        std::map<std::string, Id, std::less<>> _byFile;     ///< By Entry::file.
        struct BuiltIn { Kind kind; std::string legacy; };
        std::unordered_map<Id, BuiltIn> _builtIns;
        std::map<std::string, Id, std::less<>> _builtInsByLegacy;
        std::unordered_map<Id, std::vector<Reference>> _uses;     ///< By the asset that names them.
    };
}
