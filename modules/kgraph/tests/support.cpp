#include "support.h"

#include <kecs/json.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace {
    /**
     * A temporary folder of this program's own, before anything asks for one: what is generated where there is no
     * project — material modules, the editor's own shaders, the folders tests make — is written there. Run side by side
     * (`ctest -j`), two programs would otherwise write the same files at once, and each read what the other half wrote.
     * Gone when the program ends.
     */
    const std::filesystem::path s_temporary = [] {
        std::error_code ignored;
#ifdef _WIN32
        const int id = _getpid();
#else
        const int id = static_cast<int>(getpid());
#endif
        const std::filesystem::path folder = std::filesystem::temp_directory_path(ignored) / "koral-tests" / std::to_string(id);
        std::filesystem::remove_all(folder, ignored);       // one a program of this id left
        std::filesystem::create_directories(folder, ignored);
#ifdef _WIN32
        _putenv_s("TMP", folder.string().c_str());
        _putenv_s("TEMP", folder.string().c_str());
#else
        setenv("TMPDIR", folder.c_str(), 1);
#endif
        std::atexit([] { std::error_code gone; std::filesystem::remove_all(s_temporary, gone); });
        return folder;
    }();

    std::unique_ptr<kor::App> s_app;
    std::string s_reason = "no device";

    class Environment final : public ::testing::Environment {
    public:
        void SetUp() override {
            try {
                s_app = std::make_unique<kor::App>(kor::AppSettings { .platform = kor::WindowPlatform::eNone });
                kor::Shader::AddSearchPath(KGRAPH_TEST_SHADERS);
                kor::Shader::AddSearchPath(KGRAPH_PACKAGE_SHADERS);     // the engine's modules: `import entity;`
            } catch (const std::exception& e) {
                s_reason = e.what();
                s_app.reset();
            }
        }
        void TearDown() override { s_app.reset(); }
    };
    const auto* const s_environment = ::testing::AddGlobalTestEnvironment(new Environment);
}

namespace support
{
    std::shared_ptr<const kgraph::Graph> EngineFunction(const std::string& name)
    {
        const std::filesystem::path file = kscript::PackageDirectory() / "functions" / (name + ".kfunc");
        std::ifstream in(file, std::ios::binary);
        std::stringstream read;
        read << in.rdbuf();
        kecs::json::Value parsed;
        std::string error;
        auto graph = std::make_shared<kgraph::Graph>();
        if (!kecs::json::Parse(read.str(), parsed, error) || !parsed.Find("graph") || !kgraph::Graph::FromJson(parsed.Find("graph")->raw, *graph, error))
            ADD_FAILURE() << name << ": " << error;
        return graph;
    }

    kor::App* App() { return s_app.get(); }
    const std::string& Reason() { return s_reason; }
    void Frame()
    {
        s_app->Frame();
        kor::Context::Scheduler().WaitIdle();
    }

    void Give(kecs::World& world, const kecs::Entity entity, const std::string_view component, const std::initializer_list<std::pair<std::string_view, std::vector<float>>> fields)
    {
        const kecs::ComponentId id = world.Find(component);
        if (id == kecs::NoComponent) throw std::invalid_argument(std::format("there is no component '{}'", component));
        world.Add(entity, id);
        auto* bytes = static_cast<std::byte*>(world.Mutable(entity, id));
        for (const auto& [name, numbers] : fields) {
            const kecs::FieldDescription* field = world.Describe(id).FindField(name);
            if (!field || numbers.size() * 4 != field->size) throw std::invalid_argument(std::format("'{}' has no field '{}' of {} numbers", component, name, numbers.size()));
            std::memcpy(bytes + field->offset, numbers.data(), field->size);
        }
    }

    std::vector<float> FieldOf(const kecs::World& world, const kecs::Entity entity, const std::string_view component, const std::string_view field)
    {
        const kecs::ComponentId id = world.Find(component);
        const auto* bytes = id == kecs::NoComponent ? nullptr : static_cast<const std::byte*>(world.Get(entity, id));
        const kecs::FieldDescription* found = bytes ? world.Describe(id).FindField(field) : nullptr;
        if (!found) return {};
        std::vector<float> numbers(found->size / 4);
        std::memcpy(numbers.data(), bytes + found->offset, numbers.size() * 4);
        return numbers;
    }
}
