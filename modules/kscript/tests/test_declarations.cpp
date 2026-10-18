// A script's component, read from its source without running it: the public fields of a C# or Kotlin script, laid
// out as its host lays them out, starting as they are initialised. And the engine's own package, read the same way.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <kscript/declarations.h>

namespace {
    const kecs::FieldDescription* Field(const kscript::ScriptType& type, const std::string_view name)
    {
        for (const auto& field : type.fields)
            if (field.name == name) return &field;
        return nullptr;
    }
    float FloatAt(const kscript::ScriptType& type, const std::string_view name, const std::size_t component = 0)
    {
        const auto* field = Field(type, name);
        if (!field || type.initial.empty()) return 0.f;
        float v = 0.f;
        std::memcpy(&v, type.initial.data() + field->offset + component * 4, 4);
        return v;
    }

    constexpr const char* Lamp = R"(
        using Koral;
        // A lamp: what it is, and a little of what it does.
        [ScriptName("Lamp")]
        public sealed class LampScript : Script
        {
            public Vec3 Color = new(1f, 0.5f, 0.25f);
            public float Range = 5f;        /* how far: { not a brace } */
            public bool On = true;
            public int Count;
            public double Exact = 2.5;
            private float _flicker = 3f;     // the object's own
            public float Doubled => Range * 2f;
            public static float Shared = 1f;
            public string Label = "{";       // a type no script keeps... were it a field the host kept
            protected override void Update(float delta) { _flicker += delta; }
        }
        public abstract class Base : Script { public float X; }
        public sealed class Other { public float Y; }
    )";
}

// Each public field, where the C# host puts it, starting as written; what is no field, and what is no script, is not.
TEST(Declarations, ACSharpScriptsFieldsAreItsComponent) {
    std::string source = Lamp;
    source.erase(source.find("            public string Label"), std::string_view("            public string Label = \"{\";       // a type no script keeps... were it a field the host kept\n").size());
    const auto found = kscript::Declare(source, false, "Lamp.cs");
    for (const auto& problem : found.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(found.types.size(), 1u) << "the abstract one and the one that is no script are not";
    const auto& lamp = found.types.front();
    EXPECT_EQ(lamp.name, "Lamp") << "named by its ScriptName";
    ASSERT_EQ(lamp.fields.size(), 5u);
    const char* names[] { "Color", "Range", "On", "Count", "Exact" };
    const char* types[] { "vec3", "f32", "bool", "i32", "f64" };
    const std::size_t offsets[] { 0, 12, 16, 20, 24 };
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(lamp.fields[i].name, names[i]);
        EXPECT_EQ(lamp.fields[i].type, types[i]);
        EXPECT_EQ(lamp.fields[i].offset, offsets[i]) << names[i] << ": each on its own alignment, as the host lays them out";
    }
    EXPECT_EQ(lamp.size, 32u);
    EXPECT_EQ(lamp.alignment, 8u);
    EXPECT_FLOAT_EQ(FloatAt(lamp, "Color", 1), 0.5f);
    EXPECT_FLOAT_EQ(FloatAt(lamp, "Range"), 5.f);
    EXPECT_EQ(lamp.initial[16], std::byte { 1 }) << "On";
    double exact = 0.0;
    std::memcpy(&exact, lamp.initial.data() + 24, 8);
    EXPECT_EQ(exact, 2.5);

    // A field of a type no script can keep: the type is not declared, and that is said.
    const auto bad = kscript::Declare(Lamp, false, "Lamp.cs");
    EXPECT_TRUE(bad.types.empty());
    ASSERT_FALSE(bad.problems.empty());
    EXPECT_NE(bad.problems.front().find("Lamp.cs: Lamp.Label is a string"), std::string::npos) << bad.problems.front();
}

// Kotlin's public vars, typed as written or as what they start as; private ones, vals and functions are not.
TEST(Declarations, AKotlinScriptsPropertiesAreItsComponent) {
    const auto found = kscript::Declare(R"(
        import koral.Vec3
        import koral.engine.Script

        class Lamp : Script() {
            var color = Vec3(1f, 0.5f,
                             0.25f)
            var range: Float = 5f
            var on = true
            var count = 3
            private var flicker = 0f
            val fixed = 1f
            override fun update(delta: Float) {
                flicker += delta
            }
        }
        abstract class Base : Script() { var x = 0f }
    )", true);
    for (const auto& problem : found.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(found.types.size(), 1u);
    const auto& lamp = found.types.front();
    ASSERT_EQ(lamp.fields.size(), 4u);
    EXPECT_EQ(lamp.fields[0].type, "vec3");
    EXPECT_EQ(lamp.fields[2].type, "bool");
    EXPECT_EQ(lamp.fields[3].type, "i32");
    EXPECT_EQ(lamp.fields[3].offset, 20u);
    EXPECT_FLOAT_EQ(FloatAt(lamp, "color", 2), 0.25f) << "a vector made over two lines";
    std::int32_t count = 0;
    std::memcpy(&count, lamp.initial.data() + 20, 4);
    EXPECT_EQ(count, 3);

    const auto unclear = kscript::Declare("class A : Script() { var at = Vec3.Zero\n var speed = 2f }", true);
    ASSERT_EQ(unclear.types.size(), 1u) << "a type it can tell is declared, with a field it cannot start at zero";
    ASSERT_EQ(unclear.problems.size(), 1u);
    EXPECT_NE(unclear.problems.front().find("A.at starts as 'Vec3.Zero'"), std::string::npos) << unclear.problems.front();
}

// Declared, a component is data: it starts as its source says, nothing is called on it, and it is not a script that
// runs. Registered afterwards — its host loaded it — it is the same component, and starts the same way.
TEST(Declarations, ADeclaredComponentStartsAsWrittenAndIsTheScriptsLater) {
    kecs::World world;
    kscript::Scripts scripts(world);
    auto declared = kscript::Declare("public sealed class Lamp : Script { public float Range = 5f; public float Speed; }", false);
    ASSERT_EQ(declared.types.size(), 1u);
    const kecs::ComponentId id = scripts.Declare(declared.types.front());
    ASSERT_NE(id, kecs::NoComponent);
    EXPECT_FALSE(scripts.Registered("Lamp"));
    const kecs::Entity lamp = world.Create();
    world.Add(lamp, id);
    float range = 0.f;
    std::memcpy(&range, world.Get(lamp, id), 4);
    EXPECT_EQ(range, 5.f);

    // The host's own type of it: no start of its own, so the declared one's.
    kscript::ScriptType hosted;
    hosted.name = "Lamp";
    hosted.size = 8;
    hosted.alignment = 4;
    hosted.fields = { { "Range", "f32", 0, 4 }, { "Speed", "f32", 4, 4 } };
    int updates = 0;
    hosted.callbacks.user = &updates;
    hosted.callbacks.update = [](void* user, kecs::Entity, void*, float) { ++*static_cast<int*>(user); };
    EXPECT_EQ(scripts.Register(hosted), id);
    EXPECT_TRUE(scripts.Registered("Lamp"));
    const kecs::Entity second = world.Create();
    world.Add(second, id);
    std::memcpy(&range, world.Get(second, id), 4);
    EXPECT_EQ(range, 5.f);
    scripts.Update(0.1f);
    EXPECT_EQ(updates, 2);

    // Declared again — its source read once more — what runs it keeps running.
    EXPECT_EQ(scripts.Declare(declared.types.front()), id);
    EXPECT_TRUE(scripts.Registered("Lamp"));
}

// The engine's package: the same components in C# and in Kotlin, declared into any world.
TEST(Declarations, ThePackageIsTheSameInEitherLanguage) {
    const auto folder = kscript::PackageDirectory() / "scripts";
    ASSERT_TRUE(std::filesystem::is_directory(folder)) << folder;
    for (const char* name : { "Camera", "DirectionalLight", "PointLight", "SpotLight", "Sky" }) {
        const auto csharp = kscript::DeclareFiles(folder / (std::string(name) + ".cs"));
        const auto kotlin = kscript::DeclareFiles(folder / (std::string(name) + ".kt"));
        for (const auto& problem : csharp.problems) ADD_FAILURE() << problem;
        for (const auto& problem : kotlin.problems) ADD_FAILURE() << problem;
        ASSERT_EQ(csharp.types.size(), 1u) << name;
        ASSERT_EQ(kotlin.types.size(), 1u) << name;
        const auto& a = csharp.types.front();
        const auto& b = kotlin.types.front();
        EXPECT_EQ(a.name, name);
        EXPECT_EQ(b.name, name);
        EXPECT_EQ(a.size, b.size) << name;
        EXPECT_EQ(a.initial, b.initial) << name << " starts the same in both";
        ASSERT_EQ(a.fields.size(), b.fields.size()) << name;
        for (std::size_t i = 0; i < a.fields.size(); ++i) {
            EXPECT_TRUE(a.fields[i].SameAs(b.fields[i])) << name << "." << a.fields[i].name;
            EXPECT_EQ(a.fields[i].offset, b.fields[i].offset);
        }
    }

    kecs::World world;
    kscript::Scripts scripts(world);
    for (const auto& problem : kscript::DeclarePackage(scripts)) ADD_FAILURE() << problem;
    const kecs::ComponentId lamp = world.Find("PointLight");
    ASSERT_NE(lamp, kecs::NoComponent);
    const kecs::Entity entity = world.Create();
    world.Add(entity, lamp);
    const auto* range = world.Describe(lamp).FindField("range");
    ASSERT_NE(range, nullptr);
    float value = 0.f;
    std::memcpy(&value, static_cast<const std::byte*>(world.Get(entity, lamp)) + range->offset, 4);
    EXPECT_EQ(value, 5.f);
}
