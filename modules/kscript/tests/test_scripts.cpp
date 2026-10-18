// Scripts as the engine sees them: a component, and functions called on each one as it is made, runs and goes.
// No language but this one, and no GPU.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <kscript/scripts.h>

namespace {
    struct Mover {
        float x = 0.f;
        float speed = 0.f;
    };

    /** What was called, in order: "created 3", "update 3". */
    struct Log {
        std::vector<std::string> calls;
        kecs::World* world = nullptr;
        kecs::ComponentId id = kecs::NoComponent;
        void Say(const char* what, const kecs::Entity entity) { calls.push_back(std::string(what) + " " + std::to_string(entity.index)); }
        std::vector<std::string> Take() { return std::exchange(calls, {}); }
    };

    kscript::ScriptType MoverType(Log& log)
    {
        kscript::ScriptType type;
        type.name = "Mover";
        type.size = sizeof(Mover);
        type.alignment = alignof(Mover);
        type.fields = { { "x", "f32", offsetof(Mover, x), 4 }, { "speed", "f32", offsetof(Mover, speed), 4 } };
        type.callbacks.user = &log;
        type.callbacks.created = [](void* user, const kecs::Entity e, void*) { static_cast<Log*>(user)->Say("created", e); };
        type.callbacks.enabled = [](void* user, const kecs::Entity e, void*) { static_cast<Log*>(user)->Say("enabled", e); };
        type.callbacks.started = [](void* user, const kecs::Entity e, void*) { static_cast<Log*>(user)->Say("started", e); };
        type.callbacks.update = [](void* user, const kecs::Entity e, void* data, const float delta) {
            auto& mover = *static_cast<Mover*>(data);
            mover.x += mover.speed * delta;
            static_cast<Log*>(user)->Say("update", e);
        };
        type.callbacks.lateUpdate = [](void* user, const kecs::Entity e, void*, float) { static_cast<Log*>(user)->Say("late", e); };
        type.callbacks.fixedUpdate = [](void* user, const kecs::Entity e, void*, float) { static_cast<Log*>(user)->Say("fixed", e); };
        type.callbacks.prepare = [](void* user, const kecs::Entity e, void*) { static_cast<Log*>(user)->Say("prepare", e); };
        type.callbacks.disabled = [](void* user, const kecs::Entity e, void*) { static_cast<Log*>(user)->Say("disabled", e); };
        type.callbacks.destroyed = [](void* user, const kecs::Entity e, void* data) {
            static_cast<Log*>(user)->Say(data ? "destroyed" : "destroyed-with-nothing", e);
        };
        return type;
    }
    using Calls = std::vector<std::string>;
}

TEST(Scripts, AComponentIsToldOfItsLifeInOrder) {
    kecs::World world;
    kscript::Scripts scripts(world);
    Log log;
    const kecs::ComponentId mover = scripts.Register(MoverType(log));
    ASSERT_NE(mover, kecs::NoComponent);
    EXPECT_EQ(world.Find("Mover"), mover);
    EXPECT_TRUE(scripts.Registered("Mover"));
    ASSERT_EQ(world.Describe(mover).fields.size(), 2u) << "its fields are the component's, for an inspector and a file";

    const kecs::Entity entity = world.Create();
    const Mover start { 1.f, 2.f };
    world.Add(entity, mover, &start);
    EXPECT_TRUE(log.calls.empty()) << "nothing until the frame";

    scripts.Update(0.5f);
    EXPECT_EQ(log.Take(), (Calls { "created 0", "enabled 0", "started 0", "update 0" }));
    EXPECT_FLOAT_EQ(static_cast<const Mover*>(std::as_const(world).Get(entity, mover))->x, 2.f) << "its data is the world's: 1 + 2 * 0.5";
    scripts.LateUpdate(0.5f);
    scripts.Prepare();
    scripts.FixedUpdate(1.f / 60.f);
    scripts.Update(0.5f);
    EXPECT_EQ(log.Take(), (Calls { "late 0", "prepare 0", "fixed 0", "update 0" })) << "made and started once";

    // Switched off: told, and left alone. Switched on again: told, and not started twice.
    world.SetEnabled(entity, mover, false);
    scripts.Update(0.5f);
    scripts.Update(0.5f);
    scripts.FixedUpdate(0.1f);
    EXPECT_EQ(log.Take(), (Calls { "disabled 0" }));
    world.SetEnabled(entity, mover, true);
    scripts.Update(0.5f);
    EXPECT_EQ(log.Take(), (Calls { "enabled 0", "update 0" }));

    // Taken away while on: switched off, then destroyed, with its data still there to read.
    world.Remove(entity, mover);
    EXPECT_EQ(log.Take(), (Calls { "disabled 0", "destroyed 0" }));
    scripts.Update(0.5f);
    EXPECT_TRUE(log.Take().empty());
}

TEST(Scripts, PrepareAloneStartsNothing) {
    // An editor's scene is prepared and drawn every frame, played or not: what a script does as it starts waits for Play.
    kecs::World world;
    kscript::Scripts scripts(world);
    Log log;
    const kecs::ComponentId mover = scripts.Register(MoverType(log));
    const kecs::Entity entity = world.Create();
    world.Add(entity, mover);
    scripts.Prepare();
    scripts.Prepare();
    EXPECT_EQ(log.Take(), (Calls { "prepare 0", "prepare 0" }));
    scripts.Update(1.f);
    EXPECT_EQ(log.Take(), (Calls { "created 0", "enabled 0", "started 0", "update 0" })) << "the first update starts it";
    world.SetEnabled(entity, mover, false);
    scripts.Prepare();
    EXPECT_TRUE(log.Take().empty()) << "one switched off is not prepared";
}

TEST(Scripts, OneMadeSwitchedOffIsNotStartedUntilItIsOn) {
    kecs::World world;
    kscript::Scripts scripts(world);
    Log log;
    const kecs::ComponentId mover = scripts.Register(MoverType(log));
    const kecs::Entity entity = world.Create();
    world.Add(entity, mover);
    world.SetEnabled(entity, mover, false);
    scripts.Update(1.f);
    EXPECT_EQ(log.Take(), (Calls { "created 0" }));
    world.SetEnabled(entity, mover, true);
    scripts.Update(1.f);
    EXPECT_EQ(log.Take(), (Calls { "enabled 0", "started 0", "update 0" }));

    // Destroyed with its entity while off: destroyed, and no switching off to tell of.
    world.SetEnabled(entity, mover, false);
    scripts.Update(1.f);
    log.Take();
    world.Destroy(entity);
    EXPECT_EQ(log.Take(), (Calls { "destroyed 0" }));
}

TEST(Scripts, WhatIsCalledMayChangeTheWorld) {
    kecs::World world;
    kscript::Scripts scripts(world);
    Log log;
    log.world = &world;
    kscript::ScriptType type = MoverType(log);
    // Each update makes another entity with the script, and the third to run destroys itself.
    type.callbacks.update = [](void* user, const kecs::Entity e, void* data, float) {
        auto& self = *static_cast<Log*>(user);
        self.Say("update", e);
        auto& mover = *static_cast<Mover*>(data);
        if (mover.speed > 0.f) {
            const kecs::Entity spawned = self.world->Create();
            self.world->Add(spawned, self.id);
        }
        if (e.index == 2) self.world->Destroy(e);
    };
    const kecs::ComponentId mover = log.id = scripts.Register(std::move(type));
    const kecs::Entity first = world.Create();
    const Mover spawner { 0.f, 1.f };
    world.Add(first, mover, &spawner);

    scripts.Update(1.f);
    EXPECT_EQ(log.Take(), (Calls { "created 0", "enabled 0", "started 0", "update 0" })) << "the one it made runs from the next frame";
    scripts.Update(1.f);
    EXPECT_EQ(log.Take(), (Calls { "created 1", "enabled 1", "started 1", "update 0", "update 1" }));
    scripts.Update(1.f);
    const Calls third = log.Take();
    EXPECT_NE(std::ranges::find(third, "update 2"), third.end());
    EXPECT_NE(std::ranges::find(third, "destroyed 2"), third.end()) << "gone by its own hand, and told so";
    EXPECT_EQ(world.EntityCount(), 3u);
    scripts.Update(1.f);
    const Calls fourth = log.Take();
    EXPECT_EQ(std::ranges::find(fourth, "update 2"), fourth.end());
}

TEST(Scripts, AScriptLoadedAgainKeepsWhatKeepsItsNameAndType) {
    kecs::World world;
    kscript::Scripts scripts(world);
    Log log;
    const kecs::ComponentId mover = scripts.Register(MoverType(log));
    const kecs::Entity entity = world.Create();
    const Mover start { 3.f, 4.f };
    world.Add(entity, mover, &start);
    scripts.Update(0.f);
    log.Take();

    // Its code going: nothing is called, and nothing is told it was destroyed. The data stays.
    scripts.Unregister("Mover");
    EXPECT_FALSE(scripts.Registered("Mover"));
    scripts.Update(1.f);
    EXPECT_TRUE(log.Take().empty());
    EXPECT_FLOAT_EQ(static_cast<const Mover*>(std::as_const(world).Get(entity, mover))->x, 3.f);

    // The edited script: speed is a double now, x is as it was, and there is a new field.
    struct Mover2 { float x; double speed; std::int32_t laps; };
    kscript::ScriptType edited = MoverType(log);
    edited.size = sizeof(Mover2);
    edited.alignment = alignof(Mover2);
    edited.fields = { { "x", "f32", offsetof(Mover2, x), 4 }, { "speed", "f64", offsetof(Mover2, speed), 8 }, { "laps", "i32", offsetof(Mover2, laps), 4 } };
    edited.callbacks.update = [](void* user, const kecs::Entity e, void* data, float) {
        static_cast<Mover2*>(data)->laps += 1;
        static_cast<Log*>(user)->Say("update2", e);
    };
    kecs::Migration migration;
    std::string error;
    EXPECT_EQ(scripts.Register(std::move(edited), &error, &migration), mover) << "the same component, in a new shape: " << error;
    EXPECT_EQ(migration.kept, std::vector<std::string> { "x" });
    EXPECT_EQ(migration.reset, (std::vector<std::string> { "speed", "laps" }));
    EXPECT_EQ(migration.entities, 1u);

    scripts.Update(1.f);
    EXPECT_EQ(log.Take(), (Calls { "update2 0" })) << "the new code, on a component that is not made or started again";
    const auto* now = static_cast<const Mover2*>(std::as_const(world).Get(entity, mover));
    EXPECT_FLOAT_EQ(now->x, 3.f);
    EXPECT_EQ(now->speed, 0.0);
    EXPECT_EQ(now->laps, 1);

    // Loaded again unchanged: nothing moves.
    kscript::ScriptType same = MoverType(log);
    same.size = sizeof(Mover2);
    same.alignment = alignof(Mover2);
    same.fields = world.Describe(mover).fields;
    kecs::Migration none;
    EXPECT_EQ(scripts.Register(std::move(same), &error, &none), mover);
    EXPECT_EQ(none.entities, 0u);
    EXPECT_EQ(static_cast<const Mover2*>(std::as_const(world).Get(entity, mover))->laps, 1);
}

TEST(Scripts, AScriptTakesTheDataAFileBroughtBeforeIt) {
    kecs::World world;
    // As a saved world loads it, before any script is: plain data, by name.
    kecs::ComponentDescription loaded { .name = "Mover", .size = sizeof(Mover), .alignment = alignof(Mover) };
    loaded.fields = { { "x", "f32", offsetof(Mover, x), 4 }, { "speed", "f32", offsetof(Mover, speed), 4 } };
    const kecs::ComponentId id = world.Register(loaded);
    const kecs::Entity entity = world.Create();
    const Mover saved { 5.f, 6.f };
    world.Add(entity, id, &saved);

    kscript::Scripts scripts(world);
    Log log;
    EXPECT_EQ(scripts.Register(MoverType(log)), id);
    scripts.Update(1.f);
    EXPECT_FLOAT_EQ(static_cast<const Mover*>(std::as_const(world).Get(entity, id))->x, 11.f) << "5 + 6: the values the file had";

    // A name that is something else's is refused.
    struct Owning { std::vector<int> items; };
    world.Id<Owning>();
    kscript::ScriptType clash = MoverType(log);
    clash.name = std::string(kecs::Name<Owning>::value);
    std::string error;
    EXPECT_EQ(scripts.Register(std::move(clash), &error), kecs::NoComponent);
    EXPECT_NE(error.find("not a script's"), std::string::npos) << error;
}

TEST(Scripts, AScriptWithNoFieldsAndTheEndOfTheWorld) {
    kecs::World world;
    Log log;
    {
        kscript::Scripts scripts(world);
        kscript::ScriptType bare = MoverType(log);
        bare.name = "Bare";
        bare.size = 0;
        bare.fields.clear();
        bare.callbacks.update = [](void* user, const kecs::Entity e, void*, float) { static_cast<Log*>(user)->Say("update", e); };
        const kecs::ComponentId id = scripts.Register(std::move(bare));
        ASSERT_NE(id, kecs::NoComponent);
        EXPECT_EQ(scripts.Types(), std::vector<std::string> { "Bare" });
        for (int i = 0; i < 3; ++i) world.Add(world.Create(), id);
        world.SetEnabled(kecs::Entity { 2, 0 }, id, false);
        scripts.Update(1.f);
        log.Take();
        scripts.Shutdown();
        EXPECT_EQ(log.Take(), (Calls { "disabled 0", "destroyed 0", "disabled 1", "destroyed 1", "destroyed 2" }));
        scripts.Shutdown();
        EXPECT_TRUE(log.Take().empty()) << "once";
    }
    // The scripts gone, the world goes on, and tells nobody.
    world.Destroy(kecs::Entity { 0, 0 });
    EXPECT_TRUE(log.Take().empty());
}
