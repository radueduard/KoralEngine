// The scene the editor's tests build: E4's, made a step at a time as someone at the editor would make it. Four
// entities, each with an image of its own that a compute node fills with the entity's colour and a graphics node
// draws in the entity's quarter of the picture.

#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <keditor/document.h>
#include <keditor/project.h>

namespace scene
{
    using kgraph::Value;

    inline const std::array<std::array<double, 4>, 4> Tints { { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 1, 1, 0, 1 }, { 0, 1, 1, 1 } } };
    /** What each quarter of the picture comes to: the tint (the third at half gain), over the backdrop's blue. */
    inline const std::array<std::array<int, 3>, 4> Colors { { { 255, 0, 64 }, { 0, 255, 64 }, { 128, 128, 64 }, { 0, 255, 255 } } };

    /** A project in a folder of its own under the temporary directory, with the tests' shaders in it. */
    inline keditor::Project MakeProject(const std::string& name)
    {
        const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / name;
        std::filesystem::remove_all(directory);
        keditor::Project project;
        std::string error;
        EXPECT_TRUE(keditor::Project::Create(directory, name, project, error)) << error;
        for (const char* shader : { "stamp.slang", "show.slang" })
            std::filesystem::copy_file(std::filesystem::path(KEDITOR_TEST_SHADERS) / shader, project.ShaderDirectory() / shader);
        project.Use();
        return project;
    }

    /** The graph: every node added, set and wired through the document, as the editor's panels do it. */
    inline void BuildGraph(keditor::Document& doc)
    {
        const auto image = [&](const std::string& id, const int size, Value::List usage, const double x, const double y) {
            EXPECT_EQ(doc.AddNode("Image", x, y, id), id);
            doc.SetInput(id, "format", "eRGBA8_UNORM");
            doc.SetInput(id, "extent.x", size);
            doc.SetInput(id, "extent.y", size);
            doc.SetInput(id, "extent.z", 1);
            doc.SetInput(id, "usage", std::move(usage));
        };
        image("backdrop", 4, { "eSampled", "eTransferDst" }, 0, 0);
        doc.AddNode("Clear", 200, 0, "tint backdrop");
        doc.Connect("backdrop", "out", "tint backdrop", "image");
        doc.SetInput("tint backdrop", "color", Value::List { 0, 0, 0.25, 0 });

        // A pipeline names its shader: the module, and the entry point in it.
        doc.AddNode("ComputePipeline", 200, 120, "stamp");
        doc.SetInput("stamp", "compute.module", "stamp");
        doc.SetInput("stamp", "compute.entry", "computeMain");
        doc.Connect("backdrop", "out", "stamp", "param.backdrop");
        doc.AddNode("Dispatch", 400, 120, "run");
        doc.Connect("stamp", "out", "run", "pipeline");
        doc.SetInput("run", "cover", "target");
        doc.SetInput("run", "groupSize.x", 8);
        doc.SetInput("run", "groupSize.y", 8);
        doc.SetInput("run", "with", Value::List { "Lit" });

        image("canvas", 32, { "eColorAttachment", "eTransferSrc" }, 0, 300);
        doc.AddNode("ImageView", 200, 300, "canvas view");
        doc.Connect("canvas", "out", "canvas view", "image");
        doc.AddNode("Framebuffer", 400, 300, "target");
        doc.SetInput("target", "color[0].name", "color");
        doc.Connect("canvas view", "out", "target", "color[0].view");
        doc.AddNode("Primitive", 0, 460, "square");
        doc.SetInput("square", "shape", "eQuad");
        doc.AddNode("GraphicsPipeline", 400, 460, "show");
        doc.SetInput("show", "vertex.module", "show");
        doc.SetInput("show", "vertex.entry", "vertexMain");
        doc.Connect("square", "layout", "show", "vertex.layout");
        doc.SetInput("show", "fragment.module", "show");
        doc.SetInput("show", "fragment.entry", "fragmentMain");
        doc.Connect("target", "out", "show", "framebuffer");
        doc.AddNode("Draw", 600, 460, "draw");
        doc.Connect("show", "out", "draw", "pipeline");
        doc.SetInput("draw", "without", Value::List { "Hidden" });

        doc.AddNode("Screen", 600, 300, "screen");
        doc.AddNode("Blit", 800, 300, "blit");
        doc.Connect("canvas", "out", "blit", "source");
        doc.Connect("screen", "out", "blit", "destination");

        for (int i = 0; i < 4; ++i) image(std::format("image {}", i), i == 1 ? 16 : 8, { "eStorage", "eSampled" }, 800, 460 + i * 80.0);
    }

    /** The entities: each named, given its components, and its fields set one at a time. */
    inline std::array<kecs::Entity, 4> BuildEntities(keditor::Document& doc)
    {
        doc.AddTag("Lit");
        doc.AddTag("Hidden");
        std::array<kecs::Entity, 4> entities;
        for (int i = 0; i < 4; ++i) {
            const kecs::Entity entity = entities[static_cast<std::size_t>(i)] = doc.CreateEntity(std::format("Quarter {}", i));
            for (const char* component : { "stamp", "show", "Mesh", "Lit" }) EXPECT_TRUE(doc.AddComponent(entity, component)) << component;
            std::string error;
            const std::string image = std::format("image {}", i);
            const auto& tint = Tints[static_cast<std::size_t>(i)];
            EXPECT_TRUE(doc.SetField(entity, "stamp", "target", image, &error)) << error;
            EXPECT_TRUE(doc.SetField(entity, "stamp", "tint", Value::List { tint[0], tint[1], tint[2], tint[3] }, &error)) << error;
            EXPECT_TRUE(doc.SetField(entity, "stamp", "gain", i == 2 ? 0.5 : 1.0, &error)) << error;
            EXPECT_TRUE(doc.SetField(entity, "show", "source", image, &error)) << error;
            EXPECT_TRUE(doc.SetField(entity, "show", "offset", Value::List { i % 2 ? 0.5 : -0.5, i / 2 ? 0.5 : -0.5 }, &error)) << error;
            EXPECT_TRUE(doc.SetField(entity, "Mesh", "mesh", "square", &error)) << error;
        }
        return entities;
    }
}
