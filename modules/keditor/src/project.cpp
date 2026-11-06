#include <map>
#include <kscript/declarations.h>
#include <keditor/project.h>

#include <algorithm>
#include <format>
#include <functional>
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

    void Project::Use() const
    {
        // Its modules' first, each put in front of the last: its own then go in front of them all.
        const auto imported = ImportedDirectories();
        for (auto it = imported.rbegin(); it != imported.rend(); ++it) kor::Shader::AddSearchPath(*it / "shaders", true);
        kor::Shader::AddSearchPath(ShaderDirectory(), true);
    }

    std::string Project::LanguageName(const char language) { return language == 'k' ? "kotlin" : language == 'c' ? "csharp" : std::string(); }

    namespace {
        /**
         * Whether a script's file declares Data — material kinds among it — and nothing else: read from the file, with nothing to run,
         * so that it is no reason for a project to need its scripts' runner. Kept by when the file was written, as this is
         * asked often.
         */
        bool OnlyData(const std::filesystem::path& file)
        {
            struct Kept { std::filesystem::file_time_type written; bool only = false; };
            static std::map<std::filesystem::path, Kept> kept;
            std::error_code ignored;
            const auto written = std::filesystem::last_write_time(file, ignored);
            if (const auto it = kept.find(file); it != kept.end() && it->second.written == written) return it->second.only;
            const auto declared = kscript::DeclareFiles(file);
            const bool only = !declared.types.empty() && std::ranges::all_of(declared.types, [](const kscript::ScriptType& type) { return type.data; });
            kept[file] = { written, only };
            return only;
        }
    }

    char Project::Language() const
    {
        if (scripts == "kotlin") return 'k';
        if (scripts == "csharp") return 'c';
        std::error_code ignored;
        bool kotlin = false, csharp = false;
        for (const auto& entry : std::filesystem::directory_iterator(directory / "scripts", ignored)) {
            const auto ending = entry.path().extension();
            if ((ending != ".kt" && ending != ".cs") || OnlyData(entry.path())) continue;
            kotlin = kotlin || ending == ".kt";
            csharp = csharp || ending == ".cs";
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
        // How its pictures are compressed, when that is not as it starts: what each kind is offered as, and whether to ask.
        std::string textures;
        if (!textureFormats.empty() || !askToCompress) {
            std::string formats;
            for (const auto& [kind, format] : textureFormats) formats += std::format("{}{}: {}", formats.empty() ? "" : ", ", kecs::json::Quote(kind), kecs::json::Quote(format));
            textures = std::format(",\n  \"textures\": {{ \"askToCompress\": {}, \"formats\": {{ {} }} }}", askToCompress ? "true" : "false", formats);
        }
        // What models' materials are imported as, when that is not as it starts.
        std::string materials;
        if (!importKinds.empty()) {
            std::string files;
            for (const auto& [file, kinds] : importKinds) {
                std::string each;
                for (const auto& [model, kind] : kinds) each += std::format("{}{}: {}", each.empty() ? "" : ", ", kecs::json::Quote(model), kecs::json::Quote(kind));
                files += std::format("{}{}: {{ {} }}", files.empty() ? "" : ", ", kecs::json::Quote(file), each);
            }
            materials = std::format(",\n  \"materials\": {{ \"importAs\": {{ {} }} }}", files);
        }
        // A module says so; and what it, or a project, imports.
        std::string imports;
        for (const auto& module : modules) imports += std::format("{}{}", imports.empty() ? "" : ", ", kecs::json::Quote(module));
        out << std::format("{{\n  \"koral-engine-project\": 1,\n  \"name\": {},{}\n  \"scenes\": [{}],\n  \"start\": {}{}{}{}{}\n}}\n",
                           kecs::json::Quote(name), module ? "\n  \"kind\": \"module\"," : "", list, kecs::json::Quote(start),
                           scripts.empty() ? std::string() : std::format(",\n  \"scripts\": {}", kecs::json::Quote(scripts)),
                           imports.empty() ? std::string() : std::format(",\n  \"modules\": [{}]", imports), textures, materials);
        return true;
    }

    bool Project::Create(const std::filesystem::path& directory, std::string name, Project& out, std::string& error, const char language,
                         const bool module, const std::vector<std::filesystem::path>& modules)
    {
        if (std::filesystem::exists(directory / "project.json")) { error = std::format("there is a project in '{}' already", directory.string()); return false; }
        // A module's one scene is for trying it out: what imports it never sees it.
        const std::string scene = module ? "test" : "main";
        out = Project { directory, std::move(name), { scene }, scene };
        out.scripts = LanguageName(language);
        out.module = module;
        for (const auto& imported : modules) out.modules.push_back(out.Reference(imported));
        if (!out.Save(error)) return false;
        std::ofstream file(out.ScenePath(scene), std::ios::trunc);
        file << "{ \"koral-scene\": 1, \"tags\": [], \"graph\": { \"kgraph\": 1, \"nodes\": [] }, \"world\": { \"kecs\": 1, \"types\": [], \"entities\": [] } }\n";
        return true;
    }

    std::string Project::Reference(const std::filesystem::path& module) const
    {
        std::error_code ignored;
        const std::filesystem::path whole = std::filesystem::weakly_canonical(std::filesystem::absolute(module, ignored), ignored);
        const std::filesystem::path from = std::filesystem::weakly_canonical(std::filesystem::absolute(directory, ignored), ignored);
        // From this one's folder where both are on one drive, so that the two can be moved together; whole otherwise.
        if (whole.root_name() == from.root_name())
            if (const std::filesystem::path relative = whole.lexically_relative(from); !relative.empty()) return relative.generic_string();
        return whole.generic_string();
    }

    std::vector<Project> Project::Imported(std::vector<std::string>* problems) const
    {
        std::vector<Project> found;
        std::vector<std::filesystem::path> seen;
        const auto say = [&](std::string what) { if (problems) problems->push_back(std::move(what)); };
        std::error_code ignored;
        const auto canonical = [&](const std::filesystem::path& folder) { return std::filesystem::weakly_canonical(std::filesystem::absolute(folder, ignored), ignored); };
        seen.push_back(canonical(directory));
        const char language = Language();
        // Each one's modules after it, depth first: the nearest first, and each once however many import it.
        const std::function<void(const Project&)> visit = [&](const Project& from) {
            for (const auto& reference : from.modules) {
                const std::filesystem::path folder = std::filesystem::path(reference).is_absolute() ? std::filesystem::path(reference) : from.directory / reference;
                const std::filesystem::path whole = canonical(folder);
                if (std::ranges::contains(seen, whole)) continue;     // this one, one already found, or round in a circle
                seen.push_back(whole);
                Project module;
                std::string error;
                if (!Open(whole, module, error)) { say(std::format("{} imports '{}', which is not there: {}", from.name, reference, error)); continue; }
                if (!module.module) { say(std::format("{} imports {}, which is a project: only a module is imported", from.name, module.name)); continue; }
                if (const char theirs = module.Language(); theirs != 0 && language != 0 && theirs != language) {
                    say(std::format("{} imports {}, whose scripts are in {}: {}'s are in {}", from.name, module.name, theirs == 'k' ? "Kotlin" : "C#", name,
                                    language == 'k' ? "Kotlin" : "C#"));
                    continue;
                }
                found.push_back(module);
                visit(module);
            }
        };
        visit(*this);
        return found;
    }

    std::vector<std::filesystem::path> Project::ImportedDirectories(std::vector<std::string>* problems) const
    {
        std::vector<std::filesystem::path> folders;
        for (const auto& module : Imported(problems)) folders.push_back(module.directory);
        return folders;
    }

    std::vector<Project> Project::ModulesIn(const std::filesystem::path& folder)
    {
        std::vector<Project> modules;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
            if (!entry.is_directory(ignored)) continue;
            Project found;
            std::string error;
            if (Open(entry.path(), found, error) && found.module) modules.push_back(std::move(found));
        }
        std::ranges::sort(modules, {}, &Project::name);
        return modules;
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
        if (const auto* kind = file.Find("kind")) out.module = kind->string == "module";
        if (const auto* modules = file.Find("modules"))
            for (const auto& module : modules->items) if (module.kind == kecs::json::Value::Kind::eString) out.modules.push_back(module.string);
        if (const auto* textures = file.Find("textures")) {
            if (const auto* ask = textures->Find("askToCompress"); ask && ask->kind == kecs::json::Value::Kind::eBool) out.askToCompress = ask->boolean;
            if (const auto* formats = textures->Find("formats"))
                for (const auto& [kind, format] : formats->members) if (format.kind == kecs::json::Value::Kind::eString) out.textureFormats[kind] = format.string;
        }
        if (const auto* materials = file.Find("materials"))
            if (const auto* importAs = materials->Find("importAs"))
                for (const auto& [model, kinds] : importAs->members)
                    for (const auto& [reported, kind] : kinds.members)
                        if (kind.kind == kecs::json::Value::Kind::eString) out.importKinds[model][reported] = kind.string;
        if (out.start.empty() && !out.scenes.empty()) out.start = out.scenes.front();
        return true;
    }
}
