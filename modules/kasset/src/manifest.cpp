#include <kasset/manifest.h>

#include <kasset/beside.h>
#include <kasset/database.h>

#include <kecs/json.h>

#include <algorithm>
#include <deque>
#include <format>
#include <set>

namespace kasset
{
    Manifest Reach(Database& assets, const std::vector<Id>& scenes)
    {
        (void)assets.Scan();
        assets.IndexReferences();
        Manifest manifest;
        manifest.scenes = scenes;
        std::set<Id> reached;
        std::set<std::filesystem::path> files;
        const auto file = [&](const std::filesystem::path& path) {
            std::error_code failed;
            if (std::filesystem::is_regular_file(assets.Root() / path, failed) && files.insert(path.lexically_normal()).second) manifest.files.push_back(path.lexically_normal());
        };
        std::deque<Id> next(scenes.begin(), scenes.end());
        while (!next.empty()) {
            const Id id = next.front();
            next.pop_front();
            const Entry* entry = assets.Find(id);
            if (!entry || !reached.insert(id).second) continue;     // the engine's own are no file of the project's
            manifest.assets.push_back(id);
            file(entry->file);
            if (MetaPathOf(entry->file) != entry->file) file(MetaPathOf(entry->file));
            if (entry->meta.kind == Kind::eModel) {
                file(std::filesystem::path(entry->file).concat(".materials.json"));
                for (const auto& beside : NamedBeside(assets.Root() / entry->file)) {
                    const std::filesystem::path path = entry->file.parent_path() / beside;
                    file(path);
                    file(MetaPathOf(path));
                }
            }
            for (const Reference& use : assets.Uses(id)) next.push_back(use.asset);
        }
        // What is found by its name, not its file: every game of the project has all of it.
        file("project.json");
        for (const char* folder : { "shaders", "scripts", "functions", "objects" }) {
            std::error_code failed;
            std::vector<std::filesystem::path> found;
            for (auto it = std::filesystem::recursive_directory_iterator(assets.Root() / folder, failed); !failed && it != std::filesystem::recursive_directory_iterator(); it.increment(failed))
                if (it->is_regular_file(failed)) found.push_back(it->path().lexically_relative(assets.Root()));
            std::ranges::sort(found);
            for (const auto& path : found) file(path);
        }
        return manifest;
    }

    std::string Write(const Manifest& manifest)
    {
        using kecs::json::Quote;
        const auto list = [](const auto& items, const auto& text) {
            std::string out = "[";
            for (std::size_t i = 0; i < items.size(); ++i) out += std::format("{}\n    {}", i ? "," : "", Quote(text(items[i])));
            return out + (items.empty() ? "]" : "\n  ]");
        };
        const auto id = [](const Id& i) { return i.Text(); };
        return std::format("{{\n  \"koral-manifest\": 1,\n  \"scenes\": {},\n  \"assets\": {},\n  \"files\": {}\n}}\n", list(manifest.scenes, id), list(manifest.assets, id),
                           list(manifest.files, [](const std::filesystem::path& p) { return p.generic_string(); }));
    }

    std::expected<Manifest, std::string> ReadManifest(const std::string_view json)
    {
        using kecs::json::Value;
        Value root;
        std::string error;
        if (!kecs::json::Parse(json, root, error)) return std::unexpected(error);
        const Value* version = root.Find("koral-manifest");
        if (!version || version->number != 1.0) return std::unexpected("it is no manifest: 'koral-manifest' 1");
        Manifest manifest;
        const auto ids = [&](const char* key, std::vector<Id>& out) -> bool {
            if (const Value* list = root.Find(key))
                for (const auto& item : list->items) {
                    const auto id = Id::Parse(item.string);
                    if (!id) return false;
                    out.push_back(*id);
                }
            return true;
        };
        if (!ids("scenes", manifest.scenes) || !ids("assets", manifest.assets)) return std::unexpected("an ID in it is not one");
        if (const Value* list = root.Find("files"))
            for (const auto& item : list->items) manifest.files.emplace_back(item.string);
        return manifest;
    }
}
