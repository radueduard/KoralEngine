// kgraph: a buffer as a shader lays it out, read from its SPIR-V (blocks.h) — of what a ShaderBuffer fills: fields of
// 16 and 64 bits, arrays of arrays, and what a block has before its array. Compiled from GLSL, which says all three.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <format>
#include <string>

#include <shader.h>

#include "../src/blocks.h"
#include "support.h"

namespace {
    namespace fs = std::filesystem;

    /** The blocks of a compute shader of @p source, compiled from GLSL. */
    std::vector<kgraph::BlockLayout> Blocks(const std::string& name, const std::string& source)
    {
        const fs::path file = fs::temp_directory_path() / std::format("kgraph-blocks-{}.comp.glsl", name);
        std::ofstream(file) << source;
        const auto shader = kor::Shader::Builder {}.SetLang<kor::Shader::Lang::eGLSL>().SetStage(kor::Shader::Stage::eCompute).SetPath(file).Build();
        fs::remove(file);
        if (!shader.Valid()) {
            ADD_FAILURE() << "did not compile: " << (shader.Failure() ? shader.Failure()->message : std::string());
            return {};
        }
        return kgraph::BlocksOf(shader->Spirv());
    }

    const kgraph::BlockLayout* Named(const std::vector<kgraph::BlockLayout>& blocks, const std::string_view name)
    {
        const auto it = std::ranges::find(blocks, name, &kgraph::BlockLayout::name);
        return it == blocks.end() ? nullptr : &*it;
    }

    class BlockTest : public ::testing::Test {
    protected:
        void SetUp() override { if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason(); }
    };
}

// A block with fields before its array: those are the buffer's own, once, where they are from its start; the elements
// start after them. And an array of arrays in an element: a field for each of the outer, each an array of the inner.
TEST_F(BlockTest, WhatABlockHasBeforeItsArrayIsTheBuffersOwn) {
    const auto blocks = Blocks("header", R"(#version 450
layout(local_size_x = 1) in;
struct Light { vec4 color; float grid[2][3]; };
layout(std430, binding = 0) buffer Lights { uint count; float scale; Light lights[]; } lit;
void main() { lit.lights[gl_GlobalInvocationID.x].color *= lit.scale * float(lit.count) + lit.lights[0].grid[1][2]; }
)");
    const auto* lights = Named(blocks, "lit");
    ASSERT_NE(lights, nullptr) << "the block, by what the shader calls it";
    EXPECT_TRUE(lights->array);
    ASSERT_EQ(lights->header.size(), 2u);
    const auto* count = lights->FindHeader("count");
    const auto* scale = lights->FindHeader("scale");
    ASSERT_TRUE(count && scale);
    EXPECT_EQ(count->offset, 0u);
    EXPECT_EQ(count->scalar, 'u');
    EXPECT_EQ(scale->offset, 4u);
    EXPECT_EQ(lights->first, 16u) << "the elements after them, on a vec4's sixteen";

    const auto* first = lights->Find("grid[0]");
    const auto* second = lights->Find("grid[1]");
    ASSERT_TRUE(first && second) << "a field for each of the outer";
    EXPECT_EQ(first->count, 3u) << "each an array of the inner";
    EXPECT_EQ(first->arrayStride, 4u);
    EXPECT_EQ(first->offset, 16u);
    EXPECT_EQ(second->offset, 28u);
    EXPECT_EQ(lights->Find("color")->offset, 0u);
}

// Numbers of 16 and of 64 bits: each its own width, where the shader has it. The SPIR-V is put together here, of only what
// says how a block is laid out: a shader of them would ask the device for what it may not have (shaderFloat16, 16-bit
// storage), and only the layout is looked at.
TEST(Blocks, NumbersOfSixteenAndSixtyFourBitsAreWhereTheShaderHasThem) {
    std::vector<std::uint32_t> words { 0x07230203, 0x00010500, 0, 100, 0 };
    const auto op = [&](const std::uint32_t code, std::initializer_list<std::uint32_t> operands) {
        words.push_back(static_cast<std::uint32_t>(operands.size() + 1) << 16 | code);
        words.insert(words.end(), operands);
    };
    const auto name = [&](const std::uint32_t code, std::initializer_list<std::uint32_t> before, const std::string& text) {
        std::vector<std::uint32_t> packed((text.size() + 4) / 4, 0);
        std::memcpy(packed.data(), text.data(), text.size());
        words.push_back(static_cast<std::uint32_t>(before.size() + packed.size() + 1) << 16 | code);
        words.insert(words.end(), before);
        words.insert(words.end(), packed.begin(), packed.end());
    };
    enum : std::uint32_t { Half = 1, Short = 2, UShort = 3, Float = 4, Double = 5, ULong = 6, HalfPair = 7, Wide = 8, Wides = 9, Block = 10, Pointer = 11, Variable = 12 };
    op(22, { Half, 16 });
    op(21, { Short, 16, 1 });
    op(21, { UShort, 16, 0 });
    op(22, { Float, 32 });
    op(22, { Double, 64 });
    op(21, { ULong, 64, 0 });
    op(23, { HalfPair, Half, 2 });
    op(30, { Wide, Half, Short, UShort, Float, Double, ULong, HalfPair });
    op(29, { Wides, Wide });
    op(30, { Block, Wides });
    op(32, { Pointer, 12, Block });       // StorageBuffer
    op(59, { Pointer, Variable, 12 });
    name(5, { Variable }, "all");
    const char* members[] { "h", "s", "us", "f", "d", "big", "pair" };
    const std::uint32_t offsets[] { 0, 2, 4, 8, 16, 24, 32 };
    for (std::uint32_t i = 0; i < 7; ++i) {
        name(6, { Wide, i }, members[i]);
        op(72, { Wide, i, 35, offsets[i] });
    }
    op(72, { Block, 0, 35, 0 });
    op(71, { Wides, 6, 40 });             // ArrayStride

    const auto blocks = kgraph::BlocksOf(words);
    const auto* wides = Named(blocks, "all");
    ASSERT_NE(wides, nullptr);
    EXPECT_TRUE(wides->array);
    EXPECT_EQ(wides->stride, 40u);
    const auto expect = [&](const char* field, const char scalar, const std::uint32_t bytes, const std::uint32_t offset) {
        const auto* found = wides->Find(field);
        ASSERT_NE(found, nullptr) << field;
        EXPECT_EQ(found->scalar, scalar) << field;
        EXPECT_EQ(found->bytes, bytes) << field;
        EXPECT_EQ(found->offset, offset) << field;
    };
    expect("h", 'f', 2, 0);
    expect("s", 'i', 2, 2);
    expect("us", 'u', 2, 4);
    expect("f", 'f', 4, 8);
    expect("d", 'f', 8, 16);
    expect("big", 'u', 8, 24);
    expect("pair", 'f', 2, 32);
    EXPECT_EQ(wides->Find("pair")->components, 2u);
}

// A half, as a shader's: exact where it can be, rounded to the nearest where not, and its ends where a float passes them.
TEST(Half, AFloatIsTheHalfNearestIt) {
    EXPECT_EQ(kgraph::HalfOf(0.f), 0x0000);
    EXPECT_EQ(kgraph::HalfOf(-0.f), 0x8000);
    EXPECT_EQ(kgraph::HalfOf(1.f), 0x3C00);
    EXPECT_EQ(kgraph::HalfOf(-2.f), 0xC000);
    EXPECT_EQ(kgraph::HalfOf(0.5f), 0x3800);
    EXPECT_EQ(kgraph::HalfOf(65504.f), 0x7BFF) << "the greatest";
    EXPECT_EQ(kgraph::HalfOf(1e6f), 0x7C00) << "past it, an infinity";
    EXPECT_EQ(kgraph::HalfOf(std::numeric_limits<float>::infinity()), 0x7C00);
    EXPECT_EQ(kgraph::HalfOf(-std::numeric_limits<float>::infinity()), 0xFC00);
    EXPECT_EQ(kgraph::HalfOf(std::numeric_limits<float>::quiet_NaN()) & 0x7C00, 0x7C00);
    EXPECT_NE(kgraph::HalfOf(std::numeric_limits<float>::quiet_NaN()) & 0x03FF, 0) << "a NaN stays one";
    EXPECT_EQ(kgraph::HalfOf(6.103515625e-05f), 0x0400) << "the least of the normal ones";
    EXPECT_EQ(kgraph::HalfOf(5.960464477539063e-08f), 0x0001) << "the least of all, a subnormal";
    EXPECT_EQ(kgraph::HalfOf(1.f + 1.f / 2048.f), 0x3C00) << "halfway between two: to the even one";
    EXPECT_EQ(kgraph::HalfOf(1.f + 3.f / 2048.f), 0x3C02) << "halfway, the other way: to the even one";
    EXPECT_EQ(kgraph::HalfOf(0.1f), 0x2E66);
}
