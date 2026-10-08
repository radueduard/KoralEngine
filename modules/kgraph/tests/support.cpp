#include "support.h"

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
}
