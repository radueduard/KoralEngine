// Entities and their components: made, read, replaced, taken away, and stored right as others come and go.

#include <gtest/gtest.h>

#include "types.h"

using kecs::Entity;
using kecs::World;

TEST(World, EntitiesAreMadeDestroyedAndTheirPlacesUsedAgain) {
    World world;
    const Entity a = world.Create(), b = world.Create();
    EXPECT_NE(a, b);
    EXPECT_TRUE(world.Alive(a));
    EXPECT_EQ(world.EntityCount(), 2u);
    EXPECT_FALSE(world.Alive(kecs::NullEntity));

    EXPECT_TRUE(world.Destroy(a));
    EXPECT_FALSE(world.Alive(a));
    EXPECT_FALSE(world.Destroy(a)) << "already gone";
    const Entity c = world.Create();
    EXPECT_EQ(c.index, a.index) << "its place is used again";
    EXPECT_NE(c.generation, a.generation);
    EXPECT_FALSE(world.Alive(a)) << "the old entity is not the new one";
    EXPECT_TRUE(world.Alive(c));
}

TEST(World, ComponentsAreAddedReadReplacedAndRemoved) {
    World world;
    const Entity e = world.Create();
    EXPECT_EQ(world.Get<Position>(e), nullptr);
    world.Add<Position>(e, { 1, 2 });
    world.Add<Velocity>(e, { 3, 4 });
    ASSERT_NE(world.Get<Position>(e), nullptr);
    EXPECT_EQ(world.Get<Position>(e)->x, 1);
    EXPECT_EQ(world.Get<Velocity>(e)->y, 4);
    EXPECT_TRUE(world.Has<Position>(e));

    world.Add<Position>(e, { 9, 9 });
    EXPECT_EQ(world.Get<Position>(e)->x, 9) << "replaced";
    world.Mutable<Position>(e)->x = 5;
    EXPECT_EQ(world.Get<Position>(e)->x, 5);

    EXPECT_TRUE(world.Remove<Position>(e));
    EXPECT_FALSE(world.Remove<Position>(e));
    EXPECT_FALSE(world.Has<Position>(e));
    EXPECT_EQ(world.Get<Velocity>(e)->x, 3) << "what it kept moved with it";

    const Entity made = world.Create(Position { 7, 8 }, Health { 3 }, Player {});
    EXPECT_EQ(world.Get<Position>(made)->y, 8);
    EXPECT_EQ(world.Get<Health>(made)->value, 3);
    EXPECT_TRUE(world.Has<Player>(made));
    EXPECT_EQ(world.ComponentsOf(made).size(), 3u);

    EXPECT_THROW(world.Add<Position>(Entity { 999, 0 }, {}), std::logic_error);
    EXPECT_THROW(world.Create(Position {}, Position {}), std::logic_error);
}

TEST(World, ADefaultComponentHasItsTypesDefaults) {
    World world;
    const Entity e = world.Create();
    EXPECT_EQ(world.Add<Health>(e).value, 100);
    ASSERT_TRUE(world.Add(e, world.Id<Position>()));
    EXPECT_EQ(world.Get<Position>(e)->x, 0);
}

TEST(World, ComponentsThatOwnThingsAreMovedAndDestroyedOnce) {
    Counted::alive = 0;
    {
        World world;
        std::vector<Entity> entities;
        for (int i = 0; i < 100; ++i) entities.push_back(world.Create(Counted(std::to_string(i))));
        EXPECT_EQ(Counted::alive, 100);
        // Moved between archetypes, and their storage grown.
        for (int i = 0; i < 100; i += 2) world.Add<Position>(entities[i], { static_cast<float>(i), 0 });
        for (int i = 0; i < 100; i += 4) world.Add<Player>(entities[i]);
        EXPECT_EQ(Counted::alive, 100);
        for (int i = 0; i < 100; ++i) EXPECT_EQ(world.Get<Counted>(entities[i])->text, std::to_string(i));

        // Destroyed from the middle: the last of each archetype takes the place, and stays itself.
        for (int i = 0; i < 100; i += 3) world.Destroy(entities[i]);
        EXPECT_EQ(Counted::alive, 100 - 34);
        for (int i = 0; i < 100; ++i) {
            if (i % 3 == 0) continue;
            ASSERT_NE(world.Get<Counted>(entities[i]), nullptr);
            EXPECT_EQ(world.Get<Counted>(entities[i])->text, std::to_string(i));
            if (i % 2 == 0) EXPECT_EQ(world.Get<Position>(entities[i])->x, static_cast<float>(i));
        }
        world.Add<Counted>(entities[1], Counted("again"));
        EXPECT_EQ(Counted::alive, 100 - 34) << "replacing one leaves one";
        world.Remove<Counted>(entities[2]);
        EXPECT_EQ(Counted::alive, 100 - 35);
    }
    EXPECT_EQ(Counted::alive, 0) << "the world destroys what is left in it";
}

TEST(World, AComponentThatCanOnlyBeMovedIsMovedIn) {
    World world;
    const Entity e = world.Create();
    world.Add<Owned>(e, Owned { std::make_unique<int>(7) });
    world.Add<Position>(e);
    ASSERT_NE(world.Get<Owned>(e), nullptr);
    EXPECT_EQ(*world.Get<Owned>(e)->value, 7);
    Owned other { std::make_unique<int>(1) };
    EXPECT_FALSE(world.Add(e, world.Id<Owned>(), &other)) << "it cannot be copied in";
    EXPECT_EQ(*world.Get<Owned>(e)->value, 7);
}

TEST(World, TagsStoreNothing) {
    World world;
    const Entity e = world.Create();
    world.Add<Player>(e);
    EXPECT_TRUE(world.Has<Player>(e));
    EXPECT_EQ(world.Get(e, world.Id<Player>()), nullptr) << "nothing to point at";
    EXPECT_TRUE(world.Describe(world.Id<Player>()).IsTag());
    const kecs::ComponentId visible = world.RegisterTag("Visible");
    EXPECT_TRUE(world.Add(e, visible));
    EXPECT_TRUE(world.Has(e, visible));
    EXPECT_TRUE(world.Remove<Player>(e));
    EXPECT_TRUE(world.Has(e, visible));
}

TEST(World, ComponentTypesAreDescribedAtRunTime) {
    World world;
    // A plain one: twelve bytes, zeroed, copied byte for byte.
    kecs::ComponentDescription plain { .name = "blur.entity", .size = 12, .alignment = 4 };
    plain.fields = { { "radius", "float", 0, 4 }, { "direction", "kor::Vec2", 4, 8 } };
    const kecs::ComponentId id = world.Register(plain);
    EXPECT_EQ(world.Find("blur.entity"), id);
    EXPECT_EQ(world.Find("nothing"), kecs::NoComponent);
    EXPECT_EQ(world.Describe(id).fields.size(), 2u);
    EXPECT_EQ(world.Register(plain), id) << "the same name and shape is the same type";
    plain.size = 16;
    EXPECT_THROW(world.Register(plain), std::logic_error) << "another shape under the name";

    const Entity e = world.Create();
    ASSERT_TRUE(world.Add(e, id));
    const float* zeroed = static_cast<const float*>(world.Get(e, id));
    EXPECT_EQ(zeroed[0], 0.f);
    const float values[3] { 4.f, 1.f, 0.f };
    ASSERT_TRUE(world.Add(e, id, values));
    EXPECT_EQ(static_cast<const float*>(world.Get(e, id))[0], 4.f);
    static_cast<float*>(world.Mutable(e, id))[1] = 2.f;
    world.Add<Position>(e);
    EXPECT_EQ(static_cast<const float*>(world.Get(e, id))[1], 2.f) << "moved with its entity";

    // One with operations of its own, which read their shape from the description: so many counters.
    struct Shape { int count; };
    static int made = 0, destroyed = 0;
    made = destroyed = 0;
    kecs::ComponentDescription generated { .name = "generated", .size = sizeof(int) * 3, .alignment = alignof(int) };
    generated.context = std::make_shared<Shape>(Shape { 3 });
    generated.construct = [](void* at, const kecs::ComponentDescription& type) {
        for (int i = 0; i < static_cast<const Shape*>(type.context.get())->count; ++i) static_cast<int*>(at)[i] = i + 1;
        ++made;
    };
    generated.destroy = [](void*, const kecs::ComponentDescription&) { ++destroyed; };
    const kecs::ComponentId gen = world.Register(generated);
    ASSERT_TRUE(world.Add(e, gen));
    EXPECT_EQ(static_cast<const int*>(world.Get(e, gen))[2], 3);
    EXPECT_EQ(made, 1);
    world.Destroy(e);
    EXPECT_EQ(destroyed, 1);

    EXPECT_FALSE(world.Add(e, id)) << "the entity is gone";
    EXPECT_FALSE(world.Add(world.Create(), 9999)) << "no such type";
}

TEST(World, ATypeIsNamedAfterItselfUnlessToldOtherwise) {
    World world;
    EXPECT_EQ(world.Describe(world.Id<Position>()).name, "Position");
    EXPECT_EQ(world.Describe(world.Id<Health>()).name, "Health");
    EXPECT_EQ(world.Describe(world.Id<kecs::Parent>()).name, "kecs::Parent");
    EXPECT_EQ(world.Id<Position>(), world.Id<const Position&>());
    World other;
    (void)other.Id<Health>();
    EXPECT_EQ(other.Find("Position"), kecs::NoComponent) << "each world has its own types";
}

TEST(World, EveryEntityAliveIsVisited) {
    World world;
    const Entity a = world.Create(), b = world.Create(Position {}), c = world.Create();
    world.Destroy(a);
    std::vector<Entity> seen;
    world.EachEntity([&](const Entity e) { seen.push_back(e); });
    EXPECT_EQ(seen, (std::vector<Entity> { b, c }));
}
