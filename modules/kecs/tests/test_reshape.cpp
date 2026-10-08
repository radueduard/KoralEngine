// A component type given a new shape: what every entity keeps of the old one, and what it does not.

#include <gtest/gtest.h>

#include "types.h"

using kecs::Entity;
using kecs::World;

namespace {
    kecs::ComponentDescription Shape(std::string name, const std::size_t size, std::vector<kecs::FieldDescription> fields)
    {
        kecs::ComponentDescription d;
        d.name = std::move(name);
        d.size = size;
        d.alignment = 4;
        d.fields = std::move(fields);
        return d;
    }
    template <typename T> T At(const void* component, const std::size_t offset)
    {
        T value;
        std::memcpy(&value, static_cast<const std::byte*>(component) + offset, sizeof(T));
        return value;
    }
}

// The plan's gate: a field added, one retyped, one removed — and what should be kept is, across all three.
TEST(Reshape, KeepsWhatKeepsItsNameAndTypeAcrossThreeEdits) {
    World world;
    const kecs::ComponentId blur = world.Register(Shape("blur.entity", 8, { { "radius", "f32", 0, 4 }, { "strength", "f32", 4, 4 } }));
    std::vector<Entity> entities;
    for (int i = 0; i < 50; ++i) {
        const float values[2] { static_cast<float>(i), static_cast<float>(i) * 0.5f };
        const Entity e = world.Create();
        world.Add(e, blur, values);
        if (i % 2) world.Add<Position>(e, { 1, 1 });     // in two archetypes
        if (i % 5 == 0) world.SetEnabled(e, blur, false);
        entities.push_back(e);
    }
    const kecs::Tick before = world.AdvanceTick();

    // A field added, and the others moved about: found by name, not by where they were.
    kecs::Migration m = world.Reshape(blur, Shape("", 16, { { "direction", "vec2", 0, 8 }, { "radius", "f32", 8, 4 }, { "strength", "f32", 12, 4 } }));
    EXPECT_EQ(m.kept, (std::vector<std::string> { "radius", "strength" }));
    EXPECT_EQ(m.reset, std::vector<std::string> { "direction" });
    EXPECT_TRUE(m.removed.empty());
    EXPECT_EQ(m.entities, 50u);
    EXPECT_EQ(world.Find("blur.entity"), blur) << "the same type, by the same id";
    EXPECT_EQ(world.Describe(blur).size, 16u);
    for (int i = 0; i < 50; ++i) {
        const void* c = world.Get(entities[i], blur);
        ASSERT_NE(c, nullptr);
        EXPECT_EQ(At<float>(c, 8), static_cast<float>(i));
        EXPECT_EQ(At<float>(c, 12), static_cast<float>(i) * 0.5f);
        EXPECT_EQ(At<float>(c, 0), 0.f) << "the new field starts at its default";
        EXPECT_EQ(world.IsEnabled(entities[i], blur), i % 5 != 0) << "switched off stays switched off";
    }
    EXPECT_EQ(world.Count({ .all = { blur }, .changed = { blur }, .since = before - 1, .includeDisabled = true }), 50u) << "every one counts as changed";

    // Write the new field, then retype another: what was retyped starts again, the rest stay.
    for (int i = 0; i < 50; ++i) static_cast<float*>(world.Mutable(entities[i], blur))[0] = 7.f;
    m = world.Reshape(blur, Shape("", 16, { { "direction", "vec2", 0, 8 }, { "radius", "f32", 8, 4 }, { "strength", "i32", 12, 4 } }));
    EXPECT_EQ(m.kept, (std::vector<std::string> { "direction", "radius" }));
    EXPECT_EQ(m.reset, std::vector<std::string> { "strength" });
    for (int i = 0; i < 50; ++i) {
        const void* c = world.Get(entities[i], blur);
        EXPECT_EQ(At<float>(c, 0), 7.f);
        EXPECT_EQ(At<float>(c, 8), static_cast<float>(i));
        EXPECT_EQ(At<std::int32_t>(c, 12), 0) << "another type under the name: not the old value's bits";
    }

    // A field removed.
    m = world.Reshape(blur, Shape("", 8, { { "radius", "f32", 0, 4 }, { "strength", "i32", 4, 4 } }));
    EXPECT_EQ(m.removed, std::vector<std::string> { "direction" });
    EXPECT_EQ(m.kept, (std::vector<std::string> { "radius", "strength" }));
    for (int i = 0; i < 50; ++i) EXPECT_EQ(At<float>(world.Get(entities[i], blur), 0), static_cast<float>(i));
    EXPECT_EQ(world.Count({ .all = { blur } }), 40u) << "queries of it go on working";
}

namespace {
    struct Before { std::string name; int count = 0; Counted counted; };
    struct After { int count = 0; float added = 2.5f; std::string name; Counted counted; };
}
KECS_FIELDS(Before, name, count, counted)
KECS_FIELDS(After, count, added, name, counted)
template <> struct kecs::Name<Counted> { static constexpr std::string_view value = "Counted"; };

TEST(Reshape, CarriesFieldsThatOwnThingsByAssigningThem) {
    Counted::alive = 0;
    {
        World world;
        const kecs::ComponentId id = world.Id<Before>();
        std::vector<Entity> entities;
        for (int i = 0; i < 20; ++i) entities.push_back(world.Create(Before { std::string(40, static_cast<char>('a' + i)), i, Counted(std::to_string(i)) }));
        EXPECT_EQ(Counted::alive, 20);

        const kecs::Migration m = world.Reshape(id, kecs::ComponentDescription::Of<After>("After"));
        EXPECT_EQ(m.kept, (std::vector<std::string> { "count", "name", "counted" }));
        EXPECT_EQ(m.reset, std::vector<std::string> { "added" });
        EXPECT_EQ(Counted::alive, 20) << "one each: the old ones were destroyed";
        EXPECT_EQ(world.Find("After"), id) << "renamed";
        EXPECT_EQ(world.Find(kecs::Name<Before>::value), kecs::NoComponent);
        for (int i = 0; i < 20; ++i) {
            const auto* after = static_cast<const After*>(world.Get(entities[i], id));
            EXPECT_EQ(after->name, std::string(40, static_cast<char>('a' + i)));
            EXPECT_EQ(after->count, i);
            EXPECT_EQ(after->added, 2.5f) << "its own default";
            EXPECT_EQ(after->counted.text, std::to_string(i));
        }
        // And it is a working type from here: moved, destroyed.
        world.Add<Position>(entities[3]);
        world.Destroy(entities[4]);
        EXPECT_EQ(Counted::alive, 19);
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(Reshape, TurnsATagIntoDataAndBack) {
    World world;
    const kecs::ComponentId id = world.RegisterTag("marker");
    const Entity e = world.Create();
    world.Add(e, id);
    world.Reshape(id, Shape("", 4, { { "level", "i32", 0, 4 } }));
    ASSERT_NE(world.Get(e, id), nullptr);
    EXPECT_EQ(At<std::int32_t>(world.Get(e, id), 0), 0);
    *static_cast<std::int32_t*>(world.Mutable(e, id)) = 9;
    world.Add<Position>(e);
    EXPECT_EQ(At<std::int32_t>(world.Get(e, id), 0), 9);

    world.Reshape(id, Shape("", 0, {}));
    EXPECT_TRUE(world.Has(e, id));
    EXPECT_EQ(world.Get(e, id), nullptr) << "a tag again";
}

TEST(Reshape, IsRefusedWhereItCannotBeDone) {
    World world;
    const kecs::ComponentId a = world.RegisterTag("a");
    world.RegisterTag("b");
    EXPECT_THROW(world.Reshape(a, Shape("b", 4, {})), std::logic_error) << "another type's name";
    EXPECT_THROW(world.Reshape(999, Shape("", 4, {})), std::logic_error);
    world.Create(Position {});
    world.Query<const Position>().Each([&](const Position&) { EXPECT_THROW(world.Reshape(a, Shape("", 4, {})), std::logic_error); });
}
