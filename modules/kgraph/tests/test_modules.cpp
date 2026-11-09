// The engine's own modules (packages/modules), each function of each run on a picture — a white dot on a gradient, with
// a depth and normals — as a project that imports the module would: it builds, runs with nothing said wrong, and what
// it makes is there and holds no number that is not one. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <shader.h>

#include <kgraph/scene.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using kgraph::Value;

    /** A half float's value. */
    float HalfOf(const std::uint16_t h)
    {
        const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
        if (exponent == 0) { const float f = static_cast<float>(mantissa) / 1024.f / 16384.f; return sign ? -f : f; }
        std::uint32_t bits = exponent == 31 ? sign | 0x7F800000u | (mantissa << 13) : sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    /** A function of a module, called on the test's inputs: what is wired, set and watched of it. */
    struct Case {
        std::string module, function;
        std::function<void(kgraph::Node&)> call;
        std::string watched;            ///< The node of the call whose picture is looked at: `call/<node>`.
    };

    class ModuleTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            // What a project that imports them has: their shaders on the path.
            static const bool searched = [] {
                std::error_code ignored;
                for (const auto& entry : std::filesystem::directory_iterator(support::OfficialModules(), ignored))
                    if (std::filesystem::exists(entry.path() / "shaders")) kor::Shader::AddSearchPath(entry.path() / "shaders");
                return true;
            }();
            (void)searched;
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "modules", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }

        void Run(const Case& c) {
            SCOPED_TRACE(c.module + "/" + c.function);
            auto& runtime = *scene->runtime;
            kgraph::Graph graph;
            graph.functions[c.function] = support::ModuleFunction(c.module, c.function);
            const auto image = [&](const char* id, const char* format) {
                graph.Add(id, "Image").Set("format", format).Set("extent", Value::List { 64, 64, 1 }).Set("usage", Value::List { "eStorage", "eSampled", "eTransferSrc" });
            };
            image("picture", "eRGBA16_SFLOAT");
            image("depth", "eR32_SFLOAT");
            image("normals", "eRGBA16_SFLOAT");
            graph.Add("fill", "ComputePipeline").Set("compute.module", "inputs").Set("compute.entry", "computeMain")
                .Wire("param.picture", "picture").Wire("param.depth", "depth").Wire("param.normals", "normals");
            graph.Add("run fill", "Dispatch").Wire("pipeline", "fill").Set("cover", "picture");
            kgraph::Node& call = graph.Add("call", "Function").Set("function", c.function);
            c.call(call);
            runtime.Set(graph);
            for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
            for (int i = 0; i < 4; ++i) Frame();
            for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
            if (c.watched.empty()) return;
            runtime.Watch(c.watched);
            std::optional<kgraph::Peek> peek;
            for (int i = 0; i < 8 && !(peek && !peek->bytes.empty()); ++i) { Frame(); peek = runtime.Watched(c.watched); }
            ASSERT_TRUE(peek && !peek->bytes.empty()) << c.watched << ": nothing to look at. " << runtime.WatchProblem(c.watched);
            if (peek->format == "eRGBA16_SFLOAT") {
                float largest = 0.f;
                for (std::size_t at = 0; at + 2 <= peek->bytes.size(); at += 2) {
                    std::uint16_t h;
                    std::memcpy(&h, peek->bytes.data() + at, 2);
                    const float v = HalfOf(h);
                    ASSERT_TRUE(std::isfinite(v)) << c.watched << ": a number that is not one, at byte " << at;
                    largest = std::max(largest, std::abs(v));
                }
                EXPECT_GT(largest, 0.f) << c.watched << ": nothing was made";
            }
        }

        GraphScene* scene = nullptr;
    };

    const auto picture = [](kgraph::Node& call) { call.Wire("picture", "picture"); };
}

TEST_F(ModuleTest, ColorGradingGradesAPicture) {
    Run({ "color_grading", "Grade", [](kgraph::Node& call) { picture(call); call.Set("curve", 2).Set("temperature", 0.3).Set("saturation", 1.2); }, "call/shown" });
    Run({ "color_grading", "AutoExposure", picture, "call/adapted" });
}

TEST_F(ModuleTest, FiltersFilterAPicture) {
    Run({ "filters", "KawaseBlur", picture, "call/blurred" });
    Run({ "filters", "DualBlur", picture, "call/up chain" });
    Run({ "filters", "Bilateral", picture, "call/filtered" });
    Run({ "filters", "Denoise", [](kgraph::Node& call) { call.Wire("picture", "picture").Wire("depth", "depth").Wire("normals", "normals"); }, "call/denoised" });
    Run({ "filters", "Sharpen", picture, "call/sharpened" });
}

TEST_F(ModuleTest, AntialiasingSmoothsAndUpscales) {
    Run({ "antialiasing", "FXAA", picture, "call/smoothed" });
    Run({ "antialiasing", "Upscale", [](kgraph::Node& call) { picture(call); call.Set("extent", Value::List { 96, 80, 1 }); }, "call/upscaled" });
}

TEST_F(ModuleTest, CameraEffectsDoWhatALensDoes) {
    const auto withDepth = [](kgraph::Node& call) { call.Wire("picture", "picture").Wire("depth", "depth"); };
    Run({ "camera_effects", "DepthOfField", [&](kgraph::Node& call) { withDepth(call); call.Set("focus", 2).Set("near", 0.5).Set("far", 10); }, "call/focused" });
    Run({ "camera_effects", "MotionBlur", [&](kgraph::Node& call) {
        withDepth(call);
        call.Set("currentToPrevious", Value::List { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.02, 0, 0, 1 });
    }, "call/blurred" });
    Run({ "camera_effects", "LensEffects", picture, "call/lensed" });
    Run({ "camera_effects", "LensFlare", picture, "call/flare" });
}
