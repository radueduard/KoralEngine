// Queries: what they visit, what switching a component off does to them, and what counts as changed.

#include <gtest/gtest.h>

#include <map>

#include "types.h"

using kecs::Entity;
using kecs::World;

TEST(Query, VisitsEveryEntityWithAllItsComponents) {
    World world;
    const Entity moving = world.Create(Position { 1, 1 }, Velocity { 2, 0 });
    const Entity still = world.Create(Position { 5, 5 });
    const Entity other = world.Create(Position { 0, 0 }, Velocity { 0, 3 }, Health {});

    world.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; });
    EXPECT_EQ(world.Get<Position>(moving)->x, 3);
    EXPECT_EQ(world.Get<Position>(other)->y, 3);
    EXPECT_EQ(world.Get<Position>(still)->x, 5) << "it has no velocity";

    std::vector<Entity> seen;
    world.Query<const Position>().Each([&](const Entity e, const Position&) { seen.push_back(e); });
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_EQ((world.Query<const Position, const Velocity>().Count()), 2u);
    EXPECT_EQ(world.Query<const Health>().Count(), 1u);
}

TEST(Query, IsNarrowedByTagsItMustAndMustNotHave) {
    World world;
    const Entity player = world.Create(Position {}, Player {});
    const Entity frozen = world.Create(Position {}, Player {}, Frozen {});
    world.Create(Position {});

    std::vector<Entity> seen;
    world.Query<const Position>().With<Player>().Without<Frozen>().Each([&](const Entity e, const Position&) { seen.push_back(e); });
    EXPECT_EQ(seen, std::vector<Entity> { player });
    EXPECT_EQ(world.Query<const Position>().With<Player>().Count(), 2u);
    EXPECT_EQ(world.Query<const Position>().Without<Player>().Count(), 1u);
    (void)frozen;
}

TEST(Query, PassesByAComponentSwitchedOff) {
    World world;
    const Entity a = world.Create(Position { 1, 0 }, Velocity {});
    const Entity b = world.Create(Position { 2, 0 }, Velocity {});
    EXPECT_TRUE(world.IsEnabled<Velocity>(a));
    EXPECT_TRUE(world.SetEnabled<Velocity>(a, false));
    EXPECT_FALSE(world.IsEnabled<Velocity>(a));
    EXPECT_TRUE(world.Has<Velocity>(a)) << "it still has it";
    EXPECT_FALSE(world.SetEnabled<Health>(a, false)) << "it has none";

    std::vector<Entity> seen;
    const auto run = [&](auto&& query) { seen.clear(); query.Each([&](const Entity e, const Position&) { seen.push_back(e); }); };
    run(world.Query<const Position>().With<Velocity>());
    EXPECT_EQ(seen, std::vector<Entity> { b });
    run(world.Query<const Position>().With<Velocity>().IncludeDisabled());
    EXPECT_EQ(seen.size(), 2u);
    run(world.Query<const Position>().Without<Velocity>());
    EXPECT_EQ(seen, std::vector<Entity> { a }) << "switched off is as good as not had";
    run(world.Query<const Position>());
    EXPECT_EQ(seen.size(), 2u) << "its position is on";

    world.SetEnabled<Velocity>(a, true);
    EXPECT_EQ(world.Query<const Position>().With<Velocity>().Count(), 2u);
}

TEST(Query, SwitchedOffStaysWithItsEntityAsOthersComeAndGo) {
    World world;
    std::vector<Entity> entities;
    for (int i = 0; i < 200; ++i) entities.push_back(world.Create(Position { static_cast<float>(i), 0 }));
    for (int i = 0; i < 200; i += 5) world.SetEnabled<Position>(entities[i], false);
    // Moved to another archetype, and others destroyed around them.
    for (int i = 0; i < 200; i += 2) world.Add<Health>(entities[i]);
    for (int i = 0; i < 200; i += 7) world.Destroy(entities[i]);
    for (int i = 0; i < 200; ++i) {
        if (i % 7 == 0) continue;
        EXPECT_EQ(world.IsEnabled<Position>(entities[i]), i % 5 != 0) << i;
        EXPECT_EQ(world.Get<Position>(entities[i])->x, static_cast<float>(i));
    }
    // A place left by a switched-off entity is not switched off for whoever takes it.
    const Entity fresh = world.Create(Position {});
    EXPECT_TRUE(world.IsEnabled<Position>(fresh));
}

TEST(Query, ChangedSinceIsWhatWasAddedOrWrittenAfter) {
    World world;
    const Entity a = world.Create(Health { 1 }), b = world.Create(Health { 2 }), c = world.Create(Health { 3 });
    const auto changed = [&](const kecs::Tick since) {
        std::vector<Entity> seen;
        world.Query<const Health>().Changed<Health>(since).Each([&](const Entity e, const Health&) { seen.push_back(e); });
        return seen;
    };
    kecs::Tick mine = world.AdvanceTick();
    EXPECT_EQ(changed(0).size(), 3u) << "added counts";
    EXPECT_TRUE(changed(mine).empty()) << "nothing since this run began";

    world.Mutable<Health>(b)->value = 20;
    EXPECT_EQ(changed(mine - 1), std::vector<Entity> { b }) << "written since the run before this one began";
    EXPECT_EQ(changed(0), (std::vector<Entity> { a, b, c }));
    mine = world.AdvanceTick();
    world.Mutable<Health>(c)->value = 30;
    EXPECT_EQ(changed(mine - 1), std::vector<Entity> { c });

    // Reading marks nothing; a plain reference marks everything visited; Mut marks what is written.
    mine = world.AdvanceTick();
    world.Query<const Health>().Each([](const Health&) {});
    EXPECT_TRUE(changed(mine - 1).empty());
    world.Query<kecs::Mut<Health>>().Each([](const kecs::Mut<Health> h) { if (h->value == 1) h.Write().value = 10; });
    EXPECT_EQ(changed(mine - 1), std::vector<Entity> { a });
    EXPECT_EQ(world.Get<Health>(a)->value, 10);
    mine = world.AdvanceTick();
    world.Query<Health>().Each([](Health&) {});
    EXPECT_EQ(changed(mine - 1).size(), 3u);

    mine = world.AdvanceTick();
    world.MarkChanged(b, world.Id<Health>());
    EXPECT_EQ(changed(mine - 1), std::vector<Entity> { b });
}

TEST(Query, MadeAtRunTimeWalksColumns) {
    World world;
    const kecs::ComponentId weight = world.Register({ .name = "weight", .size = sizeof(float), .alignment = alignof(float) });
    const kecs::ComponentId heavy = world.RegisterTag("heavy");
    float sum = 0.f;
    for (int i = 1; i <= 10; ++i) {
        const Entity e = world.Create();
        const float w = static_cast<float>(i);
        world.Add(e, weight, &w);
        if (i > 5) world.Add(e, heavy);
        if (i == 10) world.SetEnabled(e, weight, false);
    }
    kecs::QueryDescription query { .all = { weight, heavy } };
    std::size_t chunks = 0;
    world.Each(query, [&](const kecs::ChunkView& chunk) {
        ++chunks;
        const auto* weights = static_cast<const float*>(chunk.Column(weight));
        EXPECT_EQ(chunk.Column(heavy), nullptr) << "a tag has no column";
        for (std::size_t row = 0; row < chunk.Count(); ++row)
            if (chunk.AllMatch() || chunk.Matches(row)) sum += weights[row];
    });
    EXPECT_EQ(chunks, 1u);
    EXPECT_EQ(sum, 6.f + 7.f + 8.f + 9.f) << "the tenth's weight is switched off";
    EXPECT_EQ(world.Count(query), 4u);
    EXPECT_EQ(world.Count({ .all = { weight }, .none = { heavy } }), 5u);
    EXPECT_EQ(world.Count({ .all = { weight, heavy }, .includeDisabled = true }), 5u);
}

TEST(Query, SeesShapesOfEntityMadeAfterItFirstRan) {
    World world;
    world.Create(Position {});
    EXPECT_EQ(world.Query<const Position>().Count(), 1u);
    world.Create(Position {}, Velocity {});
    world.Create(Position {}, Health {}, Player {});
    EXPECT_EQ(world.Query<const Position>().Count(), 3u) << "the two new archetypes are found";
}

TEST(Query, RefusesAChangeOfShapeWhileItRuns) {
    World world;
    const Entity e = world.Create(Position {});
    world.Query<const Position>().Each([&](const Entity entity, const Position&) {
        EXPECT_TRUE(world.Iterating());
        EXPECT_THROW(world.Create(), std::logic_error);
        EXPECT_THROW(world.Destroy(entity), std::logic_error);
        EXPECT_THROW(world.Add<Velocity>(entity), std::logic_error);
        EXPECT_THROW(world.Remove<Position>(entity), std::logic_error);
        EXPECT_NO_THROW(world.SetEnabled<Position>(entity, false)) << "switching off moves nothing";
        EXPECT_NO_THROW((void)world.Get<Position>(entity));
    });
    EXPECT_FALSE(world.Iterating());
    EXPECT_NO_THROW(world.Add<Velocity>(e));
}

TEST(Query, TakesAComponentWhereThereIsOne) {
    World world;
    const Entity moving = world.Create(Position { 1, 0 }, Velocity { 5, 0 });
    const Entity still = world.Create(Position { 2, 0 });
    const Entity paused = world.Create(Position { 3, 0 }, Velocity { 7, 0 });
    world.SetEnabled<Velocity>(paused, false);

    std::map<Entity, float> speed;
    world.Query<const Position, kecs::Maybe<const Velocity>>().Each([&](const Entity e, const Position&, const Velocity* v) {
        speed[e] = v ? v->x : -1.f;
    });
    EXPECT_EQ(speed.size(), 3u) << "an entity without one is still visited";
    EXPECT_EQ(speed[moving], 5.f);
    EXPECT_EQ(speed[still], -1.f);
    EXPECT_EQ(speed[paused], -1.f) << "switched off is as good as not had";

    const kecs::Tick mine = world.AdvanceTick();
    world.Query<const Position, kecs::Maybe<Velocity>>().Each([](const Position&, Velocity* v) { if (v) v->x = 0; });
    EXPECT_EQ(world.Get<Velocity>(moving)->x, 0.f);
    EXPECT_EQ(world.Query<const Velocity>().Changed<Velocity>(mine - 1).Count(), 1u) << "one to write is marked changed";
}
