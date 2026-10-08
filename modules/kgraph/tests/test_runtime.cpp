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

#include <koral_c.h>
#include <log.h>

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
    EXPECT_NE(std::ranges::find(names, "compute.module"), names.end());
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

TEST_F(RuntimeTest, APrimitiveIsAMeshWithItsLayout) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("quad", "Primitive").Set("shape", "eQuad");
    graph.Add("cube", "Primitive").Set("shape", "eCube");
    graph.Add("triangle", "Primitive").Set("shape", "eTriangle");
    graph.Add("plain", "Primitive");
    graph.Add("blob", "Primitive").Set("shape", "eBlob");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("blob")->error.find("eQuad, eCube or eTriangle"), std::string::npos) << runtime.Status("blob")->error;

    const auto vertices = [&](const char* id) { return koral_mesh_vertex_count(runtime.ResourceOf(id)->handle); };
    const auto indices = [&](const char* id) { uint32_t count = 0; koral_mesh_index_count(runtime.ResourceOf(id)->handle, &count); return count; };
    ASSERT_TRUE(runtime.Status("cube")->ok) << runtime.Status("cube")->error;
    EXPECT_EQ(runtime.ResourceOf("quad")->kind, "Mesh");
    EXPECT_EQ(vertices("quad"), 4u);
    EXPECT_EQ(indices("quad"), 6u);
    EXPECT_EQ(vertices("cube"), 24u) << "a face has corners of its own: its normal is its own";
    EXPECT_EQ(indices("cube"), 36u);
    EXPECT_EQ(vertices("triangle"), 3u);
    EXPECT_EQ(vertices("plain"), 4u) << "a quad, when it is not said";

    // What a pipeline that draws it is built against: position, normal and texture coordinate, 32 bytes a vertex.
    const kgraph::Value layout = runtime.ValueOf("cube", "layout");
    ASSERT_NE(layout.Find("attributes"), nullptr);
    ASSERT_EQ(layout.Find("attributes")->Items()->size(), 3u);
    EXPECT_EQ(*(*layout.Find("attributes")->Items())[2].Find("semantic")->String(), "UV");
    EXPECT_EQ(*(*layout.Find("attributes")->Items())[2].Find("offset")->Number(), 24.0);
    EXPECT_EQ(*(*layout.Find("bindings")->Items())[0].Find("stride")->Number(), 32.0);
    EXPECT_EQ(*layout.Find("positionAttribute")->Number(), 0.0);
}

// A pipeline names its shaders: no node for each, and no wire to it.
TEST_F(RuntimeTest, APipelineNamesItsShadersByModuleAndEntryPoint) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    std::erase_if(graph.nodes, [](const kgraph::Node& node) { return node.id == "shader"; });
    auto& pipeline = *graph.Find("pipeline");
    pipeline.inputs.erase("computeShader");
    pipeline.Set("compute.module", "fill").Set("compute.entry", "computeMain");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();
    EXPECT_NEAR(scene->Pixel(7, 7).x, 128, 1) << "it runs as it did with a shader node wired in";
    EXPECT_NEAR(scene->Pixel(7, 7).y, 128, 1);
    // What the shader declares is the node's to wire and set, as before.
    std::vector<std::string> names;
    for (const auto& port : runtime.PortsOf("pipeline")) names.push_back(port.name);
    EXPECT_NE(std::ranges::find(names, "param.target"), names.end());
    EXPECT_NE(std::ranges::find(names, "constant.level"), names.end());

    // The entry point not said: the stage's name and Main, as the shader here calls it.
    graph.Find("pipeline")->inputs.erase("compute.entry");
    runtime.Set(graph);
    EXPECT_TRUE(runtime.Status("pipeline")->ok) << runtime.Status("pipeline")->error;

    // What is wrong with it is said on the node, by the input's name.
    graph.Find("pipeline")->Set("compute.module", "no such module");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("pipeline")->error.find("'compute':"), std::string::npos) << runtime.Status("pipeline")->error;
    EXPECT_NE(runtime.Status("pipeline")->error.find("no such module"), std::string::npos) << runtime.Status("pipeline")->error;
    graph.Find("pipeline")->inputs.erase("compute.module");
    graph.Find("pipeline")->Set("compute.entry", "computeMain");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("pipeline")->error.find("'compute.module' says which"), std::string::npos) << runtime.Status("pipeline")->error;
    graph.Find("pipeline")->Set("compute.module", "fill").Set("stages", "eMesh");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("pipeline")->error.find("no input 'stages'"), std::string::npos) << runtime.Status("pipeline")->error;

    // A graphics pipeline drawn with mesh shaders has no vertex shader to be given.
    graph.Find("pipeline")->inputs.erase("stages");
    graph.Add("drawn", "GraphicsPipeline").Set("stages", "eMesh").Set("vertex.module", "fill");
    runtime.Set(graph);
    EXPECT_TRUE(runtime.Status("pipeline")->ok);
    EXPECT_NE(runtime.Status("drawn")->error.find("'vertex.module' is part of a pipeline whose 'stages' is eVertex"), std::string::npos) << runtime.Status("drawn")->error;
    graph.Find("drawn")->Set("stages", "eTriangles");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("drawn")->error.find("eVertex or eMesh"), std::string::npos) << runtime.Status("drawn")->error;
}

TEST_F(RuntimeTest, AShaderAPipelineNamesIsPickedUpWhenEdited) {
    static int run = 0;
    const std::string module = std::format("fillNamed{}", run++);
    const auto directory = std::filesystem::temp_directory_path() / "kgraph-reload-named";
    std::filesystem::create_directories(directory);
    std::ifstream in(std::filesystem::path(KGRAPH_TEST_SHADERS) / "fill.slang");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    { std::ofstream(directory / (module + ".slang")) << text; }
    kor::Shader::AddSearchPath(directory, true);

    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    std::erase_if(graph.nodes, [](const kgraph::Node& node) { return node.id == "shader"; });
    graph.Find("pipeline")->inputs.erase("computeShader");
    graph.Find("pipeline")->Set("compute.module", module);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 128, 1);
    const auto builds = runtime.Status("pipeline")->builds;

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
    EXPECT_GT(runtime.Status("pipeline")->builds, builds) << "the pipeline is built again from its edited shader";
    std::filesystem::remove_all(directory);
}

// What is worked out of values: arithmetic and functions, on numbers and on vectors a component at a time;
// comparisons and choices; a vector taken apart and put together.
TEST_F(RuntimeTest, ValuesAreWorkedOutByNodes) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    const auto node = [&](const char* id, const char* type) -> kgraph::Node& { return graph.Add(id, type); };
    node("clamp", "Clamp").Set("x", 1.5).Set("min", 0).Set("max", 1);
    node("lerp", "Lerp").Set("a", Value::List { 0, 10 }).Set("b", Value::List { 10, 20 }).Set("t", 0.25);
    node("pow", "Pow").Set("a", 3).Set("b", 2);
    node("mod", "Mod").Set("a", -1).Set("b", 4);
    node("sqrt", "Sqrt").Set("a", Value::List { 4, 9, 16 });
    node("smooth", "Smoothstep").Set("edge0", 0).Set("edge1", 2).Set("x", 1);
    node("step", "Step").Set("edge", 0.5).Set("x", 0.75);
    node("sat", "Saturate").Set("a", -3);
    node("fract", "Fract").Set("a", 2.25);
    node("length", "Length").Set("a", Value::List { 3, 4 });
    node("unit", "Normalize").Set("a", Value::List { 0, 5, 0 });
    node("dot", "Dot").Set("a", Value::List { 1, 2, 3 }).Set("b", Value::List { 4, 5, 6 });
    node("cross", "Cross").Set("a", Value::List { 1, 0, 0 }).Set("b", Value::List { 0, 1, 0 });
    node("far", "Distance").Set("a", Value::List { 1, 1 }).Set("b", Value::List { 4, 5 });
    node("less", "Less").Set("a", 1).Set("b", 2);
    node("same", "Equal").Set("a", Value::List { 1, 2 }).Set("b", Value::List { 1, 2 });
    node("both", "And").Wire("a", "less").Wire("b", "same");
    node("neither", "Not").Wire("a", "both");
    node("pick", "Select").Wire("condition", "less").Set("a", "yes").Set("b", "no");
    node("parts", "Split").Wire("vector", "cross");
    node("again", "Vector").Wire("x", "parts", "z").Wire("y", "parts", "x");
    node("colour", "Color").Set("color", Value::List { 1, 0.5, 0, 1 });
    node("half", "Multiply").Wire("a", "colour").Set("b", 0.5);
    node("degrees", "Degrees").Set("a", 3.14159265358979323846);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto number = [&](const char* id, const char* output = "out") { const double* n = runtime.ValueOf(id, output).Number(); return n ? *n : -999.0; };

    EXPECT_EQ(number("clamp"), 1.0);
    EXPECT_EQ(runtime.ValueOf("lerp"), Value(Value::List { 2.5, 12.5 }));
    EXPECT_EQ(number("pow"), 9.0);
    EXPECT_EQ(number("mod"), 3.0) << "it wraps, as a shader's does";
    EXPECT_EQ(runtime.ValueOf("sqrt"), Value(Value::List { 2, 3, 4 }));
    EXPECT_EQ(number("smooth"), 0.5);
    EXPECT_EQ(number("step"), 1.0);
    EXPECT_EQ(number("sat"), 0.0);
    EXPECT_EQ(number("fract"), 0.25);
    EXPECT_EQ(number("length"), 5.0);
    EXPECT_EQ(runtime.ValueOf("unit"), Value(Value::List { 0, 1, 0 }));
    EXPECT_EQ(number("dot"), 32.0);
    EXPECT_EQ(runtime.ValueOf("cross"), Value(Value::List { 0, 0, 1 }));
    EXPECT_EQ(number("far"), 5.0);
    EXPECT_EQ(runtime.ValueOf("less"), Value(true));
    EXPECT_EQ(runtime.ValueOf("same"), Value(true));
    EXPECT_EQ(runtime.ValueOf("both"), Value(true));
    EXPECT_EQ(runtime.ValueOf("neither"), Value(false));
    EXPECT_EQ(runtime.ValueOf("pick"), Value("yes"));
    EXPECT_EQ(number("parts", "z"), 1.0);
    EXPECT_EQ(number("parts", "w"), 0.0) << "a part the vector does not have is 0";
    EXPECT_EQ(runtime.ValueOf("again"), Value(Value::List { 1, 0 }));
    EXPECT_EQ(runtime.ValueOf("half"), Value(Value::List { 0.5, 0.25, 0, 0.5 }));
    EXPECT_NEAR(number("degrees"), 180.0, 1e-9);

    // What cannot be worked out says why.
    graph.Find("cross")->Set("a", Value::List { 1, 0 });
    graph.Find("less")->Set("a", "one");
    graph.Find("clamp")->inputs.erase("max");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("cross")->error.find("do not go together"), std::string::npos) << runtime.Status("cross")->error;
    EXPECT_NE(runtime.Status("less")->error.find("numbers"), std::string::npos) << runtime.Status("less")->error;
    EXPECT_NE(runtime.Status("clamp")->error.find("'max' is not given"), std::string::npos) << runtime.Status("clamp")->error;
}

// Everything about a node is there to be wired from: what the object it made says of itself, each value it was
// given, the parts of one thing as one, and what an input is when nobody set it.
TEST_F(RuntimeTest, ANodeGivesWhatItIsMadeOfAsOutputs) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("picture", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 48).Set("extent.y", 24).Set("usage", Value::List { "eSampled", "eTransferDst" });
    // A second image, half the size of the first each way: sized from it, not by hand.
    graph.Add("half", "Divide").Wire("a", "picture", "size").Set("b", 2);
    graph.Add("across", "Component").Wire("vector", "half").Set("index", 0);
    graph.Add("down", "Component").Wire("vector", "half").Set("index", 1);
    graph.Add("small", "Image").Set("format", "eRGBA8_UNORM").Wire("extent.x", "across").Wire("extent.y", "down").Set("usage", Value::List { "eSampled" });
    graph.Add("data", "Buffer").Set("instanceCount", 96).Set("usage", Value::List { "eStorage" });
    graph.Add("shape", "Primitive").Set("shape", "eCube");
    graph.Add("clear", "Clear").Wire("image", "picture").Set("color", Value::List { 1, 0, 0, 1 });
    graph.Add("same colour", "Clear").Wire("image", "picture").Wire("color", "clear", "color");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;

    EXPECT_EQ(runtime.ValueOf("picture", "extent"), Value(Value::List { 48, 24, 1 })) << "what the image is, whatever was or was not asked for";
    EXPECT_EQ(runtime.ValueOf("picture", "size"), Value(Value::List { 48, 24 }));
    EXPECT_EQ(runtime.ValueOf("picture", "width"), Value(48));
    EXPECT_EQ(runtime.ValueOf("picture", "height"), Value(24));
    EXPECT_EQ(runtime.ValueOf("picture", "mipLevels"), Value(1));
    EXPECT_EQ(runtime.ValueOf("picture", "format"), Value("eRGBA8_UNORM")) << "a value it was given";
    EXPECT_EQ(runtime.ValueOf("picture", "usage"), Value(Value::List { "eSampled", "eTransferDst" }));
    EXPECT_EQ(runtime.ValueOf("picture", "sampleCount"), Value("e1")) << "one it was not given is what it starts as";
    EXPECT_EQ(runtime.ValueOf("small", "size"), Value(Value::List { 24, 12 })) << "an image sized from another's";
    EXPECT_EQ(runtime.ValueOf("data", "bytes"), Value(96));
    EXPECT_EQ(runtime.ValueOf("shape", "vertexCount"), Value(24));
    EXPECT_EQ(runtime.ValueOf("shape", "indexCount"), Value(36));
    EXPECT_EQ(runtime.ValueOf("clear", "color"), Value(Value::List { 1, 0, 0, 1 })) << "a pass's values too";
    EXPECT_TRUE(runtime.Status("same colour")->ok);

    // The first resized: what was worked out of it follows.
    graph.Find("picture")->Set("extent.x", 96);
    runtime.Set(graph);
    EXPECT_EQ(runtime.ValueOf("small", "width"), Value(48));
    EXPECT_EQ(runtime.Status("data")->builds, 1u) << "and what has nothing to do with it is left alone";

    // The catalog says what each kind of node gives, for an editor to offer.
    const kgraph::NodeType* image = kgraph::FindType("Image");
    for (const char* name : { "size", "extent", "width", "height", "mipLevels", "format", "usage" }) {
        const auto it = std::ranges::find(image->outputs, name, &kgraph::Port::name);
        ASSERT_NE(it, image->outputs.end()) << name;
        EXPECT_TRUE(it->optional) << name << ": there to be asked for";
    }
    EXPECT_FALSE(image->outputs.front().optional);
}

// A node is made with what it needs set to something that works, and a struct given in part has the rest as it
// should be, not as nothing.
TEST_F(RuntimeTest, InputsStartAsSomethingThatWorks) {
    using kgraph::Value;
    const auto initial = [](const char* type, const std::string_view name) -> Value {
        const kgraph::NodeType* found = kgraph::FindType(type);
        const auto it = std::ranges::find(found->inputs, name, &kgraph::Port::name);
        return it == found->inputs.end() ? Value("no such input") : it->initial;
    };
    EXPECT_EQ(initial("Image", "format"), Value("eRGBA8_UNORM"));
    EXPECT_EQ(initial("Image", "extent.x"), Value(512));
    EXPECT_EQ(initial("Image", "usage"), Value(Value::List { "eSampled", "eTransferSrc", "eTransferDst" }));
    EXPECT_EQ(initial("Image", "mipLevels"), Value(1));
    EXPECT_EQ(initial("Sampler", "minFilter"), Value("eLinear"));
    EXPECT_EQ(initial("Clear", "color"), Value(Value::List { 0, 0, 0, 1 }));
    EXPECT_EQ(initial("Dispatch", "groupSize.x"), Value(8));
    EXPECT_EQ(initial("Draw", "instances"), Value(1));
    EXPECT_EQ(*initial("GraphicsPipeline", "rasterizationState").Find("lineWidth"), Value(1));
    EXPECT_EQ(*initial("GraphicsPipeline", "depthStencilState").Find("depthCompareOp"), Value("eLess"));

    // An image with nothing but those is an image.
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    auto& image = graph.Add("image", "Image");
    for (const auto& port : kgraph::FindType("Image")->inputs)
        if (!port.optional) image.Set(port.name, port.initial);
    runtime.Set(graph);
    ASSERT_TRUE(runtime.Status("image")->ok) << runtime.Status("image")->error;
    EXPECT_EQ(runtime.ValueOf("image", "extent"), Value(Value::List { 512, 512, 1 }));
}

TEST_F(RuntimeTest, ImagesAndBuffersAreCopiedAndMipsFilled) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    const auto image = [&](const char* id, const int size, Value::List usage, const int mips = 1) {
        graph.Add(id, "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", size).Set("extent.y", size).Set("usage", std::move(usage)).Set("mipLevels", mips);
    };
    image("first", 32, { "eTransferSrc", "eTransferDst" });
    image("second", 32, { "eTransferSrc", "eTransferDst" });
    image("mipped", 32, { "eTransferSrc", "eTransferDst", "eSampled" }, 4);
    graph.Add("paint", "Clear").Wire("image", "first").Set("color", Value::List { 0, 1, 0, 1 });
    graph.Add("copy", "Copy").Wire("source", "first").Wire("destination", "second");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "second").Wire("destination", "screen");
    graph.Add("paint mipped", "Clear").Wire("image", "mipped").Set("color", Value::List { 1, 1, 1, 1 });
    graph.Add("mips", "GenerateMipmaps").Wire("image", "mipped");
    graph.Add("from", "Buffer").Set("instanceCount", 64).Set("usage", Value::List { "eTransferSrc", "eTransferDst" });
    graph.Add("to", "Buffer").Set("instanceCount", 32).Set("usage", Value::List { "eTransferDst", "eTransferSrc" });
    graph.Add("copy bytes", "CopyBuffer").Wire("source", "from").Wire("destination", "to");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const std::vector<kor::u8> bytes(64, 0xAB);
    koral_buffer_write(runtime.ResourceOf("from")->handle, bytes.data(), bytes.size(), 0);
    const auto logged = kor::log::LastSequence();
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(5, 5).y, 255) << "painted, copied, and shown: in the order what they use puts them";
    EXPECT_EQ(scene->Pixel(5, 5).x, 0);
    std::vector<kor::u8> copied(32, 0);
    koral_buffer_read(runtime.ResourceOf("to")->handle, copied.data(), copied.size(), 0);
    EXPECT_EQ(copied, std::vector<kor::u8>(32, 0xAB)) << "as many bytes as the smaller holds";
    EXPECT_EQ(runtime.ValueOf("mipped", "mipLevels"), Value(4));
    for (const auto& record : kor::log::HistorySince(logged)) EXPECT_EQ(record.message.find("VUID"), std::string::npos) << record.message;

    // What cannot be copied, or filled, says why.
    graph.Find("second")->Set("extent.x", 16);
    graph.Find("mipped")->Set("mipLevels", 1);
    graph.Find("to")->Set("usage", Value::List { "eStorage" });
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("copy")->error.find("a Blit scales"), std::string::npos) << runtime.Status("copy")->error;
    EXPECT_NE(runtime.Status("mips")->error.find("one mip level"), std::string::npos) << runtime.Status("mips")->error;
    EXPECT_NE(runtime.Status("copy bytes")->error.find("eTransferDst"), std::string::npos) << runtime.Status("copy bytes")->error;
}
