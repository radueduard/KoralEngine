// IDs and references: written, read back, and what is none.

#include <gtest/gtest.h>

#include <kasset/id.h>

#include <unordered_set>

using kasset::Id;
using kasset::Reference;

TEST(Id, NewOnesDiffer)
{
    std::unordered_set<Id> seen;
    for (int i = 0; i < 10'000; ++i) EXPECT_TRUE(seen.insert(Id::New()).second);
}

TEST(Id, TextIsReadBack)
{
    const Id id = Id::New();
    const std::string text = id.Text();
    ASSERT_EQ(text.size(), 32u);
    EXPECT_EQ(Id::Parse(text), id);
}

TEST(Id, NamedIsTheSameEachTime)
{
    EXPECT_EQ(Id::Named("mesh/cube"), Id::Named("mesh/cube"));
    EXPECT_NE(Id::Named("mesh/cube"), Id::Named("mesh/cubf"));
    EXPECT_TRUE(Id::Named("").Valid());
}

TEST(Id, WhatIsNoId)
{
    EXPECT_FALSE(Id::Parse(""));
    EXPECT_FALSE(Id::Parse("00000000000000000000000000000000"));    // the zero ID is none
    EXPECT_FALSE(Id::Parse("0123456789ABCDEF0123456789abcdef"));    // lowercase only
    EXPECT_FALSE(Id::Parse("0123456789abcdef0123456789abcde"));     // one short
    EXPECT_FALSE(Id::Parse("@textures/wall.png"));
}

TEST(Reference, OfAnAssetAndOfAPart)
{
    const Reference whole { Id::New(), {} };
    EXPECT_EQ(whole.Text(), whole.asset.Text());
    EXPECT_EQ(Reference::Parse(whole.Text()), whole);

    const Reference part { Id::New(), Id::New() };
    EXPECT_EQ(part.Text(), part.asset.Text() + "/" + part.part.Text());
    EXPECT_EQ(Reference::Parse(part.Text()), part);

    EXPECT_FALSE(Reference::Parse(part.asset.Text() + "/"));
    EXPECT_FALSE(Reference::Parse("$cube"));
    EXPECT_FALSE(Reference::Parse("@sponza.gltf#3"));
}
