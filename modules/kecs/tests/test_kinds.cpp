// Kinds: an abstract component type that an entity is of through a component that is one of it.

#include <gtest/gtest.h>

#include <cstring>

#include <kecs/kecs.h>
#include <kecs/serialize.h>

using kecs::ComponentId;
using kecs::Entity;
using kecs::World;

namespace {
    /** A Camera's fields — near, far — at the front of each camera: a perspective's field of view, an orthographic's height after them. */
    kecs::ComponentDescription Camera()
    {
        kecs::ComponentDescription d;
        d.name = "Camera";
        d.abstract = true;
        d.size = 8;
        d.alignment = 4;
        d.fields = { { "near", "f32", 0, 4 }, { "far", "f32", 4, 4 } };
        return d;
    }

    kecs::ComponentDescription OneOf(const char* name, const char* own)
    {
        kecs::ComponentDescription d;
        d.name = name;
        d.kinds = { "Camera" };
        d.size = 12;
        d.alignment = 4;
        d.fields = { { "near", "f32", 0, 4 }, { "far", "f32", 4, 4 }, { own, "f32", 8, 4 } };
        return d;
    }

    struct Lens { float near, far, own; };
}

TEST(Kinds, AnEntityIsOfAKindThroughWhatItHas) {
    World world;
    const ComponentId camera = world.Register(Camera());
    const ComponentId perspective = world.Register(OneOf("PerspectiveCamera", "fieldOfView"));
    const ComponentId orthographic = world.Register(OneOf("OrthographicCamera", "height"));
    const Lens wide { 0.1f, 100.f, 60.f }, box { 0.5f, 50.f, 10.f };
    const Entity a = world.Create();
    world.Add(a, perspective, &wide);
    const Entity b = world.Create();
    world.Add(b, orthographic, &box);
    const Entity none = world.Create();

    EXPECT_TRUE(world.Has(a, camera));
    EXPECT_TRUE(world.Has(b, camera));
    EXPECT_FALSE(world.Has(none, camera));
    EXPECT_EQ(world.Which(a, camera), perspective);
    EXPECT_EQ(world.Which(b, camera), orthographic);
    EXPECT_EQ(world.Which(none, camera), kecs::NoComponent);
    EXPECT_TRUE(world.IsOf(perspective, camera));
    EXPECT_FALSE(world.IsOf(camera, perspective));

    // The kind's fields, read through its id from whichever the entity has.
    const auto* of = static_cast<const Lens*>(world.Get(b, camera));
    ASSERT_NE(of, nullptr);
    EXPECT_FLOAT_EQ(of->near, 0.5f);
    EXPECT_FLOAT_EQ(of->far, 50.f);

    kecs::QueryDescription query;
    query.all = { camera };
    EXPECT_EQ(world.Count(query), 2u);
    // Nothing has the kind itself, and nothing loses it: what is one of it is what is added and removed.
    EXPECT_FALSE(world.Add(none, camera));
    EXPECT_FALSE(world.Remove(a, camera));
    EXPECT_TRUE(world.Has(a, perspective));
    EXPECT_EQ(world.ComponentsOf(a), std::vector { perspective });
}

TEST(Kinds, WhatIsOneOfAKindChangesIt) {
    World world;
    const ComponentId camera = world.Register(Camera());
    const ComponentId perspective = world.Register(OneOf("PerspectiveCamera", "fieldOfView"));
    const Entity a = world.Create();
    const std::uint64_t before = world.ChangesOf(camera);
    world.Add(a, perspective);
    EXPECT_GT(world.ChangesOf(camera), before);
    const std::uint64_t added = world.ChangesOf(perspective);
    ASSERT_NE(world.Mutable(a, camera), nullptr);       // written through the kind: what is one of it is written
    EXPECT_GT(world.ChangesOf(perspective), added);
    EXPECT_TRUE(world.SetEnabled(a, camera, false));
    EXPECT_FALSE(world.IsEnabled(a, perspective));
    kecs::QueryDescription query;
    query.all = { camera };
    EXPECT_EQ(world.Count(query), 0u);
}

TEST(Kinds, AKindRegisteredAfterWhatIsOneOfItFindsIt) {
    World world;
    const ComponentId perspective = world.Register(OneOf("PerspectiveCamera", "fieldOfView"));
    const Entity a = world.Create();
    world.Add(a, perspective);
    kecs::QueryDescription early;
    early.all = { perspective };
    EXPECT_EQ(world.Count(early), 1u);
    const ComponentId camera = world.Register(Camera());
    EXPECT_TRUE(world.Has(a, camera));
    kecs::QueryDescription query;
    query.all = { camera };
    EXPECT_EQ(world.Count(query), 1u);
}

TEST(Kinds, OneSavedAsWhatIsAKindNowLoadsAsWhatItSays) {
    // Saved before Camera was a kind: one component of every field, `orthographic` saying which it was.
    const std::string saved = R"({
  "kecs": 1,
  "types": [
    { "name": "Camera", "size": 16, "alignment": 4, "plain": true, "fields": [
        { "name": "fieldOfView", "type": "f32", "offset": 0, "size": 4 },
        { "name": "near", "type": "f32", "offset": 4, "size": 4 },
        { "name": "far", "type": "f32", "offset": 8, "size": 4 },
        { "name": "orthographic", "type": "bool", "offset": 12, "size": 1 },
        { "name": "height", "type": "f32", "offset": 13, "size": 4 } ] }
  ],
  "entities": [
    { "id": 0, "name": "Wide", "components": { "Camera": { "fieldOfView": 45, "near": 0.2, "far": 80, "orthographic": false, "height": 10 } } },
    { "id": 1, "name": "Box", "components": { "Camera": { "fieldOfView": 60, "near": 0.5, "far": 40, "orthographic": true, "height": 7 } } }
  ]
})";
    World world;
    const ComponentId camera = world.Register(Camera());
    const ComponentId perspective = world.Register(OneOf("PerspectiveCamera", "fieldOfView"));
    const ComponentId orthographic = world.Register(OneOf("OrthographicCamera", "height"));
    kecs::Codecs codecs;
    codecs.Becomes("Camera", [](const std::string_view fields) {
        return fields.find("\"orthographic\": true") != std::string_view::npos ? "OrthographicCamera" : "PerspectiveCamera";
    });
    const kecs::Loaded loaded = kecs::Load(world, saved, codecs);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    EXPECT_TRUE(loaded.problems.empty()) << loaded.problems.front();
    ASSERT_EQ(loaded.entities.size(), 2u);
    EXPECT_EQ(world.Which(loaded.entities[0], camera), perspective);
    EXPECT_EQ(world.Which(loaded.entities[1], camera), orthographic);
    const auto* wide = static_cast<const Lens*>(world.Get(loaded.entities[0], perspective));
    const auto* box = static_cast<const Lens*>(world.Get(loaded.entities[1], orthographic));
    EXPECT_FLOAT_EQ(wide->own, 45.f);
    EXPECT_FLOAT_EQ(wide->far, 80.f);
    EXPECT_FLOAT_EQ(box->own, 7.f);
    EXPECT_FLOAT_EQ(box->near, 0.5f);

    // With nothing to say which, it is left out and said.
    World other;
    other.Register(Camera());
    const kecs::Loaded unsaid = kecs::Load(other, saved, kecs::Codecs());
    ASSERT_TRUE(unsaid.ok);
    EXPECT_FALSE(unsaid.problems.empty());
    EXPECT_TRUE(other.ComponentsOf(unsaid.entities[0]).empty());
}

TEST(Kinds, WhatIsSavedSaysItsKinds) {
    World world;
    world.Register(Camera());
    const ComponentId perspective = world.Register(OneOf("PerspectiveCamera", "fieldOfView"));
    const Entity a = world.Create();
    world.Add(a, perspective);
    const kecs::Saved saved = kecs::Save(world);
    EXPECT_NE(saved.json.find("\"kinds\": [\"Camera\"]"), std::string::npos) << saved.json;
    EXPECT_EQ(saved.json.find("\"name\": \"Camera\""), std::string::npos) << "a kind is never saved: nothing has it itself";

    // Loaded where neither is known, it is still of the kind once the kind is.
    World other;
    const kecs::Loaded loaded = kecs::Load(other, saved.json);
    ASSERT_TRUE(loaded.ok);
    const ComponentId camera = other.Register(Camera());
    EXPECT_TRUE(other.Has(loaded.entities[0], camera));
}
