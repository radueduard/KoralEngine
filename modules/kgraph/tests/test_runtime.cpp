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
#include <kmath/quaternion.h>
#include <kmath/transform.h>

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

// What a pipeline's node leaves unwired is each draw's to give: an input of the Dispatch, not of the pipeline.
TEST_F(RuntimeTest, WhatIsNotWiredIsTheDrawsToGive) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    graph.Find("pipeline")->inputs.erase("param.target");
    runtime.Set(graph);
    EXPECT_TRUE(runtime.Status("pipeline")->ok) << runtime.Status("pipeline")->error;
    EXPECT_EQ(runtime.PerEntityParameters("pipeline"), std::vector<std::string> { "target" });
    EXPECT_FALSE(runtime.Status("run")->ok) << "given nothing for it";
    EXPECT_NE(runtime.Status("run")->error.find("'param.target' is given nothing"), std::string::npos) << runtime.Status("run")->error;
    EXPECT_EQ(runtime.ComponentOf("pipeline"), kecs::NoComponent) << "this runtime was given no world to hold one in";
    const auto dispatch = runtime.PortsOf("run");
    EXPECT_NE(std::ranges::find(dispatch, "param.target", &kgraph::Port::name), dispatch.end()) << "the Dispatch takes it";

    // Wired on the Dispatch, it is that draw's, and it runs.
    graph.Find("run")->Wire("param.target", "target");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;

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
    EXPECT_NE(runtime.Status("blob")->error.find("eCube, eSphere, ePlane"), std::string::npos) << runtime.Status("blob")->error;

    const auto vertices = [&](const char* id) { return koral_mesh_vertex_count(runtime.ResourceOf(id)->handle); };
    const auto indices = [&](const char* id) { uint32_t count = 0; koral_mesh_index_count(runtime.ResourceOf(id)->handle, &count); return count; };
    ASSERT_TRUE(runtime.Status("cube")->ok) << runtime.Status("cube")->error;
    EXPECT_EQ(runtime.ResourceOf("quad")->kind, "Mesh");
    EXPECT_EQ(vertices("quad"), 4u);
    EXPECT_EQ(indices("quad"), 6u);
    EXPECT_EQ(vertices("cube"), 24u) << "a face has corners of its own: its normal is its own";
    EXPECT_EQ(indices("cube"), 36u);
    EXPECT_EQ(vertices("triangle"), 3u);
    EXPECT_EQ(vertices("plain"), 24u) << "a cube, when it is not said";

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

// The rest of what a shader's arithmetic has: inverse trigonometry, logarithms of other bases, ranges mapped, vectors
// bounced and bent, and matrices made, taken apart and applied.
TEST_F(RuntimeTest, MoreFunctionsAndMatricesAreWorkedOut) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    const auto node = [&](const char* id, const char* type) -> kgraph::Node& { return graph.Add(id, type); };
    node("asin", "Asin").Set("a", 1);
    node("acos", "Acos").Set("a", Value::List { 1, -1 });
    node("atan", "Atan").Set("a", 1);
    node("tanh", "Tanh").Set("a", 0);
    node("exp2", "Exp2").Set("a", 10);
    node("log2", "Log2").Set("a", 8);
    node("log10", "Log10").Set("a", 1000);
    node("rsqrt", "InverseSqrt").Set("a", 4);
    node("trunc", "Trunc").Set("a", Value::List { -1.75, 1.75 });
    node("rcp", "Reciprocal").Set("a", 4);
    node("one minus", "OneMinus").Set("a", 0.25);
    node("unlerp", "InverseLerp").Set("a", 10).Set("b", 20).Set("value", 15);
    node("remap", "Remap").Set("value", 5).Set("fromMin", 0).Set("fromMax", 10).Set("toMin", 100).Set("toMax", 200);
    node("mad", "MultiplyAdd").Set("a", 2).Set("b", 3).Set("c", 4);
    node("pi", "Constant").Set("value", "ePi");
    node("bounce", "Reflect").Set("incident", Value::List { 1, -1, 0 }).Set("normal", Value::List { 0, 1, 0 });
    node("straight", "Refract").Set("incident", Value::List { 0, -1, 0 }).Set("normal", Value::List { 0, 1, 0 }).Set("eta", 0.75);
    node("total", "Refract").Set("incident", Value::List { 0.8, -0.6, 0 }).Set("normal", Value::List { 0, 1, 0 }).Set("eta", 1.5);
    node("along", "Project").Set("a", Value::List { 3, 4, 0 }).Set("onto", Value::List { 2, 0, 0 });
    // An entity's matrix, made as the world makes it, moves a point as the world does.
    node("placed", "MatrixCompose").Set("position", Value::List { 1, 2, 3 }).Set("rotation", Value::List { 0, 90, 0 }).Set("scale", 2);
    node("moved", "TransformPoint").Wire("matrix", "placed").Set("point", Value::List { 1, 0, 0 });
    node("turned", "TransformDirection").Wire("matrix", "placed").Set("direction", Value::List { 1, 0, 0 });
    node("flipped", "MatrixTranspose").Wire("a", "placed");
    node("volume", "MatrixDeterminant").Wire("a", "placed");
    node("by hand", "MatrixMultiply").Wire("a", "moving").Wire("b", "turning");
    node("moving", "MatrixTranslate").Set("offset", Value::List { 1, 2, 3 });
    node("turning", "MatrixAxisAngle").Set("axis", Value::List { 0, 1, 0 }).Set("angle", 90);
    node("growing", "MatrixScale").Set("scale", Value::List { 1, 2, 3 });
    node("rotating", "MatrixRotate").Set("rotation", Value::List { 0, 90, 0 });
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto number = [&](const char* id) { const double* n = runtime.ValueOf(id).Number(); return n ? *n : -999.0; };
    const auto near = [&](const char* id, const std::vector<double>& numbers, const double within = 1e-5) {
        const Value value = runtime.ValueOf(id);
        const auto* items = value.Items();
        if (!items || items->size() != numbers.size()) return ::testing::AssertionFailure() << id << " is " << value.ToJson();
        for (std::size_t i = 0; i < numbers.size(); ++i)
            if (std::abs(*(*items)[i].Number() - numbers[i]) > within) return ::testing::AssertionFailure() << id << " is " << value.ToJson();
        return ::testing::AssertionSuccess();
    };
    constexpr double Pi = 3.14159265358979323846;
    EXPECT_NEAR(number("asin"), Pi / 2, 1e-9);
    EXPECT_TRUE(near("acos", { 0, Pi }));
    EXPECT_NEAR(number("atan"), Pi / 4, 1e-9);
    EXPECT_EQ(number("tanh"), 0.0);
    EXPECT_EQ(number("exp2"), 1024.0);
    EXPECT_NEAR(number("log2"), 3.0, 1e-12);
    EXPECT_NEAR(number("log10"), 3.0, 1e-12);
    EXPECT_EQ(number("rsqrt"), 0.5);
    EXPECT_TRUE(near("trunc", { -1, 1 })) << "towards zero, as Floor is not";
    EXPECT_EQ(number("rcp"), 0.25);
    EXPECT_EQ(number("one minus"), 0.75);
    EXPECT_EQ(number("unlerp"), 0.5);
    EXPECT_EQ(number("remap"), 150.0);
    EXPECT_EQ(number("mad"), 10.0);
    EXPECT_NEAR(number("pi"), Pi, 1e-12);
    EXPECT_TRUE(near("bounce", { 1, 1, 0 }));
    EXPECT_TRUE(near("straight", { 0, -1, 0 })) << "straight through, along the normal";
    EXPECT_TRUE(near("total", { 0, 0, 0 })) << "all of it reflected: nothing goes through";
    EXPECT_TRUE(near("along", { 3, 0, 0 }));
    // A quarter turn round y takes +x to -z: scaled by two, then moved.
    EXPECT_TRUE(near("moved", { 1, 2, 1 }));
    EXPECT_TRUE(near("turned", { 0, 0, -2 }));
    EXPECT_NEAR(number("volume"), 8.0, 1e-4) << "two each way";
    EXPECT_EQ(runtime.ValueOf("flipped").Items()->at(1), runtime.ValueOf("placed").Items()->at(4)) << "a row made a column";
    const kecs::Mat4 world = kecs::Compose({ 1, 2, 3 }, { 0, 90, 0 }, { 2, 2, 2 });
    std::vector<double> expected(world.m.begin(), world.m.end());
    EXPECT_TRUE(near("placed", expected)) << "as the world composes an entity's";
    const kor::Mat4 byHand = kor::Compose(kor::Vec3(1, 2, 3), kor::Quat::AngleAxis(static_cast<float>(Pi / 2), kor::Vec3(0, 1, 0)), kor::Vec3(1.f));
    std::vector<double> composed;
    for (int i = 0; i < 16; ++i) composed.push_back(byHand[i / 4][i % 4]);
    EXPECT_TRUE(near("by hand", composed));
    EXPECT_TRUE(near("growing", { 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1 }));
    const kecs::Mat4 rotated = kecs::Compose({}, { 0, 90, 0 }, { 1, 1, 1 });
    EXPECT_TRUE(near("rotating", std::vector<double>(rotated.m.begin(), rotated.m.end())));
}

// What a camera sees through is a lens of numbers: each as glm makes it, Y pointing down unless told not. None of them
// knows of a camera, or of where it is seen from: a MatrixMultiply of a lens and a View is the two as one.
TEST_F(RuntimeTest, LensesAreProjectionsOfNumbers) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("view", "View").Set("position", Value::List { 0, 0, 5 }).Set("target", Value::List { 0, 0, 0 });
    graph.Add("perspective", "Perspective").Set("fieldOfView", 60).Set("aspect", 2).Set("near", 0.5).Set("far", 50);
    graph.Add("seen", "MatrixMultiply").Wire("a", "perspective").Wire("b", "view");
    graph.Add("unseen", "MatrixInverse").Wire("a", "seen");
    graph.Add("screen", "Screen");
    graph.Add("glm", "Perspective").Set("fieldOfView", 60).Wire("aspect", "screen", "aspect").Set("near", 0.5).Set("far", 50).Set("flipY", false);
    graph.Add("endless", "Perspective").Set("far", 0);
    graph.Add("reversed", "Perspective").Set("reversedZ", true).Set("near", 1).Set("far", 10);
    graph.Add("box", "Orthographic").Set("size", 4).Set("aspect", 2).Set("near", 1).Set("far", 9).Set("flipY", false);
    graph.Add("off centre", "Frustum").Set("left", -0.1).Set("right", 0.3).Set("bottom", -0.1).Set("top", 0.1).Set("near", 0.1).Set("far", 10).Set("flipY", false);
    // A mirror at y = 0: what is below it is left out of what is seen.
    graph.Add("plane", "TransformPlane").Wire("matrix", "view").Set("plane", Value::List { 0, 1, 0, 0 });     // the world's, as the view sees it
    graph.Add("mirror", "Oblique").Wire("projection", "perspective").Wire("plane", "plane");
    graph.Add("mirror seen", "MatrixMultiply").Wire("a", "mirror").Wire("b", "view");
    graph.Add("front", "Paraboloid").Set("near", 1).Set("far", 11);
    graph.Add("back", "Paraboloid").Set("hemisphere", "eBack");
    graph.Add("back view", "MatrixMultiply").Wire("a", "back", "turn").Wire("b", "view");
    graph.Add("face", "CubeFace").Set("face", 3);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto matrix = [&](const char* id, const char* output = "out") {
        kor::Mat4 m(1.f);
        const Value value = runtime.ValueOf(id, output);
        if (const auto* items = value.Items(); items && items->size() == 16)
            for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = static_cast<float>(*(*items)[static_cast<std::size_t>(i)].Number());
        else ADD_FAILURE() << id << "." << output << " is no matrix: " << value.ToJson();
        return m;
    };
    const auto same = [](const kor::Mat4& a, const kor::Mat4& b) {
        for (int i = 0; i < 16; ++i)
            if (std::abs(a[i / 4][i % 4] - b[i / 4][i % 4]) > 1e-4f) return ::testing::AssertionFailure() << "differ at " << i << ": " << a[i / 4][i % 4] << " and " << b[i / 4][i % 4];
        return ::testing::AssertionSuccess();
    };
    constexpr float Radians = 3.14159265358979323846f / 180.f;
    const kor::Mat4 glm = kor::Perspective(60.f * Radians, 2.f, 0.5f, 50.f);
    scene->runtime->SetScreenExtent({ 200, 100 });
    scene->runtime->Set(graph);
    EXPECT_EQ(runtime.ValueOf("screen", "aspect"), Value(2.0));
    EXPECT_TRUE(same(matrix("glm"), glm)) << "the screen's aspect";
    kor::Mat4 flipped = glm;
    flipped[1][1] = -flipped[1][1];
    EXPECT_TRUE(same(matrix("perspective"), flipped)) << "Y down, as the device has it";
    const kor::Mat4 view = kor::LookAt(kor::Vec3(0, 0, 5), kor::Vec3(0));
    EXPECT_TRUE(same(matrix("view"), view));
    EXPECT_TRUE(same(matrix("seen"), flipped * view)) << "the projection after the view";
    EXPECT_TRUE(same(matrix("unseen"), kor::Inverse(flipped * view)));
    EXPECT_EQ(runtime.ValueOf("view", "forward"), Value(Value::List { 0, 0, -1 }));
    EXPECT_EQ(runtime.ValueOf("view", "up"), Value(Value::List { 0, 1, 0 }));
    EXPECT_EQ(runtime.ValueOf("view", "right"), Value(Value::List { 1, 0, 0 }));
    EXPECT_EQ(runtime.ValueOf("endless", "far"), Value(0)) << "no far plane";
    const auto depth = [&](const kor::Mat4& m, const float z) { const kor::Vec4 clip = m * kor::Vec4(0.f, 0.f, -z, 1.f); return clip.z / clip.w; };
    EXPECT_NEAR(depth(matrix("endless"), 0.1f), 0.f, 1e-5);
    EXPECT_GT(depth(matrix("endless"), 1e5f), 0.99f) << "depth reaches 1 only at no distance";
    // Reversed: the near plane is at depth 1, the far one at 0.
    const kor::Mat4 reversed = matrix("reversed");
    EXPECT_NEAR(depth(reversed, 1.f), 1.f, 1e-5);
    EXPECT_NEAR(depth(reversed, 10.f), 0.f, 1e-5);
    EXPECT_TRUE(same(matrix("box"), kor::Orthographic(-4.f, 4.f, -2.f, 2.f, 1.f, 9.f))) << "as high as its size, as wide as that times the aspect";
    EXPECT_TRUE(same(matrix("off centre"), kor::FrustumProjection(-0.1f, 0.3f, -0.1f, 0.1f, 0.1f, 10.f)));

    // The mirror's plane is its near plane: a point on it at depth 0, one above it in front, one below it behind.
    const kor::Mat4 mirror = matrix("mirror seen");
    const auto at = [&](const kor::Vec3 p) { const kor::Vec4 clip = mirror * kor::Vec4(p.x, p.y, p.z, 1.f); return clip.z / clip.w; };
    EXPECT_NEAR(at({ 0.3f, 0.f, -1.f }), 0.f, 1e-4);
    EXPECT_GT(at({ 0.f, 0.5f, -1.f }), 0.f);
    EXPECT_LT(at({ 0.f, -0.5f, -1.f }), 0.f);
    for (int column = 0; column < 4; ++column) {      // x and y are as they were
        EXPECT_FLOAT_EQ(matrix("mirror")[column][0], flipped[column][0]);
        EXPECT_FLOAT_EQ(matrix("mirror")[column][1], flipped[column][1]);
    }

    const auto params = runtime.ValueOf("front", "params");
    ASSERT_TRUE(params.Items() && params.Items()->size() == 4) << params.ToJson();
    for (std::size_t i = 0; i < 4; ++i) EXPECT_NEAR(*params.Items()->at(i).Number(), (std::array { 1.0, 11.0, 0.1, 1.0 })[i], 1e-6) << i;
    EXPECT_EQ(*runtime.ValueOf("back", "params").Items()->at(3).Number(), -1.0);
    kor::Mat4 halfTurn(1.f);
    halfTurn[0][0] = halfTurn[2][2] = -1.f;
    EXPECT_TRUE(same(matrix("back", "turn"), halfTurn));
    EXPECT_TRUE(same(matrix("back view"), halfTurn * view)) << "the back faces the other way";
    EXPECT_EQ(runtime.ValueOf("face", "direction"), Value(Value::List { 0, -1, 0 }));
    EXPECT_EQ(runtime.ValueOf("face", "up"), Value(Value::List { 0, 0, 1 }));

    // What cannot be is said.
    graph.Find("glm")->Set("far", 0.25);
    graph.Find("box")->Set("left", 1).Set("right", 1);
    graph.Find("face")->Set("face", 6);
    graph.Find("perspective")->Wire("view", "view");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("perspective")->error.find("no lens's now"), std::string::npos) << runtime.Status("perspective")->error;
    EXPECT_NE(runtime.Status("glm")->error.find("farther than 'near'"), std::string::npos) << runtime.Status("glm")->error;
    EXPECT_NE(runtime.Status("box")->error.find("are to be apart"), std::string::npos) << runtime.Status("box")->error;
    EXPECT_NE(runtime.Status("face")->error.find("from 0 to 5"), std::string::npos) << runtime.Status("face")->error;
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

// How many groups cover an image is its extent over what one group works on, and the pipeline says what that is:
// so the three are wired, vector to vector, with nothing typed in.
TEST_F(RuntimeTest, AnImagesExtentOverThePipelinesNumThreadsIsHowManyGroupsCoverIt) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    // 26 across: three groups of 8 and a part of a fourth.
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 26, 26 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain").Wire("param.target", "target").Set("constant.level", 0.5);
    graph.Add("count", "Divide").Wire("a", "target", "extent").Wire("b", "pipeline", "numThreads");
    graph.Add("clear", "Clear").Wire("image", "target").Set("color", Value::List { 1, 0, 0, 1 });
    graph.Add("run", "Dispatch").Wire("pipeline", "pipeline").Wire("groups", "count");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "target").Wire("destination", "screen");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    EXPECT_EQ(runtime.ValueOf("pipeline", "numThreads"), Value(Value::List { 8, 8, 1 })) << "what the shader's [numthreads] says";
    EXPECT_EQ(runtime.ValueOf("target", "extent"), Value(Value::List { 26, 26, 1 })) << "an extent given whole, of two: its depth as it starts";
    EXPECT_EQ(runtime.ValueOf("count"), Value(Value::List { 3.25, 3.25, 1 }));
    EXPECT_EQ(runtime.ValueOf("run", "groups"), Value(Value::List { 3.25, 3.25, 1 })) << "given whole, it is given in its parts";

    Frame();
    Frame();
    // Every pixel was worked on, the last few of each row too: a part of a group is a group.
    for (const auto [x, y] : { std::pair { 0u, 0u }, std::pair { 31u, 31u }, std::pair { 31u, 4u }, std::pair { 13u, 31u } }) {
        const kor::U8Vec4 pixel = scene->Pixel(x, y);
        EXPECT_NEAR(pixel.x, 128, 1) << "at " << x << ", " << y << ": the clear's red, halved by the shader";
        EXPECT_NEAR(pixel.y, 128, 1);
    }

    // The whole or its parts: not both.
    graph.Find("run")->Set("groups.x", 2);
    runtime.Set(graph);
    ASSERT_FALSE(runtime.Status("run")->ok);
    EXPECT_NE(runtime.Status("run")->error.find("'groups' is given whole, and 'groups.x' on its own as well"), std::string::npos) << runtime.Status("run")->error;
    // And a port left open for the whole, with nothing wired yet, is nothing given.
    graph.Find("run")->inputs.erase("groups.x");
    graph.Find("run")->Set("groups", Value {});
    runtime.Set(graph);
    EXPECT_TRUE(runtime.Status("run")->ok) << runtime.Status("run")->error;

    // The catalog offers both, for an editor.
    const kgraph::NodeType* dispatch = kgraph::FindType("Dispatch");
    EXPECT_EQ(kgraph::PartsOf(*dispatch, "groups"), (std::vector<std::string> { "groups.x", "groups.y", "groups.z" }));
    EXPECT_TRUE(kgraph::PartsOf(*dispatch, "cover").empty());
    EXPECT_EQ(kgraph::PartsOf(*kgraph::FindType("Image"), "extent").size(), 3u);
    const kgraph::NodeType* compute = kgraph::FindType("ComputePipeline");
    EXPECT_NE(std::ranges::find(compute->outputs, "numThreads", &kgraph::Port::name), compute->outputs.end());
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
    EXPECT_EQ(initial("Dispatch", "groups.x"), Value(1));
    EXPECT_TRUE(initial("Dispatch", "groupSize.x").IsNull()) << "the pipeline's numThreads, which no node type can say";
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
