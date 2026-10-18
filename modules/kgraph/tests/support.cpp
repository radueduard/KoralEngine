#include "support.h"

#include <cstring>
#include <format>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {
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
