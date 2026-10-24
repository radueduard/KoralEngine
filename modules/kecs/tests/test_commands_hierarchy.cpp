// Changes queued for after a query; the hierarchy; and observers of components coming and going.

#include <cmath>

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
    EXPECT_EQ(world.Roots(), (std::vector<Entity> { root, b })) << "at the top again, after what was there";
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

namespace {
    void ExpectNear(const kecs::Vec3& a, const kecs::Vec3& b, const char* what)
    {
        EXPECT_NEAR(a.x, b.x, 1e-3f) << what;
        EXPECT_NEAR(a.y, b.y, 1e-3f) << what;
        EXPECT_NEAR(a.z, b.z, 1e-3f) << what;
    }
    kecs::Vec3 PlaceOf(const kecs::Mat4& m) { return { m.At(3, 0), m.At(3, 1), m.At(3, 2) }; }
}

// A name, an order and a depth: every entity has them, and they are no components of it.
TEST(Hierarchy, EveryEntityHasANameAndAPlaceInOrder) {
    World world;
    const Entity root = world.Create(), a = world.Create(), b = world.Create(), c = world.Create();
    EXPECT_EQ(world.NameOf(root), "Entity");
    EXPECT_TRUE(world.ComponentsOf(root).empty()) << "none of it is a component";
    world.Rename(root, "Root");
    world.Rename(b, "Second");
    EXPECT_EQ(world.FindEntity("Second"), b);
    EXPECT_TRUE(world.FindEntity("Nobody").IsNull());

    world.SetParent(a, root);
    world.SetParent(b, root);
    world.SetParent(c, root, 0);
    EXPECT_EQ(world.ChildrenOf(root), (std::vector<Entity> { c, a, b })) << "put first, where it was asked to go";
    world.SetParent(c, root, 3);
    EXPECT_EQ(world.ChildrenOf(root), (std::vector<Entity> { a, b, c })) << "moved later among the same: its own place does not count";
    world.SetParent(b, a);
    EXPECT_EQ(world.All(), (std::vector<Entity> { root, a, b, c })) << "parents before their children";
    EXPECT_EQ(world.DepthOf(root), 0u);
    EXPECT_EQ(world.DepthOf(b), 2u);
    EXPECT_TRUE(world.IsAncestor(root, b));
    EXPECT_FALSE(world.IsAncestor(b, root));
    EXPECT_FALSE(world.IsAncestor(c, b));
    EXPECT_EQ(world.Roots(), std::vector<Entity> { root });

    // A place given back is a new entity's from the start.
    world.Destroy(a);
    const Entity again = world.Create();
    EXPECT_EQ(world.NameOf(again), "Entity");
    EXPECT_TRUE(world.ParentOf(again).IsNull());
    EXPECT_TRUE(world.ChildrenOf(again).empty());
    EXPECT_EQ(world.NameOf(a), "") << "what is gone has no name";
}

// Where an entity is in the world is where everything above it is, and then its own place.
TEST(Transform, AnEntityIsPlacedRelativeToWhatItIsUnder) {
    World world;
    const Entity parent = world.Create(), child = world.Create();
    ExpectNear(world.ScaleOf(child), { 1, 1, 1 }, "a scale of one, to begin with");
    EXPECT_EQ(world.WorldMatrix(child), kecs::Mat4 {});

    world.SetParent(child, parent);
    world.SetPosition(parent, { 10, 0, 0 });
    world.SetPosition(child, { 1, 0, 0 });
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 11, 0, 0 }, "moved with its parent");
    ExpectNear(PlaceOf(world.LocalMatrix(child)), { 1, 0, 0 }, "its own");

    // Turned a quarter about Y, +X goes to -Z; scaled, twice as far.
    world.SetRotation(parent, { 0, 90, 0 });
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 10, 0, -1 }, "turned with its parent");
    world.SetScale(parent, { 2, 2, 2 });
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 10, 0, -2 }, "and scaled with it");

    // Taken out from under it, it stays where it is: its own transform makes up for the parent's.
    world.SetParent(child, kecs::NullEntity);
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 10, 0, -2 }, "kept where it was");
    ExpectNear(world.PositionOf(child), { 10, 0, -2 }, "its own place is now the world's");
    EXPECT_NEAR(world.RotationOf(child).y, 90.f, 0.05f) << "turned as its parent was (a quarter turn is where the angle is least exact)";
    ExpectNear(world.ScaleOf(child), { 2, 2, 2 }, "and as big");
    // Or goes where the same numbers put it under the new one, when asked.
    world.SetParent(child, parent, -1, false);
    ExpectNear(world.PositionOf(child), { 10, 0, -2 }, "its own numbers as they were");
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 10 - 4, 0, -20 }, "so somewhere else in the world");

    // Placed in the world: only the parts asked for change.
    world.SetRotation(child, { 0, 0, 30 });
    kecs::Mat4 there;
    there.At(3, 0) = 3; there.At(3, 1) = 4; there.At(3, 2) = 5;
    world.SetWorldMatrix(child, there, true, false, false);
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 3, 4, 5 }, "where it was put");
    ExpectNear(world.RotationOf(child), { 0, 0, 30 }, "turned as it was");

    const std::uint64_t before = world.SceneVersion();
    world.SetPosition(child, world.PositionOf(child));
    EXPECT_EQ(world.SceneVersion(), before) << "nothing changed";
    world.SetPosition(child, { 0, 0, 0 });
    EXPECT_GT(world.SceneVersion(), before);
}

// Angles go to a matrix and come back the same.
TEST(Transform, ComposedIsDecomposedToWhatItWas) {
    const kecs::Vec3 position { 1, -2, 3 }, rotation { 20, -35, 50 }, scale { 2, 0.5f, 3 };
    kecs::Vec3 p, r, s;
    ASSERT_TRUE(kecs::Decompose(kecs::Compose(position, rotation, scale), p, r, s));
    ExpectNear(p, position, "position");
    ExpectNear(r, rotation, "rotation");
    ExpectNear(s, scale, "scale");
    const kecs::Mat4 m = kecs::Compose(position, rotation, scale);
    const kecs::Mat4 one = m * kecs::Inverse(m);
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) EXPECT_NEAR(one.At(column, row), column == row ? 1.f : 0.f, 1e-4f);
    EXPECT_FALSE(kecs::Decompose(kecs::Compose({}, {}, { 1, 0, 1 }), p, r, s)) << "flat along an axis: nothing says how it is turned";
}

// A rotation integrated a step at a time, as a body is turned by its angular velocity, is kept as the quaternion it was
// given: going round by the Euler angles would round it off a little each step, and badly near a quarter turn about Y.
TEST(Transform, AnEntityTurnedByAQuaternionKeepsIt) {
    World world;
    const Entity body = world.Create(), child = world.Create();
    world.SetParent(child, body);
    world.SetPosition(child, { 1, 0, 0 });
    EXPECT_EQ(world.OrientationOf(body), kecs::Quat {}) << "unturned, to begin with";

    // A quarter turn about Y, as Euler angles and as a quaternion, is the same turn.
    const float half = std::sqrt(0.5f);
    world.SetRotation(body, { 0, 90, 0 });
    const kecs::Quat byAngles = world.OrientationOf(body);
    EXPECT_NEAR(std::abs(byAngles.y), half, 1e-5f);
    EXPECT_NEAR(std::abs(byAngles.w), half, 1e-5f);
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 0, 0, -1 }, "turned by the angles");

    // Set as a quaternion — not of length 1 — it is that turn, made length 1, and the angles are worked out of it.
    world.SetRotation(body, {});
    world.SetOrientation(body, { 0, 2 * half, 0, 2 * half });
    const kecs::Quat given = world.OrientationOf(body);
    EXPECT_NEAR(given.y, half, 1e-6f);
    EXPECT_NEAR(given.w, half, 1e-6f);
    ExpectNear(PlaceOf(world.WorldMatrix(child)), { 0, 0, -1 }, "turned by the quaternion");
    EXPECT_NEAR(world.RotationOf(body).y, 90.f, 0.05f) << "the angles follow";
    world.SetOrientation(body, { 0, 0, 0, 0 });
    EXPECT_EQ(world.OrientationOf(body), kecs::Quat {}) << "one of length 0 is no turn";

    // Turned through the pole a step at a time: what is set is what is read back, made length 1 again and no more.
    kecs::Quat q;
    const float step = 0.01f;     // radians about X, then about Y, each step
    for (int i = 0; i < 1000; ++i) {
        const kecs::Quat about { std::sin(step / 2), std::sin(step / 2), 0, std::cos(step / 2) };
        q = kecs::Normalize({ about.w * q.x + about.x * q.w + about.y * q.z - about.z * q.y,
                              about.w * q.y - about.x * q.z + about.y * q.w + about.z * q.x,
                              about.w * q.z + about.x * q.y - about.y * q.x + about.z * q.w,
                              about.w * q.w - about.x * q.x - about.y * q.y - about.z * q.z });
        world.SetOrientation(body, q);
        const kecs::Quat back = world.OrientationOf(body);
        ASSERT_NEAR(back.x, q.x, 1e-6f) << "step " << i;
        ASSERT_NEAR(back.y, q.y, 1e-6f) << "step " << i;
        ASSERT_NEAR(back.z, q.z, 1e-6f) << "step " << i;
        ASSERT_NEAR(back.w, q.w, 1e-6f) << "step " << i;
    }
    // Its matrix is the quaternion's.
    const kecs::Mat4 m = kecs::ComposeQuat({}, q, { 1, 1, 1 });
    for (std::size_t i = 0; i < 16; ++i) EXPECT_NEAR(world.LocalMatrix(body).m[i], m.m[i], 1e-6f);

    // Placed by a matrix, it takes the matrix's turn as a quaternion.
    world.SetRotation(body, {});
    world.SetWorldMatrix(body, kecs::ComposeQuat({ 1, 2, 3 }, kecs::Quat { 0, half, 0, half }, { 1, 1, 1 }));
    const kecs::Quat placed = world.OrientationOf(body);
    EXPECT_NEAR(std::abs(placed.y), half, 1e-5f);
    EXPECT_NEAR(std::abs(placed.w), half, 1e-5f);
    EXPECT_NEAR(world.RotationOf(body).y, 90.f, 0.05f);
}

// Euler angles go to a quaternion and back.
TEST(Transform, AnglesAndQuaternionsAreTheSameTurn) {
    const kecs::Vec3 angles { 20, -35, 50 };
    ExpectNear(kecs::ToEuler(kecs::ToQuat(angles)), angles, "round trip");
    const kecs::Mat4 byAngles = kecs::Compose({ 1, 2, 3 }, angles, { 2, 2, 2 }), byQuat = kecs::ComposeQuat({ 1, 2, 3 }, kecs::ToQuat(angles), { 2, 2, 2 });
    for (std::size_t i = 0; i < 16; ++i) EXPECT_NEAR(byAngles.m[i], byQuat.m[i], 1e-6f);
    kecs::Vec3 p, s;
    kecs::Quat q;
    ASSERT_TRUE(kecs::Decompose(byQuat, p, q, s));
    const kecs::Quat expected = kecs::ToQuat(angles);
    const float dot = q.x * expected.x + q.y * expected.y + q.z * expected.z + q.w * expected.w;
    EXPECT_NEAR(std::abs(dot), 1.f, 1e-5f) << "the same turn, either sign";
}

// What a list of entities is made of changes the structure's version; moving, or writing what a component holds, does
// not — and what is written is counted for that component alone.
TEST(World, TheStructuresVersionIsWhatListsOfEntitiesAreMadeOf) {
    kecs::World world;
    const kecs::Entity a = world.Create(), b = world.Create();
    world.Add<Position>(a);
    const kecs::ComponentId position = world.Id<Position>(), velocity = world.Id<Velocity>();
    const auto changes = [&] { return world.StructureVersion(); };

    std::uint64_t before = changes();
    world.SetPosition(a, { 1, 2, 3 });
    world.SetRotation(a, { 0, 45, 0 });
    EXPECT_EQ(changes(), before) << "moved: the same entities, the same components";
    const std::uint64_t written = world.ChangesOf(position), other = world.ChangesOf(velocity);
    world.Mutable<Position>(a)->x = 5;
    EXPECT_EQ(changes(), before) << "what it holds is no part of the structure";
    EXPECT_GT(world.ChangesOf(position), written) << "but it is counted as changed";
    EXPECT_EQ(world.ChangesOf(velocity), other) << "for that component only";

    const auto grows = [&](const char* what, const auto& change) {
        const std::uint64_t was = changes();
        change();
        EXPECT_GT(changes(), was) << what;
    };
    grows("given a component", [&] { world.Add<Velocity>(b); });
    grows("rid of one", [&] { world.Remove<Velocity>(b); });
    grows("one switched off", [&] { world.SetEnabled<Position>(a, false); });
    before = changes();
    world.SetEnabled<Position>(a, false);
    EXPECT_EQ(changes(), before) << "switched off again: nothing changed";
    grows("and on", [&] { world.SetEnabled<Position>(a, true); });
    grows("under another", [&] { world.SetParent(b, a); });
    grows("renamed", [&] { world.Rename(b, "b"); });
    grows("made", [&] { (void)world.Create(); });
    grows("destroyed", [&] { world.Destroy(b); });
}

// What a reader of world matrices needs to know to read again only what moved: when each entity's last changed, by a
// clock that goes up with every change — its own, something's above it, or being put under something else.
TEST(Transform, AnEntityIsStampedWhenWhereItIsChanges) {
    World world;
    const Entity parent = world.Create(), child = world.Create(), apart = world.Create();
    world.SetParent(child, parent);
    std::uint64_t seen = world.MoveClock();
    EXPECT_GT(world.MovedAt(child), 0u) << "made, and put under another";
    const auto read = [&] {     // what a reader does: reads every matrix, and the clock
        (void)world.WorldMatrix(parent); (void)world.WorldMatrix(child); (void)world.WorldMatrix(apart);
        seen = world.MoveClock();
    };
    read();
    EXPECT_EQ(world.MoveClock(), seen) << "read, nothing moved";

    world.SetPosition(parent, { 1, 0, 0 });
    EXPECT_GT(world.MovedAt(parent), seen);
    EXPECT_GT(world.MovedAt(child), seen) << "what is under it moved with it";
    EXPECT_LE(world.MovedAt(apart), seen) << "and nothing else";
    read();

    // Moved twice before it is read: still moved since.
    world.SetPosition(child, { 0, 1, 0 });
    world.SetPosition(child, { 0, 2, 0 });
    world.SetPosition(parent, { 2, 0, 0 });
    EXPECT_GT(world.MovedAt(child), seen);
    read();
    // The parent read alone leaves the child stale; moved again, the child is still moved since what read it last.
    world.SetPosition(parent, { 3, 0, 0 });
    (void)world.WorldMatrix(parent);
    const std::uint64_t parentOnly = world.MoveClock();
    world.SetPosition(parent, { 4, 0, 0 });
    EXPECT_GT(world.MovedAt(child), seen);
    EXPECT_GT(world.MovedAt(child), parentOnly) << "stamped again, though it was stale already";

    world.SetScale(apart, { 2, 2, 2 });
    EXPECT_GT(world.MovedAt(apart), seen);
    read();
    world.SetParent(apart, child);
    EXPECT_GT(world.MovedAt(apart), seen) << "put under another: its world matrix is another";
    world.Destroy(apart);
    EXPECT_EQ(world.MovedAt(apart), 0u) << "gone";
    const Entity again = world.Create();
    EXPECT_GT(world.MovedAt(again), seen) << "one made in its place is moved since";
}

// A component written is stamped with the tick it was written at, for a reader to read again only what changed since.
TEST(Ticks, AComponentWrittenSaysWhen) {
    World world;
    const Entity a = world.Create(Position { 1, 2 }), b = world.Create(Position { 3, 4 });
    const kecs::ComponentId position = world.Id<Position>(), velocity = world.Id<Velocity>();
    const kecs::Tick first = world.ChangedTickOf(a, position);
    EXPECT_GT(first, 0u);
    EXPECT_EQ(world.ChangedTickOf(a, velocity), 0u) << "one it does not have";
    const kecs::Tick read = world.CurrentTick();
    world.AdvanceTick();
    world.Mutable<Position>(b)->x = 5;
    EXPECT_LE(world.ChangedTickOf(a, position), read);
    EXPECT_GT(world.ChangedTickOf(b, position), read) << "written since";
    world.Add<Velocity>(a);
    EXPECT_GT(world.ChangedTickOf(a, velocity), read) << "given since";
    EXPECT_EQ(world.ChangedTickOf(a, position), first) << "moved to another archetype, its tick with it";
}
