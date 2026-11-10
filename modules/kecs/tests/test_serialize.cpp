// A world saved as JSON and loaded again.

#include <gtest/gtest.h>

#include <cmath>

#include "types.h"

using kecs::Entity;
using kecs::World;

namespace {
    /** A world with something of everything: fields that own things, references, tags, what is switched off, a hierarchy. */
    void Fill(World& world)
    {
        const kecs::ComponentId weight = world.Register([] {
            kecs::ComponentDescription d;
            d.name = "generated.weight";
            d.size = 8;
            d.alignment = 4;
            d.fields = { { "kilograms", "f32", 0, 4 }, { "pieces", "u32", 4, 4 } };
            return d;
        }());
        const Entity root = world.Create(Named { "root \"quoted\"\n", kecs::NullEntity, 0.1 });
        const Entity a = world.Create(Position { 1.5f, -2.f }, Velocity { 0.1f, 1e-7f }, Player {});
        const Entity b = world.Create(Position { 3, 4 }, Health { 7 });
        const Entity c = world.Create(Named { "c", a, 2.0 });
        world.SetEnabled<Velocity>(a, false);
        const float w[2] { 12.5f, 0 };
        world.Add(b, weight, w);
        static_cast<std::uint32_t*>(world.Mutable(b, weight))[1] = 3;
        // Under the root, in an order that is not the order they were made in.
        world.SetParent(c, root);
        world.SetParent(a, root);
        world.SetParent(b, a);
        world.Mutable<Named>(root)->target = c;
        world.Rename(root, "The \"root\"");
        world.SetPosition(a, { 1, 2.5f, -3 });
        world.SetRotation(a, { 0, 45, 0 });
        world.SetScale(b, { 2, 2, 2 });
    }
}

TEST(Serialize, AWorldComesBackAsItWas) {
    World world;
    Fill(world);
    const kecs::Saved saved = kecs::Save(world);
    EXPECT_TRUE(saved.problems.empty()) << saved.problems.front();

    World other;
    (void)other.Id<Named>();   // a type that owns something has to be known: it cannot be made from a description
    const kecs::Loaded loaded = kecs::Load(other, saved.json);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    EXPECT_TRUE(loaded.problems.empty()) << loaded.problems.front();
    ASSERT_EQ(loaded.entities.size(), 4u);
    // In the hierarchy's order: parents before their children.
    const Entity root = loaded.entities[0], c = loaded.entities[1], a = loaded.entities[2], b = loaded.entities[3];

    EXPECT_EQ(other.Get<Named>(root)->name, "root \"quoted\"\n");
    EXPECT_EQ(other.Get<Named>(root)->weight, 0.1);
    EXPECT_EQ(other.Get<Named>(root)->target, c) << "a reference to an entity is to the entity it became";
    EXPECT_EQ(other.Get<Named>(c)->target, a);
    EXPECT_EQ(other.Get<Position>(a)->x, 1.5f);
    EXPECT_EQ(other.Get<Velocity>(a)->y, 1e-7f) << "a float reads back exactly";
    EXPECT_TRUE(other.Has<Player>(a));
    EXPECT_FALSE(other.IsEnabled<Velocity>(a));
    EXPECT_TRUE(other.IsEnabled<Position>(a));
    EXPECT_EQ(other.Get<Health>(b)->value, 7);

    // The type made at run time came with the file.
    const kecs::ComponentId weight = other.Find("generated.weight");
    ASSERT_NE(weight, kecs::NoComponent);
    EXPECT_EQ(other.Describe(weight).fields.size(), 2u);
    EXPECT_EQ(static_cast<const float*>(other.Get(b, weight))[0], 12.5f);
    EXPECT_EQ(static_cast<const std::uint32_t*>(other.Get(b, weight))[1], 3u);

    EXPECT_EQ(other.ChildrenOf(root), (std::vector<Entity> { c, a })) << "in the order they were under it";
    EXPECT_EQ(other.ParentOf(b), a);
    EXPECT_EQ(other.NameOf(root), "The \"root\"");
    EXPECT_EQ(other.NameOf(c), "Entity");
    EXPECT_EQ(other.PositionOf(a), (kecs::Vec3 { 1, 2.5f, -3 })) << "where it was, relative to its parent";
    EXPECT_EQ(other.RotationOf(a), (kecs::Vec3 { 0, 45, 0 }));
    EXPECT_EQ(other.ScaleOf(b), (kecs::Vec3 { 2, 2, 2 }));
    EXPECT_EQ(other.ScaleOf(a), (kecs::Vec3 { 1, 1, 1 }));

    EXPECT_EQ(kecs::Save(other).json, saved.json) << "saved again, it is the same file";
}

TEST(Serialize, ATypeThatOwnsSomethingMustBeKnownToTheWorld) {
    World world;
    Fill(world);
    World other;
    const kecs::Loaded loaded = kecs::Load(other, kecs::Save(world).json);
    ASSERT_TRUE(loaded.ok);
    ASSERT_EQ(loaded.problems.size(), 1u);
    EXPECT_NE(loaded.problems[0].find("Named"), std::string::npos) << loaded.problems[0];
    EXPECT_EQ(other.Find("Named"), kecs::NoComponent);
    EXPECT_NE(other.Find("Position"), kecs::NoComponent) << "plain types are made from the file";
    EXPECT_EQ(static_cast<const float*>(other.Get(loaded.entities[2], other.Find("Position")))[0], 1.5f);
}

TEST(Serialize, AFieldThatIsNotWhatItWasIsLeftAtItsDefault) {
    World world;
    const kecs::ComponentId id = world.Register([] {
        kecs::ComponentDescription d;
        d.name = "blur";
        d.size = 8;
        d.alignment = 4;
        d.fields = { { "radius", "f32", 0, 4 }, { "passes", "i32", 4, 4 } };
        return d;
    }());
    const Entity e = world.Create();
    const float values[2] { 4.f, 0.f };
    world.Add(e, id, values);
    static_cast<std::int32_t*>(world.Mutable(e, id))[1] = 3;
    const std::string json = kecs::Save(world).json;

    // The world it is loaded into has the type already, with passes a float now and a field the file has not.
    World other;
    const kecs::ComponentId now = other.Register([] {
        kecs::ComponentDescription d;
        d.name = "blur";
        d.size = 12;
        d.alignment = 4;
        d.fields = { { "passes", "f32", 0, 4 }, { "radius", "f32", 4, 4 }, { "sigma", "f32", 8, 4 } };
        return d;
    }());
    const kecs::Loaded loaded = kecs::Load(other, json);
    ASSERT_TRUE(loaded.ok);
    const auto* c = static_cast<const float*>(other.Get(loaded.entities[0], now));
    EXPECT_EQ(c[1], 4.f) << "radius is the same field, wherever it now is";
    EXPECT_EQ(c[0], 0.f) << "passes changed type: not read";
    EXPECT_EQ(c[2], 0.f);
    ASSERT_EQ(loaded.problems.size(), 1u);
    EXPECT_NE(loaded.problems[0].find("blur.passes"), std::string::npos) << loaded.problems[0];
}

// Text is saved as text: how big a std::string is where it was saved — another compiler's, a debug build's — is not what
// it is, and it is read whatever size the file says it had.
TEST(Serialize, TextIsReadWhateverSizeItsStringWasWhereItWasSaved) {
    struct Named { std::string text; };
    const auto describe = [] {
        kecs::ComponentDescription d;
        d.name = "Named";
        d.size = sizeof(Named);
        d.alignment = alignof(Named);
        d.construct = [](void* at, const kecs::ComponentDescription&) { new (at) Named(); };
        d.copy = [](void* to, const void* from, const kecs::ComponentDescription&) { new (to) Named(*static_cast<const Named*>(from)); };
        d.relocate = [](void* to, void* from, const kecs::ComponentDescription&) { new (to) Named(std::move(*static_cast<Named*>(from))); static_cast<Named*>(from)->~Named(); };
        d.destroy = [](void* at, const kecs::ComponentDescription&) { static_cast<Named*>(at)->~Named(); };
        d.fields = { { "text", "string", 0, sizeof(std::string) } };
        return d;
    };
    World world;
    const kecs::ComponentId id = world.Register(describe());
    const Entity e = world.Create();
    world.Add(e, id);
    static_cast<Named*>(world.Mutable(e, id))->text = "brass";
    std::string json = kecs::Save(world).json;
    // As another build saved it: its string 8 bytes bigger.
    const std::string size = std::format("\"size\": {}", sizeof(std::string)), bigger = std::format("\"size\": {}", sizeof(std::string) + 8);
    ASSERT_NE(json.find("\"type\": \"string\", \"offset\": 0, " + size), std::string::npos) << json;
    json.replace(json.find("\"type\": \"string\", \"offset\": 0, " + size), std::string("\"type\": \"string\", \"offset\": 0, " + size).size(),
                 "\"type\": \"string\", \"offset\": 0, " + bigger);

    World other;
    const kecs::ComponentId now = other.Register(describe());
    const kecs::Loaded loaded = kecs::Load(other, json);
    ASSERT_TRUE(loaded.ok);
    EXPECT_TRUE(loaded.problems.empty()) << loaded.problems.front();
    EXPECT_EQ(static_cast<const Named*>(other.Get(loaded.entities[0], now))->text, "brass");
}

namespace {
    struct Vec3 { float x, y, z; };
    struct Opaque { int a = 1; int b = 2; };              // plain, and says nothing of its fields
    struct Secret { std::string text; };                   // owns something, and says nothing
    struct Moving { Vec3 position {}; float special = 0; };
}
template <> struct kecs::Name<Vec3> { static constexpr std::string_view value = "Vec3"; };
KECS_FIELDS(Moving, position, special)

TEST(Serialize, WhatHasNoCodecIsKeptAsBytesWherePlainAndReportedWhereNot) {
    World world;
    const Entity e = world.Create(Opaque { 5, 6 }, Secret { "hidden" }, Moving { { 1, 2, 3 }, std::numeric_limits<float>::infinity() });
    const kecs::Saved saved = kecs::Save(world);
    ASSERT_EQ(saved.problems.size(), 1u);
    EXPECT_NE(saved.problems[0].find("Secret"), std::string::npos) << saved.problems[0];
    EXPECT_NE(saved.json.find("$bytes"), std::string::npos);

    World other;
    const kecs::Loaded loaded = kecs::Load(other, saved.json);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    const Entity made = loaded.entities[0];
    EXPECT_EQ(other.Get<Opaque>(made)->b, 6) << "plain data with no fields: its bytes";
    EXPECT_EQ(other.Get<Moving>(made)->position.z, 3.f) << "a plain field with no codec: its bytes";
    EXPECT_TRUE(std::isinf(other.Get<Moving>(made)->special));
    EXPECT_FALSE(other.Has<Secret>(made));
    (void)e;

    // With a codec, the same field is written as itself.
    kecs::Codecs codecs;
    codecs.Register("Vec3", {
        .write = [](const void* v, const kecs::SaveContext&) { const auto& p = *static_cast<const Vec3*>(v); return std::format("[{}, {}, {}]", p.x, p.y, p.z); },
        .read = [](void* v, const std::string_view json, const kecs::LoadContext&) {
            auto& p = *static_cast<Vec3*>(v);
            return std::sscanf(std::string(json).c_str(), " [ %f , %f , %f ]", &p.x, &p.y, &p.z) == 3;
        },
    });
    const kecs::Saved readable = kecs::Save(world, codecs);
    EXPECT_NE(readable.json.find("\"position\": [1, 2, 3]"), std::string::npos) << readable.json;
    World third;
    const kecs::Loaded again = kecs::Load(third, readable.json, codecs);
    EXPECT_EQ(third.Get<Moving>(again.entities[0])->position.y, 2.f);
}

TEST(Serialize, RefusesWhatIsNotASavedWorld) {
    World world;
    kecs::Loaded loaded = kecs::Load(world, "{ \"kecs\": 1, \"types\": [ ");
    EXPECT_FALSE(loaded.ok);
    EXPECT_FALSE(loaded.error.empty());
    loaded = kecs::Load(world, "[1, 2, 3]");
    EXPECT_FALSE(loaded.ok);
    loaded = kecs::Load(world, "{ \"kecs\": 2, \"types\": [], \"entities\": [] }");
    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(world.EntityCount(), 0u) << "nothing was made";

    loaded = kecs::Load(world, "{ \"kecs\": 1, \"types\": [], \"entities\": [ { \"id\": 0, \"components\": { \"Ghost\": {} } } ] }");
    EXPECT_TRUE(loaded.ok);
    EXPECT_EQ(loaded.entities.size(), 1u);
    ASSERT_EQ(loaded.problems.size(), 1u);
    EXPECT_NE(loaded.problems[0].find("Ghost"), std::string::npos);
}

TEST(Serialize, AddsToWhatTheWorldHolds) {
    World world;
    Fill(world);
    const std::string json = kecs::Save(world).json;
    const kecs::Loaded loaded = kecs::Load(world, json);
    ASSERT_TRUE(loaded.ok);
    EXPECT_EQ(world.EntityCount(), 8u);
    EXPECT_EQ(world.Get<Named>(loaded.entities[1])->target, loaded.entities[2]) << "references are among what was loaded";
}

// A field whose type is named more narrowly than when it was saved — a mesh's "Mesh", saved as any "resource" — reads what
// was saved when its codecs say it does; and is left at its default when they do not.
TEST(Serialize, ATypeNamedMoreNarrowlyReadsWhatWasSavedWhenItSaysItDoes) {
    kecs::Codecs codecs;
    const kecs::Codec number {
        .write = [](const void* v, const kecs::SaveContext&) { return std::format("{}", *static_cast<const float*>(v)); },
        .read = [](void* v, const std::string_view json, const kecs::LoadContext&) { *static_cast<float*>(v) = std::stof(std::string(json)); return true; },
    };
    codecs.Register("number", number);
    codecs.Register("distance", number);
    const auto lens = [](World& world, const char* type) {
        return world.Register([type] {
            kecs::ComponentDescription d;
            d.name = "lens";
            d.size = 4;
            d.alignment = 4;
            d.fields = { { "focus", type, 0, 4 } };
            return d;
        }());
    };
    World world;
    const float focus = 2.5f;
    world.Add(world.Create(), lens(world, "number"), &focus);
    const std::string json = kecs::Save(world, codecs).json;

    World narrower;
    const kecs::ComponentId now = lens(narrower, "distance");
    const kecs::Loaded refused = kecs::Load(narrower, json, codecs);
    ASSERT_TRUE(refused.ok) << refused.error;
    EXPECT_EQ(*static_cast<const float*>(narrower.Get(refused.entities[0], now)), 0.f) << "not said: another type, its default";

    codecs.Reads("distance", "number");
    World reading;
    const kecs::ComponentId again = lens(reading, "distance");
    const kecs::Loaded read = kecs::Load(reading, json, codecs);
    ASSERT_TRUE(read.ok) << read.error;
    EXPECT_EQ(*static_cast<const float*>(reading.Get(read.entities[0], again)), 2.5f) << "said: what was saved";
    EXPECT_TRUE(read.problems.empty());
}

// A component the world works out again from what is saved is not saved: a file would only repeat it.
TEST(Serialize, WhatIsNotSavedIsLeftOut)
{
    World world;
    const kecs::ComponentId worked = world.Register([] {
        kecs::ComponentDescription d;
        d.name = "PerFrame";
        d.saved = false;
        return d;
    }());
    const Entity e = world.Create(Position { 1, 2 });
    world.Add(e, worked);
    const kecs::Saved saved = kecs::Save(world);
    EXPECT_EQ(saved.json.find("PerFrame"), std::string::npos) << saved.json;
    EXPECT_NE(saved.json.find("Position"), std::string::npos);
}
