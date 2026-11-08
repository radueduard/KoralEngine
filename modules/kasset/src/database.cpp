#include <kasset/database.h>
#include <kasset/beside.h>
#include <kasset/parameters.h>
#include <kasset/rewrite.h>

#include <kecs/json.h>

#include <fstream>
#include <sstream>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <set>
#include <utility>

namespace kasset
{
    namespace {
        /** A folder Scan does not go into: a hidden one, the cache, and what scripts are built into. */
        bool PassedBy(const std::filesystem::path& folder)
        {
            static constexpr std::array<std::string_view, 5> skipped { "cache", "bin", "obj", "build", "node_modules" };
            const std::string name = folder.filename().string();
            return name.starts_with('.') || std::ranges::contains(skipped, name);
        }

        std::optional<int> Number(const std::string_view text)
        {
            int value = 0;
            const auto [end, failed] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (failed != std::errc {} || end != text.data() + text.size() || value < 0) return std::nullopt;
            return value;
        }

        /** The file a meta kept beside it is of: `wall.png.meta` → `wall.png`. */
        std::filesystem::path FileBeside(const std::filesystem::path& meta)
        {
            std::filesystem::path source = meta;
            source.replace_extension();
            return source;
        }
    }

    Database::Database(std::filesystem::path root) : _root(std::move(root)) {}

    std::filesystem::path Database::MetaOf(const std::filesystem::path& file) const
    {
        std::error_code failed;
        const std::filesystem::path whole = std::filesystem::weakly_canonical(file.is_absolute() ? file : _root / file, failed);
        const std::filesystem::path root = std::filesystem::weakly_canonical(ModuleOf(whole).value_or(_root), failed);
        return MetaPathOf(root, whole);
    }

    std::string Database::Relative(const std::filesystem::path& file) const
    {
        std::error_code failed;
        const std::filesystem::path whole = std::filesystem::weakly_canonical(file.is_absolute() ? file : _root / file, failed);
        const std::filesystem::path root = std::filesystem::weakly_canonical(_root, failed);
        const std::filesystem::path relative = whole.lexically_relative(root);
        if (failed) return {};
        const std::string text = relative.generic_string();
        if (!relative.empty() && text != "." && !text.starts_with("..")) return text;
        // Not the project's own: one of a module's, named from the project's folder — or whole, on another drive.
        if (!ModuleOf(whole)) return {};
        return whole.root_name() == root.root_name() && !relative.empty() ? text : whole.generic_string();
    }

    std::optional<std::filesystem::path> Database::ModuleOf(const std::filesystem::path& file) const
    {
        std::error_code failed;
        const std::filesystem::path whole = std::filesystem::weakly_canonical(file.is_absolute() ? file : _root / file, failed);
        for (const auto& module : _modules) {
            const std::string text = whole.lexically_relative(std::filesystem::weakly_canonical(module, failed)).generic_string();
            if (!text.empty() && text != "." && !text.starts_with("..")) return module;
        }
        return std::nullopt;
    }

    void Database::Index(Entry entry)
    {
        const Id id = entry.meta.id;
        _byFile[entry.file.generic_string()] = id;
        _entries[id] = std::move(entry);
    }

    Entry* Database::Mutable(const Id id)
    {
        const auto at = _entries.find(id);
        return at == _entries.end() ? nullptr : &at->second;
    }

    std::expected<void, std::string> Database::Store(Entry& entry) { return Save(MetaOf(entry.file), entry.meta); }

    std::vector<std::string> Database::Scan()
    {
        _entries.clear();
        _byFile.clear();
        std::vector<std::string> problems;

        // What is in the folders: the files that are assets; and the metas, each with the folder it is kept under — the
        // project's, or a module's (MetaFolder).
        std::vector<std::filesystem::path> sources;
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> metas;
        std::error_code failed;
        // The project's folders, then each module's: a module's own scenes are for trying it out, and not the project's.
        std::vector<std::filesystem::path> roots { _root };
        roots.insert(roots.end(), _modules.begin(), _modules.end());
        for (const auto& root : roots) {
            const bool module = root != _root;
            std::vector<std::filesystem::path> beside;      // metas where they were kept before MetaFolder: beside their files
            for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, failed);
                 !failed && it != std::filesystem::recursive_directory_iterator(); it.increment(failed)) {
                if (it->is_directory(failed)) {
                    if (PassedBy(it->path()) || (module && it.depth() == 0 && it->path().filename() == "scenes")) it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(failed)) continue;
                if (module && it.depth() == 0 && it->path().filename() == "project.json") continue;
                if (IsMeta(it->path())) beside.push_back(it->path());
                else if (KindOfFile(it->path())) sources.push_back(it->path());
            }
            if (failed) {
                problems.push_back(std::format("{}'s folders could not all be read: {}", module ? root.filename().string() : std::string("The project"), failed.message()));
                failed.clear();
            }
            // Each kept beside its file, moved to where metas are kept now — unless one is there already, which is the one.
            for (const auto& meta : beside) {
                const std::filesystem::path kept = MetaPathOf(root, FileBeside(meta));
                if (std::filesystem::exists(kept, failed)) {
                    problems.push_back(std::format("{} is a meta beside its file, where they are kept no more, and {} is its file's: it is left", Relative(meta), Relative(kept)));
                    continue;
                }
                std::filesystem::create_directories(kept.parent_path(), failed);
                std::filesystem::rename(meta, kept, failed);
                if (failed) {
                    problems.push_back(std::format("{} could not be moved to {}: {}", Relative(meta), Relative(kept), failed.message()));
                    failed.clear();
                }
            }
            // The metas where they are kept.
            const std::filesystem::path folder = root / MetaFolder;
            if (!std::filesystem::is_directory(folder, failed)) { failed.clear(); continue; }
            for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, failed);
                 !failed && it != std::filesystem::recursive_directory_iterator(); it.increment(failed))
                if (it->is_regular_file(failed) && IsMeta(it->path())) metas.emplace_back(root, it->path());
            failed.clear();
        }
        // In order, so that which of two copies keeps its ID is the same each time.
        std::ranges::sort(sources);
        std::ranges::sort(metas);

        // A meta whose file is gone goes to the one file of that name with none: a file moved without it.
        std::set<std::filesystem::path> withMeta;
        for (const auto& source : sources)
            if (KindOfFile(source) == Kind::eGenerated || std::filesystem::exists(MetaOf(source), failed)) withMeta.insert(source);
        for (const auto& [root, meta] : metas) {
            const std::filesystem::path source = FileOfMeta(root, meta);
            if (std::filesystem::exists(source, failed)) continue;
            std::vector<std::filesystem::path> named;
            for (const auto& candidate : sources)
                if (candidate.filename() == source.filename() && !withMeta.contains(candidate)) named.push_back(candidate);
            if (named.size() == 1) {
                const std::filesystem::path now = MetaOf(named.front());
                std::filesystem::create_directories(now.parent_path(), failed);
                std::filesystem::rename(meta, now, failed);
                if (!failed) { withMeta.insert(named.front()); continue; }
                problems.push_back(std::format("{} is {} moved, but its meta could not go with it: {}", Relative(named.front()), source.filename().string(), failed.message()));
                failed.clear();
                continue;
            }
            problems.push_back(named.empty()
                ? std::format("{} is of {}, which is gone: what named it has nothing now", Relative(meta), Relative(source))
                : std::format("{} is of {}, which is gone, and {} files of that name have no meta: which it was is not known", Relative(meta), Relative(source), named.size()));
        }

        for (const auto& source : sources) {
            Entry entry;
            entry.file = Relative(source);
            const std::filesystem::path metaPath = MetaOf(source);
            if (withMeta.contains(source)) {
                auto meta = Load(metaPath);
                if (!meta) {
                    // One that cannot be read is left as it is, for whoever wrote it to mend: the asset is not listed till then.
                    problems.push_back(std::format("{} could not be read: {}", Relative(metaPath), meta.error()));
                    continue;
                }
                entry.meta = std::move(*meta);
                if (_entries.contains(entry.meta.id)) {
                    // A file copied with its meta: the copy is another asset.
                    problems.push_back(std::format("{} had the ID of {}, as a copy does: it is given its own", entry.file.generic_string(), _entries.at(entry.meta.id).file.generic_string()));
                    entry.meta.id = Id::New();
                    entry.meta.parts.clear();
                    if (auto saved = Save(metaPath, entry.meta); !saved) problems.push_back(saved.error());
                }
            } else {
                entry.meta.id = Id::New();
                entry.meta.kind = *KindOfFile(source);
                if (auto saved = Save(metaPath, entry.meta); !saved) problems.push_back(saved.error());
            }
            Index(std::move(entry));
        }
        return problems;
    }

    const Entry* Database::Find(const Id id) const
    {
        const auto at = _entries.find(id);
        return at == _entries.end() ? nullptr : &at->second;
    }

    const Entry* Database::FindFile(const std::filesystem::path& file) const
    {
        const auto at = _byFile.find(Relative(file));
        return at == _byFile.end() ? nullptr : Find(at->second);
    }

    std::vector<const Entry*> Database::Entries() const
    {
        std::vector<const Entry*> entries;
        entries.reserve(_entries.size());
        for (const auto& [id, entry] : _entries) entries.push_back(&entry);
        return entries;
    }

    std::expected<const Entry*, std::string> Database::Add(const std::filesystem::path& file)
    {
        if (const Entry* known = FindFile(file)) return known;
        const std::string relative = Relative(file);
        if (relative.empty()) return std::unexpected(std::format("{} is not in the project", file.string()));
        const auto kind = KindOfFile(file);
        if (!kind) return std::unexpected(std::format("{} is no kind of asset", relative));
        const std::filesystem::path whole = _root / relative;
        Entry entry;
        entry.file = relative;
        std::error_code failed;
        if (const auto metaPath = MetaOf(whole); std::filesystem::exists(metaPath, failed)) {
            auto meta = Load(metaPath);
            if (!meta) return std::unexpected(meta.error());
            entry.meta = std::move(*meta);
            if (_entries.contains(entry.meta.id)) {
                entry.meta.id = Id::New();
                entry.meta.parts.clear();
                if (auto saved = Store(entry); !saved) return std::unexpected(saved.error());
            }
        } else {
            entry.meta.id = Id::New();
            entry.meta.kind = *kind;
            if (auto saved = Store(entry); !saved) return std::unexpected(saved.error());
        }
        const Id id = entry.meta.id;
        Index(std::move(entry));
        return Find(id);
    }

    std::expected<PartsRecorded, std::string> Database::RecordParts(const Id asset, const Kind kind, const std::vector<std::string>& names)
    {
        Entry* entry = Mutable(asset);
        if (!entry) return std::unexpected(std::format("{} is no asset of the project's", asset.Text()));
        std::vector<Part>& parts = entry->meta.parts;
        std::vector<std::size_t> old;   // Among parts: those of this kind.
        for (std::size_t i = 0; i < parts.size(); ++i) if (parts[i].kind == kind) old.push_back(i);
        std::vector<bool> taken(old.size(), false);

        PartsRecorded recorded;
        recorded.ids.resize(names.size());
        // By name first: the k-th of a name is the part that was the k-th of it.
        std::map<std::string_view, int> seen;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i].empty()) continue;
            int occurrence = seen[names[i]]++;
            for (std::size_t o = 0; o < old.size(); ++o)
                if (parts[old[o]].name == names[i] && occurrence-- == 0) {
                    if (!taken[o]) { taken[o] = true; recorded.ids[i] = parts[old[o]].id; }
                    break;
                }
        }
        // Then by where it is, for a part of no name, or whose name the file no longer has (renamed in it).
        const std::set<std::string_view> named(names.begin(), names.end());
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (recorded.ids[i]) continue;
            for (std::size_t o = 0; o < old.size(); ++o) {
                const Part& part = parts[old[o]];
                if (taken[o] || part.index != static_cast<int>(i) || (!part.name.empty() && named.contains(part.name))) continue;
                taken[o] = true;
                recorded.ids[i] = part.id;
                break;
            }
        }
        for (Id& id : recorded.ids) if (!id) id = Id::New();

        std::vector<Part> rebuilt;
        for (const Part& part : parts) if (part.kind != kind) rebuilt.push_back(part);
        for (std::size_t o = 0; o < old.size(); ++o) if (!taken[o]) recorded.gone.push_back(parts[old[o]]);
        for (std::size_t i = 0; i < names.size(); ++i) rebuilt.push_back({ recorded.ids[i], kind, names[i], static_cast<int>(i) });
        if (rebuilt != parts) {
            parts = std::move(rebuilt);
            if (auto saved = Store(*entry); !saved) return std::unexpected(saved.error());
        }
        return recorded;
    }

    std::expected<Id, std::string> Database::PartAt(const Id asset, const Kind kind, const int index)
    {
        Entry* entry = Mutable(asset);
        if (!entry) return std::unexpected(std::format("{} is no asset of the project's", asset.Text()));
        if (const Part* part = entry->meta.PartAt(kind, index)) return part->id;
        const Id id = Id::New();
        entry->meta.parts.push_back({ id, kind, {}, index });
        if (auto saved = Store(*entry); !saved) return std::unexpected(saved.error());
        return id;
    }

    std::expected<Id, std::string> Database::PartNamed(const Id asset, const Kind kind, const std::string_view name)
    {
        Entry* entry = Mutable(asset);
        if (!entry) return std::unexpected(std::format("{} is no asset of the project's", asset.Text()));
        if (const Part* part = entry->meta.FindPart(kind, name)) return part->id;
        const Id id = Id::New();
        entry->meta.parts.push_back({ id, kind, std::string(name), -1 });
        if (auto saved = Store(*entry); !saved) return std::unexpected(saved.error());
        return id;
    }

    std::expected<bool, std::string> Database::SetParameter(const Id id, const std::string_view key, const std::string_view json)
    {
        Entry* entry = Mutable(id);
        if (!entry) return std::unexpected(std::format("{} is no asset of the project's", id.Text()));
        if (!kasset::SetParameter(entry->meta, key, json)) return false;
        if (auto saved = Store(*entry); !saved) return std::unexpected(saved.error());
        return true;
    }

    Id Database::AddBuiltIn(const Kind kind, const std::string_view name, std::string legacy)
    {
        const Id id = Id::Named(name);
        _builtInsByLegacy[legacy] = id;
        _builtIns[id] = { kind, std::move(legacy) };
        return id;
    }

    std::optional<Kind> Database::BuiltInKind(const Id id) const
    {
        const auto at = _builtIns.find(id);
        if (at == _builtIns.end()) return std::nullopt;
        return at->second.kind;
    }

    std::optional<Reference> Database::FromLegacy(const std::string_view legacy)
    {
        if (const auto at = _builtInsByLegacy.find(legacy); at != _builtInsByLegacy.end()) return Reference { at->second, {} };

        // A file of the project's, under a prefix and in its folder: `shader:lit#vertex`, `script:Spin.cs`.
        const auto fileIn = [&](const std::string_view prefix, const std::filesystem::path& folder, const std::string_view suffix) -> std::optional<Reference> {
            const std::string_view rest = legacy.substr(prefix.size());
            const auto hash = rest.find('#');
            const Entry* entry = FindFile(folder / std::format("{}{}", rest.substr(0, hash), suffix));
            if (!entry) return std::nullopt;
            if (hash == std::string_view::npos) return Reference { entry->meta.id, {} };
            auto part = PartNamed(entry->meta.id, Kind::eShaderEntry, rest.substr(hash + 1));
            if (!part) return std::nullopt;
            return Reference { entry->meta.id, *part };
        };
        if (legacy.starts_with("shader:")) return fileIn("shader:", "shaders", ".slang");
        if (legacy.starts_with("script:") && !legacy.contains('#')) return fileIn("script:", "scripts", "");
        if (legacy.starts_with("prefab:") && !legacy.contains('#')) return fileIn("prefab:", "prefabs", "");
        if (legacy.starts_with("scene:") && !legacy.contains('#')) return fileIn("scene:", "scenes", ".scene.json");
        if (legacy.starts_with("function:") && !legacy.contains('#')) return fileIn("function:", "functions", ".kfunc");

        if (!legacy.starts_with('@')) return std::nullopt;
        const std::string_view rest = legacy.substr(1);
        const auto hash = rest.rfind('#');
        const std::filesystem::path path(std::string(rest.substr(0, hash)));
        // Named from the assets folder, as the runtime names what is under it; or by its whole path.
        const Entry* entry = FindFile(path.is_absolute() ? path : AssetDirectory() / path);
        if (!entry) return std::nullopt;
        if (hash == std::string_view::npos) return Reference { entry->meta.id, {} };
        std::string_view what = rest.substr(hash + 1);
        Kind kind = Kind::eMesh;
        if (what.starts_with("material:")) {
            kind = Kind::eMaterial;
            what.remove_prefix(9);
        }
        const auto index = Number(what);
        if (!index) return std::nullopt;
        auto part = PartAt(entry->meta.id, kind, *index);
        if (!part) return std::nullopt;
        return Reference { entry->meta.id, *part };
    }

    std::string Database::ToLegacy(const Reference& reference) const
    {
        if (const auto at = _builtIns.find(reference.asset); at != _builtIns.end()) return reference.part ? std::string() : at->second.legacy;
        const Entry* entry = Find(reference.asset);
        if (!entry) return {};
        const std::filesystem::path& file = entry->file;
        // What the runtime names a file by: from the assets folder when it is in it, and by its whole path when not.
        const auto named = [&] {
            const std::filesystem::path whole = (_root / file).lexically_normal();
            const std::string inAssets = whole.lexically_relative(AssetDirectory().lexically_normal()).generic_string();
            return "@" + (inAssets.empty() || inAssets.starts_with("..") ? whole.generic_string() : inAssets);
        };
        if (reference.part) {
            const Part* part = entry->meta.FindPart(reference.part);
            if (!part) return {};
            switch (part->kind) {
            case Kind::eMesh: return part->index < 0 ? std::string() : std::format("{}#{}", named(), part->index);
            case Kind::eMaterial: return part->index < 0 ? std::string() : std::format("{}#material:{}", named(), part->index);
            case Kind::eShaderEntry: return std::format("shader:{}#{}", file.stem().string(), part->name);
            default: return {};
            }
        }
        switch (entry->meta.kind) {
        case Kind::eShader: return "shader:" + file.stem().string();
        case Kind::eScript: return "script:" + file.filename().string();
        case Kind::ePrefab: return "prefab:" + file.filename().string();
        case Kind::eScene: {
            const std::string name = file.filename().string();
            return "scene:" + name.substr(0, name.size() - std::string_view(".scene.json").size());
        }
        case Kind::eFunction: return "function:" + file.stem().string();     // what a call keeps it by (its `file`)
        default: return named();
        }
    }

    std::optional<Reference> Database::Lookup(const std::string_view text) const
    {
        if (const auto reference = Reference::Parse(text)) return Find(reference->asset) || _builtIns.contains(reference->asset) ? reference : std::nullopt;
        if (const auto at = _builtInsByLegacy.find(text); at != _builtInsByLegacy.end()) return Reference { at->second, {} };
        const auto fileIn = [&](const std::string_view prefix, const std::filesystem::path& folder, const std::string_view suffix) -> std::optional<Reference> {
            const std::string_view rest = text.substr(prefix.size());
            const auto hash = rest.find('#');
            const Entry* entry = FindFile(folder / std::format("{}{}", rest.substr(0, hash), suffix));
            if (!entry) return std::nullopt;
            Reference reference { entry->meta.id, {} };
            if (hash != std::string_view::npos)
                if (const Part* part = entry->meta.FindPart(Kind::eShaderEntry, rest.substr(hash + 1))) reference.part = part->id;
            return reference;
        };
        if (text.starts_with("shader:")) return fileIn("shader:", "shaders", ".slang");
        if (text.starts_with("script:")) return fileIn("script:", "scripts", "");
        if (text.starts_with("prefab:")) return fileIn("prefab:", "prefabs", "");
        if (text.starts_with("scene:")) return fileIn("scene:", "scenes", ".scene.json");
        if (text.starts_with("function:")) return fileIn("function:", "functions", ".kfunc");
        if (!text.starts_with('@')) return std::nullopt;
        const std::string_view rest = text.substr(1);
        const auto hash = rest.rfind('#');
        const std::filesystem::path path(std::string(rest.substr(0, hash)));
        const Entry* entry = FindFile(path.is_absolute() ? path : AssetDirectory() / path);
        if (!entry) return std::nullopt;
        Reference reference { entry->meta.id, {} };
        if (hash != std::string_view::npos) {
            std::string_view what = rest.substr(hash + 1);
            Kind kind = Kind::eMesh;
            if (what.starts_with("material:")) { kind = Kind::eMaterial; what.remove_prefix(9); }
            if (const auto index = Number(what))
                if (const Part* part = entry->meta.PartAt(kind, *index)) reference.part = part->id;
        }
        return reference;
    }

    namespace {
        /** Every string value in @p value, and in what is in it — and what each line of one ends with after a space, as a material's link to its asset is written. */
        void Strings(const kecs::json::Value& value, std::vector<std::string_view>& out)
        {
            if (value.kind == kecs::json::Value::Kind::eString) {
                const std::string_view text = value.string;
                out.push_back(text);
                for (std::size_t at = 0; at < text.size();) {
                    const std::size_t end = std::min(text.find('\n', at), text.size());
                    const std::string_view line = text.substr(at, end - at);
                    if (const std::size_t space = line.find(' '); space != std::string_view::npos) out.push_back(line.substr(space + 1));
                    at = end + 1;
                }
            }
            for (const auto& item : value.items) Strings(item, out);
            for (const auto& [name, member] : value.members) Strings(member, out);
        }

        bool NamesOthers(const Kind kind)
        {
            return kind == Kind::eScene || kind == Kind::ePrefab || kind == Kind::eFunction || kind == Kind::eMaterial || kind == Kind::eGenerated || kind == Kind::eModel;
        }
    }

    void Database::Reindex(const Id id)
    {
        _uses.erase(id);
        const Entry* entry = Find(id);
        if (!entry || !NamesOthers(entry->meta.kind)) return;
        // A model names nothing in its file that is the project's to say, but what was changed of its materials is.
        std::filesystem::path file = _root / entry->file;
        if (entry->meta.kind == Kind::eModel) file += ".materials.json";
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        std::ostringstream read;
        read << in.rdbuf();
        const std::string text = read.str();
        kecs::json::Value root;
        std::string error;
        if (!kecs::json::Parse(text, root, error)) return;
        std::vector<std::string_view> strings;
        Strings(root, strings);
        std::vector<Reference> uses;
        for (const auto string : strings)
            if (const auto reference = Lookup(string); reference && reference->asset != id && std::ranges::find(uses, *reference) == uses.end()) uses.push_back(*reference);
        // A material's own meta's overrides name pictures too.
        if (!entry->meta.overrides.empty() && kecs::json::Parse(entry->meta.overrides, root, error)) {
            strings.clear();
            Strings(root, strings);
            for (const auto string : strings)
                if (const auto reference = Lookup(string); reference && reference->asset != id && std::ranges::find(uses, *reference) == uses.end()) uses.push_back(*reference);
        }
        if (!uses.empty()) _uses[id] = std::move(uses);
    }

    void Database::IndexReferences()
    {
        _uses.clear();
        for (const auto& [id, entry] : _entries) Reindex(id);
    }

    std::vector<Id> Database::Users(const Id id) const
    {
        std::vector<Id> users;
        for (const auto& [user, uses] : _uses)
            if (std::ranges::any_of(uses, [&](const Reference& use) { return use.asset == id; })) users.push_back(user);
        std::ranges::sort(users, [&](const Id a, const Id b) { return Find(a)->file < Find(b)->file; });
        return users;
    }

    std::vector<Reference> Database::Uses(const Id id) const
    {
        const auto at = _uses.find(id);
        return at == _uses.end() ? std::vector<Reference> {} : at->second;
    }

    std::expected<void, std::string> Database::Move(const Id id, const std::filesystem::path& to)
    {
        Entry* entry = Mutable(id);
        if (!entry) return std::unexpected(std::format("{} is no asset of the project's", id.Text()));
        const std::string target = Relative(to.is_absolute() ? to : _root / to);
        if (target.empty()) return std::unexpected(std::format("{} is not in the project", to.string()));
        if (KindOfFile(target) != KindOfFile(entry->file)) return std::unexpected(std::format("{} would be another kind of file", std::filesystem::path(target).filename().string()));
        const std::filesystem::path from = _root / entry->file, into = _root / target;
        std::error_code failed;
        if (std::filesystem::exists(into, failed)) return std::unexpected(std::format("{} is there already", target));
        std::filesystem::create_directories(into.parent_path(), failed);
        // What goes with it: its meta, and for a model what it names beside it — kept where it is from it.
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moves { { from, into } };
        if (KindOfFile(from) != Kind::eGenerated) moves.emplace_back(MetaOf(from), MetaOf(into));     // a generator's is its own
        if (entry->meta.kind == Kind::eModel) {
            if (std::filesystem::path changes = std::filesystem::path(from).concat(".materials.json"); std::filesystem::exists(changes, failed))
                moves.emplace_back(changes, std::filesystem::path(into).concat(".materials.json"));
            if (from.parent_path() != into.parent_path())
                for (const auto& beside : NamedBeside(from)) {
                    const std::filesystem::path was = from.parent_path() / beside;
                    if (!std::filesystem::exists(was, failed)) continue;
                    moves.emplace_back(was, into.parent_path() / beside);
                    if (std::filesystem::exists(MetaOf(was), failed)) moves.emplace_back(MetaOf(was), MetaOf(into.parent_path() / beside));
                }
        }
        for (std::size_t i = 0; i < moves.size(); ++i) {
            std::filesystem::create_directories(moves[i].second.parent_path(), failed);
            std::filesystem::rename(moves[i].first, moves[i].second, failed);
            if (!failed) continue;
            // Put back what was moved: all of it goes, or none.
            const std::string why = failed.message();
            for (std::size_t j = i; j-- > 0;) std::filesystem::rename(moves[j].second, moves[j].first, failed);
            return std::unexpected(std::format("{} could not be moved: {}", Relative(moves[i].first), why));
        }
        // Each moved file that is an asset is known by where it is now.
        std::vector<std::pair<std::string, std::string>> renamed;
        for (const auto& [was, now] : moves)
            if (!IsMeta(was)) renamed.emplace_back((was.lexically_normal()).lexically_relative(_root.lexically_normal()).generic_string(), Relative(now));
        for (const auto& [was, now] : renamed) {
            const auto at = _byFile.find(was);
            if (at == _byFile.end()) continue;
            const Id moved = at->second;
            _byFile.erase(at);
            _byFile[now] = moved;
            _entries.at(moved).file = now;
        }
        return {};
    }

    std::expected<std::vector<std::filesystem::path>, std::string> Database::KeepUsersByIds(const Id id)
    {
        std::vector<std::filesystem::path> written;
        for (const Id user : Users(id)) {
            const Entry* entry = Find(user);
            std::filesystem::path file = _root / entry->file;
            if (entry->meta.kind == Kind::eModel) file += ".materials.json";
            std::ifstream in(file, std::ios::binary);
            std::ostringstream read;
            read << in.rdbuf();
            in.close();
            const std::string was = read.str();
            const std::string now = ToIds(was, *this);
            if (now == was) continue;
            std::filesystem::path writing = file;
            writing += ".writing";
            {
                std::ofstream out(writing, std::ios::binary | std::ios::trunc);
                out << now;
                if (!out.flush()) return std::unexpected(std::format("{} could not be written", Relative(file)));
            }
            std::error_code failed;
            std::filesystem::rename(writing, file, failed);
            if (failed) return std::unexpected(std::format("{} could not be written: {}", Relative(file), failed.message()));
            written.push_back(file);
        }
        return written;
    }
}
