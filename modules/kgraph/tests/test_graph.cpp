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
    EXPECT_TRUE(port("ForEach", "with").optional);
    // A ForEach is every one of them; one is an Entity node's. Which element it is, is there to be asked for.
    const auto output = [](const char* type, const std::string_view name) {
        const kgraph::NodeType* found = kgraph::FindType(type);
        const auto it = std::ranges::find(found->outputs, name, &kgraph::Port::name);
        return it == found->outputs.end() ? nullptr : &*it;
    };
    EXPECT_EQ(std::ranges::find(kgraph::FindType("ForEach")->inputs, "select", &kgraph::Port::name), kgraph::FindType("ForEach")->inputs.end());
    ASSERT_NE(output("ForEach", "index"), nullptr);
    EXPECT_TRUE(output("ForEach", "index")->optional) << "not shown until asked for";
    EXPECT_TRUE(output("Filter", "index")->optional);
    EXPECT_TRUE(port("Entity", "entity").optional) << "the first, when not given";
    EXPECT_EQ(output("Entity", "index"), nullptr) << "one entity: no index, no count";
    EXPECT_NE(output("Entity", "present"), nullptr);
    EXPECT_TRUE(port("Draw", "load").optional);

    // What kind of thing each is, for the control that sets it.
    EXPECT_EQ(port("Clear", "color").editor, "color");
    EXPECT_EQ(port("ComputePipeline", "compute.module").editor, "module");
    EXPECT_EQ(port("ComputePipeline", "compute.entry").editor, "entry");
    EXPECT_EQ(port("Reference", "node").editor, "node");
    EXPECT_EQ(port("ForEach", "without").editor, "components");
    EXPECT_EQ(port("Dispatch", "cover").editor, "parameter");
    EXPECT_EQ(port("VertexLayout", "attributes").editor, "attributes");
    EXPECT_EQ(port("Draw", "command").choices.size(), 9u);
    EXPECT_EQ(port("Primitive", "shape").choices.size(), 9u);

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

// ---- the default meshes ---------------------------------------------------------------------------------------------

#include <cmath>
#include <kgraph/meshes.h>

namespace {
    struct Bounds { float low[3] { 1e9f, 1e9f, 1e9f }, high[3] { -1e9f, -1e9f, -1e9f }; };
    Bounds BoundsOf(const kgraph::MeshData& mesh)
    {
        Bounds bounds;
        for (const auto& vertex : mesh.vertices)
            for (int axis = 0; axis < 3; ++axis) {
                bounds.low[axis] = std::min(bounds.low[axis], vertex.position[axis]);
                bounds.high[axis] = std::max(bounds.high[axis], vertex.position[axis]);
            }
        return bounds;
    }
}

// Each default mesh is a closed, outward-facing shape of the size it says: every triangle faces the way its
// corners' normals point, which is what lighting and back-face culling both go by.
TEST(Meshes, EveryDefaultMeshFacesOutAndIsTheSizeItSays) {
    EXPECT_EQ(kgraph::DefaultMeshNames(), (std::vector<std::string> { "cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus" }));
    for (const char* name : { "cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus", "quad", "triangle" }) {
        const auto mesh = kgraph::ShapeData(name);
        ASSERT_TRUE(mesh.has_value()) << name;
        ASSERT_FALSE(mesh->indices.empty()) << name;
        ASSERT_EQ(mesh->indices.size() % 3, 0u) << name;
        for (const auto& vertex : mesh->vertices) {
            const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] + vertex.normal[2] * vertex.normal[2]);
            ASSERT_NEAR(length, 1.f, 1e-3f) << name << ": a normal is one long";
        }
        std::size_t facing = 0, flat = 0;
        for (std::size_t i = 0; i < mesh->indices.size(); i += 3) {
            ASSERT_LT(mesh->indices[i], mesh->vertices.size()) << name;
            const auto& a = mesh->vertices[mesh->indices[i]];
            const auto& b = mesh->vertices[mesh->indices[i + 1]];
            const auto& c = mesh->vertices[mesh->indices[i + 2]];
            float ab[3], ac[3], sum[3];
            for (int axis = 0; axis < 3; ++axis) {
                ab[axis] = b.position[axis] - a.position[axis];
                ac[axis] = c.position[axis] - a.position[axis];
                sum[axis] = a.normal[axis] + b.normal[axis] + c.normal[axis];
            }
            const float cross[3] { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] };
            const float area = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
            if (area < 1e-9f) { ++flat; continue; }   // at a pole, where a band comes to a point
            ASSERT_GT(cross[0] * sum[0] + cross[1] * sum[1] + cross[2] * sum[2], 0.f) << name << ": triangle " << i / 3 << " faces in";
            ++facing;
        }
        EXPECT_GT(facing, flat) << name;
    }

    const auto within = [](const char* name, const float x, const float y, const float z) {
        const Bounds bounds = BoundsOf(*kgraph::ShapeData(name));
        const float half[3] { x, y, z };
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_NEAR(bounds.high[axis], half[axis], 1e-3f) << name << ", axis " << axis;
            EXPECT_NEAR(bounds.low[axis], -half[axis], 1e-3f) << name << ", axis " << axis;
        }
    };
    within("cube", 0.5f, 0.5f, 0.5f);
    within("sphere", 0.5f, 0.5f, 0.5f);
    within("plane", 0.5f, 0.f, 0.5f);
    within("cylinder", 0.5f, 0.5f, 0.5f);
    within("cone", 0.5f, 0.5f, 0.5f);
    within("capsule", 0.25f, 0.5f, 0.25f);
    within("torus", 0.5f, 0.15f, 0.5f);
    EXPECT_EQ(kgraph::ShapeData("cube")->vertices.size(), 24u);
    EXPECT_EQ(kgraph::ShapeData("sphere")->indices.size(), 40u * 80u * 6u);
    EXPECT_FALSE(kgraph::ShapeData("blob").has_value());
    EXPECT_EQ(kgraph::DefaultMeshId("cube"), "$cube");
}

// Where an input is set: on the node (what is read there — a number of what works values out, a pass being on), in
// the inspector (what takes room), or by a wire only (what only a node makes — a vertex layout, from a VertexLayout).
// A node can move one of the first two to the other, and keeps that when it is saved.
TEST(Catalog, SaysWhereEachInputIsSet) {
    using kgraph::Placement;
    const auto place = [](const char* type, const char* input) {
        const auto* found = kgraph::FindType(type);
        const auto port = std::ranges::find(found->inputs, input, &kgraph::Port::name);
        EXPECT_NE(port, found->inputs.end()) << type << "." << input;
        return port == found->inputs.end() ? kgraph::Port {} : *port;
    };
    EXPECT_EQ(place("Add", "a").place, Placement::eNode);
    EXPECT_EQ(place("Add", "a").initial, Value(0)) << "an Add made is 0 + 0, shown on it";
    EXPECT_EQ(place("Select", "condition").place, Placement::eNode);
    EXPECT_EQ(place("Color", "color").place, Placement::eNode);
    EXPECT_EQ(place("Repeat", "count").place, Placement::eNode);
    EXPECT_EQ(place("Dispatch", "enabled").place, Placement::eInspector) << "a row on every pass would be in every graph's way";
    EXPECT_EQ(place("View", "main").place, Placement::eNode);
    EXPECT_EQ(place("View", "target").place, Placement::eNode);
    EXPECT_EQ(place("Perspective", "fieldOfView").place, Placement::eNode);
    EXPECT_EQ(place("Perspective", "main").place, Placement::eNode);
    EXPECT_EQ(place("Filter", "entities").place, Placement::eWire);
    EXPECT_EQ(place("ShadowCascades", "lambda").place, Placement::eInspector);
    EXPECT_EQ(place("Image", "format").place, Placement::eInspector);
    EXPECT_EQ(place("VertexLayout", "attributes").place, Placement::eInspector);
    EXPECT_EQ(place("MatrixInverse", "a").place, Placement::eWire);
    const auto layout = place("GraphicsPipeline", "vertex.layout");
    EXPECT_EQ(layout.place, Placement::eWire);
    EXPECT_EQ(layout.companion, "VertexLayout") << "what sets it up, for an editor to make";
    EXPECT_EQ(place("Mesh", "vertexLayout").companion, "VertexLayout");

    // A node says otherwise of what is not wired only, and keeps it.
    Graph graph;
    graph.Add("add", "Add").placed["a"] = "inspector";
    graph.Add("image", "Image").placed["format"] = "node";
    graph.Find("image")->placed["usage"] = "nowhere";      // read as nothing
    graph.Add("pipeline", "GraphicsPipeline").placed["vertex.layout"] = "node";
    EXPECT_EQ(kgraph::PlacementOf(place("Add", "a"), *graph.Find("add")), Placement::eInspector);
    EXPECT_EQ(kgraph::PlacementOf(place("Image", "format"), *graph.Find("image")), Placement::eNode);
    EXPECT_EQ(kgraph::PlacementOf(layout, *graph.Find("pipeline")), Placement::eWire) << "wired only, whatever the node says";
    Graph loaded;
    std::string error;
    ASSERT_TRUE(Graph::FromJson(graph.ToJson(), loaded, error)) << error;
    EXPECT_EQ(loaded.Find("add")->placed.at("a"), "inspector");
    EXPECT_EQ(loaded.Find("image")->placed.at("format"), "node");
    EXPECT_FALSE(loaded.Find("image")->placed.contains("usage"));
}
