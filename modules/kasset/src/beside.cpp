#include <kasset/beside.h>

#include <kecs/json.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace kasset
{
    std::vector<std::filesystem::path> NamedBeside(const std::filesystem::path& model)
    {
        std::vector<std::filesystem::path> named;
        const auto keep = [&](std::string uri) {
            if (uri.empty() || uri.starts_with("data:") || uri.find("://") != std::string::npos) return;
            // Spaces and the like written as %20.
            std::string plain;
            for (std::size_t i = 0; i < uri.size(); ++i)
                if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) && std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
                    plain += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                } else plain += uri[i];
            const std::filesystem::path path = std::filesystem::path(plain).lexically_normal();
            if (path.is_absolute() || (!path.empty() && *path.begin() == "..")) return;    // only what is beside it, or under it
            if (!std::ranges::contains(named, path)) named.push_back(path);
        };
        std::ifstream in(model, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string ending = model.extension().string();
        std::ranges::transform(ending, ending.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ending == ".gltf") {
            kecs::json::Value root;
            std::string error;
            if (!kecs::json::Parse(text, root, error)) return named;
            for (const char* list : { "buffers", "images" })
                if (const auto* items = root.Find(list))
                    for (const auto& item : items->items)
                        if (const auto* uri = item.Find("uri"); uri && uri->kind == kecs::json::Value::Kind::eString) keep(uri->string);
        } else if (ending == ".obj") {
            // Its materials, and the pictures they name.
            std::istringstream lines(text);
            for (std::string line; std::getline(lines, line);) {
                if (!line.starts_with("mtllib ")) continue;
                std::string file = line.substr(7);
                while (!file.empty() && std::isspace(static_cast<unsigned char>(file.back()))) file.pop_back();
                keep(file);
                std::ifstream material(model.parent_path() / file);
                for (std::string said; std::getline(material, said);) {
                    std::istringstream words(said);
                    std::string key, last;
                    words >> key;
                    if (!key.starts_with("map_") && key != "bump" && key != "disp" && key != "norm") continue;
                    for (std::string word; words >> word;) last = word;     // options first, the file last
                    keep(last);
                }
            }
        }
        return named;
    }
}
