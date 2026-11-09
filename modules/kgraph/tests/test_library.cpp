// The engine's base library of shaders (packages/koral/shaders), checked on the GPU: library.slang runs each of its
// checks in one thread and writes what it got, what it should be and how near is near enough; each is named here by
// its line of the shader. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <kgraph/scene.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;

    class LibraryTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = false;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "library", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }

        /** What each check of a shader says, as it is written: its lines that call Check, in order. */
        static std::vector<std::string> ChecksOf(const std::string& module) {
            std::ifstream in(std::string(KGRAPH_TEST_SHADERS) + "/" + module + ".slang");
            std::vector<std::string> checks;
            for (std::string line; std::getline(in, line);) {
                const auto at = line.find("Check(");
                if (at == std::string::npos || line.find("void Check(") != std::string::npos) continue;
                checks.push_back(line.substr(at));
            }
            return checks;
        }

        /** Runs @p module's computeMain once a frame, and gives what its `results` held: four floats a check (a Buffer is sized in bytes). */
        std::vector<float> Run(const std::string& module, const std::size_t checks) {
            auto& runtime = *scene->runtime;
            const std::string json = std::format(R"json({{
              "kgraph": 1,
              "nodes": [
                {{ "id": "results", "type": "Buffer", "inputs": {{ "instanceCount": {}, "usage": ["eStorage", "eTransferSrc", "eTransferDst"] }} }},
                {{ "id": "checks shader", "type": "Shader", "inputs": {{ "entryPoint.module": "{}", "entryPoint.entry": "computeMain" }} }},
                {{ "id": "checks", "type": "ComputePipeline", "inputs": {{
                    "computeShader": {{ "$link": ["checks shader", "out"] }}, "param.results": {{ "$link": ["results", "out"] }} }} }},
                {{ "id": "run", "type": "Dispatch", "inputs": {{ "pipeline": {{ "$link": ["checks", "out"] }}, "groups.x": 1, "groups.y": 1, "groups.z": 1 }} }}
              ]
            }})json", checks * 16, module);
            kgraph::Graph graph;
            std::string error;
            if (!kgraph::Graph::FromJson(json, graph, error)) { ADD_FAILURE() << error; return {}; }
            runtime.Set(std::move(graph));
            for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
            for (int i = 0; i < 2; ++i) Frame();
            runtime.Watch("results");
            std::optional<kgraph::Peek> peek;
            for (int i = 0; i < 8 && !(peek && peek->bytes.size() >= checks * 16); ++i) { Frame(); peek = runtime.Watched("results"); }
            for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
            if (!peek || peek->bytes.size() < checks * 16) { ADD_FAILURE() << "nothing to look at"; return {}; }
            std::vector<float> numbers(checks * 4);
            std::memcpy(numbers.data(), peek->bytes.data(), numbers.size() * sizeof(float));
            return numbers;
        }

        GraphScene* scene = nullptr;
    };
}

TEST_F(LibraryTest, TheBaseLibraryWorksOutWhatItSays) {
    const std::vector<std::string> checks = ChecksOf("library");
    ASSERT_GT(checks.size(), 50u);
    const std::vector<float> got = Run("library", checks.size());
    ASSERT_EQ(got.size(), checks.size() * 4);
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const float value = got[i * 4], expected = got[i * 4 + 1], tolerance = got[i * 4 + 2], ran = got[i * 4 + 3];
        ASSERT_EQ(ran, 1.f) << "the shader stopped before " << checks[i];
        EXPECT_LE(std::abs(value - expected), tolerance) << checks[i] << ": got " << value;
    }
}

// The engine's PointLightShadows and SpotLightShadows functions draw a cube's shadow from a spot and a point light over
// it, and koral_shadows reads it: the ground under the cube is in its shadow, two units off it is not.
TEST_F(LibraryTest, TheShadowFunctionsDrawWhatTheLibraryReads) {
    support::App()->Close(*scene);
    Frame();
    GraphScene::withWorld = true;
    scene = support::App()->OpenOffscreen<GraphScene>({ .title = "shadows", .extent = { support::Size, support::Size } });
    ASSERT_NE(scene, nullptr);
    auto& runtime = *scene->runtime;
    const std::vector<std::string> checks = ChecksOf("shadowCheck");
    using kgraph::Value;
    kgraph::Graph graph;
    for (const char* name : { "SpotLightShadows", "PointLightShadows" }) graph.functions[name] = support::EngineFunction(name);
    graph.Add("all", "Entities");
    graph.Add("shape", "Primitive");
    graph.Add("spot shadows", "Function").Set("function", "SpotLightShadows").Wire("entities", "all").Wire("layout", "shape", "layout").Set("size", 256);
    graph.Add("point shadows", "Function").Set("function", "PointLightShadows").Wire("entities", "all").Wire("layout", "shape", "layout").Set("size", 256);
    graph.Add("results", "Buffer").Set("instanceCount", static_cast<double>(checks.size() * 16)).Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.Add("check", "ComputePipeline").Set("compute.module", "shadowCheck").Set("compute.entry", "computeMain")
        .Wire("param.spotMaps", "spot shadows", "maps").Wire("param.spotMatrices", "spot shadows", "matrices")
        .Wire("param.pointMaps", "point shadows", "maps").Wire("param.pointFaces", "point shadows", "matrices")
        .Wire("param.compare", "spot shadows", "compare").Wire("param.results", "results");
    graph.Add("run", "Dispatch").Wire("pipeline", "check").Set("groups.x", 1).Set("groups.y", 1).Set("groups.z", 1);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;

    kecs::World& world = scene->world;
    const kecs::Entity cube = world.Create();
    world.SetPosition(cube, { 0, 1, 0 });
    world.Add<kgraph::Mesh>(cube, { runtime.ResourceOf("$cube") });
    const kecs::Entity spot = world.Create(), lamp = world.Create();
    support::Give(world, spot, "SpotLight", { { "range", { 5 } }, { "innerAngle", { 20 } }, { "outerAngle", { 30 } } });
    support::Give(world, lamp, "PointLight", { { "range", { 5 } } });
    for (const kecs::Entity light : { spot, lamp }) world.SetPosition(light, { 0, 4, 0 });
    world.SetRotation(spot, { -90, 0, 0 });     // its forward, -z, turned to point down
    for (int i = 0; i < 4; ++i) Frame();
    runtime.Watch("results");
    std::optional<kgraph::Peek> peek;
    for (int i = 0; i < 8 && !(peek && peek->bytes.size() >= checks.size() * 16); ++i) { Frame(); peek = runtime.Watched("results"); }
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(peek && peek->bytes.size() >= checks.size() * 16) << "nothing to look at";
    std::vector<float> got(checks.size() * 4);
    std::memcpy(got.data(), peek->bytes.data(), got.size() * sizeof(float));
    for (std::size_t i = 0; i < checks.size(); ++i) {
        ASSERT_EQ(got[i * 4 + 3], 1.f) << "the shader stopped before " << checks[i];
        EXPECT_LE(std::abs(got[i * 4] - got[i * 4 + 1]), got[i * 4 + 2]) << checks[i] << ": got " << got[i * 4];
    }
}

namespace {
    /** A half float's value: what a 16-bit float channel of a picture holds. */
    float HalfOf(const std::uint16_t h)
    {
        const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
        std::uint32_t bits;
        if (exponent == 0) {
            if (mantissa == 0) bits = sign;
            else { const float f = static_cast<float>(mantissa) / 1024.f / 16384.f; return sign ? -f : f; }
        } else if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
        else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
}

// The engine's GaussianBlur spreads one white pixel the same each way and loses none of it; its MipChain's last level,
// one pixel, is the average of the whole picture.
TEST_F(LibraryTest, TheBlurAndTheMipChainKeepWhatTheyAreGiven) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    for (const char* name : { "GaussianBlur", "MipChain" }) graph.functions[name] = support::EngineFunction(name);
    graph.Add("picture", "Image").Set("format", "eRGBA16_SFLOAT").Set("extent", Value::List { 32, 32, 1 }).Set("usage", Value::List { "eStorage", "eSampled", "eTransferSrc" });
    graph.Add("dot", "ComputePipeline").Set("compute.module", "dot").Set("compute.entry", "computeMain").Wire("param.target", "picture");
    graph.Add("draw dot", "Dispatch").Wire("pipeline", "dot").Set("cover", "target");
    graph.Add("blur", "Function").Set("function", "GaussianBlur").Wire("picture", "picture").Set("radius", 4).Set("sigma", 1.5);
    graph.Add("mips", "Function").Set("function", "MipChain").Wire("picture", "picture").Set("levels", 6);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const auto texels = [&](const std::string& id, const std::uint32_t mip) {
        runtime.Watch(id, mip, 0);
        std::optional<kgraph::Peek> peek;
        for (int i = 0; i < 10 && !(peek && peek->mip == mip && !peek->bytes.empty()); ++i) { Frame(); peek = runtime.Watched(id); }
        std::vector<float> red;
        if (!peek) { ADD_FAILURE() << id << ": nothing to look at " << runtime.WatchProblem(id); return red; }
        for (std::size_t at = 0; at + 8 <= peek->bytes.size(); at += 8) {
            std::uint16_t h;
            std::memcpy(&h, peek->bytes.data() + at, 2);
            red.push_back(HalfOf(h));
        }
        return red;
    };
    const std::vector<float> blurred = texels("blur/blurred", 0);
    ASSERT_EQ(blurred.size(), 32u * 32u);
    const auto at = [&](const int x, const int y) { return blurred[static_cast<std::size_t>(y * 32 + x)]; };
    float sum = 0.f;
    for (const float v : blurred) sum += v;
    EXPECT_NEAR(sum, 1.f, 0.01f) << "none of it lost";
    EXPECT_LT(at(16, 16), 0.2f) << "spread out";
    EXPECT_GT(at(16, 16), at(17, 16));
    EXPECT_NEAR(at(15, 16), at(17, 16), 1e-3f) << "the same each way";
    EXPECT_NEAR(at(16, 15), at(16, 17), 1e-3f);
    EXPECT_NEAR(at(17, 16), at(16, 17), 1e-3f);
    const std::vector<float> last = texels("mips/chain", 5);
    ASSERT_EQ(last.size(), 1u) << "32 halved five times: one pixel";
    EXPECT_NEAR(last[0], 1.f / 1024.f, 1e-4f) << "the average of the whole picture";
}
