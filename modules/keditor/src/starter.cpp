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
        const auto node = [&](const char* type, const char* id, const double x, const double y) {
            if (doc.AddNode(type, x, y, id) != id) { ok = false; error = std::format("there is a node '{}' already", id); }
        };
        const auto set = [&](const char* id, const char* input, Value value) { ok = doc.SetInput(id, input, std::move(value)) && ok; };
        const auto wire = [&](const char* from, const char* id, const char* input, const char* output = "out") { ok = doc.Connect(from, output, id, input) && ok; };

        // The window, and three pictures its size: what is seen, how far each pixel of it is, and which entity it is.
        node("Screen", "screen", 1500, 60);
        const auto picture = [&](const char* id, const char* format, Value::List usage, const double y) {
            node("Image", id, 40, y);
            set(id, "format", format);
            set(id, "usage", Value(std::move(usage)));
            doc.ClearInput(id, "extent.x");
            doc.ClearInput(id, "extent.y");
            wire("screen", id, "extent", "extent");
        };
        picture("color", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferSrc", "eTransferDst" }, 60);
        picture("ids", "eR32_UINT", { "eColorAttachment", "eSampled", "eTransferSrc" }, 240);
        picture("depth", "eD32_SFLOAT", { "eDepthStencilAttachment" }, 420);
        for (const auto& [view, of, y] : { std::tuple { "color view", "color", 60.0 }, std::tuple { "ids view", "ids", 240.0 }, std::tuple { "depth view", "depth", 420.0 } }) {
            node("ImageView", view, 280, y);
            wire(of, view, "image");
        }

        // What the scene is drawn into: the picture and the ids, cleared, with depth. And the picture alone, with
        // depth and without, for what is drawn after over what is there.
        node("Framebuffer", "scene target", 520, 60);
        wire("color view", "scene target", "color[0].view");
        set("scene target", "color[0].name", "color");
        set("scene target", "color[0].clear", Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0, 0, 0, 1 } } } } });
        wire("ids view", "scene target", "color[1].view");
        set("scene target", "color[1].name", "ids");
        set("scene target", "color[1].clear", Value::Members { { "scalarType", "UInt" }, { "components", 4 }, { "value", Value::Members { { "u", Value::List { 0, 0, 0, 0 } } } } });
        wire("depth view", "scene target", "depthStencil.view");
        set("scene target", "depthStencil.which", "Depth");
        set("scene target", "depthStencil.name", "depth");
        set("scene target", "depthStencil.depth", 1);
        node("Framebuffer", "sky target", 520, 300);
        wire("color view", "sky target", "color[0].view");
        set("sky target", "color[0].name", "color");
        wire("depth view", "sky target", "depthStencil.view");
        set("sky target", "depthStencil.which", "Depth");
        set("sky target", "depthStencil.name", "depth");
        set("sky target", "depthStencil.depth", 1);
        node("Framebuffer", "overlay target", 520, 500);
        wire("color view", "overlay target", "color[0].view");
        set("overlay target", "color[0].name", "color");

        // The frame as the shaders see it: the camera, the sky, the lights.
        node("SceneView", "view", 280, 640);
        wire("screen", "view", "extent", "size");
        node("Primitive", "shape", 520, 700);   // for how a default mesh's vertices are laid out

        // Every entity with a Mesh and this pipeline's component, lit. The component is the shader's: what its push
        // constants ask of each entity — an albedo, how metallic and how rough it is, what light it gives off.
        node("GraphicsPipeline", "lit", 800, 60);
        set("lit", "vertex.module", "lit");
        set("lit", "vertex.entry", "vertex");
        wire("shape", "lit", "vertex.layout", "layout");
        set("lit", "fragment.module", "lit");
        set("lit", "fragment.entry", "fragment");
        wire("scene target", "lit", "framebuffer");
        wire("view", "lit", "param.frame", "frame");
        wire("view", "lit", "param.lights", "lights");

        // The sky: a triangle over the whole picture at the far plane, where nothing nearer was drawn.
        node("GraphicsPipeline", "sky", 800, 300);
        set("sky", "vertex.module", "sky");
        set("sky", "vertex.entry", "vertex");
        set("sky", "fragment.module", "sky");
        set("sky", "fragment.entry", "fragment");
        wire("sky target", "sky", "framebuffer");
        wire("view", "sky", "param.frame", "frame");
        set("sky", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", false }, { "depthCompareOp", "eLessOrEqual" } });

        // A line round what is picked, from the image of which entity is where.
        node("GraphicsPipeline", "outline", 800, 500);
        set("outline", "vertex.module", "outline");
        set("outline", "vertex.entry", "vertex");
        set("outline", "fragment.module", "outline");
        set("outline", "fragment.entry", "fragment");
        wire("overlay target", "outline", "framebuffer");
        wire("ids", "outline", "param.ids");
        set("outline", "push.outlineColor", Value::List { 1, 0.55, 0.15, 1 });
        set("outline", "push.thickness", 2);
        set("outline", "depthStencilState", Value::Members { { "depthTestEnable", false }, { "depthWriteEnable", false } });
        set("outline", "colorBlendState", Value::Members { { "attachments", Value::List { Value::Members {
            { "blendEnable", true }, { "srcColorBlendFactor", "eSrcAlpha" }, { "dstColorBlendFactor", "eOneMinusSrcAlpha" }, { "colorBlendOp", "eAdd" },
            { "srcAlphaBlendFactor", "eOne" }, { "dstAlphaBlendFactor", "eOneMinusSrcAlpha" }, { "alphaBlendOp", "eAdd" },
            { "colorWriteMask", Value::List { "eR", "eG", "eB", "eA" } } } } } });

        // The passes, in the order they are drawn where what they use does not say.
        node("Draw", "draw scene", 1100, 60);
        wire("lit", "draw scene", "pipeline");
        node("Draw", "draw sky", 1100, 300);
        wire("sky", "draw sky", "pipeline");
        set("draw sky", "load", true);
        node("Draw", "draw outline", 1100, 500);
        wire("outline", "draw outline", "pipeline");
        set("draw outline", "load", true);
        node("DebugDraw", "helpers", 1100, 660);
        wire("color", "helpers", "image");
        node("Pick", "pick", 1100, 780);
        wire("ids", "pick", "ids");
        node("Blit", "show", 1500, 240);
        wire("color", "show", "source");
        wire("screen", "show", "destination");

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
