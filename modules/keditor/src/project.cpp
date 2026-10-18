#include <keditor/project.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

#include <shader.h>

#include <kecs/json.h>

namespace keditor
{
    std::filesystem::path Project::ScenePath(const std::string_view scene) const
    {
        return directory / "scenes" / (std::string(scene) + ".scene.json");
    }

    void Project::Use() const { kor::Shader::AddSearchPath(ShaderDirectory(), true); }

    std::string Project::LanguageName(const char language) { return language == 'k' ? "kotlin" : language == 'c' ? "csharp" : std::string(); }

    char Project::Language() const
    {
        if (scripts == "kotlin") return 'k';
        if (scripts == "csharp") return 'c';
        std::error_code ignored;
        bool kotlin = false, csharp = false;
        for (const auto& entry : std::filesystem::directory_iterator(directory / "scripts", ignored)) {
            kotlin = kotlin || entry.path().extension() == ".kt";
            csharp = csharp || entry.path().extension() == ".cs";
        }
        return kotlin ? 'k' : csharp ? 'c' : 0;
    }

    std::vector<std::filesystem::path> Project::ScriptsInAnotherLanguage() const
    {
        std::vector<std::filesystem::path> others;
        const char language = Language();
        if (language == 0) return others;
        const char* other = language == 'k' ? ".cs" : ".kt";
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(directory / "scripts", ignored))
            if (entry.path().extension() == other) others.push_back(entry.path());
        std::ranges::sort(others);
        return others;
    }

    bool Project::Save(std::string& error) const
    {
        std::error_code ec;
        std::filesystem::create_directories(directory / "scenes", ec);
        std::filesystem::create_directories(ShaderDirectory(), ec);
        std::string list;
        for (const auto& scene : scenes) list += std::format("{}{}", list.empty() ? "" : ", ", kecs::json::Quote(scene));
        std::ofstream out(directory / "project.json", std::ios::trunc);
        if (!out) { error = std::format("'{}' cannot be written", (directory / "project.json").string()); return false; }
        out << std::format("{{\n  \"koral-engine-project\": 1,\n  \"name\": {},\n  \"scenes\": [{}],\n  \"start\": {}{}\n}}\n",
                           kecs::json::Quote(name), list, kecs::json::Quote(start),
                           scripts.empty() ? std::string() : std::format(",\n  \"scripts\": {}", kecs::json::Quote(scripts)));
        return true;
    }

    bool Project::Create(const std::filesystem::path& directory, std::string name, Project& out, std::string& error)
    {
        if (std::filesystem::exists(directory / "project.json")) { error = std::format("there is a project in '{}' already", directory.string()); return false; }
        out = Project { directory, std::move(name), { "main" }, "main" };
        if (!out.Save(error)) return false;
        std::ofstream scene(out.ScenePath("main"), std::ios::trunc);
        scene << "{ \"koral-scene\": 1, \"tags\": [], \"graph\": { \"kgraph\": 1, \"nodes\": [] }, \"world\": { \"kecs\": 1, \"types\": [], \"entities\": [] } }\n";
        return true;
    }

    bool Project::Open(const std::filesystem::path& given, Project& out, std::string& error)
    {
        const auto directory = given.filename() == "project.json" ? given.parent_path() : given;
        std::ifstream in(directory / "project.json");
        if (!in) { error = std::format("there is no project in '{}': no project.json", directory.string()); return false; }
        std::stringstream text;
        text << in.rdbuf();
        const std::string json = text.str();
        kecs::json::Value file;
        if (!kecs::json::Parse(json, file, error)) return false;
        const auto* version = file.Find("koral-engine-project");
        if (!version || version->number != 1.0) { error = "project.json is not a Koral Engine project's"; return false; }
        out = Project { directory, {}, {}, {} };
        if (const auto* name = file.Find("name")) out.name = name->string;
        if (const auto* scenes = file.Find("scenes"))
            for (const auto& scene : scenes->items) out.scenes.push_back(scene.string);
        if (const auto* start = file.Find("start")) out.start = start->string;
        if (const auto* scripts = file.Find("scripts")) out.scripts = scripts->string;
        if (out.start.empty() && !out.scenes.empty()) out.start = out.scenes.front();
        return true;
    }
}
