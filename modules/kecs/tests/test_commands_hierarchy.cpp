// Changes queued for after a query; the hierarchy; and observers of components coming and going.

#include <gtest/gtest.h>

#include "types.h"

using kecs::Entity;
using kecs::World;

TEST(Commands, AreMadeInOrderOnceTheQueryIsOver) {
    World world;
    std::vector<Entity> entities;
    for (int i = 0; i < 10; ++i) entities.push_back(world.Create(Health { i }));

    kecs::Commands commands(world);
    Entity spawned;
    world.Query<const Health>().Each([&](const Entity e, const Health& h) {
        if (h.value < 3) commands.Destroy(e);
        else if (h.value == 5) {
            commands.Add<Position>(e, { 5, 5 });
            commands.Remove<Health>(e);
            spawned = commands.Create();
            commands.Add<Health>(spawned, { 50 });
            commands.Add<Player>(spawned);
        }
    });
    EXPECT_EQ(world.Query<const Health>().Count(), 10u) << "nothing has happened yet";
    EXPECT_TRUE(world.Alive(spawned)) << "the entity is there to be referred to";
    EXPECT_FALSE(world.Has<Health>(spawned));
    EXPECT_FALSE(commands.Empty());

    commands.Apply();
    EXPECT_TRUE(commands.Empty());
    EXPECT_FALSE(world.Alive(entities[0]));
    EXPECT_FALSE(world.Alive(entities[2]));
    EXPECT_TRUE(world.Alive(entities[3]));
    EXPECT_EQ(world.Get<Position>(entities[5])->x, 5);
    EXPECT_FALSE(world.Has<Health>(entities[5]));
    EXPECT_EQ(world.Get<Health>(spawned)->value, 50);
    EXPECT_TRUE(world.Has<Player>(spawned));

    world.Query<const Health>().Each([&](const Health&) { EXPECT_THROW(commands.Apply(), std::logic_error); });
}

TEST(Commands, FreeWhatTheyCarriedWhenNeverApplied) {
    Counted::alive = 0;
    {
        World world;
        const Entity e = world.Create();
        {
            kecs::Commands commands(world);
            commands.Add<Counted>(e, Counted("never"));
            EXPECT_EQ(Counted::alive, 1);
        }
        EXPECT_EQ(Counted::alive, 0);
        kecs::Commands commands(world);
        commands.Add<Counted>(e, Counted("kept"));
        const Entity gone = world.Create();
        commands.Add<Counted>(gone, Counted("lost"));
        world.Destroy(gone);
        commands.Apply();
        EXPECT_EQ(Counted::alive, 1) << "what was for an entity that is gone is destroyed";
        EXPECT_EQ(world.Get<Counted>(e)->text, "kept");
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(Commands, AddComponentsByIdAndSetParents) {
    World world;
    const kecs::ComponentId weight = world.Register({ .name = "weight", .size = sizeof(float), .alignment = alignof(float) });
    const Entity parent = world.Create(), child = world.Create();
    kecs::Commands commands(world);
    const float w = 3.f;
    commands.Add(child, weight, &w);
    commands.Add(parent, weight);
    commands.SetParent(child, parent);
    commands.Apply();
    EXPECT_EQ(*static_cast<const float*>(world.Get(child, weight)), 3.f);
    EXPECT_EQ(*static_cast<const float*>(world.Get(parent, weight)), 0.f);
    EXPECT_EQ(world.ParentOf(child), parent);
}

TEST(Hierarchy, PutsEntitiesUnderOthersAndKeepsBothEndsRight) {
    World world;
    const Entity root = world.Create(), a = world.Create(), b = world.Create(), leaf = world.Create();
    EXPECT_TRUE(world.ParentOf(a).IsNull());
    EXPECT_TRUE(world.ChildrenOf(root).empty());

    EXPECT_TRUE(world.SetParent(a, root));
    EXPECT_TRUE(world.SetParent(b, root));
    EXPECT_TRUE(world.SetParent(leaf, a));
    EXPECT_EQ(world.ParentOf(leaf), a);
    EXPECT_EQ(world.ChildrenOf(root), (std::vector<Entity> { a, b }));

    EXPECT_FALSE(world.SetParent(root, leaf)) << "it would be under itself";
    EXPECT_FALSE(world.SetParent(a, a));
    EXPECT_FALSE(world.SetParent(a, Entity { 999, 0 }));

    EXPECT_TRUE(world.SetParent(leaf, b));
    EXPECT_TRUE(world.ChildrenOf(a).empty());
    EXPECT_EQ(world.ChildrenOf(b), std::vector<Entity> { leaf });

    EXPECT_TRUE(world.SetParent(b, kecs::NullEntity));
    EXPECT_TRUE(world.ParentOf(b).IsNull());
    EXPECT_FALSE(world.Has<kecs::Parent>(b));
    EXPECT_EQ(world.ChildrenOf(root), std::vector<Entity> { a });
    EXPECT_EQ(world.ParentOf(leaf), b) << "what was under it stays under it";
}

TEST(Hierarchy, DestroyingAnEntityDestroysWhatIsUnderIt) {
    World world;
    const Entity root = world.Create(), a = world.Create(), b = world.Create(), leaf = world.Create(), apart = world.Create();
    world.SetParent(a, root);
    world.SetParent(b, root);
    world.SetParent(leaf, a);

    world.Destroy(a);
    EXPECT_FALSE(world.Alive(a));
    EXPECT_FALSE(world.Alive(leaf));
    EXPECT_EQ(world.ChildrenOf(root), std::vector<Entity> { b }) << "and it leaves what it was under";

    world.Destroy(root);
    EXPECT_FALSE(world.Alive(b));
    EXPECT_TRUE(world.Alive(apart));
    EXPECT_EQ(world.EntityCount(), 1u);
}

TEST(Observers, HearOfComponentsComingAndGoing) {
    World world;
    std::vector<std::pair<char, Entity>> heard;
    world.OnAdd(world.Id<Health>(), [&](World&, const Entity e) { heard.emplace_back('+', e); });
    world.OnRemove(world.Id<Health>(), [&](World& w, const Entity e) {
        EXPECT_NE(w.Get<Health>(e), nullptr) << "it is still there to be read";
        heard.emplace_back('-', e);
    });

    const Entity a = world.Create(Health { 1 });
    const Entity b = world.Create();
    world.Add<Health>(b);
    world.Add<Health>(b, { 5 });   // replaced: not added
    world.Add<Position>(b);
    world.Remove<Health>(a);
    world.Destroy(b);
    EXPECT_EQ(heard, (std::vector<std::pair<char, Entity>> { { '+', a }, { '+', b }, { '-', a }, { '-', b } }));

    // An observer may give the entity something else: the component just added is where it is found.
    world.OnAdd(world.Id<Position>(), [](World& w, const Entity e) { w.Add<Velocity>(e, { 1, 1 }); });
    const Entity c = world.Create();
    Position& p = world.Add<Position>(c, { 4, 4 });
    EXPECT_EQ(p.x, 4);
    EXPECT_EQ(&p, world.Get<Position>(c));
    EXPECT_TRUE(world.Has<Velocity>(c));
}
