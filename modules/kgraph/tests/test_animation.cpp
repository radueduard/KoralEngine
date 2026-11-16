// A model's clips played on the entities its nodes were made as, and the matrices a skinned mesh is bent by.

#include <gtest/gtest.h>

#include <kgraph/animation.h>

#include <cmath>

using namespace kgraph;

namespace {
    /** A root, and an arm a metre above it whose clip turns it a quarter about z in a second; a skin of the two. */
    std::shared_ptr<Model> Arm()
    {
        auto model = std::make_shared<Model>();
        model->nodes.push_back({ .name = "root" });
        ModelNode arm { .name = "arm", .parent = 0 };
        arm.position[1] = 1.f;
        model->nodes.push_back(arm);
        ModelNode body { .name = "body", .parent = -1 };
        body.parts.push_back({ "#0", -1, 0 });
        model->nodes.push_back(body);
        ModelSkin skin;
        skin.joints.push_back({ 0, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } });
        skin.joints.push_back({ 1, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1 } });
        model->skins.push_back(skin);
        ModelAnimation bend { .name = "bend", .duration = 1.f };
        bend.channels.push_back({ .node = 1, .rotationTimes = { 0.f, 1.f }, .rotations = { 0, 0, 0, 1, 0, 0, 0.70710678f, 0.70710678f } });
        model->animations.push_back(bend);
        ModelAnimation lift { .name = "lift", .duration = 2.f };
        lift.channels.push_back({ .node = 1, .positionTimes = { 0.f, 2.f }, .positions = { 0, 1, 0, 0, 3, 0 } });
        model->animations.push_back(lift);
        return model;
    }

    struct Placed {
        kecs::World world;
        kecs::Entity whole, root, arm, body;
        std::shared_ptr<Model> model = Arm();
        ModelSource models = [this](const std::string& file) { return file == "@arm.gltf" ? model : nullptr; };

        Placed()
        {
            whole = world.Create();
            world.Rename(whole, "Arm");
            root = world.Create(); world.Rename(root, "root"); world.SetParent(root, whole);
            arm = world.Create(); world.Rename(arm, "arm"); world.SetParent(arm, root); world.SetPosition(arm, { 0.f, 1.f, 0.f });
            body = world.Create(); world.Rename(body, "body"); world.SetParent(body, whole);
            world.Add<Animator>(whole, Animator { .model = "@arm.gltf" });
            world.Add<Skin>(body, Skin { .model = "@arm.gltf", .skin = 0, .root = whole });
        }
        kecs::Quat Turn() const { return world.OrientationOf(arm); }
    };
}

TEST(Animation, AClipIsFoundByItsNameItsNumberOrFirst)
{
    const auto model = Arm();
    EXPECT_EQ(ClipOf(*model, ""), 0);
    EXPECT_EQ(ClipOf(*model, "lift"), 1);
    EXPECT_EQ(ClipOf(*model, "#1"), 1);
    EXPECT_EQ(ClipOf(*model, "#2"), -1);
    EXPECT_EQ(ClipOf(*model, "run"), -1);
}

TEST(Animation, TheNodesAreFoundUnderTheWholeByName)
{
    Placed placed;
    const auto found = FindNodes(placed.world, placed.whole, *placed.model);
    ASSERT_EQ(found->nodes.size(), 3u);
    EXPECT_EQ(found->nodes[0], placed.root);
    EXPECT_EQ(found->nodes[1], placed.arm);
    EXPECT_EQ(found->nodes[2], placed.body);
    // Kept while they are; found again once one is gone.
    EXPECT_EQ(FindNodes(placed.world, placed.whole, *placed.model, found), found);
    placed.world.Destroy(placed.body);
    const auto again = FindNodes(placed.world, placed.whole, *placed.model, found);
    EXPECT_NE(again, found);
    EXPECT_TRUE(again->nodes[2].IsNull());
}

TEST(Animation, AnAnimatorPlaysItsClipOnTheNodes)
{
    Placed placed;
    Animate(placed.world, placed.models, 0.5f);
    const auto* animator = placed.world.Get<Animator>(placed.whole);
    EXPECT_FLOAT_EQ(animator->time, 0.5f);
    EXPECT_FLOAT_EQ(animator->length, 1.f);
    // Half of a quarter turn about z.
    EXPECT_NEAR(placed.Turn().z, std::sin(3.14159265f / 8.f), 1e-4f);
    EXPECT_NEAR(placed.world.PositionOf(placed.arm).y, 1.f, 1e-5f) << "what the clip keys nothing of is as the model has it";
    // Round again past its end.
    Animate(placed.world, placed.models, 0.75f);
    EXPECT_NEAR(placed.world.Get<Animator>(placed.whole)->time, 0.25f, 1e-5f);
    // Held at its end when it does not loop; and still while paused.
    placed.world.Mutable<Animator>(placed.whole)->loop = false;
    Animate(placed.world, placed.models, 5.f);
    EXPECT_FLOAT_EQ(placed.world.Get<Animator>(placed.whole)->time, 1.f);
    EXPECT_NEAR(placed.Turn().z, 0.70710678f, 1e-4f);
    placed.world.Mutable<Animator>(placed.whole)->paused = true;
    Animate(placed.world, placed.models, 5.f);
    EXPECT_FLOAT_EQ(placed.world.Get<Animator>(placed.whole)->time, 1.f);
}

TEST(Animation, AnotherClipIsFadedInto)
{
    Placed placed;
    placed.world.Mutable<Animator>(placed.whole)->fade = 1.f;
    Animate(placed.world, placed.models, 1.f);     // bent all the way (the end of a loop is its start: turned none)
    Animate(placed.world, placed.models, 0.f);
    placed.world.Mutable<Animator>(placed.whole)->clip = "lift";
    Animate(placed.world, placed.models, 0.5f);
    const auto* animator = placed.world.Get<Animator>(placed.whole);
    EXPECT_FLOAT_EQ(animator->time, 0.5f) << "the next clip from its start";
    EXPECT_FLOAT_EQ(animator->length, 2.f);
    // Halfway into lift (at y 1.5 there) from bend (at y 1): 1.25.
    EXPECT_NEAR(placed.world.PositionOf(placed.arm).y, 1.25f, 1e-4f);
    Animate(placed.world, placed.models, 0.5f);
    EXPECT_NEAR(placed.world.PositionOf(placed.arm).y, 2.f, 1e-4f) << "faded all the way: lift's alone";
    EXPECT_NEAR(placed.Turn().z, 0.f, 1e-4f) << "and nothing of bend's turn";
}

TEST(Animation, ASkinsJointsAreWhereTheyAreAfterTheirInverseBind)
{
    Placed placed;
    const Skin& skin = *placed.world.Get<Skin>(placed.body);
    std::vector<kecs::Mat4> joints;
    ASSERT_TRUE(JointMatrices(placed.world, placed.body, skin, *placed.model, joints));
    ASSERT_EQ(joints.size(), 2u);
    // As bound: nothing moves.
    for (const auto& joint : joints)
        for (int i = 0; i < 16; ++i) EXPECT_NEAR(joint.m[static_cast<std::size_t>(i)], kecs::Mat4 {}.m[static_cast<std::size_t>(i)], 1e-5f);
    // The arm turned a quarter about z: a point at its tip (0, 2, 0) goes to (-1, 1, 0).
    placed.world.SetRotation(placed.arm, { 0.f, 0.f, 90.f });
    // And the whole moved: the body moves with it, so in the body's own space nothing changes for that.
    placed.world.SetPosition(placed.whole, { 5.f, 0.f, 0.f });
    joints.clear();
    ASSERT_TRUE(JointMatrices(placed.world, placed.body, skin, *placed.model, joints));
    const kecs::Mat4& m = joints[1];
    const float x = m.At(0, 0) * 0.f + m.At(1, 0) * 2.f + m.At(3, 0);
    const float y = m.At(0, 1) * 0.f + m.At(1, 1) * 2.f + m.At(3, 1);
    EXPECT_NEAR(x, -1.f, 1e-4f);
    EXPECT_NEAR(y, 1.f, 1e-4f);
    // A model that has no such skin: none.
    Skin other = skin;
    other.skin = 3;
    EXPECT_FALSE(JointMatrices(placed.world, placed.body, other, *placed.model, joints));
}
