// What is done more than once, one of several ways, or not at all — a Repeat's loop written out a turn a node, a
// Previous and an Element of it, a Switch, a pass left out — and what a node made, copied out to be looked at.
// Offscreen, with no windowing system.

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

#include <algorithm>
#include <array>
#include <cstring>

#include <kgraph/scene.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using kgraph::Value;

    class FlowTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "flow", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }
        GraphScene* scene = nullptr;
    };
}

// A call wired from another call takes what that one gives, whichever is written out first; and a call in a loop is
// there once a turn, an Element of it what one of its outputs was on that turn.
TEST_F(FlowTest, CallsAreWiredToOneAnotherAndAnElementTakesATurnOfOne) {
    auto& runtime = *scene->runtime;
    // Times three, plus one: and twice that, wired from it.
    auto tripled = std::make_shared<kgraph::Graph>();
    tripled->Add("x", "FunctionInput").Set("name", "x").Set("kind", "eValue");
    tripled->Add("times", "Multiply").Wire("a", "x").Set("b", 3);
    tripled->Add("plus", "Add").Wire("a", "times").Set("b", 1);
    tripled->Add("result", "FunctionOutput").Set("name", "result").Wire("value", "plus");
    tripled->Add("same", "FunctionOutput").Set("name", "same").Wire("value", "x");
    tripled->Find("same")->y = 100;
    kgraph::Graph graph;
    graph.functions = { { "tripled", tripled } };
    graph.Add("turns", "Repeat").Set("count", 3);
    // The one wired from the other is first in the graph: written out before what it is given comes from.
    graph.Add("after", "Function").Set("function", "tripled").Wire("x", "before", "result");
    graph.Add("before", "Function").Set("function", "tripled").Wire("x", "turns", "index");
    graph.Add("last", "Element").Set("of", "after");
    graph.Add("first", "Element").Set("of", "after").Set("index", 0);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.ValueOf("first"), Value(4)) << "(0 * 3 + 1) * 3 + 1";
    EXPECT_EQ(runtime.ValueOf("last"), Value(22)) << "(2 * 3 + 1) * 3 + 1";

    graph.Add("given", "Element").Set("of", "before").Set("index", 2).Set("output", "same");
    runtime.Set(graph);
    EXPECT_FALSE(runtime.Status("given")->ok) << "'same' is x itself, which no node of the loop makes: it says so";
    EXPECT_NE(runtime.Status("given")->error.find("take the Element of a node of the loop"), std::string::npos) << runtime.Status("given")->error;

    graph.Find("given")->Set("output", "nothing");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("given")->error.find("no output 'nothing'"), std::string::npos) << runtime.Status("given")->error;
    graph.Find("after")->Wire("x", "before", "missing");
    runtime.Set(graph);
    EXPECT_FALSE(runtime.Ok()) << "given an output its function has not";
}

// A Reference to a call, as the editor makes one to use a node of another page there: what is wired from it is wired
// from what the call's outputs are, as a wire straight from the call would be — through a Reference of a Reference too.
TEST_F(FlowTest, AReferenceToACallGivesWhatTheCallGives) {
    auto& runtime = *scene->runtime;
    auto tripled = std::make_shared<kgraph::Graph>();
    tripled->Add("x", "FunctionInput").Set("name", "x").Set("kind", "eValue");
    tripled->Add("times", "Multiply").Wire("a", "x").Set("b", 3);
    tripled->Add("result", "FunctionOutput").Set("name", "result").Wire("value", "times");
    kgraph::Graph graph;
    graph.functions = { { "tripled", tripled } };
    graph.Add("call", "Function").Set("function", "tripled").Set("x", 5);
    graph.Add("call (in b)", "Reference").Set("node", "call");
    graph.Add("call (in c)", "Reference").Set("node", "call (in b)");
    graph.Add("plus", "Add").Wire("a", "call (in b)", "result").Set("b", 1);
    graph.Add("again", "Add").Wire("a", "call (in c)", "result").Set("b", 2);
    // A call given what a Reference to another call gives: written out with it, in the same round.
    graph.Add("given", "Function").Set("function", "tripled").Wire("x", "call (in b)", "result");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.ValueOf("plus"), Value(16));
    EXPECT_EQ(runtime.ValueOf("again"), Value(17));
    EXPECT_EQ(runtime.ValueOf("given", "result"), Value(45));
    // The Reference itself, as the editor shows it: what the call gives, and how it fares.
    EXPECT_EQ(runtime.ValueOf("call (in b)", "result"), Value(15));
    EXPECT_EQ(runtime.ValueOf("call (in c)", "result"), Value(15));
    ASSERT_NE(runtime.Status("call (in c)"), nullptr);
    EXPECT_TRUE(runtime.Status("call (in c)")->ok);
    ASSERT_EQ(runtime.OutputsOf("call (in b)").size(), 1u);
    EXPECT_EQ(runtime.OutputsOf("call (in b)")[0].name, "result");
}

// A loop in a function: its Previous and its Elements name its nodes, and name them as the call has them written out.
TEST_F(FlowTest, ALoopInAFunctionNamesItsOwnNodes) {
    auto& runtime = *scene->runtime;
    auto summed = std::make_shared<kgraph::Graph>();
    summed->Add("start", "FunctionInput").Set("name", "start").Set("kind", "eValue");
    summed->Add("turns", "Repeat").Set("count", 3);
    summed->Add("first", "Add").Wire("a", "start").Set("b", 0);
    summed->Add("tens", "Multiply").Wire("a", "turns", "index").Set("b", 10);
    summed->Add("before", "Previous").Set("of", "sum").Set("first", "first");
    summed->Add("sum", "Add").Wire("a", "tens").Wire("b", "before");
    summed->Add("last", "Element").Set("of", "sum");
    summed->Add("total", "FunctionOutput").Set("name", "total").Wire("value", "last");
    kgraph::Graph graph;
    graph.functions = { { "summed", summed } };
    graph.Add("call", "Function").Set("function", "summed").Set("start", 5);
    graph.Add("again", "Function").Set("function", "summed").Set("start", 1);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.ValueOf("call", "total"), Value(35)) << "5, then 10 more, then 20 more: each turn the one before";
    EXPECT_EQ(runtime.ValueOf("again", "total"), Value(31)) << "each call its own loop";
}

// A node wired from a Repeat is there once a turn, each with the turn's number where it took the Repeat's. A
// Previous is what a node made the turn before, and an Element what it made on one turn.
TEST_F(FlowTest, ALoopIsWrittenOutATurnANode) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("turns", "Repeat").Set("count", 3);
    graph.Add("start", "Double").Set("value", 5);
    graph.Add("tens", "Multiply").Wire("a", "turns", "index").Set("b", 10);
    graph.Add("before", "Previous").Set("of", "tens").Set("first", "start");
    graph.Add("sum", "Add").Wire("a", "tens").Wire("b", "before");
    graph.Add("down", "Subtract").Wire("a", "turns", "reverse").Wire("b", "turns", "count");
    graph.Add("last", "Element").Set("of", "sum");
    graph.Add("second", "Element").Set("of", "sum").Set("index", 1);
    graph.Add("either", "Switch").Set("index", 1).Wire("option[0]", "start").Wire("option[1]", "last");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    EXPECT_EQ(runtime.TurnsOf("sum"), (std::vector<std::string> { "sum [0]", "sum [1]", "sum [2]" }));
    EXPECT_TRUE(runtime.TurnsOf("start").empty()) << "what is wired from no Repeat is in no loop";
    EXPECT_TRUE(runtime.TurnsOf("last").empty()) << "an Element is how a loop is left";
    EXPECT_EQ(runtime.ValueOf("sum [0]"), Value(5)) << "nothing on the first turn, and what 'first' names before it";
    EXPECT_EQ(runtime.ValueOf("sum [1]"), Value(10)) << "ten, and the nothing of the turn before";
    EXPECT_EQ(runtime.ValueOf("sum [2]"), Value(30));
    EXPECT_EQ(runtime.ValueOf("down [0]"), Value(-1)) << "counted from the last, less how many there are";
    EXPECT_EQ(runtime.ValueOf("last"), Value(30));
    EXPECT_EQ(runtime.ValueOf("second"), Value(10));
    EXPECT_EQ(runtime.ValueOf("either"), Value(30)) << "the option its index says";
    EXPECT_EQ(runtime.ValueOf("sum"), Value(5)) << "asked about by its own id, a node of a loop answers for its first turn";
    ASSERT_NE(runtime.Status("sum"), nullptr);
    EXPECT_TRUE(runtime.Status("sum")->ok);
    EXPECT_EQ(runtime.Current(), graph) << "the graph is kept as it was written: the loop is a loop";

    // What cannot be written out is said on the node.
    graph.Find("either")->Set("index", 4);
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("either")->error.find("no 'option[4]'"), std::string::npos) << runtime.Status("either")->error;
    graph.Find("before")->inputs.erase("first");
    graph.Find("second")->Set("index", 7);
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("before")->error.find("'first'"), std::string::npos) << runtime.Status("before")->error;
    EXPECT_NE(runtime.Status("second")->error.find("there is no turn 7"), std::string::npos) << runtime.Status("second")->error;
    EXPECT_FALSE(runtime.Ok());
}

// A loop's count wired: from what gives a number, which the loop is not of. As many turns as it gives once the graph
// has run — one until then — and written out again with as many when it gives another.
TEST_F(FlowTest, ALoopsCountIsWhatItIsWiredFrom) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("n", "Double").Set("value", 4);
    graph.Add("turns", "Repeat").Wire("count", "n");
    graph.Add("tens", "Multiply").Wire("a", "turns", "index").Set("b", 10);
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    for (int i = 0; i < 3; ++i) Frame();
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.TurnsOf("tens").size(), 4u) << "as many as it is wired from";
    EXPECT_EQ(runtime.ValueOf("tens [3]"), Value(30));

    graph.Find("n")->Set("value", 2);
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(runtime.TurnsOf("tens").size(), 2u) << "and as many again, when it gives another";
    EXPECT_EQ(runtime.ValueOf("tens [1]"), Value(10));
    EXPECT_EQ(runtime.Current(), graph) << "the graph is kept as it was written";
}

// A loop in a loop: a Repeat wired from one — here how many turns it has, one more on each turn of the outer — is in
// it, written out a loop a turn. What is wired from both is in the inner, a node for each turn of each; a Previous is
// the inner's turn before, and an Element of one takes the outer's last turn and the inner's its index says.
TEST_F(FlowTest, ALoopInALoopIsWrittenOutATurnOfEach) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("rows", "Repeat").Set("count", 3);
    graph.Add("more", "Add").Wire("a", "rows", "index").Set("b", 1);
    graph.Add("columns", "Repeat").Wire("count", "more");
    graph.Add("tens", "Multiply").Wire("a", "rows", "index").Set("b", 10);
    graph.Add("cell", "Add").Wire("a", "tens").Wire("b", "columns", "index");
    graph.Add("zero", "Double").Set("value", 0);
    graph.Add("before", "Previous").Set("of", "sum").Set("first", "zero");
    graph.Add("sum", "Add").Wire("a", "cell").Wire("b", "before");
    graph.Add("last", "Element").Set("of", "cell");
    graph.Add("first of the last", "Element").Set("of", "cell").Set("index", 0);
    graph.Add("total", "Element").Set("of", "sum");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    EXPECT_EQ(runtime.TurnsOf("cell"), (std::vector<std::string> { "cell [0] [0]", "cell [1] [0]", "cell [1] [1]", "cell [2] [0]", "cell [2] [1]", "cell [2] [2]" }))
        << "one, two, three turns of the inner on each of the outer";
    EXPECT_EQ(runtime.ValueOf("cell [2] [1]"), Value(21)) << "the outer's ten times, and the inner's";
    EXPECT_EQ(runtime.ValueOf("cell [1] [0]"), Value(10));
    EXPECT_EQ(runtime.ValueOf("sum [2] [2]"), Value(63)) << "the inner's turn before: 20, 41, 63";
    EXPECT_EQ(runtime.ValueOf("last"), Value(22)) << "the outer's last turn, the inner's last";
    EXPECT_EQ(runtime.ValueOf("first of the last"), Value(20)) << "the outer's last, the inner's first";
    EXPECT_EQ(runtime.ValueOf("total"), Value(63));

    // Two loops neither in the other: no node is in both.
    graph.Add("other", "Repeat").Set("count", 2);
    graph.Find("cell")->Wire("b", "other", "index");
    graph.Find("tens")->Wire("a", "rows", "index");
    runtime.Set(graph);
    ASSERT_NE(runtime.Status("cell"), nullptr);
    EXPECT_NE(runtime.Status("cell")->error.find("neither in the other"), std::string::npos) << runtime.Status("cell")->error;
}

// Any level and layer of an image is copied out, and the rest of it is said: how many levels and layers, its usage, how big.
TEST_F(FlowTest, AnyLevelAndLayerOfAnImageIsLookedAt) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("picture", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 8).Set("extent.y", 4).Set("mipLevels", 3).Set("arrayLayers", 2)
        .Set("usage", Value::List { "eTransferDst", "eTransferSrc", "eSampled" });
    graph.Add("paint", "Clear").Wire("image", "picture").Set("color", Value::List { 1, 0.5, 0, 1 });
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    runtime.Watch("picture", 2, 1);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(runtime.WatchProblem("picture"), "");
    auto peek = runtime.Watched("picture");
    ASSERT_TRUE(peek);
    EXPECT_EQ(peek->extent, kor::UVec3(2, 1, 1)) << "its third level";
    EXPECT_EQ(peek->fullExtent, kor::UVec3(8, 4, 1));
    EXPECT_EQ(peek->mips, 3u);
    EXPECT_EQ(peek->layers, 2u);
    EXPECT_EQ(peek->mip, 2u);
    EXPECT_EQ(peek->layer, 1u);
    EXPECT_EQ(peek->type, "2D");
    EXPECT_EQ(peek->samples, 1u);
    EXPECT_NE(std::ranges::find(peek->usage, "eTransferSrc"), peek->usage.end());
    EXPECT_EQ(peek->byteSize, (8u * 4u + 4u * 2u + 2u * 1u) * 4u * 2u) << "every level of both layers";
    EXPECT_EQ(peek->bytes.size(), 2u * 1u * 4u);

    // A level it has not is its last; and asked for another, the copy is of that from a frame or two on.
    runtime.Watch("picture", 9, 0);
    for (int i = 0; i < 4; ++i) Frame();
    peek = runtime.Watched("picture");
    ASSERT_TRUE(peek);
    EXPECT_EQ(peek->mip, 2u);
    EXPECT_EQ(peek->layer, 0u);
    runtime.Watch("picture", 0, 0);
    for (int i = 0; i < 4; ++i) Frame();
    peek = runtime.Watched("picture");
    ASSERT_TRUE(peek);
    EXPECT_EQ(peek->extent, kor::UVec3(8, 4, 1));
    ASSERT_EQ(peek->bytes.size(), 8u * 4u * 4u);
    EXPECT_EQ(peek->bytes[0], std::byte { 255 }) << "what the Clear cleared: the first level of the first layer";
}

// A picture of a file's that no node is is copied out all the same, by the name it is found by: every level of it,
// each as its levels were made — the texels of the level asked for, not the first level's size of them.
TEST_F(FlowTest, AFilesPictureIsLookedAtByItsName) {
    auto& runtime = *scene->runtime;
    // 8 by 8 texels of a TGA: its left half red and its right half blue (as the file has them: blue, green, red, alpha).
    const auto file = std::filesystem::temp_directory_path() / "kgraph-peek.tga";
    {
        std::ofstream out(file, std::ios::binary);
        const unsigned char header[18] { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 8, 0, 32, 8 };
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const unsigned char red[4] { 0, 0, 255, 255 }, blue[4] { 255, 0, 0, 255 };
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) out.write(reinterpret_cast<const char*>(x < 4 ? red : blue), 4);
    }
    const std::string id = "@" + file.generic_string();
    runtime.Set(kgraph::Graph {});
    const auto level = [&](const std::uint32_t mip) {
        runtime.Watch(id, mip, 0);
        std::optional<kgraph::Peek> peek;
        for (int i = 0; i < 200; ++i) {
            Frame();
            if ((peek = runtime.Watched(id)) && peek->mip == mip) break;
        }
        return peek;
    };
    const auto texel = [](const kgraph::Peek& peek, const std::uint32_t x) {
        return std::array { static_cast<int>(peek.bytes[x * 4]), static_cast<int>(peek.bytes[x * 4 + 1]), static_cast<int>(peek.bytes[x * 4 + 2]) };
    };
    using RGB = std::array<int, 3>;
    auto peek = level(0);
    ASSERT_TRUE(peek) << runtime.WatchProblem(id);
    EXPECT_EQ(peek->fullExtent, kor::UVec3(8, 8, 1));
    EXPECT_EQ(peek->mips, 4u) << "its levels are made when it is read";
    EXPECT_TRUE(runtime.Ok()) << "nothing of it is in the graph";
    ASSERT_EQ(peek->bytes.size(), 8u * 8u * 4u);
    EXPECT_EQ(texel(*peek, 0), (RGB { 255, 0, 0 }));
    EXPECT_EQ(texel(*peek, 7), (RGB { 0, 0, 255 }));

    peek = level(1);
    ASSERT_TRUE(peek);
    EXPECT_EQ(peek->extent, kor::UVec3(4, 4, 1));
    ASSERT_EQ(peek->bytes.size(), 4u * 4u * 4u);
    EXPECT_EQ(texel(*peek, 1), (RGB { 255, 0, 0 })) << "the second level holds its own texels";
    EXPECT_EQ(texel(*peek, 2), (RGB { 0, 0, 255 }));

    peek = level(3);
    ASSERT_TRUE(peek);
    EXPECT_EQ(peek->extent, kor::UVec3(1, 1, 1));
    ASSERT_EQ(peek->bytes.size(), 4u);
    EXPECT_NEAR(texel(*peek, 0)[0], 127, 2) << "the last is the whole of it: half red, half blue";
    EXPECT_NEAR(texel(*peek, 0)[2], 127, 2);
    runtime.Unwatch(id);
    std::filesystem::remove(file);
}

// A pass can be left out; and what a node made is copied out to be looked at: an image's texels, a buffer's bytes.
TEST_F(FlowTest, APassIsLeftOutAndWhatANodeMadeIsLookedAt) {
    auto& runtime = *scene->runtime;
    support::Give(scene->world, scene->world.Create(), "PointLight", { { "color", { 0.25f, 0.5f, 1.f } }, { "intensity", { 2.f } }, { "range", { 7.f } } });
    kgraph::Graph graph;
    graph.Add("picture", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 8).Set("extent.y", 4)
        .Set("usage", Value::List { "eTransferDst", "eTransferSrc", "eSampled" });
    graph.Add("paint", "Clear").Wire("image", "picture").Set("color", Value::List { 1, 0.5, 0, 1 });
    graph.Add("lamps", "ForEach").Set("component", "PointLight");
    graph.Add("lights", "ShaderBuffer").Set("module", "lights").Set("entry", "computeMain").Set("parameter", "lights")
        .Wire("field.color", "lamps", "field.color").Wire("field.range", "lamps", "field.range");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;

    EXPECT_FALSE(runtime.Watched("picture")) << "nothing is copied out that nobody asked to see";
    runtime.Watch("picture");
    runtime.Watch("lights");
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(runtime.WatchProblem("picture"), "");
    const auto picture = runtime.Watched("picture");
    ASSERT_TRUE(picture);
    EXPECT_TRUE(picture->image);
    EXPECT_EQ(picture->format, "eRGBA8_UNORM");
    EXPECT_EQ(picture->extent, kor::UVec3(8, 4, 1));
    ASSERT_EQ(picture->bytes.size(), 8u * 4u * 4u);
    EXPECT_EQ(picture->bytes[0], std::byte { 255 });
    EXPECT_NEAR(static_cast<int>(picture->bytes[1]), 128, 1);
    EXPECT_EQ(picture->bytes[2], std::byte { 0 });
    const auto texel = kgraph::TexelLayoutOf(picture->format);
    EXPECT_EQ(texel.channels, 4);
    EXPECT_EQ(texel.bits, 8);
    EXPECT_EQ(kgraph::TexelLayoutOf("eD32_SFLOAT").kind, 'f');
    EXPECT_TRUE(kgraph::TexelLayoutOf("eD32_SFLOAT").depth);

    // A buffer a shader lays out says what its bytes are: each field, and where in an element it is.
    const auto lights = runtime.Watched("lights");
    ASSERT_TRUE(lights);
    EXPECT_FALSE(lights->image);
    EXPECT_EQ(lights->stride, 48u);
    EXPECT_EQ(lights->count, 1u);
    const auto color = std::ranges::find(lights->fields, "color", &kgraph::Peek::Field::name);
    ASSERT_NE(color, lights->fields.end());
    EXPECT_EQ(color->numbers, 4u);
    float blue = 0.f;
    ASSERT_GE(lights->bytes.size(), color->offset + 12u);
    std::memcpy(&blue, lights->bytes.data() + color->offset + 8, 4);
    EXPECT_EQ(blue, 1.f);
    EXPECT_TRUE(runtime.Ok()) << "watching adds nothing to the graph that is saved";
    EXPECT_EQ(runtime.Current(), graph);

    // Left out, the pass does not run; the node is still there, and still ok.
    const auto runs = [&](const char* pass) { const auto order = scene->Schedule(); return std::ranges::find(order, pass) != order.end(); };
    EXPECT_TRUE(runs("paint"));
    runtime.Unwatch("lights");
    graph.Find("paint")->Set("enabled", false);
    runtime.Set(graph);
    Frame();
    Frame();
    EXPECT_FALSE(runs("paint"));
    EXPECT_TRUE(runtime.Status("paint")->ok);
    EXPECT_FALSE(runtime.Watched("lights"));
    EXPECT_TRUE(runtime.Watched("picture")) << "what is still watched is still copied out, whatever the graph is set to";
}

// A DebugView is a picture an editor's scene view can show in place of the graph's: listed by its name, its passes run
// only while it is shown, and only in the runtime that shows it. A pass is for every view, the game's or the editor's;
// and a pipeline whose shader declares `koralDebugView` is built with the channel the view asks for.
TEST_F(FlowTest, ADebugViewIsShownInTheViewThatAsksForIt) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    const auto image = [&](const char* id) {
        graph.Add(id, "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 8).Set("extent.y", 8)
            .Set("usage", Value::List { "eTransferDst", "eTransferSrc", "eSampled", "eStorage" });
    };
    image("picture");
    image("heat");
    image("editor only");
    image("game only");
    image("channel");
    graph.Add("paint", "Clear").Wire("image", "picture").Set("color", Value::List { 1, 0, 0, 1 });
    graph.Add("heat view", "DebugView").Set("name", "Heat").Wire("picture", "heat");
    graph.Add("warm", "Clear").Wire("image", "heat").Set("color", Value::List { 0, 1, 0, 1 }).Wire("enabled", "heat view", "shown");
    graph.Add("for the editor", "Clear").Wire("image", "editor only").Set("views", "editor");
    graph.Add("for the game", "Clear").Wire("image", "game only").Set("views", "game");
    graph.Add("channel pipeline", "ComputePipeline").Set("compute.module", "debugChannel").Set("compute.entry", "computeMain").Wire("param.target", "channel");
    graph.Add("show channel", "Dispatch").Wire("pipeline", "channel pipeline").Set("groups", Value::List { 1, 1, 1 });
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;

    const auto runs = [&](const char* pass) { const auto order = scene->Schedule(); return std::ranges::find(order, pass) != order.end(); };
    const auto views = runtime.DebugViews();
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views.front().id, "heat view");
    EXPECT_EQ(views.front().name, "Heat");
    Frame();
    Frame();
    EXPECT_TRUE(runs("paint"));
    EXPECT_FALSE(runs("warm")) << "nothing makes a debug view's picture while it is not shown";
    EXPECT_TRUE(runs("for the game")) << "a runtime is the game's view unless it is told otherwise";
    EXPECT_FALSE(runs("for the editor"));

    runtime.ShowDebugView("heat view");
    runtime.SetEditorView(true);
    runtime.Watch("channel");
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(runtime.ShownDebugView(), "heat view");
    EXPECT_TRUE(runs("warm"));
    EXPECT_EQ(runtime.ResourceOf("heat view"), runtime.ResourceOf("heat")) << "what it shows is its picture";
    EXPECT_FALSE(runs("for the game"));
    EXPECT_TRUE(runs("for the editor"));
    EXPECT_EQ(runtime.Current(), graph) << "what is shown is the view's, not the graph's";
    auto channel = runtime.Watched("channel");
    ASSERT_TRUE(channel);
    EXPECT_EQ(channel->bytes[1], std::byte { 0 }) << "the lit colour, as without it";

    runtime.SetShaderDebugView(static_cast<int>(kgraph::ShaderDebugChannel::eEmissive));
    for (int i = 0; i < 4; ++i) Frame();
    channel = runtime.Watched("channel");
    ASSERT_TRUE(channel);
    EXPECT_NEAR(static_cast<int>(channel->bytes[1]), 128, 1) << "built again with the channel asked for";

    // Given on the node, the graph's own says.
    graph.Find("channel pipeline")->Set("constant.koralDebugView", 2);
    runtime.Set(graph);
    for (int i = 0; i < 4; ++i) Frame();
    channel = runtime.Watched("channel");
    ASSERT_TRUE(channel);
    EXPECT_NEAR(static_cast<int>(channel->bytes[1]), 51, 1);

    runtime.ShowDebugView({});
    runtime.SetEditorView(false);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_FALSE(runs("warm"));
    EXPECT_TRUE(runs("for the game"));
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
}
