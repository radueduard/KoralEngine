// What a pass can be asked to do: every way the framework draws, dispatches and copies has a node that says it.

#include <gtest/gtest.h>
#include <log.h>

#include <algorithm>
#include <string>
#include <vector>

#include <capiInterop.h>
#include <debugDraw.h>
#include <koral_c.h>
#include <sampler.h>

#include <kgraph/catalog.h>
#include <kgraph/graph.h>
#include <kgraph/runtime.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using support::Size;
    using kgraph::Value;

    class DrawTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = false;   // nothing here is an entity's: every pass runs once
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "draw", .extent = { Size, Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }
        /** A picture to draw into, a framebuffer of it, and the picture shown. */
        static kgraph::Graph Canvas(const char* samples = "e1")
        {
            kgraph::Graph graph;
            graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { Size, Size }).Set("sampleCount", samples)
                .Set("usage", Value::List { "eColorAttachment", "eTransferSrc", "eTransferDst", "eSampled" });
            graph.Add("canvas view", "ImageView").Wire("image", "canvas");
            graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
            graph.Add("screen", "Screen");
            return graph;
        }
        static kgraph::Node& Triangles(kgraph::Graph& graph)
        {
            return graph.Add("triangles", "GraphicsPipeline").Set("vertex.module", "triangle").Set("vertex.entry", "vertexMain")
                .Set("fragment.module", "triangle").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
        }
        void Run(const kgraph::Graph& graph)
        {
            scene->runtime->Set(graph);
            for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
            Frame();
            Frame();
        }
        [[nodiscard]] bool Green(const kor::u32 x, const kor::u32 y) const { const auto p = scene->Pixel(x, y); return p.x < 8 && p.y > 247 && p.z < 8; }
        [[nodiscard]] bool Black(const kor::u32 x, const kor::u32 y) const { const auto p = scene->Pixel(x, y); return p.x < 8 && p.y < 8 && p.z < 8; }
        GraphScene* scene = nullptr;
    };
}

// A triangle, from a vertex shader that makes its own vertices: no mesh, no vertex layout, no entity.
TEST_F(DrawTest, ATriangleIsDrawnWithoutAMesh) {
    kgraph::Graph graph = Canvas();
    Triangles(graph);
    graph.Add("draw", "Draw").Wire("pipeline", "triangles");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_EQ(scene->runtime->ReportOf("draw"), nullptr) << "it draws once: nothing it is given follows a ForEach";
    EXPECT_TRUE(Green(3, 3)) << "above the diagonal, the triangle";
    EXPECT_TRUE(Black(28, 28)) << "below it, what the framebuffer was cleared to";

    // As many vertices as it is told: six are both triangles.
    graph.Find("draw")->Set("vertexCount", 6);
    Run(graph);
    EXPECT_TRUE(Green(3, 3));
    EXPECT_TRUE(Green(28, 28));
    // From the fourth on: the second triangle alone.
    graph.Find("draw")->Set("vertexCount", 3).Set("firstVertex", 3);
    Run(graph);
    EXPECT_TRUE(Black(3, 3));
    EXPECT_TRUE(Green(28, 28));

    // Which entities is a ForEach's to say, not a Draw's.
    graph.Find("draw")->Set("with", Value::List { "Lit" });
    scene->runtime->Set(graph);
    ASSERT_FALSE(scene->runtime->Status("draw")->ok);
    EXPECT_NE(scene->runtime->Status("draw")->error.find("a ForEach's now"), std::string::npos) << scene->runtime->Status("draw")->error;
}

// A setting given in part has the rest as its inputs start: a depth attachment given only its view is cleared to 1,
// the farthest, so that what is drawn with a depth test passes it — not to the builder's zero, which nothing passes.
TEST_F(DrawTest, ADepthAttachmentGivenOnlyItsViewClearsToTheFarthest) {
    kgraph::Graph graph = Canvas();
    graph.Add("depth", "Image").Set("format", "eD32_SFLOAT").Set("extent", Value::List { Size, Size }).Set("usage", Value::List { "eDepthStencilAttachment" });
    graph.Find("target")->Wire("depthStencil.view", "depth");
    Triangles(graph);
    graph.Add("draw", "Draw").Wire("pipeline", "triangles");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_TRUE(Green(3, 3)) << "the triangle passed the depth test";
    EXPECT_TRUE(Black(28, 28));
}

// So does each blend attachment given in part: one that says only how it blends still writes every channel, as the
// framework's attachment does — not none, which drew glass that left the picture as it was.
TEST_F(DrawTest, ABlendAttachmentGivenOnlyItsFactorsWritesEveryChannel) {
    kgraph::Graph graph = Canvas();
    Triangles(graph);
    graph.Find("triangles")->Set("colorBlendState", Value::Members { { "attachments", Value::List { Value::Members {
        { "blendEnable", true }, { "srcColorBlendFactor", "eSrcAlpha" }, { "dstColorBlendFactor", "eOneMinusSrcAlpha" } } } } });
    graph.Add("draw", "Draw").Wire("pipeline", "triangles");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_TRUE(Green(3, 3)) << "the triangle, blended over the clear";
}

// What the pipeline leaves to the draw is the draw's to say: where it draws, and what it clears to.
TEST_F(DrawTest, ADrawSaysWhereItDrawsAndWhatItClearsTo) {
    kgraph::Graph graph = Canvas();
    Triangles(graph);
    graph.Add("draw", "Draw").Wire("pipeline", "triangles").Set("vertexCount", 6)
        .Set("scissor", Value::List { 0, 0, 16, 32 }).Set("clearColor", Value::List { 0, 0, 1, 1 });
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_TRUE(Green(3, 28)) << "inside the scissor";
    const auto outside = scene->Pixel(28, 3);
    EXPECT_GT(outside.z, 247) << "outside it, the colour this draw clears to";
    EXPECT_LT(outside.y, 8);

    // A second draw over the first, which it loads: the rest is filled in.
    graph.Add("more", "Draw").Wire("pipeline", "triangles").Set("vertexCount", 6).Set("colorLoad", "eLoad").Set("viewport", Value::List { 16, 0, 16, 32 });
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_TRUE(Green(3, 28));
    EXPECT_TRUE(Green(28, 3)) << "the second draw, squeezed into the right half";

    // Nothing drawn: only what it clears to.
    graph.nodes.erase(std::ranges::find(graph.nodes, "more", &kgraph::Node::id));
    graph.Find("draw")->Set("rasterizerDiscard", true);
    Run(graph);
    EXPECT_GT(scene->Pixel(3, 28).z, 247) << "with the rasterizer discarding, nothing is drawn over the clear";
    EXPECT_LT(scene->Pixel(3, 28).y, 8);

    graph.Find("draw")->Set("cullMode", "eSideways");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("draw")->error.find("'cullMode' is one of eNone, eFront, eBack, eFrontAndBack"), std::string::npos) << scene->runtime->Status("draw")->error;
}

// One mesh wired to the draw: one draw of it.
TEST_F(DrawTest, AMeshWiredInIsDrawnOnce) {
    kgraph::Graph graph = Canvas();
    graph.Add("square", "Primitive").Set("shape", "eQuad");
    graph.Add("flat", "GraphicsPipeline").Set("vertex.module", "flat").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "square", "layout")
        .Set("fragment.module", "flat").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "flat").Wire("mesh", "square");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    const auto middle = scene->Pixel(16, 16);
    EXPECT_GT(middle.x, 247) << "the square, in the middle";
    EXPECT_GT(middle.y, 247);
    EXPECT_TRUE(Black(1, 1));

    // Without a mesh there is nothing for a vertex shader that reads vertices to read: it says what to wire.
    graph.Find("draw")->inputs.erase("mesh");
    scene->runtime->Set(graph);
    ASSERT_FALSE(scene->runtime->Status("draw")->ok);
    EXPECT_NE(scene->runtime->Status("draw")->error.find("wire it a 'mesh'"), std::string::npos) << scene->runtime->Status("draw")->error;
}

// What to draw, and how many groups to run, read from a buffer a compute shader wrote.
TEST_F(DrawTest, DrawsAndDispatchesAreReadFromABuffer) {
    kgraph::Graph graph = Canvas();
    Triangles(graph);
    graph.Add("arguments", "Buffer").Set("instanceCount", 32).Set("usage", Value::List { "eStorage", "eIndirect" });
    graph.Add("writer", "ComputePipeline").Set("compute.module", "args").Set("compute.entry", "computeMain").Wire("param.draws", "arguments");
    graph.Add("write", "Dispatch").Wire("pipeline", "writer");
    graph.Add("draw", "Draw").Wire("pipeline", "triangles").Set("command", "eDrawIndirect").Wire("indirect", "arguments");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    const auto schedule = scene->Schedule();
    EXPECT_LT(std::ranges::find(schedule, "write") - schedule.begin(), std::ranges::find(schedule, "draw") - schedule.begin()) << "what is read is written first";
    EXPECT_TRUE(Black(3, 3));
    EXPECT_TRUE(Green(28, 28)) << "three vertices from the fourth on, as the buffer says";

    // As many as another number in the buffer says, where the device counts draws itself.
    graph.Find("draw")->Set("command", "eDrawIndirectCount").Wire("count", "arguments").Set("countOffset", 28).Set("drawCount", 4);
    scene->runtime->Set(graph);
    ASSERT_TRUE(scene->runtime->Status("draw")->ok) << scene->runtime->Status("draw")->error;

    // An indirect command with no buffer says so; a buffer given another usage is made to be drawn from besides.
    graph.Find("draw")->Set("command", "eDrawIndirect");
    graph.Find("draw")->inputs.erase("count");
    graph.Find("draw")->inputs.erase("indirect");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("draw")->error.find("'indirect' needs a buffer wired in"), std::string::npos) << scene->runtime->Status("draw")->error;
    graph.Add("plain", "Buffer").Set("instanceCount", 32).Set("usage", Value::List { "eStorage" });
    graph.Find("draw")->Wire("indirect", "plain");
    scene->runtime->Set(graph);
    EXPECT_TRUE(scene->runtime->Status("draw")->ok) << scene->runtime->Status("draw")->error;

    // A dispatch whose groups are in the buffer: 4 by 4 of 8 by 8 cover the picture.
    kgraph::Graph fill = Canvas();
    fill.Find("canvas")->Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    fill.nodes.erase(std::ranges::find(fill.nodes, "target", &kgraph::Node::id));
    fill.Add("arguments", "Buffer").Set("instanceCount", 32).Set("usage", Value::List { "eStorage", "eIndirect" });
    fill.Add("writer", "ComputePipeline").Set("compute.module", "args").Set("compute.entry", "computeMain").Wire("param.draws", "arguments");
    fill.Add("write", "Dispatch").Wire("pipeline", "writer");
    fill.Add("clear", "Clear").Wire("image", "canvas").Set("color", Value::List { 1, 0, 0, 1 });
    fill.Add("filler", "ComputePipeline").Set("compute.module", "fill").Set("compute.entry", "computeMain").Wire("param.target", "canvas").Set("constant.level", 1);
    fill.Add("run", "Dispatch").Wire("pipeline", "filler").Wire("indirect", "arguments").Set("indirectOffset", 16);
    fill.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(fill);
    ASSERT_TRUE(scene->runtime->Ok());
    for (const auto [x, y] : { std::pair { 1u, 1u }, std::pair { 30u, 30u } }) {
        EXPECT_NEAR(scene->Pixel(x, y).x, 128, 1) << "the clear's red, halved by the shader";
        EXPECT_GT(scene->Pixel(x, y).y, 247);
    }
}

// What a script writes into a Buffer node each frame, copied into an Image node by a pass and drawn among the debug
// shapes: no pass wires the picture to the DebugDraw node, so the copy has to run, and be seen, all the same.
TEST_F(DrawTest, ADebugImageShowsWhatAScriptPutInABuffer) {
    kgraph::Graph graph = Canvas();
    graph.Add("clear", "Clear").Wire("image", "canvas").Set("color", Value::List { 0, 0, 0, 1 });
    graph.Add("bytes", "Buffer").Set("instanceCount", 16).Set("usage", Value::List { "eTransferSrc" });
    graph.Add("picture", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 2, 2 }).Set("usage", Value::List { "eSampled" });
    graph.Add("upload", "CopyBufferToImage").Wire("buffer", "bytes").Wire("image", "picture");
    graph.Add("debug", "DebugDraw").Wire("image", "canvas");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");

    kor::DebugDraw draw;
    scene->runtime->SetDebugDraw(&draw);
    scene->runtime->ReachedFromTheCpu("bytes");     // what handing a script the buffer does
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());

    const auto picture = scene->runtime->ImageOf("picture");
    ASSERT_TRUE(picture.Valid());
    const auto nearest = kor::Sampler::Builder {}.SetMinFilter(kor::Filter::eNearest).SetMagFilter(kor::Filter::eNearest).Build();
    draw.BeginLogicSpace({ 1.f, 1.f });
    draw.Image({ 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, picture, nearest, {.outline = false});
    draw.EndLogicSpace();
    EXPECT_EQ(draw.ImageCount(), 1u);

    const auto write = [&](const std::array<std::uint8_t, 16>& texels) {
        const auto bytes = scene->runtime->ResourceOf("bytes");
        ASSERT_NE(bytes, nullptr);
        EXPECT_EQ(koral_buffer_write(bytes->handle, texels.data(), texels.size(), 0), 0) << koral_last_error();
    };
    write({ 255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   255, 255, 255, 255 });
    Frame();
    Frame();
    Frame();
    const auto is = [&](const kor::u32 x, const kor::u32 y, const int r, const int g, const int b) {
        const auto p = scene->Pixel(x, y);
        EXPECT_EQ(std::tuple(int(p.x), int(p.y), int(p.z)), std::tuple(r, g, b)) << x << ", " << y;
    };
    is(2, 2, 255, 0, 0);
    is(Size - 3, 2, 0, 255, 0);
    is(2, Size - 3, 0, 0, 255);
    is(Size - 3, Size - 3, 255, 255, 255);

    // And again, as a script does every frame: the same draw shows what the buffer holds now.
    write({ 0, 255, 0, 255,   0, 255, 0, 255,   0, 255, 0, 255,   0, 255, 0, 255 });
    Frame();
    Frame();
    Frame();
    is(2, 2, 0, 255, 0);
    EXPECT_EQ(scene->Schedule(), (std::vector<std::string> { "clear", "upload", "debug", "show", "Read screen" })) << "the copy, and it before the draw";
    scene->runtime->SetDebugDraw(nullptr);
}

// An image a script holds, no node of any graph: its staging buffer written and copied in the frame's update, a copy of
// its own, and drawn among the debug shapes as a script draws it, anew each frame.
TEST_F(DrawTest, ADebugImageAScriptHoldsShowsWhatItCopiedInThatUpdate) {
    kgraph::Graph graph = Canvas();
    graph.Add("clear", "Clear").Wire("image", "canvas").Set("color", Value::List { 0, 0, 0, 1 });
    graph.Add("debug", "DebugDraw").Wire("image", "canvas");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");

    kor::DebugDraw draw;
    scene->runtime->SetDebugDraw(&draw);
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());

    auto picture = kor::Image::Builder {}.SetType(kor::Image::Type::e2D).SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2 { 2, 2 })
        .SetUsage(kor::Image::Usage::eTransferDst | kor::Image::Usage::eSampled).Build();
    auto staging = kor::Buffer::RawBuilder {}.SetRawSize(16).SetUsage(kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eStaging).Build();
    const auto nearest = kor::Sampler::Builder {}.SetMinFilter(kor::Filter::eNearest).SetMagFilter(kor::Filter::eNearest).Build();

    const auto update = [&](const std::array<std::uint8_t, 16>& texels) {
        scene->afterUpdate = [&, texels] {
            staging->Write(std::span<const std::uint8_t>(texels), 0);
            kor::CommandBuffer::SingleTimeCommand([&](kor::CommandBuffer& cb) { cb.CopyBufferToImage(staging, picture); }).Wait();
            draw.Clear();
            draw.BeginLogicSpace({ 1.f, 1.f });
            draw.Image({ 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, picture, nearest, {.outline = false});
            draw.EndLogicSpace();
        };
        Frame();
        Frame();
    };
    const auto is = [&](const kor::u32 x, const kor::u32 y, const int r, const int g, const int b) {
        const auto p = scene->Pixel(x, y);
        EXPECT_EQ(std::tuple(int(p.x), int(p.y), int(p.z)), std::tuple(r, g, b)) << x << ", " << y;
    };
    update({ 255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   255, 255, 255, 255 });
    is(2, 2, 255, 0, 0);
    is(Size - 3, 2, 0, 255, 0);
    is(2, Size - 3, 0, 0, 255);
    update({ 0, 0, 255, 255,   0, 0, 255, 255,   0, 0, 255, 255,   0, 0, 255, 255 });
    is(2, 2, 0, 0, 255);
    scene->runtime->SetDebugDraw(nullptr);
}

// A script's image goes with the script, and what was drawn of it may still be held: whenever it goes, the frames after
// draw what is left and nothing fails — held by the shapes, by the last frame's draw, or still on the GPU.
TEST_F(DrawTest, AnImageThatGoesIsLeftOutOfTheDebugShapes) {
    kgraph::Graph graph = Canvas();
    graph.Add("clear", "Clear").Wire("image", "canvas").Set("color", Value::List { 0, 0, 0, 1 });
    graph.Add("debug", "DebugDraw").Wire("image", "canvas");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");

    kor::DebugDraw& draw = scene->SceneDebug();
    scene->runtime->SetDebugDraw(&draw);
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());

    const auto make = [] {
        return kor::Image::Builder {}.SetType(kor::Image::Type::e2D).SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2 { 2, 2 })
            .SetUsage(kor::Image::Usage::eTransferDst | kor::Image::Usage::eSampled).Build();
    };
    const auto shown = [&](const kor::ResourceRef<const kor::Image>& image) {
        draw.BeginLogicSpace({ 1.f, 1.f });
        draw.Image({ 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, image, {}, {.outline = false});
        draw.EndLogicSpace();
    };

    // Gone in the update after the one that drew it, while what that frame recorded may still be running.
    auto picture = make();
    scene->afterUpdate = [&] { shown(picture); };
    Frame();
    scene->afterUpdate = [&] { picture = {}; };
    Frame();
    Frame();

    // Gone in the same update that asked for it: the shapes hold an image that is no more.
    auto second = make();
    scene->afterUpdate = [&] { shown(second); second = {}; };
    Frame();
    Frame();

    // Drawn for longer than a frame, and gone while it is.
    auto third = make();
    scene->afterUpdate = [&] {
        draw.BeginLogicSpace({ 1.f, 1.f });
        draw.Image({ 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, third, {}, {.duration = 60.f, .outline = false});
        draw.EndLogicSpace();
    };
    Frame();
    third = {};
    Frame();
    Frame();
    draw.Clear();
    EXPECT_TRUE(scene->runtime->Ok());
    scene->runtime->SetDebugDraw(nullptr);
}

// An image to a buffer and back, a buffer cleared, a part of an image copied and scaled, and many samples made one.
TEST_F(DrawTest, ImagesAndBuffersAreCopiedInPartsAndResolved) {
    kgraph::Graph graph = Canvas();
    graph.Add("fill", "Clear").Wire("image", "canvas").Set("color", Value::List { 1, 0, 0, 1 });
    graph.Add("bytes", "Buffer").Set("instanceCount", Size * Size * 4).Set("usage", Value::List { "eTransferSrc", "eTransferDst" });
    graph.Add("out", "CopyImageToBuffer").Wire("image", "canvas").Wire("buffer", "bytes");
    // The second half of the buffer zeroed: the lower half of the picture, when it is read back.
    graph.Add("zero", "ClearBuffer").Wire("buffer", "bytes").Set("offset", Size * Size * 2);
    graph.Add("copy", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { Size, Size }).Set("usage", Value::List { "eTransferSrc", "eTransferDst" });
    graph.Add("back", "CopyBufferToImage").Wire("buffer", "bytes").Wire("image", "copy");
    graph.Add("show", "Blit").Wire("source", "copy").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_EQ(scene->Schedule(), (std::vector<std::string> { "fill", "out", "zero", "back", "show", "Read screen" }));
    EXPECT_GT(scene->Pixel(5, 5).x, 247) << "the red, through the buffer";
    EXPECT_EQ(scene->Pixel(5, 28).x, 0) << "and the half that was cleared";

    // A quarter of one picture over the whole of another.
    kgraph::Graph part = Canvas();
    Triangles(part);
    part.Add("draw", "Draw").Wire("pipeline", "triangles");
    part.Add("copy", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { Size, Size }).Set("usage", Value::List { "eTransferSrc", "eTransferDst" });
    part.Add("corner", "Blit").Wire("source", "canvas").Wire("destination", "copy").Set("sourceOffset", Value::List { 16, 16 }).Set("sourceExtent", Value::List { 16, 16, 1 });
    part.Add("show", "Blit").Wire("source", "copy").Wire("destination", "screen");
    Run(part);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_TRUE(Black(3, 3)) << "the corner of the picture the triangle is not in, over all of it";
    EXPECT_TRUE(Black(28, 28));

    // A picture of four samples a pixel, drawn into and made one of one sample.
    kgraph::Graph many = Canvas("e4");
    Triangles(many).Set("multisampleState", Value::Members { { "sampleCount", "e4" } });
    many.Add("draw", "Draw").Wire("pipeline", "triangles");
    many.Add("plain", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { Size, Size }).Set("usage", Value::List { "eTransferSrc", "eTransferDst" });
    many.Add("resolve", "Resolve").Wire("source", "canvas").Wire("destination", "plain");
    many.Add("show", "Blit").Wire("source", "plain").Wire("destination", "screen");
    Run(many);
    ASSERT_TRUE(scene->runtime->Ok());
    EXPECT_TRUE(Green(3, 3));
    EXPECT_TRUE(Black(28, 28));
    many.Find("resolve")->Wire("source", "plain").Wire("destination", "canvas");
    scene->runtime->Set(many);
    EXPECT_NE(scene->runtime->Status("resolve")->error.find("one sample a pixel"), std::string::npos) << scene->runtime->Status("resolve")->error;
}

// Rays, one for each pixel of the image they are for.
TEST_F(DrawTest, RaysAreTracedForEachPixel) {
    kgraph::Graph graph = Canvas();
    graph.Find("canvas")->Set("usage", Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.nodes.erase(std::ranges::find(graph.nodes, "target", &kgraph::Node::id));
    graph.Add("tracer", "RayTracingPipeline").Set("raygen.module", "rays").Set("raygen.entry", "raygenMain").Wire("param.target", "canvas");
    graph.Add("trace", "TraceRays").Wire("pipeline", "tracer").Wire("size", "canvas", "extent");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    scene->runtime->Set(graph);
    if (const auto* status = scene->runtime->Status("tracer"); !status->ok) GTEST_SKIP() << "no ray tracing here: " << status->error;
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();
    EXPECT_GT(scene->Pixel(0, 0).z, 247);
    EXPECT_LT(scene->Pixel(0, 0).y, 8);
    EXPECT_GT(scene->Pixel(31, 31).y, 247) << "the far corner had a ray too";

    // As many as cover a parameter's image, named.
    graph.Find("trace")->inputs.erase("size");
    graph.Find("trace")->Set("cover", "target");
    scene->runtime->Set(graph);
    EXPECT_TRUE(scene->runtime->Status("trace")->ok) << scene->runtime->Status("trace")->error;
}

// A pipeline of mesh shaders is drawn as groups of them: nothing else to say, and no mesh to give.
TEST_F(DrawTest, MeshShadersAreDrawnAsTasks) {
    kgraph::Graph graph = Canvas();
    graph.Add("meshlets", "GraphicsPipeline").Set("stages", "eMesh").Set("mesh.module", "meshlet").Set("mesh.entry", "meshMain")
        .Set("fragment.module", "meshlet").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "meshlets");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    scene->runtime->Set(graph);
    if (const auto* status = scene->runtime->Status("meshlets"); !status->ok) GTEST_SKIP() << "no mesh shaders here: " << status->error;
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();
    EXPECT_TRUE(Green(3, 3));
    EXPECT_TRUE(Black(28, 28));

    graph.Find("draw")->Set("command", "eDrawIndexed");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("draw")->error.find("mesh shaders"), std::string::npos) << scene->runtime->Status("draw")->error;
    graph.Find("draw")->Set("command", "eDrawMeshTasks").Set("tasks", Value::List { 1, 1, 1 });
    scene->runtime->Set(graph);
    EXPECT_TRUE(scene->runtime->Status("draw")->ok) << scene->runtime->Status("draw")->error;
}

// The catalog says all of it, for an editor to offer.
TEST_F(DrawTest, TheCatalogOffersEveryWayOfDrawing) {
    const kgraph::NodeType* draw = kgraph::FindType("Draw");
    for (const char* name : { "mesh", "vertexCount", "firstVertex", "instances", "firstInstance", "tasks", "indirect", "count", "colorLoad", "depthStore",
                              "clearColor", "viewport", "scissor", "cullMode", "depthCompare", "depthBias", "stencilReference", "primitiveRestart" }) {
        const auto port = std::ranges::find(draw->inputs, name, &kgraph::Port::name);
        ASSERT_NE(port, draw->inputs.end()) << name;
        EXPECT_TRUE(port->optional) << name;
        EXPECT_FALSE(port->doc.empty()) << name;
    }
    for (const char* type : { "TraceRays", "Resolve", "ClearBuffer", "CopyBufferToImage", "CopyImageToBuffer" }) EXPECT_NE(kgraph::FindType(type), nullptr) << type;
    const kgraph::NodeType* pipeline = kgraph::FindType("GraphicsPipeline");
    EXPECT_TRUE(std::ranges::find(pipeline->inputs, "vertex.layout", &kgraph::Port::name)->optional) << "a vertex shader that reads no vertices has no layout to give";
}

// Drawn into a transient: the canvas the frame graph's, by way of a view and a framebuffer of it, and shown. The draw
// waits the frame or two the frame graph takes to make it — what stood in records nothing — and then draws as ever.
TEST_F(DrawTest, ATriangleIsDrawnIntoATransientCanvas) {
    const std::uint64_t logged = kor::log::LastSequence();
    kgraph::Graph graph = Canvas();
    graph.Find("canvas")->Set("transient", true);
    Triangles(graph);
    graph.Add("draw", "Draw").Wire("pipeline", "triangles");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    Run(graph);
    ASSERT_TRUE(scene->runtime->Ok());
    for (int i = 0; i < 4; ++i) Frame();
    const auto canvas = scene->runtime->ResourceOf("canvas");
    ASSERT_TRUE(canvas);
    EXPECT_TRUE(canvas->transient);
    EXPECT_TRUE(Green(3, 3)) << "above the diagonal, the triangle";
    EXPECT_TRUE(Black(28, 28)) << "below it, what the framebuffer was cleared to";
    for (const auto& record : kor::log::HistorySince(logged))
        if (record.message.find("VUID") != std::string::npos || record.level >= kor::log::Level::eError) ADD_FAILURE() << record.message;
}
