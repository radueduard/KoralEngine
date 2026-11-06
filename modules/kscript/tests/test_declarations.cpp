// A script's component, read from its source without running it: the public fields of a C# or Kotlin script, laid
// out as its host lays them out, starting as they are initialised. And the engine's own package, read the same way.

#include <gtest/gtest.h>

#include <algorithm>
#include <format>

#include <cmath>
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
// A class with no body before a script is passed by. It was taken for the script, with the script's properties, under
// its own name: `Particle` was offered to entities and `Simulator`, which is what runs, was not known.
TEST(Declarations, AClassWithNoBodyIsNotTheScriptAfterIt) {
    const auto found = kscript::Declare(R"(
        import koral.Vec2
        import koral.engine.PerFrame
        import koral.engine.Setup

        data class Particle(
            var position: Vec2,
            var mass: Float
        )

        fun Particle.moved(by: Vec2) = Particle(position + by, mass)

        class Simulator : Setup(), PerFrame {
            var center = Vec2(0f)
            var extent = Vec2(1f)
            override fun update(delta: Float) {}
        }
    )", true);
    for (const auto& problem : found.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(found.types.size(), 1u);
    EXPECT_EQ(found.types.front().name, "Simulator");
    ASSERT_EQ(found.types.front().fields.size(), 2u);
    EXPECT_FLOAT_EQ(FloatAt(found.types.front(), "extent", 0), 1.f) << "one number for a vector is each of its numbers";
    EXPECT_FLOAT_EQ(FloatAt(found.types.front(), "extent", 1), 1.f);

    const auto named = kscript::Declare("class Holder(val kind: Any = Particle::class) : Script() { var x = 1f }", true);
    ASSERT_EQ(named.types.size(), 1u) << "`::class` starts no class";
    EXPECT_EQ(named.types.front().name, "Holder");
}

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
    EXPECT_EQ(unclear.types.front().fields[0].type, "vec3") << "a type's constant is of that type, though its value is not read";

    // An entity said to start as none is one, and starts as none: nothing to note.
    const auto entity = kscript::Declare("class Joint : Script() { var other = Entity.Null\n var kind = 0 }", true);
    ASSERT_EQ(entity.types.size(), 1u);
    EXPECT_TRUE(entity.problems.empty()) << entity.problems.front();
    EXPECT_EQ(entity.types.front().fields[0].type, "entity");
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
    // Each language's files together, as a host compiles them: a camera's kind is in a file of its own.
    const auto declared = [&](const char* ending) {
        const auto copy = std::filesystem::temp_directory_path() / std::format("koral-package-{}", ending + 1);
        std::filesystem::remove_all(copy);
        std::filesystem::create_directories(copy);
        for (const auto& entry : std::filesystem::directory_iterator(folder))
            if (entry.path().extension() == ending) std::filesystem::copy_file(entry.path(), copy / entry.path().filename());
        return kscript::DeclareFiles(copy);
    };
    const auto csharp = declared(".cs"), kotlin = declared(".kt");
    for (const auto& problem : csharp.problems) ADD_FAILURE() << problem;
    for (const auto& problem : kotlin.problems) ADD_FAILURE() << problem;
    const auto find = [](const kscript::Declarations& in, const std::string_view name) -> const kscript::ScriptType* {
        for (const auto& type : in.types) if (type.name == name) return &type;
        return nullptr;
    };
    for (const char* name : { "Camera", "PerspectiveCamera", "OrthographicCamera", "Light", "LocalLight", "DirectionalLight", "PointLight", "SpotLight", "Sky", "Instancer" }) {
        const kscript::ScriptType* a = find(csharp, name);
        const kscript::ScriptType* b = find(kotlin, name);
        ASSERT_NE(a, nullptr) << name << " in C#";
        ASSERT_NE(b, nullptr) << name << " in Kotlin";
        EXPECT_TRUE(a->data && b->data) << name << " is Data";
        EXPECT_EQ(a->abstract, b->abstract) << name;
        EXPECT_EQ(a->kinds, b->kinds) << name;
        EXPECT_EQ(a->size, b->size) << name;
        EXPECT_EQ(a->initial, b->initial) << name << " starts the same in both";
        ASSERT_EQ(a->fields.size(), b->fields.size()) << name;
        for (std::size_t i = 0; i < a->fields.size(); ++i) {
            EXPECT_TRUE(a->fields[i].SameAs(b->fields[i])) << name << "." << a->fields[i].name;
            EXPECT_EQ(a->fields[i].offset, b->fields[i].offset);
        }
    }
    // The kinds, and what is one of each: nearest first.
    EXPECT_TRUE(find(csharp, "Camera")->abstract);
    EXPECT_TRUE(find(csharp, "LocalLight")->abstract);
    EXPECT_FALSE(find(csharp, "PerspectiveCamera")->abstract);
    EXPECT_EQ(find(csharp, "PerspectiveCamera")->kinds, std::vector<std::string> { "Camera" });
    EXPECT_EQ(find(csharp, "SpotLight")->kinds, (std::vector<std::string> { "LocalLight", "Light" }));
    EXPECT_TRUE(find(csharp, "Sky")->kinds.empty());
    // A kind's fields are at the front of each of it, in the same places.
    const auto& spot = *find(csharp, "SpotLight");
    const auto& local = *find(csharp, "LocalLight");
    ASSERT_GE(spot.fields.size(), local.fields.size());
    for (std::size_t i = 0; i < local.fields.size(); ++i) {
        EXPECT_TRUE(spot.fields[i].SameAs(local.fields[i])) << local.fields[i].name;
        EXPECT_EQ(spot.fields[i].offset, local.fields[i].offset) << local.fields[i].name;
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
    // Declared into a world, a kind finds what is one of it — and is had by nobody itself.
    const kecs::ComponentId light = world.Find("Light"), camera = world.Find("Camera");
    ASSERT_NE(light, kecs::NoComponent);
    ASSERT_NE(camera, kecs::NoComponent);
    EXPECT_TRUE(world.Has(entity, light));
    EXPECT_TRUE(world.Has(entity, world.Find("LocalLight")));
    EXPECT_FALSE(world.Has(entity, camera));
    EXPECT_FALSE(world.Add(entity, camera));
    const kecs::Entity eye = world.Create();
    ASSERT_TRUE(world.Add(eye, world.Find("OrthographicCamera")));
    EXPECT_EQ(world.Which(eye, camera), world.Find("OrthographicCamera"));
    const auto* far = world.Describe(camera).FindField("far");
    ASSERT_NE(far, nullptr);
    std::memcpy(&value, static_cast<const std::byte*>(world.Get(eye, camera)) + far->offset, 4);
    EXPECT_EQ(value, 100.f) << "the kind's field, read from the one of it the entity has";
}

// Data: fields and nothing else, its bases' first. An abstract one is a kind; one that takes a piece is said and passed by.
TEST(Declarations, DataIsFieldsItsBasesFirstAndAbstractDataIsAKind) {
    const auto csharp = kscript::Declare(R"(
        public abstract class Reading : Data
        {
            public float value = 2f;
        }
        public sealed class Temperature : Reading
        {
            public Vec3 where = new(1f, 2f, 3f);
        }
        public sealed class Ticking : Data, PerFrame
        {
            public float rate = 1f;
            public void Update(float delta) { }
        }
        public abstract class Mover : Setup { public float speed = 4f; }
        public sealed class Runner : Mover, PerFrame
        {
            public float stride = 1f;
            public void Update(float delta) { }
        }
    )", false, "Readings.cs");
    const auto find = [&](const std::string_view name) -> const kscript::ScriptType* {
        for (const auto& type : csharp.types) if (type.name == name) return &type;
        return nullptr;
    };
    const kscript::ScriptType* reading = find("Reading");
    const kscript::ScriptType* temperature = find("Temperature");
    ASSERT_NE(reading, nullptr) << "an abstract Data is a kind";
    ASSERT_NE(temperature, nullptr);
    EXPECT_TRUE(reading->abstract && reading->data);
    EXPECT_FALSE(temperature->abstract);
    EXPECT_EQ(temperature->kinds, std::vector<std::string> { "Reading" });
    ASSERT_EQ(temperature->fields.size(), 2u);
    EXPECT_EQ(temperature->fields[0].name, "value");
    EXPECT_EQ(temperature->fields[0].offset, 0u);
    EXPECT_EQ(temperature->fields[1].name, "where");
    EXPECT_EQ(temperature->fields[1].offset, 4u);
    EXPECT_EQ(find("Ticking"), nullptr) << "Data takes no pieces";
    EXPECT_TRUE(std::ranges::any_of(csharp.problems, [](const std::string& p) { return p.find("Ticking") != std::string::npos; }));
    // A script's abstract base is no script, but its fields are the script's, first — as a host lays them out.
    EXPECT_EQ(find("Mover"), nullptr);
    const kscript::ScriptType* runner = find("Runner");
    ASSERT_NE(runner, nullptr);
    EXPECT_FALSE(runner->data);
    EXPECT_TRUE(runner->kinds.empty()) << "only Data has kinds";
    ASSERT_EQ(runner->fields.size(), 2u);
    EXPECT_EQ(runner->fields[0].name, "speed");
    EXPECT_EQ(runner->pieces, std::vector<std::string> { "PerFrame" });

    // In Kotlin alike.
    const auto kotlin = kscript::Declare(R"(
        abstract class Reading : Data() {
            var value = 2f
        }
        class Temperature : Reading() {
            var where = Vec3(1f, 2f, 3f)
        }
    )", true, "Readings.kt");
    ASSERT_EQ(kotlin.types.size(), 2u);
    EXPECT_EQ(kotlin.types[1].fields.size(), 2u);
    EXPECT_EQ(kotlin.types[1].kinds, std::vector<std::string> { "Reading" });
    EXPECT_EQ(kotlin.types[1].initial, temperature->initial);

    // Declared, the kind finds what is one of it, and nothing has it itself.
    kecs::World world;
    kscript::Scripts scripts(world);
    for (const auto& type : csharp.types) ASSERT_NE(scripts.Declare(type), kecs::NoComponent) << type.name;
    const kecs::Entity e = world.Create();
    ASSERT_TRUE(world.Add(e, world.Find("Temperature")));
    EXPECT_TRUE(world.Has(e, world.Find("Reading")));
    EXPECT_FALSE(world.Add(e, world.Find("Reading")));
    std::string error;
    EXPECT_EQ(scripts.Register(*reading, &error), kecs::NoComponent) << "a kind is never run";
    EXPECT_FALSE(error.empty());
}

// A ResourceScript is read as one: what it makes — each field of a resource type, with what its initialiser makes it
// with — and its other fields as settings. No component.
TEST(Declarations, AResourceScriptMakesWhatItDeclares) {
    const auto csharp = kscript::Declare(R"(
        public sealed class Terrain : ResourceScript
        {
            public int Seed = 7;
            public float Scale = 4;
            public Image Height = new(format: "eR32_SFLOAT", width: 256, height: 128, usage: "eSampled | eStorage");
            public readonly ImageView HeightView = new(image: nameof(Height));
            public Buffer Table = new Buffer(instanceCount: 64, usage: ["eStorage"], isPerFrame: false);
            public Sampler Smooth = new(minFilter: Filter.eLinear, maxAnisotropy: 4f);
            public Image Plain;
            protected override void Init() { }
        }
        public sealed class Spin : Script { public float Speed; }
    )", false);
    for (const auto& problem : csharp.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(csharp.types.size(), 2u);
    const auto& terrain = csharp.types[0];
    EXPECT_TRUE(terrain.makesResources);
    ASSERT_EQ(terrain.fields.size(), 2u) << "its settings, and none of what it makes";
    EXPECT_EQ(terrain.fields[0].name, "Seed");
    std::int32_t seed = 0;
    ASSERT_GE(terrain.initial.size(), 4u);
    std::memcpy(&seed, terrain.initial.data(), 4);
    EXPECT_EQ(seed, 7) << "what it starts as, as a Script node's input is when not given";
    ASSERT_EQ(terrain.resources.size(), 5u);
    using Settings = std::vector<std::pair<std::string, std::string>>;
    EXPECT_EQ(terrain.resources[0].name, "Height");
    EXPECT_EQ(terrain.resources[0].kind, "Image");
    EXPECT_EQ(terrain.resources[0].settings, (Settings { { "format", "\"eR32_SFLOAT\"" }, { "extent.x", "256" }, { "extent.y", "128" },
                                                         { "usage", "[\"eSampled\", \"eStorage\"]" } })) << "its width and height, its extent's";
    EXPECT_EQ(terrain.resources[1].kind, "ImageView");
    EXPECT_EQ(terrain.resources[1].settings, (Settings { { "image", "\"Height\"" } })) << "another of the script's, by name";
    EXPECT_EQ(terrain.resources[2].settings, (Settings { { "instanceCount", "64" }, { "usage", "[\"eStorage\"]" }, { "isPerFrame", "false" } }));
    EXPECT_EQ(terrain.resources[3].settings, (Settings { { "minFilter", "\"eLinear\"" }, { "maxAnisotropy", "4" } })) << "an enumeration's value, by its name";
    EXPECT_TRUE(terrain.resources[4].settings.empty()) << "one given nothing: as its node type starts";
    EXPECT_FALSE(csharp.types[1].makesResources) << "a script of entities makes nothing";

    const auto kotlin = kscript::Declare(R"(
        import koral.engine.ResourceScript
        class Terrain : ResourceScript() {
            var seed = 7
            val height = Image(format = "eR32_SFLOAT", width = 256, height = 256)
            val heightView = ImageView(image = ::height.name)
            val table = ResourceScript.Buffer(instanceCount = 8,
                                              usage = listOf("eStorage"))
            override fun init() {}
        }
    )", true);
    for (const auto& problem : kotlin.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(kotlin.types.size(), 1u);
    EXPECT_TRUE(kotlin.types[0].makesResources);
    ASSERT_EQ(kotlin.types[0].fields.size(), 1u);
    EXPECT_EQ(kotlin.types[0].fields.front().name, "seed");
    ASSERT_EQ(kotlin.types[0].resources.size(), 3u);
    EXPECT_EQ(kotlin.types[0].resources[1].settings, (Settings { { "image", "\"height\"" } }));
    EXPECT_EQ(kotlin.types[0].resources[2].kind, "Buffer");
    EXPECT_EQ(kotlin.types[0].resources[2].settings, (Settings { { "instanceCount", "8" }, { "usage", "[\"eStorage\"]" } })) << "over two lines";

    // What is not understood without running it is said, and left as the node type starts it.
    const auto unclear = kscript::Declare("public sealed class A : ResourceScript { public Image Big = new(width: Size * 2, 16); }", false);
    ASSERT_EQ(unclear.types.size(), 1u);
    ASSERT_EQ(unclear.problems.size(), 2u);
    EXPECT_NE(unclear.problems[0].find("A.Big's width is 'Size * 2'"), std::string::npos) << unclear.problems[0];
    EXPECT_NE(unclear.problems[1].find("given by name"), std::string::npos) << unclear.problems[1];
    EXPECT_TRUE(unclear.types[0].resources[0].settings.empty());
}

// What a field is kept within, as its source says — `[Range(1, 179)]`, `[Min(0)]`, or Kotlin's `@Range`, `@Min`, on its
// line or the one before — is the component's, for an editor; and stays so once the host registers it.
TEST(Declarations, AFieldsRangeIsWhatItsAttributeSays) {
    const auto csharp = kscript::Declare(R"(
        public sealed class Lens : Script
        {
            [Range(1, 179)] public float FieldOfView = 60f;
            [Min(0.001f), Logarithmic] public float Near = 0.1f;
            [Min(-1.5e1)]
            public float Far = 100f;
            public float Free;
        }
    )", false);
    for (const auto& problem : csharp.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(csharp.types.size(), 1u);
    const auto& lens = csharp.types.front();
    EXPECT_EQ(Field(lens, "FieldOfView")->min, 1.f);
    EXPECT_EQ(Field(lens, "FieldOfView")->max, 179.f);
    EXPECT_FLOAT_EQ(Field(lens, "Near")->min, 0.001f);
    EXPECT_FALSE(std::isfinite(Field(lens, "Near")->max));
    EXPECT_TRUE(Field(lens, "Near")->logarithmic) << "dragged by so much of it";
    EXPECT_FALSE(Field(lens, "FieldOfView")->logarithmic);
    EXPECT_EQ(Field(lens, "Far")->min, -15.f) << "an attribute on the line before";
    EXPECT_FALSE(std::isfinite(Field(lens, "Free")->min));

    const auto kotlin = kscript::Declare(R"(
        class Lens : Script() {
            @Range(1f, 179f) var fieldOfView = 60f
            @Min(0f)
            @Logarithmic
            var near = 0.1f
            var far = 100f
        }
    )", true);
    for (const auto& problem : kotlin.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(kotlin.types.size(), 1u);
    EXPECT_EQ(Field(kotlin.types[0], "fieldOfView")->max, 179.f);
    EXPECT_EQ(Field(kotlin.types[0], "near")->min, 0.f) << "an annotation on a line of its own";
    EXPECT_TRUE(Field(kotlin.types[0], "near")->logarithmic) << "and two";
    EXPECT_FALSE(std::isfinite(Field(kotlin.types[0], "far")->min));

    // Its host says nothing of limits: the declared ones stay.
    kecs::World world;
    kscript::Scripts scripts(world);
    const kecs::ComponentId id = scripts.Declare(lens);
    kscript::ScriptType hosted;
    hosted.name = "Lens";
    hosted.size = 16;
    hosted.alignment = 4;
    hosted.fields = { { "FieldOfView", "f32", 0, 4 }, { "Near", "f32", 4, 4 }, { "Far", "f32", 8, 4 }, { "Free", "f32", 12, 4 } };
    ASSERT_EQ(scripts.Register(hosted), id);
    const auto* kept = world.Describe(id).FindField("FieldOfView");
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->min, 1.f);
    EXPECT_EQ(kept->max, 179.f);
}

// A Material is data and nothing else: a kind of what a drawn thing is made of. Its pictures (Textures) come first,
// each a shared reference that starts as the picture it names, then its other fields as any script's; a choice of so
// many is a whole number with names.
TEST(Declarations, AMaterialIsAKindOfWhatThingsAreMadeOf) {
    constexpr const char* csharp = R"(
        public sealed class Toon : Material
        {
            public Vec4 Tint = new(1f, 0.5f, 0.25f, 1f);
            public Texture Ramp = "@textures/ramp.png";
            [Range(1, 8)] public int Bands = 3;
            [Choices("Opaque", "Masked", "Blended")] public uint AlphaMode;
            public Texture Normals = Texture.Normal;
        }
    )";
    const auto found = kscript::Declare(csharp, false, "Toon.cs");
    for (const auto& problem : found.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(found.types.size(), 1u);
    const auto& toon = found.types.front();
    EXPECT_TRUE(toon.material);
    EXPECT_FALSE(toon.makesResources);
    ASSERT_EQ(toon.fields.size(), 5u);
    EXPECT_EQ(toon.fields[0].name, "Ramp") << "the pictures first, as they are written";
    EXPECT_EQ(toon.fields[1].name, "Normals");
    for (int i : { 0, 1 }) {
        EXPECT_EQ(toon.fields[i].type, "Image");
        EXPECT_EQ(toon.fields[i].size, kscript::TextureSize);
        EXPECT_EQ(toon.fields[i].offset, i * kscript::TextureSize);
    }
    EXPECT_EQ(toon.fields[2].name, "Tint");
    EXPECT_EQ(toon.fields[2].offset, 2 * kscript::TextureSize);
    ASSERT_EQ(toon.textures.size(), 2u);
    EXPECT_EQ(toon.textures[0], (std::pair<std::string, std::string> { "Ramp", "@textures/ramp.png" }));
    EXPECT_EQ(toon.textures[1], (std::pair<std::string, std::string> { "Normals", "$normal" }));
    EXPECT_FLOAT_EQ(FloatAt(toon, "Tint", 1), 0.5f);
    const auto* bands = Field(toon, "Bands");
    ASSERT_NE(bands, nullptr);
    EXPECT_EQ(bands->min, 1.f);
    const auto* mode = Field(toon, "AlphaMode");
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(mode->choices, (std::vector<std::string> { "Opaque", "Masked", "Blended" }));

    constexpr const char* kotlin = R"(
        class Toon : Material() {
            var tint = Vec4(1f, 0.5f, 0.25f, 1f)
            var ramp = Texture("@textures/ramp.png")
            var normals = Texture.Normal
        }
    )";
    const auto inKotlin = kscript::Declare(kotlin, true, "Toon.kt");
    for (const auto& problem : inKotlin.problems) ADD_FAILURE() << problem;
    ASSERT_EQ(inKotlin.types.size(), 1u);
    EXPECT_TRUE(inKotlin.types.front().material);
    EXPECT_EQ(inKotlin.types.front().textures, (std::vector<std::pair<std::string, std::string>> { { "ramp", "@textures/ramp.png" }, { "normals", "$normal" } }));

    // A Texture is a material's: a script keeps none.
    const auto script = kscript::Declare("public sealed class S : Script { public Texture T; }", false, "S.cs");
    EXPECT_TRUE(script.types.empty());
    EXPECT_FALSE(script.problems.empty());
}

// The engine's package has its two kinds, the same in either language, given apart from the scripts' components.
TEST(Declarations, ThePackagesMaterialsArePbrAndBlinnPhong) {
    const auto folder = kscript::PackageDirectory() / "scripts";
    for (const char* name : { "PbrMaterial", "BlinnPhongMaterial" }) {
        const auto csharp = kscript::DeclareFiles(folder / (std::string(name) + ".cs"));
        const auto kotlin = kscript::DeclareFiles(folder / (std::string(name) + ".kt"));
        for (const auto& problem : csharp.problems) ADD_FAILURE() << problem;
        for (const auto& problem : kotlin.problems) ADD_FAILURE() << problem;
        ASSERT_EQ(csharp.types.size(), 1u) << name;
        ASSERT_EQ(kotlin.types.size(), 1u) << name;
        const auto& a = csharp.types.front();
        const auto& b = kotlin.types.front();
        EXPECT_TRUE(a.material);
        EXPECT_EQ(a.size, b.size) << name;
        EXPECT_EQ(a.initial, b.initial) << name << " starts the same in both";
        EXPECT_EQ(a.textures, b.textures) << name;
        ASSERT_EQ(a.fields.size(), b.fields.size()) << name;
        for (std::size_t i = 0; i < a.fields.size(); ++i) {
            EXPECT_TRUE(a.fields[i].SameAs(b.fields[i])) << name << "." << a.fields[i].name;
            EXPECT_EQ(a.fields[i].offset, b.fields[i].offset);
            EXPECT_EQ(a.fields[i].choices, b.fields[i].choices);
        }
    }

    kecs::World world;
    kscript::Scripts scripts(world);
    std::vector<kscript::ScriptType> materials;
    for (const auto& problem : kscript::DeclarePackage(scripts, &materials)) ADD_FAILURE() << problem;
    EXPECT_EQ(world.Find("PbrMaterial"), kecs::NoComponent) << "a material is made a component by what keeps its pictures";
    ASSERT_EQ(materials.size(), 2u);
    EXPECT_EQ(materials[0].name, "BlinnPhongMaterial");
    EXPECT_EQ(materials[1].name, "PbrMaterial");
}

// A script is a Setup with the pieces it takes: its fields are its component as a Script's were, in either language,
// and what it implements of a piece is no field.
TEST(Declarations, ASetupWithItsPiecesIsAScript) {
    const auto csharp = kscript::Declare(R"(
        public sealed class Spin : Setup, PerFrame, FixedStep
        {
            public float Speed = 2f;
            public float Angle;
            public void Update(float delta) => Angle += Speed * delta;
            public void FixedUpdate(float step) { }
        }
        public abstract class Base : Setup { public float Ignored; })", false);
    ASSERT_EQ(csharp.types.size(), 1u);
    EXPECT_EQ(csharp.types[0].name, "Spin");
    EXPECT_EQ(csharp.types[0].fields.size(), 2u);
    EXPECT_FLOAT_EQ(FloatAt(csharp.types[0], "Speed"), 2.f);
    EXPECT_EQ(csharp.types[0].pieces, (std::vector<std::string> { "PerFrame", "FixedStep" }));
    EXPECT_TRUE(csharp.problems.empty());

    const auto kotlin = kscript::Declare(R"(
        class Spin : Setup(), PerFrame {
            var speed = 2f
            var angle = 0f
            override fun update(delta: Float) { angle += speed * delta }
        })", true);
    ASSERT_EQ(kotlin.types.size(), 1u);
    EXPECT_EQ(kotlin.types[0].fields.size(), 2u);
    EXPECT_FLOAT_EQ(FloatAt(kotlin.types[0], "speed"), 2.f);
    EXPECT_EQ(kotlin.types[0].pieces, std::vector<std::string> { "PerFrame" });
}
