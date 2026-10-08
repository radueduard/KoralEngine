// A graph running on a device: objects built from nodes, passes ordered by what they use, and only what changed
// built again. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include <app.h>
#include <buffer.h>
#include <commandBuffer.h>
#include <context.h>
#include <frameGraph.h>
#include <scene.h>
#include <scheduler.h>
#include <shader.h>

#include <kgraph/runtime.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using support::Size;

    class RuntimeTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = false;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "graph", .extent = { Size, Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }
        kgraph::Graph Load(const std::string& json) {
            kgraph::Graph graph;
            std::string error;
            EXPECT_TRUE(kgraph::Graph::FromJson(json, graph, error)) << error;
            return graph;
        }
        GraphScene* scene = nullptr;
    };

    // The plan's gate, as a saved graph: an image; a compute shader that runs once over it, everything it needs
    // wired; the image cleared before, and copied to the screen after. The passes are listed out of order.
    const std::string FillGraph = R"json({
      "kgraph": 1,
      "nodes": [
        { "id": "show", "type": "Blit", "inputs": { "source": { "$link": ["target", "out"] }, "destination": { "$link": ["screen", "out"] } } },
        { "id": "run", "type": "Dispatch", "inputs": {
            "pipeline": { "$link": ["pipeline", "out"] },
            "groups.x": { "$link": ["groups x", "out"] },
            "groups.y": { "$link": ["groups y", "out"] } } },
        { "id": "clear", "type": "Clear", "inputs": { "image": { "$link": ["target", "out"] }, "color": [1, 0, 0, 1] } },
        { "id": "screen", "type": "Screen", "inputs": {} },
        { "id": "target", "type": "Image", "inputs": {
            "format": "eRGBA8_UNORM", "extent.x": 32, "extent.y": 32, "extent.z": 1,
            "usage": ["eStorage", "eTransferSrc", "eTransferDst"] } },
        { "id": "shader", "type": "Shader", "inputs": { "entryPoint.module": "fill", "entryPoint.entry": "computeMain" } },
        { "id": "pipeline", "type": "ComputePipeline", "inputs": {
            "computeShader": { "$link": ["shader", "out"] },
            "param.target": { "$link": ["target", "out"] },
            "constant.level": 0.5 } },
        { "id": "size", "type": "ImageExtent", "inputs": { "image": { "$link": ["target", "out"] } } },
        { "id": "width", "type": "Component", "inputs": { "vector": { "$link": ["size", "out"] }, "index": 0 } },
        { "id": "height", "type": "Component", "inputs": { "vector": { "$link": ["size", "out"] }, "index": 1 } },
        { "id": "per group", "type": "Number", "inputs": { "value": 8 } },
        { "id": "x groups", "type": "Divide", "inputs": { "a": { "$link": ["width", "out"] }, "b": { "$link": ["per group", "out"] } } },
        { "id": "y groups", "type": "Divide", "inputs": { "a": { "$link": ["height", "out"] }, "b": { "$link": ["per group", "out"] } } },
        { "id": "groups x", "type": "Ceil", "inputs": { "a": { "$link": ["x groups", "out"] } } },
        { "id": "groups y", "type": "Ceil", "inputs": { "a": { "$link": ["y groups", "out"] } } }
      ]
    })json";
}

TEST_F(RuntimeTest, AGraphLoadedFromJsonClearsComputesAndShows) {
    auto& runtime = *scene->runtime;
    runtime.Set(Load(FillGraph));
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(*runtime.ValueOf("groups x").Number(), 4.0) << "32 across, 8 a group";
    EXPECT_TRUE(runtime.PerEntityParameters("pipeline").empty()) << "everything is wired: it runs once";

    Frame();
    Frame();
    // By what they use, whatever order they were listed in: the image is cleared, computed into, then shown.
    EXPECT_EQ(scene->Schedule(), (std::vector<std::string> { "clear", "run", "show", "Read screen" }));
    for (const auto [x, y] : { std::pair { 0u, 0u }, std::pair { 31u, 31u }, std::pair { 13u, 20u } }) {
        const kor::U8Vec4 pixel = scene->Pixel(x, y);
        EXPECT_NEAR(pixel.x, 128, 1) << "the clear's red, halved by the shader";
        EXPECT_NEAR(pixel.y, 128, 1) << "the constant, in green";
        EXPECT_EQ(pixel.z, 0);
    }
}

TEST_F(RuntimeTest, OnlyWhatChangedIsBuiltAgain) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    runtime.Set(graph);
    ASSERT_TRUE(runtime.Ok());
    Frame();
    const auto image = runtime.ResourceOf("target");

    runtime.Set(graph);
    for (const auto& node : graph.nodes) EXPECT_EQ(runtime.Status(node.id)->builds, 1u) << node.id << ": the same graph builds nothing";

    graph.Find("pipeline")->Set("constant.level", 1.0);
    runtime.Set(graph);
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.Status("pipeline")->builds, 2u);
    EXPECT_EQ(runtime.Status("run")->builds, 2u) << "what is wired to it follows";
    EXPECT_EQ(runtime.Status("target")->builds, 1u);
    EXPECT_EQ(runtime.Status("shader")->builds, 1u);
    EXPECT_EQ(runtime.Status("clear")->builds, 1u);
    EXPECT_EQ(runtime.ResourceOf("target"), image) << "the very same image";
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(5, 5).y, 255);

    // Moving a node in the editor changes nothing that is built.
    graph.Find("pipeline")->x = 300;
    runtime.Set(graph);
    EXPECT_EQ(runtime.Status("pipeline")->builds, 2u);
}

TEST_F(RuntimeTest, ANodeThatFailsSaysWhyAndTheRestRuns) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    graph.Find("target")->Set("format", "eNoSuchFormat");
    runtime.Set(graph);
    EXPECT_FALSE(runtime.Ok());
    EXPECT_NE(runtime.Status("target")->error.find("eNoSuchFormat"), std::string::npos) << runtime.Status("target")->error;
    EXPECT_NE(runtime.Status("target")->error.find("eRGBA8_UNORM"), std::string::npos) << "it says what there is";
    EXPECT_NE(runtime.Status("pipeline")->error.find("'target', which failed"), std::string::npos) << runtime.Status("pipeline")->error;
    EXPECT_FALSE(runtime.Status("run")->ok);
    EXPECT_TRUE(runtime.Status("shader")->ok);
    EXPECT_TRUE(runtime.Status("per group")->ok);
    Frame();   // with nothing to run, and nothing broken by it
    EXPECT_EQ(scene->Schedule(), std::vector<std::string> { "Read screen" });

    // Put right, it all comes back.
    graph.Find("target")->Set("format", "eRGBA8_UNORM");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();
    EXPECT_NEAR(scene->Pixel(3, 3).x, 128, 1);

    // Other mistakes, each named for what it is.
    graph.Find("target")->Set("formta", "eRGBA8_UNORM");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("target")->error.find("no input 'formta'"), std::string::npos) << runtime.Status("target")->error;
    graph.Find("target")->inputs.erase("formta");
    graph.Find("pipeline")->Wire("param.targte", "target");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("pipeline")->error.find("param.target"), std::string::npos) << "it says what its shaders do have: " << runtime.Status("pipeline")->error;
    graph.Find("pipeline")->inputs.erase("param.targte");
    graph.Find("clear")->Wire("image", "shader");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("clear")->error.find("Shader"), std::string::npos) << runtime.Status("clear")->error;
    graph.Add("ghost", "NoSuchType");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("ghost")->error.find("NoSuchType"), std::string::npos);
}

TEST_F(RuntimeTest, WhatIsNotWiredIsForEachEntityToSupply) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    graph.Find("pipeline")->inputs.erase("param.target");
    runtime.Set(graph);
    EXPECT_TRUE(runtime.Status("pipeline")->ok) << runtime.Status("pipeline")->error;
    EXPECT_EQ(runtime.PerEntityParameters("pipeline"), std::vector<std::string> { "target" });
    // It would run for each entity that supplies it, and this runtime was given no world of them.
    EXPECT_FALSE(runtime.Status("run")->ok);
    EXPECT_NE(runtime.Status("run")->error.find("no world"), std::string::npos) << runtime.Status("run")->error;
    EXPECT_EQ(runtime.ComponentOf("pipeline"), kecs::NoComponent);

    // Its inputs, with those its shader gives it.
    std::vector<std::string> names;
    for (const auto& port : runtime.PortsOf("pipeline")) names.push_back(port.name);
    EXPECT_NE(std::ranges::find(names, "computeShader"), names.end());
    EXPECT_NE(std::ranges::find(names, "param.target"), names.end());
    EXPECT_NE(std::ranges::find(names, "constant.level"), names.end());
}

TEST_F(RuntimeTest, ANodeStandsForAnotherAndACircleIsRefused) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    graph.Add("the image", "Reference").Set("node", "target");
    graph.Find("show")->Wire("source", "the image");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ResourceOf("the image"), runtime.ResourceOf("target"));
    Frame();
    Frame();
    EXPECT_NEAR(scene->Pixel(9, 9).x, 128, 1);

    graph.Add("a", "Add").Wire("a", "b").Set("b", 1);
    graph.Add("b", "Add").Wire("a", "a").Set("b", 1);
    runtime.Set(graph);
    EXPECT_FALSE(runtime.Status("a")->ok && runtime.Status("b")->ok);
    EXPECT_TRUE(runtime.Status("show")->ok) << "the rest is untouched";
}

TEST_F(RuntimeTest, ANodeTakenOutTakesItsPassAndItsObjectWithIt) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    runtime.Set(graph);
    Frame();
    std::weak_ptr<kgraph::Resource> pipeline = runtime.ResourceOf("pipeline");
    ASSERT_FALSE(pipeline.expired());

    std::erase_if(graph.nodes, [](const kgraph::Node& node) { return node.id == "run" || node.id == "pipeline"; });
    runtime.Set(graph);
    EXPECT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.Status("run"), nullptr);
    Frame();
    Frame();
    EXPECT_EQ(scene->Schedule(), (std::vector<std::string> { "clear", "show", "Read screen" }));
    EXPECT_TRUE(pipeline.expired()) << "nothing uses it: it is freed";
    EXPECT_EQ(scene->Pixel(2, 2).x, 255) << "cleared, and shown as cleared";
}

TEST_F(RuntimeTest, AShaderEditedIsPickedUp) {
    // A copy of the shader that can be written to, under a name of its own each run: the framework keeps a shader
    // it compiled, by name, and looks at its file twice a second.
    static int run = 0;
    const std::string module = std::format("fillReloaded{}", run++);
    const auto directory = std::filesystem::temp_directory_path() / "kgraph-reload";
    std::filesystem::create_directories(directory);
    const auto source = std::filesystem::path(KGRAPH_TEST_SHADERS) / "fill.slang";
    std::ifstream in(source);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    { std::ofstream(directory / (module + ".slang")) << text; }
    kor::Shader::AddSearchPath(directory, true);

    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    graph.Find("shader")->Set("entryPoint.module", module);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 128, 1);

    // Edited: the red is quartered now.
    const auto at = text.find("was.r * 0.5");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 11, "was.r * 0.25");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::ofstream(directory / (module + ".slang")) << text; }
    bool reloaded = false;
    for (int i = 0; i < 200 && !reloaded; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        Frame();
        reloaded = scene->Pixel(4, 4).x < 100;
    }
    EXPECT_TRUE(reloaded) << "the edit never showed";
    EXPECT_NEAR(scene->Pixel(4, 4).x, 64, 1);
    EXPECT_GE(runtime.Status("pipeline")->builds, 2u) << "the pipeline was built again from the edited shader";
    std::filesystem::remove_all(directory);
}
