// The graph as data: built, saved, loaded. And the catalog of node types. No GPU.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <limits>

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

// How picture files are kept is saved with the graph: each file's format, and the packs of others' channels.
TEST(Graph, KeepsHowItsPicturesAreKept) {
    kgraph::Graph graph;
    graph.textureImports["@wall_BaseColor.png"] = { kgraph::TextureFormat::eBC7, kgraph::TextureQuality::eBest };
    graph.textureImports["@wall_Normal.png"] = { kgraph::TextureFormat::eBC5 };
    kgraph::TexturePack pack;
    pack.channels = { kgraph::TexturePack::Channel { "@wall_AO.png", 0 }, { "@wall_Roughness.png", 1 }, { {}, 0, 0.25f }, { {}, 0, 1.f } };
    pack.format = kgraph::TextureFormat::eBC1;
    graph.texturePacks["@wall_ORM.pack"] = pack;
    kgraph::Graph loaded;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(graph.ToJson(), loaded, error)) << error;
    EXPECT_EQ(loaded.textureImports, graph.textureImports);
    EXPECT_EQ(loaded.texturePacks, graph.texturePacks);
}

// What a picture is for is told by its file's name, then by the field that holds it; and the files of a material's
// occlusion, roughness and metalness suggest one pack of them, laid out as glTF has it.
TEST(Textures, AreToldApartAndSuggestPacks) {
    using kgraph::TextureKind;
    EXPECT_EQ(kgraph::TextureKindOf("albedoMap", "@t/wall_BaseColor.png"), TextureKind::eColor);
    EXPECT_EQ(kgraph::TextureKindOf("", "@t/wall_Normal.png"), TextureKind::eNormal);
    EXPECT_EQ(kgraph::TextureKindOf("roughnessMap", "@t/wall_Roughness.png"), TextureKind::eMask);
    EXPECT_EQ(kgraph::TextureKindOf("roughnessMap", "@t/wall_RoughnessMetalness.png"), TextureKind::eData) << "two numbers in one";
    EXPECT_EQ(kgraph::TextureKindOf("normalMap", "@t/stone.png"), TextureKind::eNormal) << "nothing in its name: the field's";
    EXPECT_EQ(kgraph::DefaultTextureFormat(TextureKind::eNormal), kgraph::TextureFormat::eBC5);

    const auto packs = kgraph::SuggestTexturePacks({ "@t/wall_AO.png", "@t/wall_Roughness.png", "@t/wall_Metalness.png", "@t/wall_BaseColor.png",
                                                     "@t/floor_Roughness.png", "@t/floor-metallic.png", "@t/lone_Roughness.png" });
    ASSERT_EQ(packs.size(), 2u) << "wall and floor; lone has only one";
    const auto& wall = packs.at("@t/wall_ORM.pack");
    EXPECT_EQ(wall.channels[0].from, "@t/wall_AO.png");
    EXPECT_EQ(wall.channels[1].from, "@t/wall_Roughness.png");
    EXPECT_EQ(wall.channels[2].from, "@t/wall_Metalness.png");
    EXPECT_TRUE(wall.channels[3].from.empty());
    EXPECT_EQ(wall.channels[3].constant, 1.f);
    const auto& floor = packs.at("@t/floor_ORM.pack");
    EXPECT_TRUE(floor.channels[0].from.empty()) << "no occlusion: none, 1";
    EXPECT_EQ(floor.channels[0].constant, 1.f);
    EXPECT_EQ(floor.channels[2].from, "@t/floor-metallic.png");
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
                              "RayTracingPipeline", "Double", "Vec3", "Add", "Ceil", "ImageExtent", "Screen", "Reference", "Clear", "Blit", "Dispatch" })
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
    EXPECT_EQ(port("Primitive", "shape").choices.size(), kgraph::DefaultMeshNames().size());
    EXPECT_EQ(port("Primitive", "shape").choices[13], "eRoundedBox") << "each default mesh, by its name";

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
    EXPECT_EQ(std::vector<std::string>(kgraph::DefaultMeshNames().begin(), kgraph::DefaultMeshNames().begin() + 9),
              (std::vector<std::string> { "cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus", "quad", "triangle" }))
        << "the first are the Kotlin project's, in its order";
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

// A default mesh's tangents: at right angles to its normal, a unit long, along u; the bitangent along v.
TEST(DefaultMeshes, HaveTangentsAlongTheirPicture) {
    const auto f = [](const kgraph::VertexAttribute& a, const std::size_t v, const std::size_t c) { return std::bit_cast<float>(a.words[v * a.count + c]); };
    for (const auto& name : kgraph::DefaultMeshNames()) {
        const kgraph::VertexData four = kgraph::ToVertexData(*kgraph::ShapeData(name), kgraph::Tangents::eFour);
        EXPECT_TRUE(four.Problem().empty()) << name << ": " << four.Problem();
        const auto* normal = four.Find("NORMAL");
        const auto* tangent = four.Find("TANGENT");
        ASSERT_NE(tangent, nullptr) << name;
        ASSERT_EQ(tangent->count, 4u);
        EXPECT_EQ(four.attributes[2].semantic, "TANGENT") << "after the normal, as a model file's";
        const kgraph::VertexData both = kgraph::ToVertexData(*kgraph::ShapeData(name), kgraph::Tangents::eWithBitangent);
        const auto* bitangent = both.Find("BITANGENT");
        ASSERT_NE(bitangent, nullptr) << name;
        for (std::size_t v = 0; v < four.vertexCount; ++v) {
            float dot = 0.f, length = 0.f;
            for (std::size_t c = 0; c < 3; ++c) { dot += f(*normal, v, c) * f(*tangent, v, c); length += f(*tangent, v, c) * f(*tangent, v, c); }
            EXPECT_NEAR(dot, 0.f, 1e-4f) << name << " vertex " << v;
            EXPECT_NEAR(length, 1.f, 1e-4f) << name << " vertex " << v;
            EXPECT_EQ(std::abs(f(*tangent, v, 3)), 1.f) << name;
            // The bitangent is cross(normal, tangent) on the side w says.
            const float n[3] { f(*normal, v, 0), f(*normal, v, 1), f(*normal, v, 2) }, t[3] { f(*tangent, v, 0), f(*tangent, v, 1), f(*tangent, v, 2) };
            const float b[3] { (n[1] * t[2] - n[2] * t[1]) * f(*tangent, v, 3), (n[2] * t[0] - n[0] * t[2]) * f(*tangent, v, 3), (n[0] * t[1] - n[1] * t[0]) * f(*tangent, v, 3) };
            for (std::size_t c = 0; c < 3; ++c) EXPECT_NEAR(f(*bitangent, v, c), b[c], 1e-4f) << name << " vertex " << v;
        }
    }
    // The plane's picture: u along +x, v along +z, facing up.
    const kgraph::VertexData plane = kgraph::ToVertexData(*kgraph::ShapeData("plane"), kgraph::Tangents::eWithBitangent);
    EXPECT_NEAR(std::bit_cast<float>(plane.Find("TANGENT")->words[0]), 1.f, 1e-5f);
    EXPECT_EQ(kgraph::ToVertexData(*kgraph::ShapeData("cube")).attributes.size(), 3u) << "none, unless asked for";
    EXPECT_EQ(kgraph::TangentsNamed(kgraph::TangentsName(kgraph::Tangents::eWithBitangent)), kgraph::Tangents::eWithBitangent);
}

TEST(DefaultMeshes, TheirTangentsAreSavedWithTheGraphWhenNotTheFourOfThem) {
    kgraph::Graph graph;
    EXPECT_EQ(graph.ToJson().find("defaultMeshTangents"), std::string::npos);
    graph.defaultMeshTangents = kgraph::Tangents::eNone;
    kgraph::Graph read;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(graph.ToJson(), read, error)) << error;
    EXPECT_EQ(read.defaultMeshTangents, kgraph::Tangents::eNone);
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

TEST(Catalog, SaysWhatANumberIsKeptWithin) {
    const auto port = [](const std::string_view type, const std::string_view name) {
        for (const auto& input : kgraph::FindType(type)->inputs)
            if (input.name == name) return input;
        ADD_FAILURE() << type << " has no " << name;
        return kgraph::Port {};
    };
    constexpr float any = std::numeric_limits<float>::infinity();
    // As the node says: a field of view that sees something, a cube's six faces, a part of the way.
    EXPECT_EQ(port("Perspective", "fieldOfView").min, 1.f);
    EXPECT_EQ(port("Perspective", "fieldOfView").max, 179.f);
    EXPECT_EQ(port("CubeFace", "face").max, 5.f);
    EXPECT_EQ(port("ShadowCascades", "lambda").max, 1.f);
    EXPECT_EQ(port("Repeat", "count").min, 1.f);
    // A whole in parts, and each part, alike.
    EXPECT_EQ(port("Image", "extent").min, 1.f);
    EXPECT_EQ(port("Image", "extent.y").min, 1.f);
    EXPECT_EQ(port("Dispatch", "groupSize.z").min, 1.f);
    EXPECT_EQ(port("Dispatch", "groups.x").min, 0.f);
    // What a builder takes unsigned is none or more; what nothing says of, anything.
    EXPECT_EQ(port("Image", "mipLevels").max, 16.f);
    EXPECT_EQ(port("Orthographic", "left").min, -any);
    EXPECT_EQ(port("Draw", "vertexOffset").max, any);
    EXPECT_EQ(port("Lerp", "t").min, -any);

    // How each is dragged: a size, a distance or a count by so much of it; a count, an index a unit at a time.
    EXPECT_TRUE(port("Image", "extent.x").logarithmic);
    EXPECT_TRUE(port("Image", "extent").logarithmic) << "the whole, as its parts";
    EXPECT_TRUE(port("Perspective", "near").logarithmic);
    EXPECT_GT(port("Perspective", "near").min, 0.f) << "a plane in front of the eye";
    EXPECT_FALSE(port("Perspective", "fieldOfView").logarithmic);
    EXPECT_FALSE(port("ShadowCascades", "lambda").logarithmic);
    EXPECT_TRUE(port("Image", "mipLevels").integer);
    EXPECT_TRUE(port("Repeat", "count").integer);
    EXPECT_TRUE(port("Blit", "sourceExtent.x").integer);
    EXPECT_FALSE(port("Orthographic", "size").integer);
    EXPECT_FALSE(port("Perspective", "near").integer);
    // What it was not said of before: a copy's levels and layers, a framebuffer's depth, a sampler's levels of detail.
    EXPECT_EQ(port("CopyBufferToImage", "mip").max, 15.f);
    EXPECT_EQ(port("Framebuffer", "depthStencil.depth").max, 1.f);
    EXPECT_EQ(port("Sampler", "mipLodBias").min, -16.f);
    // A field of a builder's struct: a stencil's mask, a depth range, a vertex attribute's numbers.
    EXPECT_EQ(kgraph::LimitsOf("KoralStencilOpState", "writeMask").max, 255.f);
    EXPECT_EQ(kgraph::LimitsOf("KoralDepthStencilState", "maxDepth").max, 1.f);
    EXPECT_EQ(kgraph::LimitsOf("KoralVertexAttribute", "channelCount").min, 1.f);
    EXPECT_EQ(kgraph::LimitsOf("KoralVertexAttribute", "location").min, -1.f) << "-1: matched by its semantic";
    EXPECT_EQ(kgraph::LimitsOf("KoralRasterizationState", "depthBiasSlopeFactor").min, -any);

    // Numbers and vectors of a kind: an unsigned one's none or more, and whole.
    EXPECT_EQ(port("UInt", "value").min, 0.f);
    EXPECT_TRUE(port("Int", "value").integer);
    EXPECT_FALSE(port("Float", "value").integer);
    EXPECT_EQ(port("UVec3", "z").min, 0.f);
    EXPECT_TRUE(port("IVec2", "y").integer);
    EXPECT_EQ(port("Vec4", "w").min, -any);
    EXPECT_EQ(kgraph::FindType("Number"), nullptr) << "a number is said of its kind: Float, Double, Int or UInt";
    EXPECT_EQ(kgraph::FindType("Vector"), nullptr);
    EXPECT_TRUE(kgraph::FindType("Vec3")->listed);
}

// Where each input is in an inspector, and when it does anything: a Draw's by what it is about, a builder's struct a part
// of its own; a Draw's indirect offset only for the indirect commands, a Sampler's anisotropy only while it is on.
TEST(Catalog, SaysWhereEachInputIsShownAndWhenItDoesAnything) {
    const auto port = [](const std::string_view type, const std::string_view name) {
        for (const auto& input : kgraph::FindType(type)->inputs)
            if (input.name == name) return input;
        ADD_FAILURE() << type << " has no " << name;
        return kgraph::Port {};
    };
    EXPECT_TRUE(port("Draw", "pipeline").group.empty()) << "what it cannot do without, at the top";
    EXPECT_TRUE(port("Draw", "command").group.empty());
    EXPECT_EQ(port("Draw", "indirectOffset").group, "Indirect");
    EXPECT_EQ(port("Draw", "stencilWriteMask").group, "Stencil");
    EXPECT_EQ(port("Draw", "tasks.y").group, "What is drawn");
    EXPECT_EQ(port("Draw", "tasks").group, "What is drawn") << "the whole, as its parts";
    EXPECT_EQ(port("GraphicsPipeline", "rasterizationState").group, "rasterizationState") << "a struct, a part of its own";
    EXPECT_EQ(port("Framebuffer", "color[].view").group, "color[]") << "what several share the start of the name of";
    EXPECT_TRUE(port("Image", "format").group.empty()) << "a few, all at the top";
    EXPECT_EQ(port("Sampler", "maxLod").group, "Level of detail");

    kgraph::Graph graph;
    auto& draw = graph.Add("draw", "Draw");
    EXPECT_FALSE(kgraph::Applies(port("Draw", "indirectOffset"), draw)) << "eAuto, as it starts, draws no indirect buffer";
    EXPECT_TRUE(kgraph::Applies(port("Draw", "vertexCount"), draw));
    EXPECT_TRUE(kgraph::Applies(port("Draw", "stencilReference"), draw)) << "the pipeline's stencil test, not known here: it might";
    draw.Set("command", "eDrawIndexedIndirectCount");
    EXPECT_TRUE(kgraph::Applies(port("Draw", "indirectOffset"), draw));
    EXPECT_TRUE(kgraph::Applies(port("Draw", "countOffset"), draw));
    EXPECT_FALSE(kgraph::Applies(port("Draw", "vertexCount"), draw));
    EXPECT_EQ(port("Draw", "vertexCount").when.what, "for draws of vertices") << "what an editor says of what it leaves out";
    draw.Set("stencilTest", false);
    EXPECT_FALSE(kgraph::Applies(port("Draw", "stencilReference"), draw));
    auto& sampler = graph.Add("sampler", "Sampler");
    EXPECT_FALSE(kgraph::Applies(port("Sampler", "maxAnisotropy"), sampler)) << "anisotropy starts off";
    sampler.Set("anisotropyEnable", true);
    EXPECT_TRUE(kgraph::Applies(port("Sampler", "maxAnisotropy"), sampler));
}

// Every default mesh: numbers that make a mesh, normals a unit long, each triangle facing the way its corners' normals
// do (counter-clockwise seen from there), and about a unit across.
TEST(DefaultMeshes, EveryOneFacesTheWayItsNormalsDoAndIsAboutAUnitAcross) {
    for (const auto& name : kgraph::DefaultMeshNames()) {
        const auto mesh = kgraph::ShapeData(name);
        ASSERT_TRUE(mesh.has_value()) << name;
        ASSERT_FALSE(mesh->vertices.empty()) << name;
        ASSERT_EQ(mesh->indices.size() % 3, 0u) << name;
        const kgraph::VertexData data = kgraph::ToVertexData(*mesh, kgraph::Tangents::eFour);
        EXPECT_TRUE(data.Problem().empty()) << name << ": " << data.Problem();
        float low[3] { 1e9f, 1e9f, 1e9f }, high[3] { -1e9f, -1e9f, -1e9f };
        for (const auto& v : mesh->vertices) {
            EXPECT_NEAR(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2], 1.f, 1e-4f) << name;
            for (int a = 0; a < 3; ++a) { low[a] = std::min(low[a], v.position[a]); high[a] = std::max(high[a], v.position[a]); }
        }
        for (int a = 0; a < 3; ++a) {
            EXPECT_GE(low[a], -0.65f) << name << ", axis " << a;
            EXPECT_LE(high[a], 0.65f) << name << ", axis " << a;
        }
        EXPECT_GT(std::max({ high[0] - low[0], high[1] - low[1], high[2] - low[2] }), 0.5f) << name;
        int wrong = 0;
        for (std::size_t i = 0; i < mesh->indices.size(); i += 3) {
            const auto& a = mesh->vertices[mesh->indices[i]], & b = mesh->vertices[mesh->indices[i + 1]], & c = mesh->vertices[mesh->indices[i + 2]];
            float e1[3], e2[3];
            for (int k = 0; k < 3; ++k) { e1[k] = b.position[k] - a.position[k]; e2[k] = c.position[k] - a.position[k]; }
            const float face[3] { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const float area = std::sqrt(face[0] * face[0] + face[1] * face[1] + face[2] * face[2]);
            if (area < 1e-9f) continue;     // a band's at a pole: nothing to face
            float facing = 0.f;
            for (int k = 0; k < 3; ++k) facing += face[k] * (a.normal[k] + b.normal[k] + c.normal[k]);
            if (facing <= 0.f) ++wrong;
        }
        EXPECT_EQ(wrong, 0) << name << ": triangles facing away from their normals";
    }
    const kgraph::VertexData skinned = kgraph::ToVertexData(*kgraph::ShapeData("skinnedCylinder"));
    ASSERT_NE(skinned.Find("BONE_IDS"), nullptr);
    EXPECT_EQ(skinned.Find("BONE_IDS")->type, kgraph::AttributeType::eUInt);
    ASSERT_NE(skinned.Find("BONE_WEIGHTS"), nullptr);
    EXPECT_EQ(kgraph::ToVertexData(*kgraph::ShapeData("teapot")).Find("BONE_IDS"), nullptr) << "only the skinned one has bones";
}

// Each pass runs as often as its rate says, and the passes are in the order of their rates: setup, fixed, every frame.
TEST(PassRates, ArePutInTheOrderTheyRunAndSetOneAtATime) {
    kgraph::Graph graph;
    graph.passes = { "draw", "simulate", "noise", "post" };
    graph.setup = { "noise" };
    graph.fixed = { "simulate", "gone", "noise" };
    graph.OrderByRate();
    EXPECT_EQ(graph.passes, (std::vector<std::string> { "noise", "simulate", "draw", "post" }));
    EXPECT_EQ(graph.fixed, (std::vector<std::string> { "simulate" })) << "what is no pass, or is setup, is forgotten";
    EXPECT_EQ(graph.RateOf("noise"), kgraph::PassRate::eSetup);
    EXPECT_EQ(graph.RateOf("simulate"), kgraph::PassRate::eFixed);
    EXPECT_EQ(graph.RateOf("draw"), kgraph::PassRate::eFrame);
    EXPECT_EQ(graph.RateOf(""), kgraph::PassRate::eFrame) << "a node in no pass runs every frame";

    ASSERT_TRUE(graph.SetRate("post", kgraph::PassRate::eFixed));
    EXPECT_EQ(graph.passes, (std::vector<std::string> { "noise", "simulate", "post", "draw" }));
    ASSERT_TRUE(graph.SetRate("noise", kgraph::PassRate::eFrame));
    EXPECT_TRUE(graph.setup.empty());
    EXPECT_FALSE(graph.SetRate("nothing", kgraph::PassRate::eSetup));
    EXPECT_EQ(kgraph::PassRateName(kgraph::PassRate::eFixed), "fixed");

    // A graph saved before there was a fixed step reads as it did.
    kgraph::Graph old;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(R"({ "kgraph": 1, "setup": ["a"], "passes": ["b"], "nodes": [] })", old, error)) << error;
    EXPECT_EQ(old.passes, (std::vector<std::string> { "a", "b" }));
    EXPECT_TRUE(old.fixed.empty());
}

// Every node type is of a level — what one works at, from a value to a whole technique — and a Function's call is the
// one made of other nodes.
TEST(Catalog, EveryNodeIsOfALevelAndAFunctionsCallIsOfNodes) {
    using kgraph::Level;
    const auto level = [](const char* type) { const kgraph::NodeType* found = kgraph::FindType(type); return found ? found->level : Level::eValue; };
    EXPECT_EQ(level("Double"), Level::eValue);
    EXPECT_EQ(level("MatrixMultiply"), Level::eValue);
    EXPECT_EQ(level("Perspective"), Level::eValue);
    EXPECT_EQ(level("Switch"), Level::eValue);
    EXPECT_EQ(level("Image"), Level::eResource);
    EXPECT_EQ(level("GraphicsPipeline"), Level::eResource);
    EXPECT_EQ(level("Sampler"), Level::eResource);
    EXPECT_EQ(level("Draw"), Level::eCommand);
    EXPECT_EQ(level("Dispatch"), Level::eCommand);
    EXPECT_EQ(level("TraceRays"), Level::eCommand);
    EXPECT_EQ(level("Filter"), Level::eScene);
    EXPECT_EQ(level("MeshPack"), Level::eScene);
    EXPECT_EQ(level("RayScene"), Level::eScene);
    EXPECT_EQ(level("Meshlets"), Level::eScene);
    EXPECT_EQ(level("Function"), Level::eTechnique);
    for (const auto& type : kgraph::Catalog()) EXPECT_EQ(type.composite, type.name == "Function") << type.name;
    EXPECT_EQ(kgraph::LevelName(Level::eScene), "scene");
}
