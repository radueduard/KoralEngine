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
