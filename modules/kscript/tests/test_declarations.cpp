// A script's component, read from its source without running it: the public fields of a C# or Kotlin script, laid
// out as its host lays them out, starting as they are initialised. And the engine's own package, read the same way.

#include <gtest/gtest.h>

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
