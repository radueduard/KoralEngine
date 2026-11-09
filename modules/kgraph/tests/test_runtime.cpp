// A graph running on a device: objects built from nodes, passes ordered by what they use, and only what changed
// built again. Offscreen, with no windowing system.

#include <gtest/gtest.h>
#include <sstream>
#include <kecs/json.h>
#include <kgraph/meshes.h>

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
#include <kgraph/random.h>
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
        { "id": "sides", "type": "Split", "inputs": { "vector": { "$link": ["size", "out"] } } },
        { "id": "per group", "type": "Double", "inputs": { "value": 8 } },
        { "id": "x groups", "type": "Divide", "inputs": { "a": { "$link": ["sides", "x"] }, "b": { "$link": ["per group", "out"] } } },
        { "id": "y groups", "type": "Divide", "inputs": { "a": { "$link": ["sides", "y"] }, "b": { "$link": ["per group", "out"] } } },
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

    // What a pipeline that draws it is built against: position, normal, a tangent of four and texture coordinate,
    // 48 bytes a vertex.
    const kgraph::Value layout = runtime.ValueOf("cube", "layout");
    ASSERT_NE(layout.Find("attributes"), nullptr);
    ASSERT_EQ(layout.Find("attributes")->Items()->size(), 4u);
    EXPECT_EQ(*(*layout.Find("attributes")->Items())[2].Find("semantic")->String(), "TANGENT");
    EXPECT_EQ(*(*layout.Find("attributes")->Items())[2].Find("offset")->Number(), 24.0);
    EXPECT_EQ(*(*layout.Find("attributes")->Items())[3].Find("semantic")->String(), "UV");
    EXPECT_EQ(*(*layout.Find("attributes")->Items())[3].Find("offset")->Number(), 40.0);
    EXPECT_EQ(*(*layout.Find("bindings")->Items())[0].Find("stride")->Number(), 48.0);
    EXPECT_EQ(*layout.Find("positionAttribute")->Number(), 0.0);

    // As the graph asks: a tangent and a bitangent of three each, as a model file's are; or neither.
    graph.defaultMeshTangents = kgraph::Tangents::eWithBitangent;
    runtime.Set(graph);
    const kgraph::Value both = runtime.ValueOf("cube", "layout");
    ASSERT_EQ(both.Find("attributes")->Items()->size(), 5u);
    EXPECT_EQ(*(*both.Find("attributes")->Items())[3].Find("semantic")->String(), "BITANGENT");
    EXPECT_EQ(*(*both.Find("bindings")->Items())[0].Find("stride")->Number(), 56.0);
    graph.defaultMeshTangents = kgraph::Tangents::eNone;
    runtime.Set(graph);
    EXPECT_EQ(*runtime.ValueOf("cube", "layout").Find("bindings")->Items()->at(0).Find("stride")->Number(), 32.0);
}

// A default mesh an entity holds is made again when the graph asks for other tangents, and what held it follows.
TEST_F(RuntimeTest, ADefaultMeshIsMadeAgainWithTheTangentsAskedFor) {
    auto& runtime = *scene->runtime;
    const auto attributes = [&] {
        std::vector<std::string> semantics;
        for (const auto& attribute : runtime.ReadMesh("$sphere")->attributes) semantics.push_back(attribute.semantic + std::to_string(attribute.count));
        return semantics;
    };
    EXPECT_EQ(attributes(), (std::vector<std::string> { "POSITION3", "NORMAL3", "TANGENT4", "UV2" }));
    const auto first = runtime.ResourceOf("$sphere");
    kgraph::Graph graph;
    graph.defaultMeshTangents = kgraph::Tangents::eWithBitangent;
    runtime.Set(graph);
    EXPECT_EQ(attributes(), (std::vector<std::string> { "POSITION3", "NORMAL3", "TANGENT3", "BITANGENT3", "UV2" }));
    EXPECT_TRUE(first->replacedBy != nullptr) << "what held the old one is led to the new";
}

// Who names the same numbers holds the one mesh; another's are another mesh; one nobody holds is let go of.
TEST_F(RuntimeTest, TheSameParametersShareAMesh) {
    auto& runtime = *scene->runtime;
    auto tuned = runtime.ResourceOf("$sphere?rings=10&segments=20");
    ASSERT_TRUE(tuned);
    EXPECT_EQ(tuned->node, "$sphere?rings=10&segments=20") << "saved by what it is";
    EXPECT_EQ(runtime.ResourceOf("$sphere?segments=20&rings=10"), tuned) << "however it is written";
    EXPECT_NE(runtime.ResourceOf("$sphere?rings=11&segments=20"), tuned);
    EXPECT_EQ(runtime.ResourceOf("$sphere?rings=40"), runtime.ResourceOf("$sphere")) << "the numbers it starts with are the mesh it starts as";
    EXPECT_EQ(runtime.ReadMesh("$sphere?rings=10&segments=20")->indices.size(), 10u * 20u * 6u);
    EXPECT_FALSE(runtime.ResourceOf("$blob?rings=10"));

    const std::weak_ptr<kgraph::Resource> weak = tuned;
    const std::weak_ptr<kgraph::Resource> plain = runtime.ResourceOf("$sphere");
    tuned.reset();
    for (int frame = 0; frame < 4; ++frame) { runtime.Update(); Frame(); }
    EXPECT_TRUE(weak.expired()) << "a tuned mesh nothing holds is not kept";
    EXPECT_FALSE(plain.expired()) << "the one it starts as is";
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
    node("again", "Vec2").Wire("x", "parts", "z").Wire("y", "parts", "x");
    node("colour", "Color").Set("color", Value::List { 1, 0.5, 0, 1 });
    node("half", "Multiply").Wire("a", "colour").Set("b", 0.5);
    node("degrees", "Degrees").Set("a", 3.14159265358979323846);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto number = [&](const char* id, const char* output = "out") { const kgraph::Value value = runtime.ValueOf(id, output); const double* n = value.Number(); return n ? *n : -999.0; };

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
    EXPECT_NE(runtime.Status("less")->error.find("'a' is a vector"), std::string::npos) << runtime.Status("less")->error;
    EXPECT_NE(runtime.Status("clamp")->error.find("'max' is not given"), std::string::npos) << runtime.Status("clamp")->error;

    // Numbers and vectors of a kind: as a shader's float, a double, an int and a uint hold them.
    kgraph::Graph kinds;
    kinds.Add("third", "Float").Set("value", 1.0 / 3.0);
    kinds.Add("precise", "Double").Set("value", 1.0 / 3.0);
    kinds.Add("whole", "Int").Set("value", -2.75);
    kinds.Add("none", "UInt").Set("value", -5);
    kinds.Add("huge", "Int").Set("value", 1e12);
    kinds.Add("place", "Vec3").Set("x", 1).Set("y", 2).Set("z", 0.1);
    kinds.Add("cell", "IVec2").Set("x", 3.9).Set("y", -3.9);
    kinds.Add("size", "UVec4").Set("x", 8).Set("y", -1).Set("z", 2).Set("w", 1);
    kinds.Add("flat", "Vec2").Set("x", 1);
    runtime.Set(kinds);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(number("third"), static_cast<double>(1.f / 3.f)) << "32 bits of it";
    EXPECT_EQ(number("precise"), 1.0 / 3.0);
    EXPECT_EQ(number("whole"), -2.0) << "toward zero";
    EXPECT_EQ(number("none"), 0.0) << "a uint is none or more";
    EXPECT_EQ(number("huge"), 2147483647.0) << "as much as an int holds";
    EXPECT_EQ(runtime.ValueOf("place"), Value(Value::List { 1, 2, static_cast<double>(0.1f) }));
    EXPECT_EQ(runtime.ValueOf("cell"), Value(Value::List { 3, -3 }));
    EXPECT_EQ(runtime.ValueOf("size"), Value(Value::List { 8, 0, 2, 1 }));
    EXPECT_EQ(runtime.ValueOf("flat"), Value(Value::List { 1, 0 })) << "what is not given, 0";
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
    const auto number = [&](const char* id) { const kgraph::Value value = runtime.ValueOf(id); const double* n = value.Number(); return n ? *n : -999.0; };
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
    graph.Add("sides", "Split").Wire("vector", "half");
    graph.Add("small", "Image").Set("format", "eRGBA8_UNORM").Wire("extent.x", "sides", "x").Wire("extent.y", "sides", "y").Set("usage", Value::List { "eSampled" });
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
    EXPECT_TRUE(initial("Image", "usage").IsNull()) << "it is made for what uses it";
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
    EXPECT_TRUE(runtime.Status("copy bytes")->ok) << "a buffer given another usage is made for being copied to besides: " << runtime.Status("copy bytes")->error;
}

// An Image given no usage is made for what uses it: cleared, computed into, and copied from to be shown, here — and
// not sampled, which nothing does. What it is no longer used as, it is made again without; what it is given, it has too.
TEST_F(RuntimeTest, AnImageGivenNoUsageIsMadeForWhatUsesIt) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Load(FillGraph);
    graph.Find("target")->inputs.erase("usage");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const auto usage = [&] {
        runtime.Watch("target");
        std::optional<kgraph::Peek> peek;
        for (int i = 0; i < 8 && !(peek = runtime.Watched("target")); ++i) Frame();
        return peek ? peek->usage : std::vector<std::string> {};
    };
    EXPECT_EQ(usage(), (std::vector<std::string> { "eTransferSrc", "eTransferDst", "eStorage" }));
    EXPECT_NEAR(scene->Pixel(13, 20).x, 128, 1) << "cleared, then halved by the shader";

    // Cleared no more: it is not made to be copied into.
    std::erase_if(graph.nodes, [](const kgraph::Node& node) { return node.id == "clear"; });
    runtime.Set(graph);
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(usage(), (std::vector<std::string> { "eTransferSrc", "eStorage" }));

    // Given a usage, it has that besides.
    graph.Find("target")->Set("usage", kgraph::Value::List { "eSampled" });
    runtime.Set(graph);
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(usage(), (std::vector<std::string> { "eTransferSrc", "eSampled", "eStorage" }));
}

// A setup page's passes record once, on the frame after they are built, and again only when they are built again:
// a picture cleared there and halved every frame grows darker, where one cleared every frame would stay as it is.
TEST_F(RuntimeTest, ASetupPassRecordsWhenItIsBuiltAndRestsAfter) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "made", "frame" };
    graph.setup = { "made" };
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "made";
    graph.Add("clear", "Clear").Wire("image", "target").Set("color", Value::List { 1, 0, 0, 1 }).pass = "made";
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain")
        .Wire("param.target", "target").Set("constant.level", 0.5).pass = "frame";
    graph.Add("halve", "Dispatch").Wire("pipeline", "pipeline").Set("cover", "target").pass = "frame";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "target").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    // What uses the picture, page by page: what wrote it before a page is what comes before it.
    const std::vector<kgraph::PassUse> uses {
        { "clear", "made", false, true }, { "halve", "frame", true, true }, { "show", "frame", true, false } };
    EXPECT_EQ(runtime.UsesOf("target"), uses);
    EXPECT_TRUE(runtime.UsesOf("pipeline").empty()) << "a pipeline is no image or buffer";

    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_LT(scene->Pixel(4, 4).x, 16) << "cleared once, and halved every frame since";
    EXPECT_NEAR(scene->Pixel(4, 4).y, 128, 1) << "the frame's pass ran all the while";

    // Built again — another colour — it clears again, once.
    graph.Find("clear")->Set("color", Value::List { 1, 1, 0, 1 });
    runtime.Set(graph);
    Frame();
    Frame();
    const int again = scene->Pixel(4, 4).x;
    EXPECT_GT(again, 16) << "cleared again when it was built again";
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_LT(scene->Pixel(4, 4).x, again) << "and only then";

    // On a page of every frame, it is cleared every frame.
    graph.setup.clear();
    runtime.Set(graph);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 128, 1) << "cleared each frame, and halved once";

    // Saved, the setup pages are apart from the rest, and come first.
    graph.setup = { "made" };
    kgraph::Graph loaded;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(graph.ToJson(), loaded, error)) << error;
    EXPECT_EQ(loaded.setup, graph.setup);
    EXPECT_EQ(loaded.passes, graph.passes);
    EXPECT_NE(graph.ToJson().find("\"setup\": [\"made\"]"), std::string::npos) << graph.ToJson();
    EXPECT_NE(graph.ToJson().find("\"passes\": [\"frame\"]"), std::string::npos) << graph.ToJson();
}

// A Reference looked at is what it stands for as it is where the Reference is: when its page begins, before what the page
// does to it. And what a later page overwrites, it overwrites after what an earlier one did.
TEST_F(RuntimeTest, AReferenceLookedAtIsItsResourceAsItsPageBegins) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "cleared", "halved", "shown" };
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "cleared";
    graph.Add("clear", "Clear").Wire("image", "target").Set("color", Value::List { 1, 0, 0, 1 }).pass = "cleared";
    graph.Add("in halved", "Reference").Set("node", "target").pass = "halved";
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain")
        .Wire("param.target", "in halved").Set("constant.level", 0.5).pass = "halved";
    graph.Add("halve", "Dispatch").Wire("pipeline", "pipeline").Set("cover", "target").pass = "halved";
    graph.Add("in shown", "Reference").Set("node", "target").pass = "shown";
    graph.Add("screen", "Screen").pass = "shown";
    graph.Add("show", "Blit").Wire("source", "in shown").Wire("destination", "screen").pass = "shown";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    for (const char* id : { "in halved", "in shown", "target" }) runtime.Watch(id);
    const auto red = [&](const char* id) -> int {
        for (int i = 0; i < 6; ++i) {
            if (const auto peek = runtime.Watched(id); peek && !peek->bytes.empty()) return static_cast<int>(peek->bytes[0]);
            Frame();
        }
        ADD_FAILURE() << id << " was never copied: " << runtime.WatchProblem(id);
        return -1;
    };
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NEAR(red("in halved"), 255, 1) << "as the page that halves it begins: cleared, not halved yet";
    EXPECT_NEAR(red("in shown"), 128, 1) << "after the page before it halved it";
    EXPECT_NEAR(red("target"), 128, 1) << "the node itself: as the frame ends";
    EXPECT_NEAR(scene->Pixel(4, 4).x, 128, 1) << "looking at it changes nothing of what is drawn";

    // Cleared on a later page than it is halved on: it is halved, then cleared, as the pages are in order.
    graph.Find("clear")->pass = "shown";
    graph.Find("clear")->Set("color", Value::List { 0, 1, 0, 1 });
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(scene->Pixel(4, 4).y, 255) << "the clear of the last page is what is shown";
}

// A fixed page's passes record on a frame with a fixed step in it and rest on one without: a picture cleared in setup and
// halved on the fixed step is halved once for each frame given a step, and not at all on the others. Setup runs again
// when asked, as when the scene starts or stops playing.
TEST_F(RuntimeTest, AFixedPassRunsOnFramesWithAStepAndSetupRunsAgainWhenAsked) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "made", "simulate", "frame" };
    graph.setup = { "made" };
    graph.fixed = { "simulate" };
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "made";
    graph.Add("clear", "Clear").Wire("image", "target").Set("color", Value::List { 1, 0, 0, 1 }).pass = "made";
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain")
        .Wire("param.target", "target").Set("constant.level", 0.5).pass = "simulate";
    graph.Add("halve", "Dispatch").Wire("pipeline", "pipeline").Set("cover", "target").pass = "simulate";
    graph.Add("step", "FixedStep").pass = "simulate";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "target").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(runtime.FixedSteps(), 0u);
    EXPECT_NEAR(scene->Pixel(4, 4).x, 255, 1) << "no step taken: cleared, and never halved";

    for (int i = 0; i < 3; ++i) {
        runtime.FixedStep(1.f / 50.f);
        Frame();
        EXPECT_EQ(runtime.FixedSteps(), 1u);
    }
    for (int i = 0; i < 3; ++i) Frame();     // and frames with no step: it rests
    EXPECT_NEAR(scene->Pixel(4, 4).x, 32, 2) << "halved once for each frame with a step, three times";
    EXPECT_EQ(runtime.ValueOf("step", "count"), Value(3)) << "the steps there have been";
    EXPECT_NEAR(*runtime.ValueOf("step", "dt").Number(), 0.02, 1e-6);
    EXPECT_EQ(runtime.ValueOf("step", "steps"), Value(0)) << "this frame has none";

    runtime.RunSetupAgain();
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 255, 1) << "setup made again: cleared";

    // Saved, each rate's pages are apart: setup, the fixed step, every frame — and come in that order.
    kgraph::Graph loaded;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(graph.ToJson(), loaded, error)) << error;
    EXPECT_EQ(loaded.fixed, graph.fixed);
    EXPECT_EQ(loaded.passes, graph.passes);
    EXPECT_NE(graph.ToJson().find("\"fixed\": [\"simulate\"]"), std::string::npos) << graph.ToJson();
}

// A Time node is how long the graph has run, frame by frame, whether a fixed step is taken or not: it goes up by each
// frame's dt, and a frame held up counts a quarter of a second at most.
TEST_F(RuntimeTest, TimeGoesOnFrameByFrame) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "frame" };
    graph.Add("time", "Time").pass = "frame";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("clear", "Clear").Wire("image", "screen").Set("color", Value::List { 0, 0, 0, 1 }).pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    Frame();
    const double began = *runtime.ValueOf("time", "seconds").Number();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Frame();
    const double dt = *runtime.ValueOf("time", "dt").Number();
    EXPECT_GE(dt, 0.05) << "the frame took the time slept";
    EXPECT_NEAR(*runtime.ValueOf("time", "seconds").Number(), began + dt, 1e-9);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    Frame();
    EXPECT_DOUBLE_EQ(*runtime.ValueOf("time", "dt").Number(), 0.25) << "a frame held up counts a quarter of a second";
    EXPECT_EQ(runtime.FixedSteps(), 0u) << "and no step was taken for any of it";
}

// A Keep on a page of the fixed step is the state as it was when the step began: what that step reads as before, while it
// writes the state itself. Cleared to 255 and halved each step, the state is 32 after three steps, and the kept copy 64.
TEST_F(RuntimeTest, AKeepIsTheStateAsItWasWhenTheStepBegan) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "made", "simulate", "frame" };
    graph.setup = { "made" };
    graph.fixed = { "simulate" };
    graph.Add("state", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "made";
    graph.Add("clear", "Clear").Wire("image", "state").Set("color", Value::List { 1, 0, 0, 1 }).pass = "made";
    graph.Add("before", "Keep").Wire("of", "state").pass = "simulate";
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain")
        .Wire("param.target", "state").Set("constant.level", 0.5).pass = "simulate";
    graph.Add("halve", "Dispatch").Wire("pipeline", "pipeline").Set("cover", "target").pass = "simulate";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "before").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.ResourceOf("before")->kind, "Image");
    EXPECT_TRUE(runtime.Status("halve")->note.empty()) << "its state is one copy: " << runtime.Status("halve")->note;

    for (int i = 0; i < 3; ++i) Frame();
    for (int i = 0; i < 3; ++i) {
        runtime.FixedStep(1.f / 60.f);
        Frame();
    }
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 64, 2) << "kept before the third step halved it: 255 / 4";

    // A per-frame state on the fixed step is said to be wrong: each frame in flight would step its own copy.
    graph.Find("state")->Set("isPerFrame", true);
    runtime.Set(graph);
    ASSERT_TRUE(runtime.Status("halve") && runtime.Status("halve")->ok) << runtime.Status("halve")->error;
    EXPECT_NE(runtime.Status("halve")->note.find("per-frame"), std::string::npos) << runtime.Status("halve")->note;

    // What is not a buffer or an image is not kept.
    graph.Find("before")->Wire("of", "pipeline");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("before")->error.find("a buffer or an image"), std::string::npos) << runtime.Status("before")->error;
}

// A frame with several fixed steps runs its fixed pages once for each, each step after the last: three steps in one frame
// halve the state three times, and a Keep at the start of the step is the state the third step began with. A frame runs
// as many as the graph says at most, and the rest are lost.
TEST_F(RuntimeTest, AFrameWithSeveralFixedStepsRunsTheFixedPagesOnceForEach) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "made", "simulate", "frame" };
    graph.setup = { "made" };
    graph.fixed = { "simulate" };
    graph.Add("state", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "made";
    graph.Add("clear", "Clear").Wire("image", "state").Set("color", Value::List { 1, 1, 0, 1 }).pass = "made";
    graph.Add("before", "Keep").Wire("of", "state").pass = "simulate";
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain")
        .Wire("param.target", "state").Set("constant.level", 0.5).pass = "simulate";
    graph.Add("halve", "Dispatch").Wire("pipeline", "pipeline").Set("cover", "target").pass = "simulate";
    graph.Add("step", "FixedStep").pass = "simulate";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "before").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    for (int i = 0; i < 3; ++i) Frame();

    for (int i = 0; i < 3; ++i) runtime.FixedStep(1.f / 60.f);
    Frame();
    EXPECT_EQ(runtime.FixedSteps(), 3u);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 64, 2) << "the third step began from 255 halved twice: the steps ran in turn, in one frame";
    EXPECT_EQ(runtime.ValueOf("step", "count"), Value(3));

    // At most two a frame: of five, three are lost.
    graph.fixedStepsAtMost = 2;
    runtime.Set(graph);
    runtime.RunSetupAgain();
    for (int i = 0; i < 3; ++i) Frame();
    for (int i = 0; i < 5; ++i) runtime.FixedStep(1.f / 60.f);
    Frame();
    EXPECT_EQ(runtime.FixedSteps(), 2u);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 128, 2) << "the second step began from 255 halved once";
    EXPECT_EQ(runtime.ValueOf("step", "lost"), Value(3));
    kgraph::Graph loaded;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(graph.ToJson(), loaded, error)) << error;
    EXPECT_EQ(loaded.fixedStepsAtMost, 2);
}

// A Dispatch's `idle`: while what it is given is not zero, it records nothing. Given a number, always or never; wired
// from what is worked out each frame — here, whether the frame has no fixed step — on the frames it says.
TEST_F(RuntimeTest, AnIdleDispatchRecordsNothing) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "made", "frame" };
    graph.setup = { "made" };
    graph.Add("state", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "made";
    graph.Add("clear", "Clear").Wire("image", "state").Set("color", Value::List { 1, 0, 0, 1 }).pass = "made";
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain")
        .Wire("param.target", "state").Set("constant.level", 0.5).pass = "frame";
    graph.Add("halve", "Dispatch").Wire("pipeline", "pipeline").Set("cover", "target").Set("idle", 1).pass = "frame";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "state").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 255, 1) << "idle: never halved";

    graph.Add("step", "FixedStep").pass = "frame";
    graph.Add("none", "LessOrEqual").Wire("a", "step", "steps").Set("b", 0).pass = "frame";
    graph.Find("halve")->Wire("idle", "none");
    runtime.Set(graph);
    runtime.RunSetupAgain();
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 255, 1) << "no step: idle";
    for (int i = 0; i < 2; ++i) { runtime.FixedStep(1.f / 60.f); Frame(); }
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 64, 2) << "halved on the two frames with a step";
}

// A pass nothing that runs reads rests by itself: what it makes is left as it was, and it runs again once a pass that
// runs, or a watch, reads it. Here what is halved is read only by a copy that is idle; looked at, or the copy no longer
// idle, it is halved again.
TEST_F(RuntimeTest, APassWhoseEveryReaderRestsRestsToo) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("inner", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eSampled", "eTransferSrc", "eTransferDst" });
    graph.Add("outer", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 }).Set("usage", Value::List { "eStorage", "eTransferSrc" });
    graph.Add("halving", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain").Wire("param.target", "inner");
    graph.Add("halve", "Dispatch").Wire("pipeline", "halving").Set("cover", "target");
    graph.Add("copying", "ComputePipeline").Set("compute.module", "copy").Set("compute.entry", "computeMain")
        .Wire("param.source", "inner").Wire("param.target", "outer");
    graph.Add("copy", "Dispatch").Wire("pipeline", "copying").Set("cover", "target").Set("idle", 1);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "outer").Wire("destination", "screen");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto resting = [&](const std::string& id) {
        const auto frame = runtime.Frame();
        return std::ranges::any_of(frame.leftOut, [&](const auto& left) { return left.first == id && left.second.starts_with("resting"); });
    };
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_TRUE(resting("halve")) << "only an idle pass reads what it makes";
    EXPECT_FALSE(resting("copy")) << "idle as it says, not for want of a reader";
    EXPECT_FALSE(resting("show")) << "it writes the screen";

    runtime.Watch("inner");
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(resting("halve")) << "looked at, what it makes is read";
    runtime.Unwatch("inner");
    for (int i = 0; i < 2; ++i) Frame();
    EXPECT_TRUE(resting("halve"));

    graph.Find("copy")->Set("idle", 0);
    runtime.Set(graph);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(resting("halve")) << "the copy reads it again";
}

// A mesh's buffers, as buffers: a compute shader can read its positions and its indices — a cube's, here, copied out.
TEST_F(RuntimeTest, AMeshsBuffersAreBuffersAShaderCanRead) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("cube", "Primitive").Set("shape", "eCube");
    graph.Add("parts", "MeshBuffers").Wire("mesh", "cube");
    graph.Add("copy", "Buffer").Set("instanceCount", 4096).Set("usage", Value::List { "eStorage", "eTransferDst", "eTransferSrc" });
    graph.Add("out", "CopyBuffer").Wire("source", "parts", "vertices").Wire("destination", "copy");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ResourceOf("parts", "vertices")->kind, "Buffer");
    EXPECT_EQ(runtime.ResourceOf("parts", "indices")->kind, "Buffer");
    EXPECT_EQ(runtime.ValueOf("parts", "vertexCount"), Value(24));
    EXPECT_EQ(runtime.ValueOf("parts", "indexCount"), Value(36));
    EXPECT_EQ(runtime.ValueOf("parts", "stride"), Value(48)) << "position, normal, a tangent of four and a UV";
    EXPECT_EQ(runtime.ValueOf("parts", "position"), Value(0));
    runtime.Watch("copy");
    for (int i = 0; i < 4; ++i) Frame();
    const auto peek = runtime.Watched("copy");
    ASSERT_TRUE(peek && peek->bytes.size() >= 12);
    float first[3];
    std::memcpy(first, peek->bytes.data(), sizeof first);
    const auto cube = kgraph::ShapeData("cube");
    for (int axis = 0; axis < 3; ++axis) EXPECT_FLOAT_EQ(first[axis], cube->vertices[0].position[axis]) << axis;

    graph.Find("parts")->Wire("mesh", "copy");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("parts")->error.find("needs a mesh"), std::string::npos) << runtime.Status("parts")->error;
}

namespace {
    std::uint32_t SeedHash(std::uint32_t i) { i *= 2654435761u; i ^= i >> 15; i *= 2246822519u; i ^= i >> 13; return i; }
}

// The engine's functions, on the GPU: a radix sort of 70,000 keys with their values — stable, so that the same keys keep
// their values in order — what is kept of them by a flag, one after another, and how many groups cover a count the GPU
// worked out.
TEST_F(RuntimeTest, TheEnginesFunctionsSortCompactAndCountOnTheGpu) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    constexpr std::uint32_t n = 70000;
    kgraph::Graph graph;
    for (const char* name : { "Scan", "RadixSort", "Compact", "IndirectArgs" }) graph.functions[name] = support::EngineFunction(name);
    const Value storage = Value::List { "eStorage", "eTransferSrc", "eTransferDst" };
    for (const char* name : { "keys", "values", "flags" }) graph.Add(name, "Buffer").Set("instanceCount", n * 4).Set("usage", storage);
    graph.Add("counter", "Buffer").Set("instanceCount", 16).Set("usage", storage);
    graph.Add("seeding", "ComputePipeline").Set("compute.module", "seed").Set("compute.entry", "computeMain")
        .Wire("param.keys", "keys").Wire("param.values", "values").Wire("param.flags", "flags").Wire("param.counter", "counter");
    graph.Add("seed", "Dispatch").Wire("pipeline", "seeding").Set("groups.x", (n + 255) / 256).Set("push.count", n);
    graph.Add("kept", "Function").Set("function", "Compact").Wire("flags", "flags").Wire("values", "values").Set("count", n);
    graph.Add("sort", "Function").Set("function", "RadixSort").Wire("keys", "keys").Wire("values", "values").Set("count", n);
    graph.Add("groups", "Function").Set("function", "IndirectArgs").Wire("counter", "counter").Set("groupSize", 64);
    for (const char* name : { "keys", "values", "kept", "many", "args" }) graph.Add(std::string("out ") + name, "Buffer").Set("instanceCount", n * 4).Set("usage", storage);
    graph.Add("copy keys", "CopyBuffer").Wire("source", "sort", "keys").Wire("destination", "out keys");
    graph.Add("copy values", "CopyBuffer").Wire("source", "sort", "values").Wire("destination", "out values");
    graph.Add("copy kept", "CopyBuffer").Wire("source", "kept", "kept").Wire("destination", "out kept");
    graph.Add("copy many", "CopyBuffer").Wire("source", "kept", "count").Wire("destination", "out many");
    graph.Add("copy args", "CopyBuffer").Wire("source", "groups", "args").Wire("destination", "out args");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    for (const char* name : { "out keys", "out values", "out kept", "out many", "out args" }) runtime.Watch(name);
    for (int i = 0; i < 5; ++i) Frame();
    const auto words = [&](const char* name) {
        const auto peek = runtime.Watched(name);
        std::vector<std::uint32_t> out(peek ? peek->bytes.size() / 4 : 0);
        if (peek) std::memcpy(out.data(), peek->bytes.data(), out.size() * 4);
        return out;
    };
    const auto keys = words("out keys"), values = words("out values"), kept = words("out kept"), many = words("out many"), args = words("out args");
    ASSERT_GE(keys.size(), n);
    ASSERT_GE(values.size(), n);
    std::size_t unsorted = 0, unstable = 0, wrong = 0;
    for (std::uint32_t k = 0; k < n; ++k) {
        if (k > 0 && keys[k - 1] > keys[k]) ++unsorted;
        if (k > 0 && keys[k - 1] == keys[k] && values[k - 1] > values[k]) ++unstable;
        if (values[k] >= n || (SeedHash(values[k]) & 0xFFFFF) != keys[k]) ++wrong;
    }
    EXPECT_EQ(unsorted, 0u) << "sorted";
    EXPECT_EQ(unstable, 0u) << "the same keys keep their values in order";
    EXPECT_EQ(wrong, 0u) << "each key with its own value";

    // Kept: the indices of the odd keys, in order, and how many.
    std::vector<std::uint32_t> odd;
    for (std::uint32_t i = 0; i < n; ++i) if (SeedHash(i) & 1) odd.push_back(i);
    ASSERT_FALSE(many.empty());
    EXPECT_EQ(many[0], odd.size());
    ASSERT_GE(kept.size(), odd.size());
    EXPECT_TRUE(std::equal(odd.begin(), odd.end(), kept.begin())) << "the flagged, one after another, in their order";

    ASSERT_GE(args.size(), 3u);
    EXPECT_EQ(args[0], (n + 63) / 64);
    EXPECT_EQ(args[1], 1u);
    EXPECT_EQ(args[2], 1u);
}

namespace {
    /** What a comparing sort is tried on: sortable.slang's seeds of @p n elements, in `structs` and `floats`, and copies to look at. */
    void AddSortables(kgraph::Graph& graph, const std::uint32_t n, const std::uint32_t reach)
    {
        using kgraph::Value;
        const Value storage = Value::List { "eStorage", "eTransferSrc", "eTransferDst" };
        graph.Add("structs", "Buffer").Set("instanceCount", n * 16).Set("usage", storage);
        graph.Add("floats", "Buffer").Set("instanceCount", n * 4).Set("usage", storage);
        graph.Add("seeding", "ComputePipeline").Set("compute.module", "sortable").Set("compute.entry", "computeMain").Wire("param.structs", "structs").Wire("param.floats", "floats");
        graph.Add("seed", "Dispatch").Wire("pipeline", "seeding").Set("groups.x", (n + 255) / 256).Set("push.count", n).Set("push.reach", reach);
        graph.Add("out structs", "Buffer").Set("instanceCount", n * 16).Set("usage", storage);
        graph.Add("out floats", "Buffer").Set("instanceCount", n * 4).Set("usage", storage);
    }
    struct Sortable { std::uint32_t key, index; float rest[2]; };
    std::uint32_t SortableKey(const std::uint32_t i, const std::uint32_t reach) { return SeedHash(i) % reach; }
}

// A bitonic sort of any type that is IComparable, on the GPU: of a shader's struct, at a count that is no power of two
// and in the passes that count takes, and of a float, in the same graph.
TEST_F(RuntimeTest, ABitonicSortSortsWhatIsComparable) {
    auto& runtime = *scene->runtime;
    constexpr std::uint32_t n = 70000, reach = 1u << 20;
    kgraph::Graph graph;
    graph.functions["BitonicSort"] = support::EngineFunction("BitonicSort");
    AddSortables(graph, n, reach);
    graph.Add("sort", "Function").Set("function", "BitonicSort").Set("type", "sortable.Sortable").Wire("elements", "structs").Set("count", n).Set("passes", 153);
    graph.Add("sort floats", "Function").Set("function", "BitonicSort").Set("type", "float").Wire("elements", "floats").Set("count", n);
    graph.Add("copy structs", "CopyBuffer").Wire("source", "sort", "elements").Wire("destination", "out structs");
    graph.Add("copy floats", "CopyBuffer").Wire("source", "sort floats", "elements").Wire("destination", "out floats");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    runtime.Watch("out structs");
    runtime.Watch("out floats");
    for (int i = 0; i < 5; ++i) Frame();
    const auto peek = runtime.Watched("out structs");
    ASSERT_TRUE(peek && peek->bytes.size() >= n * sizeof(Sortable));
    std::vector<Sortable> sorted(n);
    std::memcpy(sorted.data(), peek->bytes.data(), n * sizeof(Sortable));
    std::vector<bool> seen(n, false);
    std::size_t unsorted = 0, wrong = 0;
    for (std::uint32_t k = 0; k < n; ++k) {
        if (k > 0 && sorted[k - 1].key > sorted[k].key) ++unsorted;
        const auto& made = sorted[k];
        if (made.index >= n || seen[made.index] || made.key != SortableKey(made.index, reach) || made.rest[0] != static_cast<float>(made.index) || made.rest[1] != 1.f) ++wrong;
        else seen[made.index] = true;
    }
    EXPECT_EQ(unsorted, 0u) << "sorted";
    EXPECT_EQ(wrong, 0u) << "each element whole, and each once";

    const auto floats = runtime.Watched("out floats");
    ASSERT_TRUE(floats && floats->bytes.size() >= n * 4);
    std::vector<float> numbers(n);
    std::memcpy(numbers.data(), floats->bytes.data(), n * 4);
    EXPECT_TRUE(std::ranges::is_sorted(numbers)) << "a shader's own float is comparable";
    EXPECT_LT(numbers.front(), numbers.back());
}

// An odd-even transposition sort: whole in as many phases as there are elements, and stable; and nearer sorted with fewer.
TEST_F(RuntimeTest, AnOddEvenSortSortsStablyAndALittleAtATime) {
    auto& runtime = *scene->runtime;
    constexpr std::uint32_t n = 200, reach = 8;
    const auto sortedAfter = [&](const int phases, const int start = 0) {
        kgraph::Graph graph;
        graph.functions["OddEvenSort"] = support::EngineFunction("OddEvenSort");
        AddSortables(graph, n, reach);
        graph.Add("sort", "Function").Set("function", "OddEvenSort").Set("type", "sortable.Sortable").Wire("elements", "structs").Set("count", n).Set("phases", phases).Set("start", start);
        graph.Add("copy structs", "CopyBuffer").Wire("source", "sort", "elements").Wire("destination", "out structs");
        runtime.Set(graph);
        for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
        runtime.Watch("out structs");
        for (int i = 0; i < 5; ++i) Frame();
        const auto peek = runtime.Watched("out structs");
        std::vector<Sortable> sorted(n);
        if (peek && peek->bytes.size() >= n * sizeof(Sortable)) std::memcpy(sorted.data(), peek->bytes.data(), n * sizeof(Sortable));
        else ADD_FAILURE() << "nothing to look at";
        return sorted;
    };
    // How many pairs are the wrong way round: what each phase takes from, and never adds to.
    const auto outOfOrder = [](const std::vector<Sortable>& made) {
        std::size_t count = 0;
        for (std::size_t a = 0; a < made.size(); ++a)
            for (std::size_t b = a + 1; b < made.size(); ++b) count += made[a].key > made[b].key ? 1 : 0;
        return count;
    };
    const auto whole = sortedAfter(n);
    EXPECT_EQ(outOfOrder(whole), 0u) << "sorted";
    std::size_t unstable = 0;
    for (std::size_t k = 1; k < n; ++k) if (whole[k - 1].key == whole[k].key && whole[k - 1].index > whole[k].index) ++unstable;
    EXPECT_EQ(unstable, 0u) << "the same keys keep their order";

    std::vector<Sortable> seeds(n);
    for (std::uint32_t i = 0; i < n; ++i) seeds[i].key = SortableKey(i, reach);
    EXPECT_LT(outOfOrder(sortedAfter(6)), outOfOrder(seeds)) << "a few phases leave it nearer sorted";
    EXPECT_EQ(sortedAfter(1, 1)[0].index, 0u) << "started on the odd ones, the first element meets nothing";
}

// What cannot be sorted is said on the call, by what it is.
TEST_F(RuntimeTest, ASortSaysWhatItCannotSort) {
    auto& runtime = *scene->runtime;
    const auto failure = [&](const std::string& type) {
        kgraph::Graph graph;
        graph.functions["BitonicSort"] = support::EngineFunction("BitonicSort");
        graph.Add("elements", "Buffer").Set("instanceCount", 1024).Set("usage", kgraph::Value::List { "eStorage", "eTransferDst" });
        graph.Add("sort", "Function").Set("function", "BitonicSort").Set("type", type).Wire("elements", "elements").Set("count", 16);
        runtime.Set(graph);
        return runtime.Status("sort") ? runtime.Status("sort")->error : std::string();
    };
    const std::string unsortable = failure("sortable.Unsortable");
    EXPECT_NE(unsortable.find("sortable.Unsortable"), std::string::npos) << unsortable;
    EXPECT_NE(unsortable.find("IComparable"), std::string::npos) << unsortable;
    const std::string missing = failure("nowhere.Thing");
    EXPECT_NE(missing.find("nowhere"), std::string::npos) << missing;
    const std::string malformed = failure("a b; evil");
    EXPECT_NE(malformed.find("is no type"), std::string::npos) << malformed;
}

// A Reference on a setup page is what setup made before its page, though setup is made only now and then.
TEST_F(RuntimeTest, AReferenceOnASetupPageIsWhatSetupMadeBeforeIt) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "cleared", "then", "frame" };
    graph.setup = { "cleared", "then" };
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "cleared";
    graph.Add("clear", "Clear").Wire("image", "target").Set("color", Value::List { 1, 0, 0, 1 }).pass = "cleared";
    graph.Add("in then", "Reference").Set("node", "target").pass = "then";
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain")
        .Wire("param.target", "target").Set("constant.level", 0.5).pass = "frame";
    graph.Add("halve", "Dispatch").Wire("pipeline", "pipeline").Set("cover", "target").pass = "frame";
    runtime.Set(graph);
    ASSERT_TRUE(runtime.Ok());
    for (int i = 0; i < 4; ++i) Frame();     // setup made, and the picture halved since
    runtime.Watch("in then");
    for (int i = 0; i < 4; ++i) Frame();
    const auto peek = runtime.Watched("in then");
    ASSERT_TRUE(peek && !peek->bytes.empty()) << runtime.WatchProblem("in then");
    EXPECT_NEAR(static_cast<int>(peek->bytes[0]), 255, 1) << "as setup left it before 'then', not as the frames since left it";
}

// A script makes resources and fills them on the CPU: a Script node names it, gives it its fields, and gives what it
// makes — an image, a buffer, a view of the image, a sampler — each an output by its name, made as its source says and
// as the node says otherwise. Until something runs the script, they are made empty, and the node says why.
TEST_F(RuntimeTest, AScriptMakesResourcesAndFillsThem) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    // As the source declares it: a level to fill with, what it makes, and nothing to run it yet.
    kgraph::ResourceScript script;
    script.name = "Flat";
    script.fields = { { .name = "level", .type = "f32", .offset = 0, .size = 4 }, { .name = "tint", .type = "vec3", .offset = 4, .size = 12 } };
    script.initial.assign(16, std::byte { 0 });
    const float half = 0.5f;
    std::memcpy(script.initial.data(), &half, 4);
    script.resources = {
        { "picture", "Image", { { "format", "eRGBA8_UNORM" }, { "extent", Value::List { 32, 32 } }, { "usage", Value::List { "eSampled", "eTransferSrc", "eTransferDst" } } } },
        { "seen", "ImageView", { { "image", "picture" } } },
        { "table", "Buffer", { { "instanceCount", 8 }, { "usage", Value::List { "eStorage", "eTransferSrc" } } } },
        { "smooth", "Sampler", {} },
    };
    runtime.SetResourceScript(script);

    kgraph::Graph graph;
    graph.Add("flat", "Script").Set("script", "Flat").Set("field.tint", Value::List { 0, 1, 0 });
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "flat", "picture").Wire("destination", "screen");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Status("flat")->ok) << runtime.Status("flat")->error;
    EXPECT_NE(runtime.Status("flat")->note.find("Flat fills what it makes when the project runs with its scripts"), std::string::npos) << runtime.Status("flat")->note;
    std::vector<std::string> outputs;
    for (const auto& output : runtime.OutputsOf("flat"))
        if (output.kind == kgraph::PortKind::eResource) outputs.push_back(output.name + ":" + output.type);
    EXPECT_EQ(outputs, (std::vector<std::string> { "picture:Image", "seen:ImageView", "table:Buffer", "smooth:Sampler" })) << "what it makes, by name";
    ASSERT_TRUE(runtime.ImageOf("flat", "picture").Get());
    EXPECT_EQ(runtime.ImageOf("flat", "picture")->Extent().x, 32u) << "made as its source says";
    ASSERT_NE(runtime.ResourceOf("flat", "seen"), nullptr);
    EXPECT_EQ(runtime.ResourceOf("flat", "seen")->source, runtime.ResourceOf("flat", "picture")) << "a view of the script's image";
    const auto ports = runtime.PortsOf("flat");
    EXPECT_TRUE(std::ranges::contains(ports, std::string("field.level"), &kgraph::Port::name)) << "its fields are the node's inputs";
    const auto wide = std::ranges::find(ports, std::string("picture.extent.x"), &kgraph::Port::name);
    ASSERT_NE(wide, ports.end()) << "and what each resource is made with";
    ASSERT_TRUE(wide->initial.Number());
    EXPECT_EQ(*wide->initial.Number(), 32.0) << "starting as its source says";

    // Run: it is given what it makes as it was made, and its settings, and fills them.
    std::vector<kgraph::Contents> seen;
    std::vector<float> given;
    script.fill = [&](const std::span<kgraph::Contents> contents, const std::span<const std::byte> settings) -> std::string {
        seen.assign(contents.begin(), contents.end());
        given.assign(4, 0.f);
        std::memcpy(given.data(), settings.data(), 16);
        for (auto& each : contents) {
            if (each.kind == "Image")
                for (std::size_t texel = 0; texel < each.bytes.size() / 4; ++texel)
                    for (int c = 0; c < 4; ++c)
                        each.bytes[texel * 4 + c] = static_cast<std::byte>(static_cast<int>(255.f * (c == 3 ? 1.f : given[0] * given[1 + c])));
            else
                for (std::size_t i = 0; i < each.bytes.size(); ++i) each.bytes[i] = static_cast<std::byte>(i);
        }
        return {};
    };
    const auto builds = runtime.Status("flat")->builds;
    runtime.SetResourceScript(script);
    Frame();
    Frame();
    EXPECT_GT(runtime.Status("flat")->builds, builds) << "made again, now that its script runs";
    EXPECT_TRUE(runtime.Status("flat")->note.empty());
    ASSERT_EQ(seen.size(), 2u) << "its image and its buffer: a view and a sampler hold nothing to fill";
    EXPECT_EQ(seen[0].name, "picture");
    EXPECT_EQ(seen[0].format, "eRGBA8_UNORM");
    EXPECT_EQ(seen[0].extent.x, 32u);
    EXPECT_EQ(seen[0].texelBytes, 4u);
    EXPECT_EQ(seen[1].name, "table");
    EXPECT_EQ(seen[1].kind, "Buffer");
    EXPECT_EQ(given, (std::vector<float> { 0.5f, 0.f, 1.f, 0.f })) << "what the source says for the level, and what the node gives for the tint";
    Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).y, 127, 1) << "the image is what the script filled it with";
    EXPECT_EQ(scene->Pixel(4, 4).x, 0);
    EXPECT_EQ(runtime.ResourceOf("flat", "seen")->source, runtime.ResourceOf("flat", "picture")) << "a view of the image as it was filled";

    // Looked at, it is its first image — it has no `out` — or another of what it makes, as asked.
    EXPECT_EQ(runtime.ShownOutput("flat"), "picture");
    EXPECT_EQ(runtime.ImageOf("flat").Get(), runtime.ImageOf("flat", "picture").Get()) << "what it is looked at by, when no output is said";
    runtime.Watch("flat");
    Frame();
    Frame();
    Frame();
    auto peek = runtime.Watched("flat");
    ASSERT_TRUE(peek && peek->image) << runtime.WatchProblem("flat");
    EXPECT_EQ(peek->extent.x, 32u);
    runtime.WatchOutput("flat", "table");
    EXPECT_EQ(runtime.ShownOutput("flat"), "table");
    Frame();
    Frame();
    Frame();
    peek = runtime.Watched("flat");
    ASSERT_TRUE(peek && !peek->image) << runtime.WatchProblem("flat");
    EXPECT_EQ(peek->bytes.size(), 8u) << "its buffer of eight bytes";
    EXPECT_EQ(static_cast<int>(peek->bytes[5]), 5) << "as the script filled it";
    runtime.Unwatch("flat");

    // A field, and what a resource is made with, given on the node: made and filled again.
    graph.Find("flat")->Set("field.level", 1.0).Set("picture.extent.x", 16);
    runtime.Set(graph);
    Frame();
    EXPECT_EQ(given[0], 1.f);
    EXPECT_EQ(seen[0].extent.x, 16u);
    EXPECT_EQ(seen[0].extent.y, 32u) << "the rest as its source says";
    EXPECT_EQ(scene->Pixel(4, 4).y, 255);

    // What is wrong is said: a field it has not, a resource it does not make, one made of nothing it has, a script that fails.
    graph.Find("flat")->Set("field.speed", 2.0);
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("flat")->error.find("Flat has no field 'speed': it has level, tint"), std::string::npos) << runtime.Status("flat")->error;
    graph.Find("flat")->inputs.erase("field.speed");
    graph.Find("flat")->Set("sky.format", "eRGBA8_UNORM");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("flat")->error.find("Flat makes no 'sky': it makes picture, seen, table, smooth"), std::string::npos) << runtime.Status("flat")->error;
    graph.Find("flat")->inputs.erase("sky.format");
    runtime.Set(graph);
    auto wrong = script;
    wrong.resources[1].settings = { { "image", "nothing" } };
    runtime.SetResourceScript(wrong);
    Frame();
    EXPECT_NE(runtime.Status("flat")->error.find("Flat.seen's 'image' names 'nothing'"), std::string::npos) << runtime.Status("flat")->error;
    script.fill = [](std::span<kgraph::Contents>, std::span<const std::byte>) -> std::string { return "it is out of numbers"; };
    runtime.SetResourceScript(script);
    Frame();
    EXPECT_NE(runtime.Status("flat")->error.find("Flat: it is out of numbers"), std::string::npos) << runtime.Status("flat")->error;
    runtime.RemoveResourceScript("Flat");
    Frame();
    EXPECT_NE(runtime.Status("flat")->error.find("there is no ResourceScript 'Flat'"), std::string::npos) << runtime.Status("flat")->error;
}



// A Dispatch set `async` runs on the async compute queue, ordered against the graphics passes by what it reads and
// writes, and what it makes is the same; a device with no second queue runs it in order, and its node says so.
TEST_F(RuntimeTest, ADispatchSetAsyncRunsOnTheAsyncComputeQueue) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { Size, Size })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain").Wire("param.target", "target").Set("constant.level", 0.5);
    graph.Add("clear", "Clear").Wire("image", "target").Set("color", Value::List { 1, 0, 0, 1 });
    graph.Add("run", "Dispatch").Wire("pipeline", "pipeline").Set("groups", Value::List { Size / 8, Size / 8, 1 }).Set("async", true);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "target").Wire("destination", "screen");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    Frame();
    Frame();
    Frame();

    const auto& schedule = scene->Graph().Schedule();
    const auto run = std::ranges::find(schedule, std::string("run"), &kor::FrameGraph::Scheduled::name);
    ASSERT_NE(run, schedule.end());
    EXPECT_EQ(run->async, kor::Context::SupportsAsyncCompute());
    if (!kor::Context::SupportsAsyncCompute()) EXPECT_NE(runtime.Status("run")->note.find("no second queue"), std::string::npos);
    const auto show = std::ranges::find(schedule, std::string("show"), &kor::FrameGraph::Scheduled::name);
    ASSERT_NE(show, schedule.end());
    EXPECT_FALSE(show->async) << "a pass that uses the screen stays on the graphics queue";
    const kor::U8Vec4 pixel = scene->Pixel(5, 5);
    EXPECT_NEAR(pixel.x, 128, 1) << "the clear's red, halved by the shader on the other queue";

    // Set back, it runs on the graphics queue again.
    graph.Find("run")->Set("async", false);
    runtime.Set(graph);
    Frame();
    const auto again = std::ranges::find(scene->Graph().Schedule(), std::string("run"), &kor::FrameGraph::Scheduled::name);
    ASSERT_NE(again, scene->Graph().Schedule().end());
    EXPECT_FALSE(again->async);
}

// The frame as it runs: the graph's passes in the order they are scheduled, each with what it uses by the node that made
// it, and a pass left out said so.
TEST_F(RuntimeTest, TheFrameIsItsPassesInTheOrderTheyRun) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { Size, Size })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.Add("pipeline", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain").Wire("param.target", "target").Set("constant.level", 0.5);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "target").Wire("destination", "screen");     // listed first: the order is the frame's
    graph.Add("run", "Dispatch").Wire("pipeline", "pipeline").Set("groups", Value::List { Size / 8, Size / 8, 1 });
    graph.Add("clear", "Clear").Wire("image", "target").Set("color", Value::List { 1, 0, 0, 1 });
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();

    const auto frame = runtime.Frame();
    std::vector<std::string> order;
    for (const auto& pass : frame.passes) order.push_back(pass.node);
    EXPECT_EQ(order, (std::vector<std::string> { "clear", "run", "show" }));
    const auto run = std::ranges::find(frame.passes, std::string("run"), &kgraph::Runtime::FrameReport::Pass::node);
    ASSERT_NE(run, frame.passes.end());
    EXPECT_TRUE(std::ranges::contains(run->uses, std::pair<std::string, char> { "target", 'b' })) << "the image it reads and writes, by the node that made it";
    EXPECT_GT(run->level, frame.passes.front().level) << "after the clear it depends on";
    const auto show = std::ranges::find(frame.passes, std::string("show"), &kgraph::Runtime::FrameReport::Pass::node);
    ASSERT_NE(show, frame.passes.end());
    EXPECT_TRUE(std::ranges::contains(show->uses, std::pair<std::string, char> { "the screen", 'w' }));
}

// A random number is a node of its distribution, uniform from 0 to 1 when nothing else is said: the same for the same seed, of the
// kind its `type` says or — Auto — of what it is wired to; a whole kind's whole, a vector's components each by its own of a
// vector's inputs.
TEST_F(RuntimeTest, ARandomNumberIsANodeOfItsDistribution) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    const auto node = [&](const char* id, const char* type) -> kgraph::Node& { return graph.Add(id, type); };
    node("plain", "RandomUniform");
    node("again", "RandomUniform");
    node("other seed", "RandomUniform").Set("seed", 1);
    node("spread", "RandomUniform").Set("type", "Vec3").Set("min", Value::List { 0, 10, 20 }).Set("max", Value::List { 1, 11, 21 });
    node("dice", "RandomUniform").Set("type", "IVec4").Set("min", 1).Set("max", 6).Set("seed", 3);
    node("sure", "RandomNormal").Set("type", "UInt").Set("mean", 5).Set("stddev", 0);
    node("counted", "RandomPoisson").Set("type", "Double").Set("lambda", 3);
    node("coin", "RandomBernoulli").Set("type", "Int").Set("p", 1);
    node("narrow", "RandomNormal").Set("stddev", -1);
    node("too few", "RandomUniform").Set("type", "Vec3").Set("max", Value::List { 1, 2 });
    // Auto: what it is wired to takes — a UInt's value, and through an Add, which takes any kind, what that is wired to.
    node("whole", "RandomUniform").Set("min", 1).Set("max", 6);
    node("as uint", "UInt").Wire("value", "whole");
    node("through", "RandomUniform").Set("min", 10).Set("max", 20);
    node("added", "Add").Wire("a", "through").Set("b", 0);
    node("as int", "Int").Wire("value", "added");
    runtime.Set(graph);
    const auto number = [&](const char* id) { const Value value = runtime.ValueOf(id, "out"); const double* n = value.Number(); return n ? *n : -999.0; };
    const auto items = [&](const char* id) { const Value value = runtime.ValueOf(id, "out"); return value.Items() ? *value.Items() : Value::List {}; };

    for (const char* id : { "plain", "again", "other seed", "spread", "dice", "sure", "counted", "coin", "whole", "through" })
        EXPECT_TRUE(runtime.Status(id)->ok) << id << ": " << runtime.Status(id)->error;
    EXPECT_GE(number("plain"), 0.0);
    EXPECT_LT(number("plain"), 1.0);
    EXPECT_EQ(number("plain"), number("again")) << "the same seed";
    EXPECT_NE(number("plain"), number("other seed"));
    EXPECT_EQ(number("plain"), static_cast<double>(static_cast<float>(number("plain")))) << "wired to nothing, a Float";
    const auto spread = items("spread");
    ASSERT_EQ(spread.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_GE(*spread[i].Number(), 10.0 * static_cast<double>(i));
        EXPECT_LT(*spread[i].Number(), 10.0 * static_cast<double>(i) + 1.0);
    }
    const auto dice = items("dice");
    ASSERT_EQ(dice.size(), 4u);
    for (const Value& face : dice) {
        EXPECT_EQ(*face.Number(), std::trunc(*face.Number()));
        EXPECT_GE(*face.Number(), 1.0);
        EXPECT_LE(*face.Number(), 6.0);
    }
    EXPECT_EQ(number("sure"), 5.0);
    EXPECT_EQ(number("counted"), std::trunc(number("counted")));
    EXPECT_EQ(number("coin"), 1.0);
    EXPECT_FALSE(runtime.Status("narrow")->ok);
    EXPECT_NE(runtime.Status("narrow")->error.find("stddev"), std::string::npos) << runtime.Status("narrow")->error;
    EXPECT_FALSE(runtime.Status("too few")->ok);
    EXPECT_NE(runtime.Status("too few")->error.find("'max'"), std::string::npos) << runtime.Status("too few")->error;
    EXPECT_EQ(number("whole"), std::trunc(number("whole"))) << "a UInt's, so whole";
    EXPECT_GE(number("whole"), 1.0);
    EXPECT_LE(number("whole"), 6.0);
    EXPECT_EQ(number("through"), std::trunc(number("through"))) << "through the Add, an Int's";

    // Each distribution is a node of its own, with its own inputs and no others.
    const kgraph::NodeType* normal = kgraph::FindType("RandomNormal");
    ASSERT_NE(normal, nullptr);
    std::vector<std::string> names;
    for (const auto& port : normal->inputs) names.push_back(port.name);
    EXPECT_EQ(names, (std::vector<std::string> { "type", "seed", "mean", "stddev" }));
    for (const std::string_view distribution : kgraph::random::DistributionNames())
        EXPECT_NE(kgraph::FindType(kgraph::random::NodeOf(*kgraph::random::DistributionNamed(distribution))), nullptr) << distribution;
}

// A RandomDevice is a seed no one chose, kept while the graph runs — an edit elsewhere draws nothing new, nor changes the
// random number it seeds — and drawn again when its `draw` changes.
TEST_F(RuntimeTest, ARandomDeviceSeedIsKeptUntilItIsDrawnAgain) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("device", "RandomDevice");
    graph.Add("random", "RandomUniform").Wire("seed", "device").Set("type", "Double");
    graph.Add("elsewhere", "Float").Set("value", 1);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const double seed = *runtime.ValueOf("device", "out").Number();
    const double drawn = *runtime.ValueOf("random", "out").Number();
    EXPECT_EQ(seed, std::trunc(seed));
    EXPECT_GE(seed, 0.0);
    EXPECT_LE(seed, 4294967295.0);

    graph.nodes.back().Set("value", 2);
    runtime.Set(graph);
    EXPECT_EQ(*runtime.ValueOf("device", "out").Number(), seed) << "kept, whatever else is edited";
    EXPECT_EQ(*runtime.ValueOf("random", "out").Number(), drawn);

    // Drawn again, for each new `draw`: three draws of 2^32 seeds that are all the same seed would be one in 2^64.
    bool changed = false;
    for (int draw = 1; draw <= 3 && !changed; ++draw) {
        graph.nodes.front().Set("draw", draw);
        runtime.Set(graph);
        changed = *runtime.ValueOf("device", "out").Number() != seed;
    }
    EXPECT_TRUE(changed) << "a new draw, a new seed";
}

// Every value port says exactly what it takes: a value node's, a builder's argument, and what a pipeline's shaders declare.
// Conform makes a value that shape.
TEST(Shapes, EachValuePortSaysWhatItTakes) {
    using kgraph::Scalar;
    using kgraph::Shape;
    using kgraph::Value;
    const auto shape = [](const char* type, const char* port, const bool output = false) {
        const kgraph::NodeType* found = kgraph::FindType(type);
        if (!found) return Shape {};
        const auto& ports = output ? found->outputs : found->inputs;
        const auto it = std::ranges::find(ports, std::string_view(port), &kgraph::Port::name);
        return it == ports.end() ? Shape {} : it->shape;
    };
    EXPECT_EQ(shape("Float", "value"), (Shape { Scalar::eFloat, 1 }));
    EXPECT_EQ(shape("UInt", "value"), (Shape { Scalar::eUInt, 1 }));
    EXPECT_EQ(shape("IVec3", "y"), (Shape { Scalar::eInt, 1 }));
    EXPECT_EQ(shape("Vec4", "out", true), (Shape { Scalar::eFloat, 4 }));
    EXPECT_EQ(shape("Color", "color"), (Shape { Scalar::eFloat, 4 }));
    EXPECT_EQ(shape("Bool", "value"), (Shape { Scalar::eBool, 1 }));
    EXPECT_EQ(shape("Image", "arrayLayers"), (Shape { Scalar::eUInt, 1 })) << "a builder's argument";
    EXPECT_FALSE(shape("Add", "a").Known()) << "what takes any kind says none";

    EXPECT_EQ(kgraph::Conform(Value(2.6), { Scalar::eInt, 1 }), Value(3.0));
    EXPECT_EQ(kgraph::Conform(Value(-2.0), { Scalar::eUInt, 1 }), Value(0.0));
    EXPECT_EQ(kgraph::Conform(Value(0.1), { Scalar::eFloat, 1 }), Value(static_cast<double>(0.1f)));
    EXPECT_EQ(kgraph::Conform(Value(0.1), { Scalar::eDouble, 1 }), Value(0.1));
    EXPECT_EQ(kgraph::Conform(Value(1.0), { Scalar::eBool, 1 }), Value(true));
    EXPECT_EQ(kgraph::Conform(Value(2.0), { Scalar::eFloat, 3 }), (Value(Value::List { 2.0, 2.0, 2.0 }))) << "a number for every component";
    EXPECT_EQ(kgraph::Conform(Value(Value::List { 1.4, 2.6 }), { Scalar::eUInt, 3 }), (Value(Value::List { 1.0, 3.0, 0.0 })));
    EXPECT_EQ(kgraph::Conform(Value("text"), { Scalar::eFloat, 1 }), Value("text"));
    EXPECT_EQ(kgraph::ShapeNamed("UVec3"), (Shape { Scalar::eUInt, 3 }));
    EXPECT_EQ(kgraph::ShapeName({ Scalar::eInt, 2 }), "IVec2");
}

// Comparisons of vectors are a component at a time, and an Any or an All says them as one; a Select picks by each.
// Colours are said one way and another, turns are quaternions, and a Jitter is a place within the pixel each frame.
TEST_F(RuntimeTest, ColoursTurnsComparisonsAndJitterAreWorkedOut) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    const auto node = [&](const char* id, const char* type) -> kgraph::Node& { return graph.Add(id, type); };
    node("less", "Less").Set("a", Value::List { 1, 5 }).Set("b", 3);
    node("any", "Any").Wire("a", "less");
    node("all", "All").Wire("a", "less");
    node("pick", "Select").Wire("condition", "less").Set("a", Value::List { 10, 20 }).Set("b", Value::List { 30, 40 });
    node("linear", "ColorConvert").Set("color", Value::List { 0.5, 0.5, 0.5, 0.25 });
    node("gamma", "ColorConvert").Set("color", Value::List { 0.5, 0.5, 0.5 }).Set("from", "eGamma22");
    node("back", "ColorConvert").Wire("color", "linear").Set("from", "eLinear").Set("to", "eSrgb");
    node("red", "ColorConvert").Set("color", Value::List { 0, 1, 1 }).Set("from", "eHsv");
    node("hue", "ColorConvert").Set("color", Value::List { 0, 1, 0 }).Set("to", "eHsv");
    node("daylight", "ColorConvert").Set("color", 6500).Set("from", "eKelvin");
    node("quarter", "QuatFromAxisAngle").Set("axis", Value::List { 0, 2, 0 }).Set("angle", 90);
    node("euler", "QuatFromEuler").Set("rotation", Value::List { 0, 90, 0 });
    node("turned", "QuatRotate").Wire("q", "quarter").Set("vector", Value::List { 1, 0, 0 });
    node("half", "QuatMultiply").Wire("a", "quarter").Wire("b", "quarter");
    node("back turn", "QuatInverse").Wire("q", "quarter");
    node("eighth", "QuatSlerp").Set("a", Value::List { 0, 0, 0, 1 }).Wire("b", "quarter").Set("t", 0.5);
    node("as matrix", "QuatToMatrix").Wire("q", "quarter");
    node("rotated", "MatrixRotate").Set("rotation", Value::List { 0, 90, 0 });
    node("angles", "QuatToEuler").Wire("q", "quarter");
    node("jitter", "Jitter").Set("frame", 8).Set("size", Value::List { 100, 50 }).Set("projection", Value::List { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
    node("mesh shaders", "DeviceSupports").Set("feature", "eMeshShader");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto numbers = [&](const char* id, const char* output = "out") {
        std::vector<double> out;
        const Value value = runtime.ValueOf(id, output);
        if (const auto* items = value.Items()) for (const auto& item : *items) out.push_back(item.Number() ? *item.Number() : (item.Bool() ? (*item.Bool() ? 1.0 : 0.0) : -999.0));
        else if (const double* n = value.Number()) out.push_back(*n);
        return out;
    };
    const auto near = [](const std::vector<double>& a, const std::vector<double>& b, const double within = 1e-4) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) if (std::abs(a[i] - b[i]) > within) return false;
        return true;
    };
    EXPECT_EQ(runtime.ValueOf("less"), Value(Value::List { true, false })) << "a component at a time; a number goes with each";
    EXPECT_EQ(runtime.ValueOf("any"), Value(true));
    EXPECT_EQ(runtime.ValueOf("all"), Value(false));
    EXPECT_EQ(numbers("pick"), (std::vector<double> { 10, 40 }));
    EXPECT_TRUE(near(numbers("linear"), { 0.2140411, 0.2140411, 0.2140411, 0.25 })) << "by the sRGB curve; alpha as it was";
    EXPECT_TRUE(near(numbers("gamma"), { std::pow(0.5, 2.2), std::pow(0.5, 2.2), std::pow(0.5, 2.2) })) << "by the power 2.2, as Linear had it";
    EXPECT_TRUE(near(numbers("back"), { 0.5, 0.5, 0.5, 0.25 })) << "and back again";
    EXPECT_NEAR(numbers("linear", "luminance")[0], 0.2140411, 1e-4);
    EXPECT_TRUE(near(numbers("red"), { 1, 0, 0 }));
    EXPECT_TRUE(near(numbers("hue"), { 1.0 / 3.0, 1, 1 })) << "green is a third of the way round";
    const auto day = numbers("daylight");
    ASSERT_EQ(day.size(), 3u);
    EXPECT_NEAR(day[0], 1.0, 0.02) << "6500 K is near white";
    EXPECT_GT(day[2], 0.85);

    const double s = std::sqrt(0.5);
    EXPECT_TRUE(near(numbers("quarter"), { 0, s, 0, s })) << "about an axis of any length";
    EXPECT_TRUE(near(numbers("euler"), { 0, s, 0, s })) << "as an entity's rotation turns";
    EXPECT_TRUE(near(numbers("turned"), { 0, 0, -1 })) << "x a quarter turn about y is -z";
    EXPECT_TRUE(near(numbers("half"), { 0, 1, 0, 0 }));
    EXPECT_TRUE(near(numbers("back turn"), { 0, -s, 0, s }));
    EXPECT_TRUE(near(numbers("eighth"), { 0, std::sin(std::numbers::pi / 8), 0, std::cos(std::numbers::pi / 8) }));
    EXPECT_TRUE(near(numbers("as matrix"), numbers("rotated"))) << "the matrix a rotation of the same is";
    EXPECT_TRUE(near(numbers("angles"), { 0, 90, 0 }, 1e-3));

    // Frame 8 of 8 places is the first again: (1/2, 1/3) of Halton's, from the pixel's middle.
    EXPECT_TRUE(near(numbers("jitter", "pixels"), { 0, 1.0 / 3.0 - 0.5 }));
    EXPECT_TRUE(near(numbers("jitter", "clip"), { 0, 2 * (1.0 / 3.0 - 0.5) / 50 }));
    EXPECT_EQ(numbers("jitter", "index"), (std::vector<double> { 0 }));
    const auto jittered = numbers("jitter", "jittered");
    ASSERT_EQ(jittered.size(), 16u);
    EXPECT_NEAR(jittered[13], 2 * (1.0 / 3.0 - 0.5) / 50, 1e-9) << "moved as much as w is, in clip space";
    EXPECT_TRUE(runtime.ValueOf("mesh shaders").Bool()) << "whether it has them, true or false";
}

// A Time counts frames, and a LastFrame is what it was given the frame before: valid from the second frame on.
TEST_F(RuntimeTest, ALastFrameIsWhatItWasGivenTheFrameBefore) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "frame" };
    graph.Add("time", "Time").pass = "frame";
    graph.Add("before", "LastFrame").Wire("value", "time", "frame").pass = "frame";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("clear", "Clear").Wire("image", "screen").Set("color", Value::List { 0, 0, 0, 1 }).pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ValueOf("before", "valid"), Value(false)) << "no frame before the first";
    Frame();
    const double first = *runtime.ValueOf("time", "frame").Number();
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(*runtime.ValueOf("time", "frame").Number(), first + 3) << "one more each frame";
    EXPECT_EQ(*runtime.ValueOf("before").Number(), first + 2);
    EXPECT_EQ(runtime.ValueOf("before", "valid"), Value(true));
}

// The shorthands: a ComputePass is a pipeline and its dispatch, a RenderTarget an image sized by another or the window,
// and a History what a resource was when the frame before was over. A Readback brings numbers back; a Keep says when it
// holds something; a Clear clears depth and whole numbers too.
TEST_F(RuntimeTest, ShorthandsReadbacksAndClearsOfEveryKind) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "made", "frame" };
    graph.setup = { "made" };
    graph.Add("state", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 32, 32 })
        .Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).pass = "made";
    graph.Add("clear", "Clear").Wire("image", "state").Set("color", Value::List { 1, 0, 0, 1 }).pass = "made";
    graph.Add("halve", "ComputePass").Set("module", "fill").Set("entry", "computeMain").Wire("param.target", "state").Set("constant.level", 0.5)
        .Set("cover", "target").pass = "made";
    graph.Add("half size", "RenderTarget").Set("format", "eRGBA8_UNORM").Wire("sizeOf", "state").Set("scale", 0.5).Set("mipLevels", 0).pass = "frame";
    graph.Add("window sized", "RenderTarget").Set("format", "eRGBA8_UNORM").pass = "frame";
    graph.Add("paint", "Clear").Wire("image", "half size").pass = "frame";
    graph.Add("paint window sized", "Clear").Wire("image", "window sized").pass = "frame";
    // A count one more each frame, and what it was when the frame before was over.
    graph.Add("count", "Buffer").Set("instanceCount", 16).Set("usage", Value::List { "eStorage", "eTransferSrc" }).pass = "frame";
    graph.Add("add one", "ComputePass").Set("module", "increment").Set("entry", "computeMain").Wire("param.counter", "count")
        .Set("groups", Value::List { 1, 1, 1 }).pass = "frame";
    graph.Add("before", "History").Wire("of", "count").pass = "frame";
    graph.Add("read now", "Readback").Wire("buffer", "count").Set("type", "eUInt").pass = "frame";
    graph.Add("read before", "Readback").Wire("buffer", "before").Set("type", "eUInt").pass = "frame";
    graph.Add("kept", "Keep").Wire("of", "count").pass = "frame";
    // A depth, and whole numbers.
    graph.Add("depth", "Image").Set("format", "eD32_SFLOAT").Set("extent", Value::List { 4, 4 }).pass = "frame";
    graph.Add("clear depth", "Clear").Wire("image", "depth").Set("depth", 0.25).pass = "frame";
    graph.Add("read depth", "Readback").Wire("image", "depth").pass = "frame";
    graph.Add("ids", "Image").Set("format", "eR32_UINT").Set("extent", Value::List { 4, 4 }).pass = "frame";
    graph.Add("clear ids", "Clear").Wire("image", "ids").Set("color", Value::List { 7, 0, 0, 0 }).pass = "frame";
    graph.Add("read ids", "Readback").Wire("image", "ids").pass = "frame";
    graph.Add("numbers", "Buffer").Set("instanceCount", 16).Set("usage", Value::List { "eStorage" }).pass = "frame";
    graph.Add("zero", "ClearBuffer").Wire("buffer", "numbers").pass = "frame";
    graph.Add("read numbers", "Readback").Wire("buffer", "numbers").Set("offset", 4).Set("type", "eUVec2").pass = "frame";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "state").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.ValueOf("kept", "valid"), Value(false)) << "nothing copied yet";
    EXPECT_EQ(runtime.ValueOf("read now", "ready"), Value(false));
    EXPECT_TRUE(std::ranges::contains(runtime.PortsOf("halve"), std::string("param.target"), &kgraph::Port::name)) << "its pipeline's parameters are its own";

    for (int i = 0; i < 8; ++i) Frame();
    EXPECT_NEAR(scene->Pixel(4, 4).x, 128, 2) << "halved once, on the setup page";
    EXPECT_NEAR(scene->Pixel(4, 4).y, 128, 2) << "with the constant given";
    const auto half = runtime.ImageOf("half size");
    ASSERT_TRUE(half.Get());
    EXPECT_EQ(half->Extent().x, 16u);
    EXPECT_EQ(half->Extent().y, 16u);
    EXPECT_EQ(half->MipLevels(), 5u) << "every mip down to a pixel, for 0";
    EXPECT_EQ(runtime.ImageOf("window sized")->Extent().x, static_cast<kor::u32>(*runtime.ValueOf("screen", "width").Number())) << "as wide as the window";
    EXPECT_EQ(runtime.ValueOf("half size", "extent"), Value(Value::List { 16, 16, 1 }));

    EXPECT_EQ(runtime.ValueOf("read now", "ready"), Value(true));
    const double now = *runtime.ValueOf("read now").Number(), before = *runtime.ValueOf("read before").Number();
    EXPECT_EQ(now - before, 1) << "the frame before's count: " << now << " and " << before;
    EXPECT_EQ(runtime.ValueOf("before", "valid"), Value(true));
    EXPECT_EQ(runtime.ValueOf("kept", "valid"), Value(true));
    EXPECT_EQ(runtime.ValueOf("read depth"), Value(0.25));
    EXPECT_EQ(runtime.ValueOf("read ids"), Value(7)) << "a whole number, cleared as one";
    EXPECT_EQ(runtime.ValueOf("read numbers"), Value(Value::List { 0, 0 }));
}

// The engine's HiZ: a depth pyramid whose every level is the farthest of what it covers — the least, of a reversed depth.
// And its TAA: a picture that does not move comes out as it went in, once the history it blends with is there.
TEST_F(RuntimeTest, TheEnginesHiZAndTAAWorkOnTheGpu) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "frame" };
    for (const char* name : { "HiZ", "TAA" }) graph.functions[name] = support::EngineFunction(name);
    graph.Add("depth", "Image").Set("format", "eR32_SFLOAT").Set("extent", Value::List { 64, 32 }).pass = "frame";
    graph.Add("halves", "ComputePass").Set("module", "halves").Set("entry", "computeMain").Wire("param.target", "depth").Set("cover", "target").pass = "frame";
    graph.Add("far", "Function").Set("function", "HiZ").Wire("depth", "depth").pass = "frame";
    graph.Add("near", "Function").Set("function", "HiZ").Wire("depth", "depth").Set("reversed", 1).pass = "frame";
    for (const char* id : { "far", "near" })
        for (int x = 0; x < 2; ++x)
            graph.Add(std::format("{} {}", id, x), "Readback").Wire("image", id, "pyramid").Set("mip", 8).Set("texel", Value::List { x, 0 }).pass = "frame";
    graph.Add("whole", "Readback").Wire("image", "far", "pyramid").Set("texel", Value::List { 300, 100 }).pass = "frame";

    graph.Add("picture", "Image").Set("format", "eRGBA16_SFLOAT").Set("extent", Value::List { 16, 16 }).pass = "frame";
    graph.Add("paint", "Clear").Wire("image", "picture").Set("color", Value::List { 0.5, 0.25, 0, 1 }).pass = "frame";
    graph.Add("flat", "Image").Set("format", "eR32_SFLOAT").Set("extent", Value::List { 16, 16 }).pass = "frame";
    graph.Add("flatten", "Clear").Wire("image", "flat").Set("color", Value::List { 0.5, 0, 0, 0 }).pass = "frame";
    graph.Add("taa", "Function").Set("function", "TAA").Wire("picture", "picture").Wire("depth", "flat")
        .Set("viewProjection", Value::List { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }).pass = "frame";
    graph.Add("resolved", "Readback").Wire("image", "taa", "picture").Set("texel", Value::List { 3, 3 }).pass = "frame";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "taa", "picture").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    for (int i = 0; i < 8; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ValueOf("far 0"), Value(0.25)) << "the left half, at the last level but one: two texels, each a half";
    EXPECT_EQ(runtime.ValueOf("far 1"), Value(0.75));
    EXPECT_EQ(runtime.ValueOf("near 0"), Value(0.25));
    EXPECT_EQ(runtime.ValueOf("whole"), Value(0.75)) << "the first level: of what each of its texels covers";
    const Value resolved = runtime.ValueOf("resolved");
    ASSERT_TRUE(resolved.Items()) << resolved.ToJson();
    EXPECT_NEAR(*(*resolved.Items())[0].Number(), 0.5, 1e-3);
    EXPECT_NEAR(*(*resolved.Items())[1].Number(), 0.25, 1e-3);
}

// The engine's Bloom: what is brighter than its threshold bleeds round it, into the picture in place.
TEST_F(RuntimeTest, TheEnginesBloomBleedsWhatIsBright) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "frame" };
    graph.functions["Bloom"] = support::EngineFunction("Bloom");
    graph.Add("picture", "Image").Set("format", "eRGBA16_SFLOAT").Set("extent", Value::List { 64, 64 }).pass = "frame";
    graph.Add("paint", "Clear").Wire("image", "picture").Set("color", Value::List { 4, 4, 4, 1 }).pass = "frame";
    graph.Add("linear", "Sampler").Set("minFilter", "eLinear").Set("magFilter", "eLinear").Set("addressModeU", "eClampToEdge").Set("addressModeV", "eClampToEdge").pass = "frame";
    graph.Add("bloom", "Function").Set("function", "Bloom").Wire("picture", "picture").Wire("sampler", "linear").pass = "frame";
    graph.Add("read", "Readback").Wire("image", "bloom", "picture").Set("texel", Value::List { 32, 32 }).pass = "frame";
    graph.Add("screen", "Screen").pass = "frame";
    graph.Add("show", "Blit").Wire("source", "bloom", "picture").Wire("destination", "screen").pass = "frame";
    runtime.Set(graph);
    for (int i = 0; i < 8; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    for (const auto& [id, turns] : std::map<std::string, int> {})
        (void)id, (void)turns;
    for (const char* id : { "bloom/bloom size", "bloom/bloom down", "bloom/bloom up", "bloom/source view", "bloom/down [0]", "bloom/run down [0]",
                            "bloom/run up [0]", "bloom/run add bloom" })
        if (const auto* status = runtime.Status(id); status && (!status->ok || !status->note.empty())) ADD_FAILURE() << id << ": " << status->error << status->note;
    const Value read = runtime.ValueOf("read");
    ASSERT_TRUE(read.Items()) << read.ToJson();
    EXPECT_GT(*(*read.Items())[0].Number(), 4.0) << "brighter than it was: its own light bled back into it";
}

// Transient images: the frame graph's, made for the frame. Two never needed at the same time — the second written from
// what the first was copied into — share their memory, and the picture goes through both as through images of their own.
// What stood in for each until the frame graph made it is gone; and an image that cannot be one says why.
TEST_F(RuntimeTest, TransientImagesShareTheirMemoryAndHoldWhatTheyAreGiven) {
    auto& runtime = *scene->runtime;
    const std::uint64_t logged = kor::log::LastSequence();
    using kgraph::Value;
    kgraph::Graph graph;
    const auto image = [&](const std::string& id, const bool transient) -> kgraph::Node& {
        auto& node = graph.Add(id, "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 16, 16 })
            .Set("usage", Value::List { "eTransferSrc", "eTransferDst" });
        if (transient) node.Set("transient", true);
        return node;
    };
    image("first", true);
    image("between", false);
    image("second", true);
    image("out", false);
    graph.Add("paint", "Clear").Wire("image", "first").Set("color", Value::List { 1, 0, 0, 1 });
    graph.Add("keep", "Blit").Wire("source", "first").Wire("destination", "between");
    graph.Add("again", "Blit").Wire("source", "between").Wire("destination", "second");
    graph.Add("show", "Blit").Wire("source", "second").Wire("destination", "out");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const std::weak_ptr<kgraph::Resource> standIn = runtime.ResourceOf("first");
    runtime.Watch("out");
    for (int i = 0; i < 8; ++i) Frame();

    const auto made = runtime.ResourceOf("first");
    ASSERT_TRUE(made);
    EXPECT_TRUE(made->transient);
    EXPECT_NE(made, standIn.lock()) << "the frame graph's, not what stood in for it";
    EXPECT_TRUE(standIn.expired()) << "what stood in is gone";
    const auto report = runtime.Frame();
    EXPECT_GT(report.unsharedBytes, report.bytes) << "the two share: " << report.bytes << " of " << report.unsharedBytes;
    const auto out = runtime.Watched("out");
    ASSERT_TRUE(out);
    ASSERT_GE(out->bytes.size(), 4u);
    EXPECT_EQ(out->bytes[0], std::byte { 255 }) << "red, through both";
    EXPECT_EQ(out->bytes[1], std::byte { 0 });
    for (const auto& record : kor::log::HistorySince(logged))
        if (record.message.find("VUID") != std::string::npos || record.level >= kor::log::Level::eError) ADD_FAILURE() << record.message;

    // One of three dimensions is no transient.
    graph.Find("second")->Set("extent", Value::List { 16, 16, 4 }).Set("type", "e3D");
    runtime.Set(graph);
    const auto* status = runtime.Status("second");
    ASSERT_NE(status, nullptr);
    EXPECT_NE(status->error.find("2D image"), std::string::npos) << status->error;
}

// A transient buffer: made by the frame graph, zeroed, counted up once and copied out, each frame. And the frame graph
// made again — another pass in it — makes its transients again: what is wired from them follows, and nothing is run with
// what is gone. Nothing the device would not have, all the while.
TEST_F(RuntimeTest, ATransientBufferIsTheFrameGraphsAndFollowsItWhenItIsMadeAgain) {
    auto& runtime = *scene->runtime;
    using kgraph::Value;
    const std::uint64_t logged = kor::log::LastSequence();
    kgraph::Graph graph;
    graph.Add("count", "Buffer").Set("instanceCount", 16).Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" }).Set("transient", true);
    graph.Add("kept", "Buffer").Set("instanceCount", 16).Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.Add("zero", "ClearBuffer").Wire("buffer", "count");
    graph.Add("add one", "ComputePass").Set("module", "increment").Set("entry", "computeMain").Wire("param.counter", "count").Set("groups", Value::List { 1, 1, 1 });
    graph.Add("copy", "CopyBuffer").Wire("source", "count").Wire("destination", "kept");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    runtime.Watch("kept");
    const auto counted = [&] {
        for (int i = 0; i < 8; ++i) Frame();
        const auto peek = runtime.Watched("kept");
        std::uint32_t value = 0;
        if (peek && peek->bytes.size() >= 4) std::memcpy(&value, peek->bytes.data(), 4);
        else ADD_FAILURE() << "nothing to look at";
        return value;
    };
    EXPECT_EQ(counted(), 1u) << "zeroed, then one more, each frame";
    const auto made = runtime.ResourceOf("count");
    ASSERT_TRUE(made);
    EXPECT_TRUE(made->transient);

    // Another pass: the frame graph is made again, and its buffer with it.
    graph.Add("other", "Buffer").Set("instanceCount", 16).Set("usage", Value::List { "eStorage", "eTransferDst" });
    graph.Add("clear other", "ClearBuffer").Wire("buffer", "other");
    runtime.Set(graph);
    EXPECT_EQ(counted(), 1u) << "and the same, with the frame graph made again";
    for (const auto& record : kor::log::HistorySince(logged))
        if (record.message.find("VUID") != std::string::npos || record.level >= kor::log::Level::eError) ADD_FAILURE() << record.message;
}
