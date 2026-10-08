// A million entities made, changed, destroyed and walked: that it holds together at that size, and how long it takes.

#include <gtest/gtest.h>

#include <chrono>

#include "types.h"

namespace {
    double Milliseconds(const std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
    }
}

TEST(Scale, AMillionEntitiesAreMadeChangedDestroyedAndWalked) {
    constexpr int N = 1'000'000;
    kecs::World world;
    std::vector<kecs::Entity> entities;
    entities.reserve(N);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) entities.push_back(world.Create(Position { static_cast<float>(i), 0 }, Velocity { 1, 2 }));
    const double createMs = Milliseconds(start);
    ASSERT_EQ(world.EntityCount(), static_cast<std::size_t>(N));

    start = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i += 2) world.Add<Health>(entities[i], { i });
    const double addMs = Milliseconds(start);

    start = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i += 4) world.Remove<Velocity>(entities[i]);
    const double removeMs = Milliseconds(start);

    start = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i += 3) world.Destroy(entities[i]);
    const double destroyMs = Milliseconds(start);

    start = std::chrono::steady_clock::now();
    std::size_t visited = 0;
    world.Query<Position, const Velocity>().Each([&](Position& p, const Velocity& v) { p.y += v.y; ++visited; });
    const double walkMs = Milliseconds(start);

    // What should be left, counted the slow way.
    std::size_t alive = 0, moving = 0;
    for (int i = 0; i < N; ++i) {
        if (i % 3 == 0) { ASSERT_FALSE(world.Alive(entities[i])); continue; }
        ++alive;
        const Position* p = world.Get<Position>(entities[i]);
        ASSERT_NE(p, nullptr);
        ASSERT_EQ(p->x, static_cast<float>(i));
        const bool hasVelocity = i % 4 != 0;
        ASSERT_EQ(world.Has<Velocity>(entities[i]), hasVelocity);
        ASSERT_EQ(p->y, hasVelocity ? 2.f : 0.f);
        if (i % 2 == 0) { ASSERT_EQ(world.Get<Health>(entities[i])->value, i); }
        moving += hasVelocity ? 1 : 0;
    }
    EXPECT_EQ(world.EntityCount(), alive);
    EXPECT_EQ(visited, moving);

    std::printf("1,000,000 entities: create %.1f ms, add to half %.1f ms, remove from a quarter %.1f ms, destroy a third %.1f ms, "
                "walk %zu with two components %.2f ms\n", createMs, addMs, removeMs, destroyMs, visited, walkMs);
}
