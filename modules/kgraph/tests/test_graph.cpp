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

    const kgraph::NodeType* pipeline = kgraph::FindType("ComputePipeline");
    EXPECT_TRUE(has(*pipeline, "computeShader"));
    EXPECT_FALSE(has(*pipeline, "binding.name")) << "bindings follow from what is wired";
    EXPECT_EQ(pipeline->category, "Pipelines");

    const kgraph::NodeType* framebuffer = kgraph::FindType("Framebuffer");
    const auto color = std::ranges::find(framebuffer->inputs, "color[].view", &kgraph::Port::name);
    ASSERT_NE(color, framebuffer->inputs.end());
    EXPECT_TRUE(color->repeatable);
}
