//
// keditor: the scene a new project starts from.
//

#include <keditor/starter.h>

#include <format>
#include <fstream>

#include <kgraph/meshes.h>

#include "standard_shaders.h"

namespace keditor
{
    using kgraph::Value;

    const std::vector<std::pair<std::string, std::string>>& StandardShaders()
    {
        static const std::vector<std::pair<std::string, std::string>> shaders {
            { "engineCommon.slang", detail::EngineCommonSlang }, { "lit.slang", detail::LitSlang },
            { "sky.slang", detail::SkySlang }, { "outline.slang", detail::OutlineSlang },
        };
        return shaders;
    }

    std::vector<std::string> WriteStandardShaders(const std::filesystem::path& directory)
    {
        std::vector<std::string> written;
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        for (const auto& [name, source] : StandardShaders()) {
            if (std::filesystem::exists(directory / name)) continue;
            std::ofstream(directory / name, std::ios::binary) << source;
            written.push_back(name);
        }
        return written;
    }

    const std::vector<std::pair<std::string, std::string>>& StandardObjects()
    {
        // What draws a mesh is the standard graph's `lit` pipeline: a Mesh, and the component the pipeline's shader makes.
        static const std::vector<std::pair<std::string, std::string>> objects {
            { "Cube.json", R"json({
  "template": "Cube",
  "icon": "view_in_ar",
  "name": "Cube",
  "components": {
    "Mesh": { "mesh": "$cube" },
    "lit": { "albedo": [0.85, 0.6, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
  }
}
)json" },
            { "Sphere.json", R"json({
  "template": "Sphere",
  "icon": "circle",
  "name": "Sphere",
  "components": {
    "Mesh": { "mesh": "$sphere" },
    "lit": { "albedo": [0.85, 0.6, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
  }
}
)json" },
            { "Plane.json", R"json({
  "template": "Plane",
  "icon": "crop_square",
  "name": "Plane",
  "components": {
    "Mesh": { "mesh": "$plane" },
    "lit": { "albedo": [0.85, 0.6, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
  }
}
)json" },
            { "Cylinder.json", R"json({
  "template": "Cylinder",
  "icon": "view_in_ar",
  "name": "Cylinder",
  "components": {
    "Mesh": { "mesh": "$cylinder" },
    "lit": { "albedo": [0.85, 0.6, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
  }
}
)json" },
            { "Cone.json", R"json({
  "template": "Cone",
  "icon": "change_history",
  "name": "Cone",
  "components": {
    "Mesh": { "mesh": "$cone" },
    "lit": { "albedo": [0.85, 0.6, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
  }
}
)json" },
            { "Capsule.json", R"json({
  "template": "Capsule",
  "icon": "medication",
  "name": "Capsule",
  "components": {
    "Mesh": { "mesh": "$capsule" },
    "lit": { "albedo": [0.85, 0.6, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
  }
}
)json" },
            { "Torus.json", R"json({
  "template": "Torus",
  "icon": "donut_large",
  "name": "Torus",
  "components": {
    "Mesh": { "mesh": "$torus" },
    "lit": { "albedo": [0.85, 0.6, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
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
    "lit": { "albedo": [0.3, 0.55, 1], "metallic": 0, "roughness": 0.5, "emission": [0, 0, 0] }
  },
  "children": [
    {
      "name": "Moon",
      "position": [1.2, 0, 0],
      "scale": [0.3, 0.3, 0.3],
      "components": {
        "Mesh": { "mesh": "$sphere" },
        "lit": { "albedo": [0.8, 0.8, 0.75], "metallic": 0, "roughness": 0.8, "emission": [0, 0, 0] }
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

    bool AddStandardGraph(Document& doc, std::string& error)
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
        // The scene's camera, sky, sun and lights as values, and the two buffers of them the shaders read. A value
        // is wired into its place in a buffer, in the order the shader's block declares it.
        pass = "frame";
        doc.AddPass(pass);
        node("Screen", "screen", 40, 40);
        node("Camera", "camera", 40, 180);
        wire("screen", "camera", "extent", "size");
        node("Sky", "sky values", 40, 460);
        node("Sun", "sun", 40, 700);
        node("Lights", "lights", 40, 860);
        node("DataBuffer", "frame data", 420, 180);     // FrameData, in engineCommon.slang
        const auto field = [&](const int index, const char* from, const char* output, const char* type = nullptr) {
            wire(from, "frame data", std::format("field[{}].value", index).c_str(), output);
            if (type) set("frame data", std::format("field[{}].type", index).c_str(), type);
        };
        field(0, "camera", "viewProjection");
        field(1, "camera", "inverseViewProjection");
        field(2, "camera", "position", "eVec4");
        field(3, "sky values", "zenith");               // rgb, and in its fourth place:
        field(4, "sky values", "ambient");              //   how much light the sky gives all round
        field(5, "sky values", "horizon");
        field(6, "sky values", "sunCosine");            //   the cosine of the sun disc's radius
        field(7, "sky values", "ground", "eVec4");
        field(8, "sun", "direction");
        field(9, "sun", "present");                     //   1 when there is a sun
        field(10, "sun", "color", "eVec4");
        field(11, "lights", "count", "eUVec4");
        node("DataBuffer", "light data", 420, 860);     // Light[], in engineCommon.slang
        set("light data", "usage", "eStorage");
        wire("lights", "light data", "field[0].value", "data");
        node("Primitive", "shape", 420, 40);            // for how a default mesh's vertices are laid out

        // ---- scene: every entity with a Mesh and the lit shader's component -------------------------------------
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
            const std::string view = std::string(id) + " view";
            node("ImageView", view.c_str(), 520, y);
            wire(id, view, "image");
        };
        picture("color", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferSrc", "eTransferDst" }, 40);
        picture("ids", "eR32_UINT", { "eColorAttachment", "eSampled", "eTransferSrc" }, 200);
        picture("depth", "eD32_SFLOAT", { "eDepthStencilAttachment" }, 360);
        target("scene target", "color view", "depth view", 760, 40);
        set("scene target", "color[0].clear", Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0, 0, 0, 1 } } } } });
        wire("ids view", "scene target", "color[1].view");
        set("scene target", "color[1].name", "ids");
        set("scene target", "color[1].clear", Value::Members { { "scalarType", "UInt" }, { "components", 4 }, { "value", Value::Members { { "u", Value::List { 0, 0, 0, 0 } } } } });
        // The pipeline's component is the shader's: what its push constants ask of each entity — an albedo, how
        // metallic and how rough it is, what light it gives off.
        node("GraphicsPipeline", "lit", 1020, 40);
        shaders("lit", "lit");
        wire(bring("shape", 760, 420), "lit", "vertex.layout", "layout");
        wire("scene target", "lit", "framebuffer");
        wire(bring("frame data", 760, 540), "lit", "param.frame");
        wire(bring("light data", 760, 660), "lit", "param.lights");
        node("Draw", "draw scene", 1300, 40);
        wire("lit", "draw scene", "pipeline");

        // ---- sky: a triangle over the whole picture at the far plane, where nothing nearer was drawn -----------
        pass = "sky";
        doc.AddPass(pass);
        target("sky target", bring("color view", 40, 40), bring("depth view", 40, 160), 320, 40);
        node("GraphicsPipeline", "sky", 600, 40);
        shaders("sky", "sky");
        wire("sky target", "sky", "framebuffer");
        wire(bring("frame data", 320, 300), "sky", "param.frame");
        set("sky", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", false }, { "depthCompareOp", "eLessOrEqual" } });
        node("Draw", "draw sky", 880, 40);
        wire("sky", "draw sky", "pipeline");
        set("draw sky", "load", true);

        // ---- outline: a line round what is picked, from the image of which entity is where -----------------------
        pass = "outline";
        doc.AddPass(pass);
        target("overlay target", bring("color view", 40, 40), {}, 320, 40);
        node("GraphicsPipeline", "outline", 600, 40);
        shaders("outline", "outline");
        wire("overlay target", "outline", "framebuffer");
        wire(bring("ids", 320, 240), "outline", "param.ids");
        set("outline", "push.outlineColor", Value::List { 1, 0.55, 0.15, 1 });
        set("outline", "push.thickness", 2);
        set("outline", "depthStencilState", Value::Members { { "depthTestEnable", false }, { "depthWriteEnable", false } });
        set("outline", "colorBlendState", Value::Members { { "attachments", Value::List { Value::Members {
            { "blendEnable", true }, { "srcColorBlendFactor", "eSrcAlpha" }, { "dstColorBlendFactor", "eOneMinusSrcAlpha" }, { "colorBlendOp", "eAdd" },
            { "srcAlphaBlendFactor", "eOne" }, { "dstAlphaBlendFactor", "eOneMinusSrcAlpha" }, { "alphaBlendOp", "eAdd" },
            { "colorWriteMask", Value::List { "eR", "eG", "eB", "eA" } } } } } });
        node("Draw", "draw outline", 880, 40);
        wire("outline", "draw outline", "pipeline");
        set("draw outline", "load", true);

        // ---- editor: what an editor draws over the picture, and reads back of it ----------------------------------
        pass = "editor";
        doc.AddPass(pass);
        node("DebugDraw", "helpers", 320, 40);
        wire(bring("color", 40, 40), "helpers", "image");
        node("Pick", "pick", 320, 200);
        wire(bring("ids", 40, 200), "pick", "ids");

        // ---- show: the picture, in the window ----------------------------------------------------------------------
        pass = "show";
        doc.AddPass(pass);
        node("Blit", "show", 320, 40);
        wire(bring("color", 40, 40), "show", "source");
        wire(bring("screen", 40, 180), "show", "destination");

        if (!ok && error.empty()) error = "a node of the standard graph could not be set up";
        return ok;
    }

    kecs::Entity AddStandardScene(Document& doc)
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
        if (doc.AddComponent(sphere, "lit")) {
            doc.SetField(sphere, "lit", "albedo", vec(0.85f, 0.6f, 1.f));
            doc.SetField(sphere, "lit", "roughness", Value(0.5));
        }
        return sphere;
    }
}
