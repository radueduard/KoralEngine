// Material kinds: the engine's package's PbrMaterial and BlinnPhongMaterial, declared as their scripts say, made
// components of the world whose pictures start as the pictures their kind names, and a Slang module of each that a
// shader imports. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <kgraph/entities.h>
#include <kgraph/materials.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;

    /** A Material script's type as kgraph keeps it: what the editor hands the runtime. */
    kgraph::MaterialKind KindOf(kscript::ScriptType type)
    {
        return { .name = std::move(type.name), .fields = std::move(type.fields), .size = type.size, .alignment = type.alignment,
                 .initial = std::move(type.initial), .textures = std::move(type.textures), .declaredIn = "the package" };
    }

    class MaterialTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "materials", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
            directory = std::filesystem::temp_directory_path() / "koral_material_test";
            std::filesystem::remove_all(directory);
            scene->runtime->SetGeneratedShaderDirectory(directory);
            std::vector<kscript::ScriptType> materials;
            kecs::World scratch;
            kscript::Scripts scripts(scratch);
            for (const auto& problem : kscript::DeclarePackage(scripts, &materials)) ADD_FAILURE() << problem;
            for (auto& type : materials) {
                std::string error;
                EXPECT_NE(scene->runtime->DeclareMaterialKind(KindOf(std::move(type)), &error), kecs::NoComponent) << error;
            }
            (void)scene->runtime->MaterialNotes();  // written by now, for what reads the files
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
        [[nodiscard]] std::string Read(const std::filesystem::path& file) const {
            std::ifstream in(file);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        GraphScene* scene = nullptr;
        std::filesystem::path directory;
    };
}

// A kind is a component of the world: a new one's pictures are the pictures its kind names — the runtime's, a texel each
// — and its numbers what its script initialises them to. A copy shares its pictures; one taken away lets them go.
TEST_F(MaterialTest, AKindIsAComponentWhosePicturesStartAsItSays) {
    auto& world = scene->world;
    ASSERT_EQ(scene->runtime->MaterialKinds().size(), 2u);
    EXPECT_NE(scene->runtime->MaterialKindOf("PbrMaterial"), nullptr);
    EXPECT_NE(scene->runtime->MaterialKindOf("BlinnPhongMaterial"), nullptr);
    const kecs::ComponentId pbr = world.Find("PbrMaterial");
    ASSERT_NE(pbr, kecs::NoComponent);

    const kecs::Entity entity = world.Create();
    world.Add(entity, pbr);
    kgraph::Parameters material(world, entity, pbr);
    ASSERT_TRUE(material.Valid());
    const auto white = scene->runtime->ResourceOf("$white"), normal = scene->runtime->ResourceOf("$normal");
    ASSERT_NE(white, nullptr);
    EXPECT_EQ(material.ResourceAt("baseColorMap"), white);
    EXPECT_EQ(material.ResourceAt("normalMap"), normal);
    EXPECT_EQ(support::FieldOf(world, entity, "PbrMaterial", "roughness"), std::vector<float> { 0.5f });
    EXPECT_EQ(support::FieldOf(world, entity, "PbrMaterial", "baseColor"), (std::vector<float> { 1.f, 1.f, 1.f, 1.f }));

    const long held = white.use_count();
    const kecs::Entity copy = world.Create();
    world.Add(copy, pbr, world.Get(entity, pbr));
    EXPECT_EQ(kgraph::Parameters(world, copy, pbr).ResourceAt("baseColorMap"), white) << "a copy holds the same picture";
    EXPECT_GT(white.use_count(), held) << "its four white pictures are held twice";
    world.Destroy(copy);
    world.Destroy(entity);
    Frame();
    EXPECT_LT(white.use_count(), held) << "the pictures are let go of with what held them";
}

// A project opened again: what its kinds were written as last time is there, and its own kind is declared after the
// engine's. Nothing is written or taken away on the way, or every shader that imports them is built again, every time.
TEST_F(MaterialTest, KindsDeclaredOneAfterAnotherLeaveWhatIsWrittenAlone) {
    kgraph::MaterialKind project = scene->runtime->MaterialKinds().front();
    project.name = "ProjectMaterial";
    std::vector<kgraph::MaterialKind> all = scene->runtime->MaterialKinds();
    all.push_back(project);
    std::ranges::sort(all, {}, &kgraph::MaterialKind::name);
    const auto opened = directory / "opened again";
    EXPECT_TRUE(kgraph::WriteMaterialModules(opened, all).empty());
    const auto longAgo = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    for (const auto& file : std::filesystem::directory_iterator(opened)) std::filesystem::last_write_time(file.path(), longAgo);

    scene->runtime->SetGeneratedShaderDirectory(opened);
    EXPECT_EQ(std::filesystem::last_write_time(opened / "ProjectMaterial.slang"), longAgo) << "not taken away for not being declared yet";
    std::string error;
    EXPECT_NE(scene->runtime->DeclareMaterialKind(project, &error), kecs::NoComponent) << error;
    Frame();
    for (const char* file : { "ProjectMaterial.slang", "PbrMaterial.slang", "koral_materials.slang" })
        EXPECT_EQ(std::filesystem::last_write_time(opened / file), longAgo) << file;
}

// What a kind is to a shader: a module of its name with a struct of it — a picture as its bindless index — and the
// attribute a shader takes one of its fields by; `koral_materials` imports them all. A shader that imports it is built.
TEST_F(MaterialTest, EachKindHasAModuleAShaderImports) {
    const std::string pbr = Read(directory / "PbrMaterial.slang");
    EXPECT_NE(pbr.find("module PbrMaterial;"), std::string::npos) << pbr;
    EXPECT_NE(pbr.find("public struct PbrMaterialAttribute"), std::string::npos) << pbr;
    EXPECT_NE(pbr.find("[PbrMaterial(\"roughness\")] public float roughness;"), std::string::npos) << pbr;
    EXPECT_NE(pbr.find("[PbrMaterial(\"baseColorMap\")] public uint baseColorMap;"), std::string::npos) << pbr;
    EXPECT_NE(pbr.find("[PbrMaterial(\"doubleSided\")] public uint doubleSided;"), std::string::npos) << "true or false is a uint: " << pbr;
    const std::string all = Read(directory / "koral_materials.slang");
    EXPECT_NE(all.find("__exported import BlinnPhongMaterial;"), std::string::npos) << all;
    EXPECT_NE(all.find("__exported import PbrMaterial;"), std::string::npos) << all;
    for (const auto& note : scene->runtime->MaterialNotes()) ADD_FAILURE() << note;

    kgraph::Graph graph;
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 8).Set("extent.y", 8).Set("usage", kgraph::Value::List { "eStorage" });
    graph.Add("read", "ComputePipeline").Set("compute.module", "materialKinds").Set("compute.entry", "computeMain").Wire("param.target", "target");
    scene->runtime->Set(std::move(graph));
    const auto* status = scene->runtime->Status("read");
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->ok) << status->error;
}

// A kind no longer there takes its module with it; a field of no GPU form is left out of the module, and said.
TEST_F(MaterialTest, AFieldOfNoGpuFormIsLeftOutAndSaid) {
    kgraph::MaterialKind kind { .name = "Linked", .fields = { { .name = "other", .type = "entity", .offset = 0, .size = 8 }, { .name = "weight", .type = "f32", .offset = 8, .size = 4 } },
                                .size = 12, .alignment = 4 };
    const auto module = kgraph::MaterialModuleOf(kind);
    EXPECT_EQ(module.source.find("other;"), std::string::npos) << module.source;
    EXPECT_NE(module.source.find("public float weight;"), std::string::npos) << module.source;
    ASSERT_EQ(module.left.size(), 1u);
    EXPECT_NE(module.left.front().find("Linked.other"), std::string::npos);

    std::string error;
    EXPECT_EQ(scene->runtime->DeclareMaterialKind({ .name = "PointLight" }, &error), kecs::NoComponent) << "a component that is no kind's keeps its name";
    EXPECT_FALSE(error.empty());
}

namespace {
    /** A graph that draws every entity with a Mesh, batched, its instances' fields as @p module 's shader says. */
    kgraph::Graph DrawnWith(const std::string& module)
    {
        using kgraph::Value;
        kgraph::Graph graph;
        graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
            .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
        graph.Add("canvas view", "ImageView").Wire("image", "canvas");
        graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
        graph.Add("all", "Entities");
        graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
        graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
        graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
        graph.Add("lit", "GraphicsPipeline").Set("vertex.module", module).Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
            .Set("fragment.module", module).Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
        graph.Add("draw", "Draw").Wire("pipeline", "lit").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
            .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
        graph.Add("screen", "Screen");
        graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
        return graph;
    }
}

// A shader takes a field of an entity's material by annotating its own member: the Draw fills it from the entity's
// material of that kind — a picture by its index — and an entity without one is not drawn, until it has one.
TEST_F(MaterialTest, AShaderTakesWhatItAnnotatesOfAnEntitysMaterial) {
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    const kecs::ComponentId pbr = world.Find("PbrMaterial");
    std::vector<kecs::Entity> entities;
    for (const float x : { -0.5f, 0.5f }) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, 0, 0.5f });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { cube });
        entities.push_back(entity);
    }
    world.Add(entities[0], pbr);
    kgraph::Parameters(world, entities[0], pbr).Set("baseColor", kor::Vec4 { 1, 0, 0, 1 });     // its roughness is 0.5: as it is

    runtime.Set(DrawnWith("materialDrawn"));
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.MaterialsOf("lit"), std::vector<std::string> { "PbrMaterial" });
    EXPECT_EQ(runtime.ComponentOf("lit"), kecs::NoComponent) << "what the shader takes of the material is no field of the pipeline's own";

    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(8, { 255, 0, 0 })) << "the material's colour, by its roughness, with a picture";
    EXPECT_TRUE(is(24, { 0, 0, 0 })) << "an entity without a PbrMaterial is not drawn";

    world.Add(entities[1], pbr);
    kgraph::Parameters(world, entities[1], pbr).Set("baseColor", kor::Vec4 { 0, 1, 0, 1 }).Set("roughness", 0.25f);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(24, { 0, 128, 0 })) << "given one, it is";

    // Changed, it is drawn as it is now; taken away, it is not drawn.
    kgraph::Parameters(world, entities[0], pbr).Set("baseColor", kor::Vec4 { 0, 0, 1, 1 });
    world.Remove(entities[1], pbr);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(8, { 0, 0, 255 }));
    EXPECT_TRUE(is(24, { 0, 0, 0 }));
}

// What a kind has not, or has as another type, its pipeline says: which member, which field, and what it is.
TEST_F(MaterialTest, AMemberThatDoesNotFitItsFieldIsSaid) {
    kgraph::Runtime& runtime = *scene->runtime;
    runtime.Set(DrawnWith("materialWrong"));
    Frame();
    const auto* status = runtime.Status("lit");
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(status->ok);
    EXPECT_NE(status->error.find("shininess"), std::string::npos) << status->error;
    EXPECT_NE(status->error.find("PbrMaterial"), std::string::npos) << status->error;
}
