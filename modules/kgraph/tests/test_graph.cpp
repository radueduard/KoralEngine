// The graph as data: built, saved, loaded. And the catalog of node types. No GPU.

#include <gtest/gtest.h>

#include <algorithm>

#include <kgraph/catalog.h>
#include <kgraph/graph.h>

using kgraph::Graph;
using kgraph::Value;

TEST(Graph, IsSavedAndLoadedAsItWas) {
    Graph graph;
    graph.Add("target", "Image")
        .Set("format", "eRGBA8_UNORM")
        .Set("extent.x", 64).Set("extent.y", 32)
        .Set("usage", Value::List { "eStorage", "eTransferSrc" });
    auto& pipeline = graph.Add("pipeline", "ComputePipeline");
    pipeline.title = "Fill \"it\"";
    pipeline.x = 120.5;
    pipeline.y = -40;
    pipeline.Wire("computeShader", "shader").Wire("param.target", "target").Set("constant.level", 0.25);
    graph.Add("state", "GraphicsPipeline").Set("rasterizationState", Value::Members { { "cullMode", Value::List { "eBack" } }, { "lineWidth", 1.0 } });
    graph.Add("flag", "Bool").Set("value", true);

    const std::string json = graph.ToJson();
    Graph loaded;
    std::string error;
    ASSERT_TRUE(Graph::FromJson(json, loaded, error)) << error;
    EXPECT_EQ(loaded, graph);
    EXPECT_EQ(loaded.ToJson(), json);

    const kgraph::Node* node = loaded.Find("pipeline");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->title, "Fill \"it\"");
    ASSERT_TRUE(node->inputs.at("param.target").link.has_value());
    EXPECT_EQ(node->inputs.at("param.target").link->node, "target");
    EXPECT_EQ(node->inputs.at("param.target").link->output, "out");
    EXPECT_EQ(*node->inputs.at("constant.level").value.Number(), 0.25);
}

TEST(Graph, RefusesWhatIsNotOne) {
    Graph graph;
    std::string error;
    EXPECT_FALSE(Graph::FromJson("{ \"nodes\": [", graph, error));
    EXPECT_FALSE(Graph::FromJson("{ \"kgraph\": 2, \"nodes\": [] }", graph, error));
    EXPECT_FALSE(Graph::FromJson("{ \"kgraph\": 1, \"nodes\": [ { \"type\": \"Image\" } ] }", graph, error)) << "a node with no id";
    EXPECT_FALSE(Graph::FromJson("{ \"kgraph\": 1, \"nodes\": [ { \"id\": \"a\", \"type\": \"Image\" }, { \"id\": \"a\", \"type\": \"Image\" } ] }", graph, error));
    EXPECT_NE(error.find("'a'"), std::string::npos) << error;
    EXPECT_TRUE(Graph::FromJson("{ \"kgraph\": 1, \"nodes\": [] }", graph, error));
    EXPECT_TRUE(graph.nodes.empty());
}

TEST(Catalog, HasANodeForEveryBuilderAndTheEnginesOwn) {
    const auto has = [](const kgraph::NodeType& type, const std::string_view input) {
        return std::ranges::find(type.inputs, input, &kgraph::Port::name) != type.inputs.end();
    };
    for (const char* name : { "Image", "ImageView", "Sampler", "Buffer", "BufferView", "Shader", "Framebuffer", "ComputePipeline", "GraphicsPipeline",
                              "RayTracingPipeline", "Number", "Vector", "Add", "Ceil", "ImageExtent", "Screen", "Reference", "Clear", "Blit", "Dispatch" })
        EXPECT_NE(kgraph::FindType(name), nullptr) << name;
    EXPECT_EQ(kgraph::FindType("DescriptorSet"), nullptr);

    const kgraph::NodeType* image = kgraph::FindType("Image");
    ASSERT_NE(image, nullptr);
    EXPECT_TRUE(has(*image, "format"));
    EXPECT_TRUE(has(*image, "extent.x")) << "a setting of several arguments has an input each";
    EXPECT_FALSE(has(*image, "data")) << "bytes are not a graph's to hold";
    const auto format = std::ranges::find(image->inputs, "format", &kgraph::Port::name);
    EXPECT_EQ(format->type, "enum kor::Image::Format");
    EXPECT_EQ(image->outputs.front().type, "Image");

    const kgraph::NodeType* view = kgraph::FindType("ImageView");
    const auto of = std::ranges::find(view->inputs, "image", &kgraph::Port::name);
    ASSERT_NE(of, view->inputs.end()) << "what the builder is made from is an input";
    EXPECT_EQ(of->kind, kgraph::PortKind::eResource);

    // A pipeline names its shaders itself, a stage at a time: the module, and the entry point in it.
    const kgraph::NodeType* pipeline = kgraph::FindType("ComputePipeline");
    EXPECT_TRUE(has(*pipeline, "compute.module"));
    EXPECT_TRUE(has(*pipeline, "compute.entry"));
    EXPECT_FALSE(has(*pipeline, "computeShader")) << "a shader wired in still works, and is not offered";
    EXPECT_FALSE(kgraph::FindType("Shader")->listed);
    EXPECT_TRUE(pipeline->listed);
    EXPECT_FALSE(has(*pipeline, "binding.name")) << "bindings follow from what is wired";
    EXPECT_EQ(pipeline->category, "Pipelines");

    const kgraph::NodeType* framebuffer = kgraph::FindType("Framebuffer");
    const auto color = std::ranges::find(framebuffer->inputs, "color[].view", &kgraph::Port::name);
    ASSERT_NE(color, framebuffer->inputs.end());
    EXPECT_TRUE(color->repeatable);
}

TEST(Catalog, SaysWhatANodeCannotDoWithoutAndWhatAnEditorOffersForEach) {
    const auto port = [](const char* type, const std::string_view name) -> const kgraph::Port& {
        const kgraph::NodeType* found = kgraph::FindType(type);
        const auto it = std::ranges::find(found->inputs, name, &kgraph::Port::name);
        EXPECT_NE(it, found->inputs.end()) << type << "." << name;
        static const kgraph::Port none;
        return it == found->inputs.end() ? none : *it;
    };
    // The few inputs a node is made with, and the many it can be given.
    EXPECT_FALSE(port("Image", "format").optional);
    EXPECT_FALSE(port("Image", "extent.x").optional);
    EXPECT_TRUE(port("Image", "extent.z").optional);
    EXPECT_TRUE(port("Image", "mipLevels").optional);
    EXPECT_FALSE(port("ImageView", "image").optional);
    EXPECT_TRUE(port("GraphicsPipeline", "rasterizationState").optional);
    EXPECT_FALSE(port("GraphicsPipeline", "fragment.module").optional);
    EXPECT_TRUE(port("GraphicsPipeline", "geometry.module").optional);
    EXPECT_FALSE(port("Dispatch", "pipeline").optional);
    EXPECT_TRUE(port("Dispatch", "with").optional);
    EXPECT_TRUE(port("Draw", "load").optional);

    // What kind of thing each is, for the control that sets it.
    EXPECT_EQ(port("Clear", "color").editor, "color");
    EXPECT_EQ(port("ComputePipeline", "compute.module").editor, "module");
    EXPECT_EQ(port("ComputePipeline", "compute.entry").editor, "entry");
    EXPECT_EQ(port("Reference", "node").editor, "node");
    EXPECT_EQ(port("Draw", "without").editor, "components");
    EXPECT_EQ(port("Dispatch", "cover").editor, "parameter");
    EXPECT_EQ(port("VertexLayout", "attributes").editor, "attributes");
    EXPECT_EQ(port("Draw", "command").choices, (std::vector<std::string> { "eAuto", "eDraw", "eDrawIndexed" }));
    EXPECT_EQ(port("Primitive", "shape").choices.size(), 3u);

    // A graphics pipeline is drawn one of two ways, and has the inputs of the one it is.
    const kgraph::NodeType* graphics = kgraph::FindType("GraphicsPipeline");
    EXPECT_EQ(port("GraphicsPipeline", "stages").choices, (std::vector<std::string> { "eVertex", "eMesh" }));
    const auto names = [&](const kgraph::Node& node) {
        std::vector<std::string> out;
        for (const auto& each : kgraph::PortsFor(*graphics, node)) out.push_back(each.name);
        return out;
    };
    const auto in = [](const std::vector<std::string>& list, const char* name) { return std::ranges::find(list, name) != list.end(); };
    kgraph::Node node { .id = "p", .type = "GraphicsPipeline" };
    auto vertex = names(node);
    for (const char* name : { "vertex.module", "vertex.entry", "vertex.layout", "tessellationControl.module", "tessellationEvaluation.entry",
                              "patchControlPoints", "geometry.module", "fragment.module", "framebuffer", "inputAssemblyState" })
        EXPECT_TRUE(in(vertex, name)) << name;
    EXPECT_FALSE(in(vertex, "mesh.module") || in(vertex, "task.module")) << "drawn with a vertex shader, it has no mesh shader";
    node.Set("stages", "eMesh");
    auto mesh = names(node);
    for (const char* name : { "task.module", "mesh.module", "mesh.entry", "fragment.module", "framebuffer", "rasterizationState" }) EXPECT_TRUE(in(mesh, name)) << name;
    for (const char* name : { "vertex.module", "vertex.layout", "geometry.module", "tessellationControl.module", "patchControlPoints", "inputAssemblyState" })
        EXPECT_FALSE(in(mesh, name)) << name;

    // A ray tracing pipeline's shaders, some of them any number of times.
    EXPECT_FALSE(port("RayTracingPipeline", "raygen.module").optional);
    EXPECT_TRUE(port("RayTracingPipeline", "miss[].module").repeatable);
    EXPECT_TRUE(port("RayTracingPipeline", "hitGroup[].closestHit.module").repeatable);
}
