//
// keditor: the scene a new project starts from.
//

#include <keditor/starter.h>

#include <cstdlib>
#include <format>
#include <fstream>

#include <kgraph/meshes.h>
#include <kscript/declarations.h>

#include "standard_shaders.h"

namespace keditor
{
    using kgraph::Value;

    const std::vector<std::pair<std::string, std::string>>& StandardShaders()
    {
        static const std::vector<std::pair<std::string, std::string>> shaders { { "default.slang", detail::DefaultSlang } };
        return shaders;
    }

    const std::vector<std::pair<std::string, std::string>>& LitShaders()
    {
        static const std::vector<std::pair<std::string, std::string>> shaders {
            { "engineCommon.slang", detail::EngineCommonSlang }, { "lit.slang", detail::LitSlang },
            { "sky.slang", detail::SkySlang },
        };
        return shaders;
    }

    std::vector<std::string> WriteShaders(const std::filesystem::path& directory, const std::vector<std::pair<std::string, std::string>>& shaders)
    {
        std::vector<std::string> written;
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        for (const auto& [name, source] : shaders) {
            if (std::filesystem::exists(directory / name)) continue;
            std::ofstream(directory / name, std::ios::binary) << source;
            written.push_back(name);
        }
        return written;
    }

    std::vector<std::string> WriteStandardShaders(const std::filesystem::path& directory) { return WriteShaders(directory, StandardShaders()); }
    std::vector<std::string> WriteLitShaders(const std::filesystem::path& directory) { return WriteShaders(directory, LitShaders()); }

    const std::vector<std::pair<std::string, std::string>>& StandardObjects()
    {
        // What draws a mesh is the standard graph's `default` pipeline: a Mesh, and the component the pipeline's shader makes.
        static const std::vector<std::pair<std::string, std::string>> objects {
            { "Cube.json", R"json({
  "template": "Cube",
  "icon": "view_in_ar",
  "name": "Cube",
  "components": {
    "Mesh": { "mesh": "$cube" },
    "default": { "color": [0.85, 0.6, 1] }
  }
}
)json" },
            { "Sphere.json", R"json({
  "template": "Sphere",
  "icon": "circle",
  "name": "Sphere",
  "components": {
    "Mesh": { "mesh": "$sphere" },
    "default": { "color": [0.85, 0.6, 1] }
  }
}
)json" },
            { "Plane.json", R"json({
  "template": "Plane",
  "icon": "crop_square",
  "name": "Plane",
  "components": {
    "Mesh": { "mesh": "$plane" },
    "default": { "color": [0.85, 0.6, 1] }
  }
}
)json" },
            { "Cylinder.json", R"json({
  "template": "Cylinder",
  "icon": "view_in_ar",
  "name": "Cylinder",
  "components": {
    "Mesh": { "mesh": "$cylinder" },
    "default": { "color": [0.85, 0.6, 1] }
  }
}
)json" },
            { "Cone.json", R"json({
  "template": "Cone",
  "icon": "change_history",
  "name": "Cone",
  "components": {
    "Mesh": { "mesh": "$cone" },
    "default": { "color": [0.85, 0.6, 1] }
  }
}
)json" },
            { "Capsule.json", R"json({
  "template": "Capsule",
  "icon": "medication",
  "name": "Capsule",
  "components": {
    "Mesh": { "mesh": "$capsule" },
    "default": { "color": [0.85, 0.6, 1] }
  }
}
)json" },
            { "Torus.json", R"json({
  "template": "Torus",
  "icon": "donut_large",
  "name": "Torus",
  "components": {
    "Mesh": { "mesh": "$torus" },
    "default": { "color": [0.85, 0.6, 1] }
  }
}
)json" },
            { "Planet.json", R"json({
  "template": "Planet",
  "icon": "public",
  "description": "A sphere with a moon under it: turn the planet, and the moon goes round.",
  "name": "Planet",
  "components": {
    "Mesh": { "mesh": "$sphere" },
    "default": { "color": [0.3, 0.55, 1] }
  },
  "children": [
    {
      "name": "Moon",
      "position": [1.2, 0, 0],
      "scale": [0.3, 0.3, 0.3],
      "components": {
        "Mesh": { "mesh": "$sphere" },
        "default": { "color": [0.8, 0.8, 0.75] }
      }
    }
  ]
}
)json" },
            { "Camera.json", R"json({
  "template": "Camera",
  "icon": "videocam",
  "name": "Camera",
  "position": [2, 1.5, 3],
  "rotation": [-20, 34, 0],
  "components": {
    "Camera": { "fieldOfView": 60, "near": 0.1, "far": 100 }
  }
}
)json" },
            { "Fly Camera.json", R"json({
  "template": "Fly Camera",
  "icon": "videocam",
  "description": "A camera flown with the mouse and the keys while the right button is held: W A S D, E and Q, Shift faster.",
  "name": "Camera",
  "position": [2, 1.5, 3],
  "rotation": [-20, 34, 0],
  "components": {
    "Camera": { "fieldOfView": 60, "near": 0.1, "far": 100 },
    "FlyCamera": { "speed": 4, "sensitivity": 0.12 }
  }
}
)json" },
            { "Directional Light.json", R"json({
  "template": "Directional Light",
  "icon": "light_mode",
  "name": "Sun",
  "position": [0, 3, 0],
  "rotation": [-50, 30, 0],
  "components": {
    "DirectionalLight": { "color": [1, 0.96, 0.9], "intensity": 1 }
  }
}
)json" },
            { "Point Light.json", R"json({
  "template": "Point Light",
  "icon": "lightbulb",
  "name": "Point Light",
  "position": [0, 1.5, 0],
  "components": {
    "PointLight": { "color": [1, 0.85, 0.6], "intensity": 2, "range": 5 }
  }
}
)json" },
            { "Spot Light.json", R"json({
  "template": "Spot Light",
  "icon": "flashlight_on",
  "name": "Spot Light",
  "position": [0, 3, 0],
  "rotation": [-90, 0, 0],
  "components": {
    "SpotLight": { "color": [1, 1, 1], "intensity": 3, "range": 8, "innerAngle": 20, "outerAngle": 30 }
  }
}
)json" },
            { "Sky.json", R"json({
  "template": "Sky",
  "icon": "cloud",
  "name": "Sky",
  "components": {
    "Sky": { "zenith": [0.22, 0.42, 0.78], "horizon": [0.72, 0.8, 0.9], "ground": [0.23, 0.21, 0.19], "ambient": 0.35, "sunSize": 1.5 }
  }
}
)json" },
        };
        return objects;
    }

    std::vector<std::string> WriteStandardObjects(const std::filesystem::path& directory)
    {
        std::vector<std::string> written;
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        for (const auto& [name, source] : StandardObjects()) {
            if (std::filesystem::exists(directory / name)) continue;
            std::ofstream(directory / name, std::ios::binary) << source;
            written.push_back(name);
        }
        return written;
    }

    std::vector<std::string> WriteKotlinBuild(const std::filesystem::path& project, const std::string& name)
    {
        std::vector<std::string> written;
        std::error_code ignored;
        const auto write = [&](const char* file, const std::string& text, const bool always = false) {
            if (!always && std::filesystem::exists(project / file, ignored)) return;
            std::ofstream(project / file, std::ios::binary) << text;
            written.emplace_back(file);
        };
        std::string safe = name;
        for (char& c : safe)
            if (c == '"' || c == '\\' || c == '$') c = '_';
        write("settings.gradle.kts", std::format(R"kts(// {}: a Koral Engine project with scripts in Kotlin.
pluginManagement {{
    repositories {{ gradlePluginPortal(); mavenCentral() }}
}}
rootProject.name = "{}"
)kts", safe, safe));
        write("build.gradle.kts", R"kts(// The project's scripts: the Kotlin files in `scripts`, each class a component of its name. Written by the editor.
//
//   ./gradlew edit      the project in the editor, its scripts built again and loaded whenever one is saved
//   ./gradlew play      the project on its own
//
// gradle.properties, which is this machine's, says where the Koral SDK, the engine and the JDK are.
plugins {
    kotlin("jvm") version "2.4.20"
}

fun setting(name: String, variable: String): String =
    (findProperty(name) as String?)?.takeIf { it.isNotBlank() } ?: System.getenv(variable)
        ?: error("$name is not set: open the project in the editor once, which writes gradle.properties, or set $variable")

val koralSdk = setting("koral.sdk", "KORAL_SDK")
val engineLibrary = setting("koral.engine.library", "KORAL_ENGINE_LIBRARY")
val engineMaven = setting("koral.engine.maven", "KORAL_ENGINE_MAVEN")
val enginePackage = setting("koral.engine.package", "KORAL_ENGINE_PACKAGE")

repositories {
    maven(url = file("$koralSdk/share/Koral/maven"))    // Koral's own Kotlin libraries, as the SDK carries them
    maven(url = file(engineMaven))                      // the engine's
    mavenCentral()
    google()
}

dependencies {
    implementation("koral:koral-engine:0.1.0")
}

kotlin {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_25) }
    sourceSets["main"].kotlin.srcDir("scripts")
    sourceSets["main"].kotlin.srcDir("$enginePackage/scripts")      // the engine's own: the Camera, the lights, the Sky
}
java { sourceCompatibility = JavaVersion.VERSION_25; targetCompatibility = JavaVersion.VERSION_25 }

// The engine, run on this project: a saved script is built again by this same build and loaded while it runs.
fun engine(name: String, what: String, vararg more: String) = tasks.register<JavaExec>(name) {
    group = "koral"
    description = what
    dependsOn(tasks.classes)
    classpath = sourceSets.main.get().runtimeClasspath
    mainClass.set("koral.engine.MainKt")
    jvmArgs("--enable-native-access=ALL-UNNAMED")
    environment("KORAL_SDK", koralSdk)
    environment("KORAL_ENGINE_LIBRARY", engineLibrary)
    if (System.getenv("JAVA_HOME").isNullOrBlank()) environment("JAVA_HOME", System.getProperty("java.home"))
    workingDir = projectDir
    val wrapper = projectDir.resolve(if (System.getProperty("os.name").startsWith("Windows")) "gradlew.bat" else "gradlew")
    args(projectDir.absolutePath, "--classes", layout.buildDirectory.dir("classes/kotlin/main").get().asFile.absolutePath,
         "--sources", projectDir.resolve("scripts").absolutePath, "--compile", "$wrapper -p $projectDir -q --offline classes", *more)
}
engine("play", "Runs the project")
engine("edit", "Opens the project in the editor, with its scripts", "--edit")
)kts");
        // This machine's: where things are here. Written each time, since it is not the project's to keep.
        std::string properties = "# Written by the Koral Engine editor: where this machine's Koral SDK, engine and JDK are. Not the project's to commit.\n";
        const char* sdk = std::getenv("KORAL_SDK");
        properties += std::format("koral.sdk={}\nkoral.engine.library={}\nkoral.engine.maven={}\nkoral.engine.package={}\n", sdk && *sdk ? sdk : KEDITOR_SDK, KEDITOR_ENGINE_LIBRARY,
                                  KEDITOR_MAVEN, kscript::PackageDirectory().generic_string());
        if (std::string_view(KEDITOR_JAVA_HOME).size() > 0) properties += std::format("org.gradle.java.home={}\n", KEDITOR_JAVA_HOME);
        properties += "org.gradle.jvmargs=-Xmx1g\n";
        write("gradle.properties", properties, true);
        write(".gitignore", "build/\n.gradle/\n.kotlin/\ngradle.properties\n");
        // The wrapper, so that nothing has to be installed to build.
        const std::filesystem::path wrapper = KEDITOR_GRADLE_WRAPPER;
        for (const char* file : { "gradlew", "gradlew.bat", "gradle/wrapper/gradle-wrapper.jar", "gradle/wrapper/gradle-wrapper.properties" }) {
            if (std::filesystem::exists(project / file, ignored) || !std::filesystem::exists(wrapper / file, ignored)) continue;
            std::filesystem::create_directories((project / file).parent_path(), ignored);
            std::filesystem::copy_file(wrapper / file, project / file, ignored);
            written.emplace_back(file);
        }
        return written;
    }

    std::vector<std::string> WriteEditorSetup(const std::filesystem::path& project, const std::string& name)
    {
        std::vector<std::string> written;
        std::error_code ignored;
        const auto write = [&](const std::filesystem::path& file, const std::string& text, const bool always) {
            const std::filesystem::path path = project / file;
            if (!always && std::filesystem::exists(path, ignored)) return;
            std::filesystem::create_directories(path.parent_path(), ignored);
            std::ifstream in(path, std::ios::binary);
            const std::string now((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (in && now == text) return;
            std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
            written.push_back(file.generic_string());
        };
        // A shader imports the modules beside it, and the engine's (`import entity;`); a script is the project's, and
        // the folders a build leaves are not.
        write(".vscode/settings.json", std::string(R"json({
  "files.associations": { "*.slang": "slang" },
  "slang.additionalSearchPaths": ["${workspaceFolder}/shaders", ")json") + (kscript::PackageDirectory() / "shaders").generic_string() + R"json("],
  "files.exclude": { "**/bin": true, "**/obj": true, "**/build": true, "**/.gradle": true, "**/.kotlin": true },
  "dotnet.defaultSolution": "disable",
  "java.import.gradle.enabled": true
}
)json", false);
        write(".vscode/extensions.json", R"json({
  "recommendations": ["shader-slang.slang-language-extension", "ms-dotnettools.csharp", "fwcd.kotlin"]
}
)json", false);
        // The C# scripts as a project of their own: nothing builds it — a project's scripts are compiled when it runs
        // — but it is what tells an editor which names there are. It names this machine's assemblies, so it is written
        // again each time, and is not the project's to commit.
        const std::filesystem::path assemblies = KEDITOR_DOTNET;
        if (std::filesystem::exists(assemblies / "Koral.Engine.dll", ignored)) {
            std::string references;
            for (const char* assembly : { "Koral.Engine", "Koral" })
                if (std::filesystem::exists(assemblies / (std::string(assembly) + ".dll"), ignored))
                    references += std::format("    <Reference Include=\"{}\"><HintPath>{}</HintPath><Private>false</Private></Reference>\n", assembly,
                                              (assemblies / (std::string(assembly) + ".dll")).generic_string());
            write(std::filesystem::path(name + ".Scripts.csproj"), std::format(R"xml(<!-- Written by the Koral Engine editor, for an editor of code to know the engine's names by. Nothing builds it. -->
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <OutputType>Library</OutputType>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="scripts/**/*.cs" />
    <Compile Include="{}/scripts/*.cs" Link="koral/%(Filename)%(Extension)" />
{}  </ItemGroup>
</Project>
)xml", (kscript::PackageDirectory()).generic_string(), references), true);
        }
        return written;
    }

    bool AddStandardGraph(Document& doc, std::string& error)
    {
        bool ok = true;
        std::string pass;       // the pass whose page the nodes being made go on
        const auto node = [&](const char* type, const char* id, const double x, const double y) {
            if (doc.AddNode(type, x, y, id, pass) != id) { ok = false; error = std::format("there is a node '{}' already", id); }
        };
        const auto set = [&](const std::string& id, const char* input, Value value) { ok = doc.SetInput(id, input, std::move(value)) && ok; };
        const auto wire = [&](const std::string& from, const std::string& id, const char* input, const char* output = "out") { ok = doc.Connect(from, output, id, input) && ok; };
        const auto bring = [&](const char* id, const double x, const double y) {
            const std::string made = doc.Import(id, pass, x, y);
            if (made.empty()) { ok = false; error = std::format("there is no node '{}' to bring in", id); }
            return made;
        };

        // ---- Draw: every entity with a Mesh and the pipeline's component, seen by the camera and lit by the sun ------
        pass = "Draw";
        doc.AddPass(pass);
        node("Screen", "screen", 320, 580);
        // A picture the size of the window, and how far each pixel of it is.
        const auto picture = [&](const char* id, const char* format, Value::List usage, const double y) {
            node("Image", id, 620, y);
            set(id, "format", format);
            set(id, "usage", Value(std::move(usage)));
            doc.ClearInput(id, "extent.x");
            doc.ClearInput(id, "extent.y");
            wire("screen", id, "extent", "extent");
        };
        picture("color", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferDst", "eTransferSrc" }, 40);
        picture("depth", "eD32_SFLOAT", { "eDepthStencilAttachment", "eSampled", "eTransferDst", "eTransferSrc" }, 360);
        node("Framebuffer", "target", 1020, 300);
        wire("color", "target", "color[0].view");
        wire("depth", "target", "depthStencil.view");
        node("VertexLayout", "layout", 1020, 140);
        set("layout", "attributes", Value::List {
            Value::Members { { "semantic", "POSITION" }, { "type", "eFloat" }, { "count", 3 } },
            Value::Members { { "semantic", "NORMAL" }, { "type", "eFloat" }, { "count", 3 } },
        });
        // The camera: a view from where its entity is, towards a step along the way it faces; a lens of its field of
        // view, near and far. In the editor, the editor's view. The shader's `camera`, a field each.
        node("Entity", "eye", 40, 1240);
        set("eye", "component", "Camera");
        node("Add", "looking at", 400, 1300);
        wire("eye", "looking at", "a", "worldPosition");
        wire("eye", "looking at", "b", "forward");
        node("View", "view", 660, 1400);
        set("view", "main", true);
        wire("eye", "view", "position", "worldPosition");
        wire("looking at", "view", "target");
        wire("eye", "view", "up", "up");
        node("Perspective", "lens", 660, 1100);
        set("lens", "main", true);
        wire("screen", "lens", "aspect", "aspect");
        wire("eye", "lens", "fieldOfView", "field.fieldOfView");
        wire("eye", "lens", "near", "field.near");
        wire("eye", "lens", "far", "field.far");
        node("ShaderBuffer", "camera data", 1020, 820);
        set("camera data", "module", "default");
        set("camera data", "entry", "fragment");
        set("camera data", "parameter", "camera");
        wire("view", "camera data", "field.view");
        wire("lens", "camera data", "field.projection");
        wire("eye", "camera data", "field.eye", "worldPosition");
        // The sun: the shader's `light`.
        node("ForEach", "sun", 620, 700);
        set("sun", "component", "DirectionalLight");
        node("ShaderBuffer", "light data", 1020, 540);
        set("light data", "module", "default");
        set("light data", "entry", "fragment");
        set("light data", "parameter", "light");
        wire("sun", "light data", "field.direction", "forward");
        wire("sun", "light data", "field.intensity", "field.intensity");
        wire("sun", "light data", "field.color", "field.color");
        // The pipeline, and a draw of each entity with a Mesh and its component — the shader's `color`, which nothing
        // else gives — where the entity is.
        node("GraphicsPipeline", "default", 1300, 300);
        set("default", "stages", "eVertex");
        set("default", "vertex.module", "default");
        set("default", "vertex.entry", "vertex");
        set("default", "fragment.module", "default");
        set("default", "fragment.entry", "fragment");
        wire("layout", "default", "vertex.layout");
        wire("target", "default", "framebuffer");
        wire("camera data", "default", "param.camera");
        wire("light data", "default", "param.light");
        node("ForEach", "drawn", 1340, 640);
        set("drawn", "component", "Mesh");
        set("drawn", "with", Value::List { "default", "Mesh" });
        node("Draw", "draw", 1660, 640);
        wire("default", "draw", "pipeline");
        wire("drawn", "draw", "mesh", "field.mesh");
        wire("drawn", "draw", "push.model", "matrix");
        wire("drawn", "draw", "push.color", "default.color");

        // ---- Blit: the picture, in the window ----------------------------------------------------------------------
        pass = "Blit";
        doc.AddPass(pass);
        node("Blit", "show", 500, 40);
        wire(bring("color", 40, 40), "show", "source");
        wire(bring("screen", 40, 360), "show", "destination");

        if (!ok && error.empty()) error = "a node of the standard graph could not be set up";
        return ok;
    }

    kecs::Entity AddStandardScene(Document& doc)
    {
        const auto vec = [](const float x, const float y, const float z) { return Value(Value::List { x, y, z }); };
        const kecs::Entity camera = doc.CreateEntity("Camera");
        doc.AddComponent(camera, "Camera");
        doc.SetPosition(camera, { 2.f, 1.5f, 3.f });
        doc.SetRotation(camera, { -20.f, 34.f, 0.f });
        const kecs::Entity sun = doc.CreateEntity("Sun");
        doc.AddComponent(sun, "DirectionalLight");
        doc.SetField(sun, "DirectionalLight", "color", vec(1.f, 0.96f, 0.9f));
        doc.SetPosition(sun, { 0.f, 3.f, 0.f });
        doc.SetRotation(sun, { -50.f, 30.f, 0.f });
        const kecs::Entity sphere = doc.CreateEntity("Sphere");
        doc.AddComponent(sphere, "Mesh");
        doc.SetField(sphere, "Mesh", "mesh", Value(kgraph::DefaultMeshId("sphere")));
        if (doc.AddComponent(sphere, "default")) doc.SetField(sphere, "default", "color", vec(0.81f, 0.19f, 0.19f));
        const kecs::Entity stand = doc.CreateEntity("Cylinder");
        doc.AddComponent(stand, "Mesh");
        doc.SetField(stand, "Mesh", "mesh", Value(kgraph::DefaultMeshId("cylinder")));
        doc.SetPosition(stand, { 0.f, -1.f, 0.f });
        if (doc.AddComponent(stand, "default")) doc.SetField(stand, "default", "color", vec(0.f, 1.f, 0.53f));
        return sphere;
    }

    bool AddLitGraph(Document& doc, std::string& error)
    {
        bool ok = true;
        std::string pass;       // the pass whose page the nodes being made go on
        const auto node = [&](const char* type, const char* id, const double x, const double y) {
            if (doc.AddNode(type, x, y, id, pass) != id) { ok = false; error = std::format("there is a node '{}' already", id); }
        };
        const auto set = [&](const std::string& id, const char* input, Value value) { ok = doc.SetInput(id, input, std::move(value)) && ok; };
        const auto wire = [&](const std::string& from, const std::string& id, const char* input, const char* output = "out") { ok = doc.Connect(from, output, id, input) && ok; };
        // What an earlier pass made, brought into this one: a node here that stands for it.
        const auto bring = [&](const char* id, const double x, const double y) {
            const std::string made = doc.Import(id, pass, x, y);
            if (made.empty()) { ok = false; error = std::format("there is no node '{}' to bring in", id); }
            return made;
        };
        const auto shaders = [&](const char* id, const char* module) {
            set(id, "vertex.module", module);
            set(id, "vertex.entry", "vertex");
            set(id, "fragment.module", module);
            set(id, "fragment.entry", "fragment");
        };
        const auto target = [&](const char* id, const std::string& colorView, const std::string& depthView, const double x, const double y) {
            node("Framebuffer", id, x, y);
            wire(colorView, id, "color[0].view");
            set(id, "color[0].name", "color");
            if (depthView.empty()) return;
            wire(depthView, id, "depthStencil.view");
            set(id, "depthStencil.which", "Depth");
            set(id, "depthStencil.name", "depth");
            set(id, "depthStencil.depth", 1);
        };

        // ---- frame: what every pass after it sees of the frame ------------------------------------------------
        // The scene's camera, sky, sun and lights — components of the engine's package, each read by a ForEach of it —
        // and the two buffers of them the shaders read. A value is wired into its place in a buffer, in the order the
        // shader's block declares it; the lights are the shader's own struct, a buffer of each kind after the last.
        pass = "frame";
        doc.AddPass(pass);
        node("Screen", "screen", 40, 40);
        const auto first = [&](const char* id, const char* component, const double y) {
            node("Entity", id, 40, y);
            set(id, "component", component);
        };
        // The camera: a view from where its entity is, towards a step along the way it faces, rolled as it is; and a
        // lens of its field of view, near and far. What the scene is seen through: in the editor, the editor's view.
        first("eye", "Camera", 180);
        node("Add", "looking at", 280, 260);
        wire("eye", "looking at", "a", "worldPosition");
        wire("eye", "looking at", "b", "forward");
        node("View", "camera", 480, 180);
        wire("eye", "camera", "position", "worldPosition");
        wire("looking at", "camera", "target");
        wire("eye", "camera", "up", "up");
        set("camera", "main", true);
        node("Perspective", "lens", 680, 180);
        set("lens", "main", true);
        wire("eye", "lens", "fieldOfView", "field.fieldOfView");
        wire("eye", "lens", "near", "field.near");
        wire("eye", "lens", "far", "field.far");
        wire("screen", "lens", "aspect", "aspect");
        // The two as one: the projection after the view, and what undoes it.
        node("MatrixMultiply", "seen", 680, 300);
        wire("lens", "seen", "a");
        wire("camera", "seen", "b");
        node("MatrixInverse", "unseen", 680, 380);
        wire("seen", "unseen", "a");
        first("sky values", "Sky", 460);
        // A colour as picked, in the linear terms light adds up in: by the display's curve as the power 2.2.
        const auto linear = [&](const char* id, const std::string& from, const char* output, const double x, const double y) {
            node("ColorConvert", id, x, y);
            wire(from, id, "color", output);
            set(id, "from", "eGamma22");
            set(id, "to", "eLinear");
        };
        linear("zenith", "sky values", "field.zenith", 280, 420);
        linear("horizon", "sky values", "field.horizon", 280, 500);
        linear("ground", "sky values", "field.ground", 280, 580);
        // The cosine of the sun disc's radius: half its size, in radians.
        node("Multiply", "sun radius", 280, 660);
        wire("sky values", "sun radius", "a", "field.sunSize");
        set("sun radius", "b", 0.5);
        node("Radians", "sun radians", 480, 660);
        wire("sun radius", "sun radians", "a");
        node("Cos", "sun cosine", 680, 660);
        wire("sun radians", "sun cosine", "a");
        first("sun", "DirectionalLight", 760);
        node("Negate", "towards the sun", 280, 760);
        wire("sun", "towards the sun", "a", "forward");
        // A light's colour as it shines: in linear terms, times how bright it is.
        const auto shining = [&](const std::string& id, const std::string& from, const double x, const double y) {
            linear((id + " linear").c_str(), from, "field.color", x, y);
            node("Multiply", (id + " color").c_str(), x + 200, y);
            wire(id + " linear", id + " color", "a");
            wire(from, id + " color", "b", "field.intensity");
        };
        shining("sun", "sun", 280, 840);
        // FrameData, in engineCommon.slang: a float after a float3 is the float4's fourth, as a ConstantBuffer lays them out.
        node("BufferBuilder", "frame data", 920, 340);
        set("frame data", "usage", "eUniform");
        Value::List layout;
        for (const auto& [name, type] : { std::pair { "viewProjection", "eMat4" }, { "inverseViewProjection", "eMat4" }, { "cameraPosition", "eVec4" },
                                          { "skyZenith", "eVec3" }, { "skyAmbient", "eFloat" }, { "skyHorizon", "eVec3" }, { "sunCosine", "eFloat" },
                                          { "skyGround", "eVec4" }, { "sunDirection", "eVec3" }, { "sunPresent", "eFloat" }, { "sunColor", "eVec4" }, { "counts", "eUVec4" } })
            layout.emplace_back(Value::Members { { "name", name }, { "type", type } });
        set("frame data", "layout", Value(std::move(layout)));
        const auto field = [&](const char* name, const char* from, const char* output) { wire(from, "frame data", std::format("field.{}", name).c_str(), output); };
        field("viewProjection", "seen", "out");
        field("inverseViewProjection", "unseen", "out");
        field("cameraPosition", "camera", "position");
        field("skyZenith", "zenith", "out");
        field("skyAmbient", "sky values", "field.ambient");     // how much light the sky gives all round
        field("skyHorizon", "horizon", "out");
        field("sunCosine", "sun cosine", "out");                // the cosine of the sun disc's radius
        field("skyGround", "ground", "out");
        field("sunDirection", "towards the sun", "out");
        field("sunPresent", "sun", "present");                  // 1 when there is a sun
        field("sunColor", "sun color", "out");

        // Every light, a kind after another: where it is and how far it reaches, the way it shines and its kind, its
        // colour, and a spot's cone as the cosines of its angles.
        const char* kinds[] { "DirectionalLight", "PointLight", "SpotLight" };
        const char* ids[] { "suns", "lamps", "spots" };
        std::string before;
        for (int kind = 0; kind < 3; ++kind) {
            const double y = 1000.0 + kind * 360.0;
            const std::string each = ids[kind], data = kind == 2 ? std::string("light data") : each + " data";
            node("ForEach", each.c_str(), 40, y);
            set(each, "component", kinds[kind]);
            shining(each, each, 280, y);
            node("Append", (each + " position").c_str(), 280, y + 80);
            wire(each, each + " position", "a", "worldPosition");
            if (kind == 0) set(each + " position", "b", 0);
            else wire(each, each + " position", "b", "field.range");
            node("Append", (each + " direction").c_str(), 280, y + 160);
            wire(each, each + " direction", "a", "forward");
            set(each + " direction", "b", kind);
            node("ShaderBuffer", data.c_str(), 920, y);  // Light[], in engineCommon.slang
            set(data, "module", "lit");
            set(data, "entry", "fragment");
            set(data, "parameter", "lights");
            wire(each + " position", data, "field.position");
            wire(each + " direction", data, "field.direction");
            wire(each + " color", data, "field.color");
            if (kind == 2)
                for (const char* angle : { "innerAngle", "outerAngle" }) {
                    const std::string at = std::format("{} {}", each, angle);
                    node("Radians", (at + " radians").c_str(), 480, y + 240 + (angle[0] == 'o' ? 60 : 0));
                    wire(each, at + " radians", "a", std::format("field.{}", angle).c_str());
                    node("Cos", (at + " cosine").c_str(), 680, y + 240 + (angle[0] == 'o' ? 60 : 0));
                    wire(at + " radians", at + " cosine", "a");
                }
            if (kind == 2) {
                node("Append", "spots cone", 880, y + 270);
                wire("spots innerAngle cosine", "spots cone", "a");
                wire("spots outerAngle cosine", "spots cone", "b");
                wire("spots cone", data, "field.cone");
            }
            if (!before.empty()) wire(before, data, "after");
            before = data;
        }
        field("counts", "light data", "count");
        node("Primitive", "shape", 280, 40);            // for how a default mesh's vertices are laid out

        // ---- scene: every entity with the lit shader's component and a Mesh, a draw each -----------------------------
        // Three pictures the size of the window: what is seen, how far each pixel of it is, and which entity it is.
        pass = "scene";
        doc.AddPass(pass);
        const std::string screen = bring("screen", 40, 40);
        const auto picture = [&](const char* id, const char* format, Value::List usage, const double y) {
            node("Image", id, 300, y);
            set(id, "format", format);
            set(id, "usage", Value(std::move(usage)));
            doc.ClearInput(id, "extent.x");
            doc.ClearInput(id, "extent.y");
            wire(screen, id, "extent", "extent");
        };
        picture("color", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferSrc", "eTransferDst" }, 40);
        picture("depth", "eD32_SFLOAT", { "eDepthStencilAttachment" }, 360);
        target("scene target", "color", "depth", 760, 40);
        set("scene target", "color[0].clear", Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0, 0, 0, 1 } } } } });
        // The pipeline is what every draw shares: the frame's values and the lights. What each draw has of its own —
        // its mesh, where it is, its colour, how metallic and how rough it is, what light it gives off — is wired to the
        // Draw from a Filter of what has a mesh and a PbrMaterial: the material's fields, each to a push constant.
        node("GraphicsPipeline", "lit", 1020, 40);
        shaders("lit", "lit");
        wire(bring("shape", 760, 420), "lit", "vertex.layout", "layout");
        wire("scene target", "lit", "framebuffer");
        wire(bring("frame data", 760, 540), "lit", "param.frame");
        wire(bring("light data", 760, 660), "lit", "param.lights");
        // Every entity; those with a Mesh and a PbrMaterial; a draw of each, with what its material is.
        node("Entities", "all", 1020, 260);
        node("Filter", "drawn", 1020, 360);
        wire("all", "drawn", "entities");
        set("drawn", "with", Value::List { "Mesh", "PbrMaterial" });
        node("Draw", "draw scene", 1300, 40);
        wire("lit", "draw scene", "pipeline");
        wire("drawn", "draw scene", "mesh", "Mesh.mesh");
        wire("drawn", "draw scene", "push.model", "matrix");
        for (const auto& [push, field] : { std::pair { "albedo", "baseColor" }, { "metallic", "metallic" }, { "roughness", "roughness" }, { "emission", "emission" } })
            wire("drawn", "draw scene", (std::string("push.") + push).c_str(), (std::string("PbrMaterial.") + field).c_str());

        // ---- sky: a triangle over the whole picture at the far plane, where nothing nearer was drawn -----------
        pass = "sky";
        doc.AddPass(pass);
        target("sky target", bring("color", 40, 40), bring("depth", 40, 160), 320, 40);
        node("GraphicsPipeline", "sky", 600, 40);
        shaders("sky", "sky");
        wire("sky target", "sky", "framebuffer");
        wire(bring("frame data", 320, 300), "sky", "param.frame");
        set("sky", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", false }, { "depthCompareOp", "eLessOrEqual" } });
        node("Draw", "draw sky", 880, 40);
        wire("sky", "draw sky", "pipeline");
        set("draw sky", "load", true);

        // ---- show: the picture, in the window ----------------------------------------------------------------------
        pass = "show";
        doc.AddPass(pass);
        node("Blit", "show", 320, 40);
        wire(bring("color", 40, 40), "show", "source");
        wire(bring("screen", 40, 300), "show", "destination");     // under the picture's: a card as tall as its picture

        if (!ok && error.empty()) error = "a node of the lit graph could not be set up";
        return ok;
    }

    kecs::Entity AddLitScene(Document& doc)
    {
        const auto vec = [](const float x, const float y, const float z) { return Value(Value::List { x, y, z }); };
        const kecs::Entity sky = doc.CreateEntity("Sky");
        doc.AddComponent(sky, "Sky");
        const kecs::Entity camera = doc.CreateEntity("Camera");
        doc.AddComponent(camera, "Camera");
        doc.SetPosition(camera, { 2.f, 1.5f, 3.f });
        doc.SetRotation(camera, { -20.f, 34.f, 0.f });
        const kecs::Entity sun = doc.CreateEntity("Sun");
        doc.AddComponent(sun, "DirectionalLight");
        doc.SetField(sun, "DirectionalLight", "color", vec(1.f, 0.96f, 0.9f));
        doc.SetPosition(sun, { 0.f, 3.f, 0.f });
        doc.SetRotation(sun, { -50.f, 30.f, 0.f });
        const kecs::Entity sphere = doc.CreateEntity("Sphere");
        doc.AddComponent(sphere, "Mesh");
        doc.SetField(sphere, "Mesh", "mesh", Value(kgraph::DefaultMeshId("sphere")));
        if (doc.AddComponent(sphere, "PbrMaterial")) {
            doc.SetField(sphere, "PbrMaterial", "baseColor", Value(Value::List { 0.85f, 0.6f, 1.f, 1.f }));
            doc.SetField(sphere, "PbrMaterial", "roughness", Value(0.5));
        }
        return sphere;
    }
}
