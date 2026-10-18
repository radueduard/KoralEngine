//
// Forward+, with nodes: the renderer of the framework's ForwardPlus project made as a scene's graph, every pass of it
// a page. Nothing in the engine knows of it — it is ordinary nodes, added through the document as an editor's panels
// add them, and the shaders beside this file are what a project of it has in its `shaders` folder.
//
//   frame     the camera, the sun and its cascades, the sky and the point lights, as the buffers the shaders declare
//   lights    the point lights sorted into clusters: a compute pass, a work group a cluster
//   shadows   what the sun sees, four cascades into a quarter each of one depth image
//   prepass   how far everything is from the camera, before anything is shaded
//   forward   every entity with a Mesh and the `forward` component, lit by its cluster's lights and the sun
//   occlusion what is in a corner is darker: horizon-based ambient occlusion, blurred, over the picture
//   sky       the sky, where nothing was drawn
//   reflections  what a polished surface mirrors of the picture, traced through it
//   bloom     what is brighter than white bleeds light round it
//   show      the picture as light, tone mapped, in the window — or, with `debug view` at 1, how many lights each
//             pixel's cluster holds
//

#pragma once

#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <keditor/document.h>
#include <kgraph/meshes.h>

namespace forwardplus
{
    using kgraph::Value;

    /// The clusters lights are sorted into: across the picture, down it, and into it.
    inline constexpr int Grid[3] { 16, 9, 24 };
    /// The most lights one cluster keeps: what lightCulling.slang calls MaxLightsPerCluster.
    inline constexpr int MaxLightsPerCluster = 256;
    /// How big one cascade's shadow map is: the atlas is two of them each way.
    inline constexpr int ShadowTile = 2048;
    inline constexpr int BloomLevels = 6;

    /** The shaders a project of it needs, by file name. */
    inline constexpr const char* Shaders[] {
        "forwardCommon.slang", "lightCulling.slang", "depth.slang", "forward.slang", "sky.slang", "toneMapping.slang",
        "ambientOcclusion.slang", "reflections.slang", "bloom.slang", "clusterHeat.slang",
    };

    inline bool AddLampShadows(keditor::Document& doc, std::string& error);

    /**
     * Adds the graph to @p doc. False, with @p error, when a node could not be added, set or wired.
     * @param environment A picture of the whole sky — an .hdr, longitude across and latitude down — for the sky to
     *                    be: under the project's assets, or by its whole path. Without one, it is the scene's Sky.
     */
    inline bool BuildGraph(keditor::Document& doc, std::string& error, const std::string& environment = {})
    {
        bool ok = true;
        std::string pass;
        const auto failed = [&](const std::string& what) { ok = false; if (error.empty()) error = what; };
        const auto node = [&](const char* type, const std::string& id, const double x, const double y) {
            if (doc.AddNode(type, x, y, id, pass) != id) failed(std::format("there is a node '{}' already", id));
        };
        const auto set = [&](const std::string& id, const std::string& input, Value value) {
            if (!doc.SetInput(id, input, std::move(value))) failed(std::format("'{}' could not be given '{}'", id, input));
        };
        const auto wire = [&](const std::string& from, const std::string& id, const std::string& input, const std::string& output = "out") {
            if (!doc.Connect(from, output, id, input)) failed(std::format("'{}' could not be wired to '{}.{}'", from, id, input));
        };
        // What an earlier pass made, brought into this one: a node here that stands for it. Once a pass.
        double broughtAt = 0;
        const auto bring = [&](const std::string& id) {
            const std::string made = doc.Import(id, pass, 40, 40 + (broughtAt += 110));
            if (made.empty()) failed(std::format("there is no node '{}' to bring in", id));
            return made;
        };
        const auto page = [&](const char* name) {
            pass = name;
            broughtAt = 0;
            doc.AddPass(pass);
        };
        // An image, sized by the window (divided by `shrink`) or as given, and a view of all of it.
        const auto image = [&](const std::string& id, const char* format, Value::List usage, const double x, const double y, const std::string& sizedBy, const int width = 0, const int height = 0,
                               const char* sizeOutput = "extent") {
            node("Image", id, x, y);
            set(id, "format", format);
            set(id, "usage", Value(std::move(usage)));
            if (sizedBy.empty()) {
                set(id, "extent.x", width);
                set(id, "extent.y", height);
            } else {
                doc.ClearInput(id, "extent.x");
                doc.ClearInput(id, "extent.y");
                wire(sizedBy, id, "extent", sizeOutput);
            }
            node("ImageView", id + " view", x + 220, y);
            wire(id, id + " view", "image");
        };
        const auto sampler = [&](const std::string& id, const char* filter, const double x, const double y) {
            node("Sampler", id, x, y);
            for (const char* which : { "minFilter", "magFilter" }) set(id, which, filter);
            for (const char* which : { "addressModeU", "addressModeV" }) set(id, which, "eClampToEdge");
        };
        // A buffer a shader declares, by the entry point that uses it: its fields are then inputs, by name.
        const auto shaderBuffer = [&](const std::string& id, const char* module, const char* entry, const char* parameter, const double x, const double y) {
            node("ShaderBuffer", id, x, y);
            set(id, "module", module);
            set(id, "entry", entry);
            set(id, "parameter", parameter);
        };
        const auto field = [&](const std::string& id, const char* name, const std::string& from, const std::string& output = "out") {
            wire(from, id, std::string("field.") + name, output);
        };
        // A compute pass over an image: the pipeline, what is wired to it, and a Dispatch that covers `cover`.
        const auto compute = [&](const std::string& id, const char* module, const char* entry, const char* cover, const double x, const double y) {
            node("ComputePipeline", id, x, y);
            set(id, "compute.module", module);
            set(id, "compute.entry", entry);
            node("Dispatch", "run " + id, x + 260, y);
            wire(id, "run " + id, "pipeline");
            if (cover) {
                for (const char* axis : { "groups.x", "groups.y", "groups.z" }) doc.ClearInput("run " + id, axis);
                set("run " + id, "cover", cover);
            }
        };
        // A draw for each of the entities a Filter keeps of every entity: its mesh, where it is, and whatever else its
        // pipeline leaves to each draw, from the field of the same name of the pipeline's component, when it keeps those.
        const auto drawEach = [&](const std::string& draw, const std::string& each, const char* material, Value::List with, Value::List without) {
            node("Filter", each, doc.Graph().Find(draw)->x, doc.Graph().Find(draw)->y + 160);
            wire(bring("all entities"), each, "entities");
            with.insert(with.begin(), Value("Mesh"));
            if (material) with.emplace_back(material);
            set(each, "with", Value(std::move(with)));
            if (!without.empty()) set(each, "without", Value(std::move(without)));
            wire(each, draw, "mesh", "Mesh.mesh");
            wire(each, draw, "push.model", "matrix");
            if (!material) return;
            const auto gives = doc.Runtime().OutputsOf(each);
            for (const auto& port : doc.Runtime().PortsOf(draw)) {
                if (!port.name.starts_with("param.") && !port.name.starts_with("push.")) continue;
                const std::string field = std::string(material) + "." + port.name.substr(port.name.find('.') + 1);
                if (port.name != "push.model" && std::ranges::find(gives, field, &kgraph::Port::name) != gives.end()) wire(each, draw, port.name, field);
            }
        };
        const auto shaders = [&](const std::string& id, const char* module, const char* vertex, const char* fragment) {
            set(id, "vertex.module", module);
            set(id, "vertex.entry", vertex);
            set(id, "fragment.module", module);
            set(id, "fragment.entry", fragment);
        };
        const auto depthTarget = [&](const std::string& id, const std::string& view) {
            wire(view, id, "depthStencil.view");
            set(id, "depthStencil.which", "Depth");
            set(id, "depthStencil.name", "depth");
            set(id, "depthStencil.depth", 1);
        };
        const Value clearToNothing = Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0, 0, 0, 0 } } } } };

        // ---- frame: what every pass sees of the frame -------------------------------------------------------------
        // The scene's camera, sun, sky and lights as values, and the three buffers of them the shaders declare: each
        // a ShaderBuffer, which has the shader's struct as its inputs. The point lights are an element an entity.
        page("frame");
        node("Screen", "screen", 40, 40);
        node("Primitive", "shape", 40, 160);                 // for how a default mesh's vertices are laid out
        sampler("nearest", "eNearest", 420, 40);             // a texel as it is
        sampler("linear", "eLinear", 420, 160);              // between texels, a blend of them
        // What an entity's own pictures are read with: repeated across a surface, and their smaller levels used
        // where the surface is far or seen at a slant.
        node("Sampler", "texture sampler", 660, 40);
        for (const char* which : { "minFilter", "magFilter" }) set("texture sampler", which, "eLinear");
        for (const char* which : { "addressModeU", "addressModeV" }) set("texture sampler", which, "eRepeat");
        set("texture sampler", "mipmapMode", "eLinear");
        set("texture sampler", "maxLod", 16);
        set("texture sampler", "anisotropyEnable", true);
        set("texture sampler", "maxAnisotropy", 8);
        // Each of the scene's own is a component of the engine's package: the first Camera, Sky and DirectionalLight,
        // each read by an Entity node; every point light, by a ForEach.
        const auto first = [&](const char* id, const char* component, const double y) {
            node("Entity", id, 40, y);
            set(id, "component", component);
        };
        // The camera: a view from where its entity is, towards a step along the way it faces, rolled as it is; and a
        // lens of its field of view, near and far. What the scene is seen through: in the editor, the editor's view.
        first("eye", "Camera", 300);
        node("Add", "looking at", 220, 380);
        wire("eye", "looking at", "a", "worldPosition");
        wire("eye", "looking at", "b", "forward");
        node("View", "camera", 220, 300);
        wire("eye", "camera", "position", "worldPosition");
        wire("looking at", "camera", "target");
        wire("eye", "camera", "up", "up");
        set("camera", "main", true);
        node("Perspective", "lens", 220, 460);
        set("lens", "main", true);
        wire("eye", "lens", "fieldOfView", "field.fieldOfView");
        wire("eye", "lens", "near", "field.near");
        wire("eye", "lens", "far", "field.far");
        wire("screen", "lens", "aspect", "aspect");
        node("MatrixMultiply", "seen", 220, 540);    // the projection after the view: what a position in the world is multiplied by
        wire("lens", "seen", "a");
        wire("camera", "seen", "b");
        // Every entity: what each pass's draws keep some of.
        node("Entities", "all entities", 40, 1500);
        shaderBuffer("camera data", "forward", "fragment", "camera", 420, 300);
        for (const char* matrix : { "view", "inverseView", "position" }) field("camera data", matrix, "camera", matrix);
        for (const char* matrix : { "projection", "inverseProjection" }) field("camera data", matrix, "lens", matrix);
        field("camera data", "viewProjection", "seen");
        field("camera data", "zNear", "lens", "near");
        field("camera data", "zFar", "lens", "far");
        field("camera data", "screenSize", "screen", "size");
        set("camera data", "field.gridSize", Value::List { Grid[0], Grid[1], Grid[2] });

        node("ForEach", "lamps", 40, 760);
        set("lamps", "component", "PointLight");
        shaderBuffer("light data", "forward", "fragment", "lights", 420, 760);
        field("light data", "position", "lamps", "worldPosition");
        field("light data", "radius", "lamps", "field.range");
        field("light data", "color", "lamps", "field.color");
        field("light data", "intensity", "lamps", "field.intensity");
        field("camera data", "lightCount", "light data", "count");

        first("sky values", "Sky", 1000);
        const auto linear = [&](const std::string& id, const std::string& from, const char* output, const double y) {
            node("Linear", id, 220, y);
            wire(from, id, "a", output);
        };
        linear("zenith", "sky values", "field.zenith", 1000);
        linear("horizon", "sky values", "field.horizon", 1060);
        linear("ground", "sky values", "field.ground", 1120);
        node("Multiply", "sun radius", 220, 1180);      // the cosine of half the sun disc's size
        wire("sky values", "sun radius", "a", "field.sunSize");
        set("sun radius", "b", 0.5);
        node("Radians", "sun radians", 400, 1180);
        wire("sun radius", "sun radians", "a");
        node("Cos", "sun cosine", 580, 1180);
        wire("sun radians", "sun cosine", "a");
        first("sun", "DirectionalLight", 1240);
        linear("sun linear", "sun", "field.color", 1240);
        node("Multiply", "sun color", 400, 1240);       // in linear terms, times how bright it is
        wire("sun linear", "sun color", "a");
        wire("sun", "sun color", "b", "field.intensity");
        node("ShadowCascades", "cascades", 40, 1400);
        wire("camera", "cascades", "view");
        wire("lens", "cascades", "projection");
        wire("sun", "cascades", "direction", "forward");
        set("cascades", "resolution", ShadowTile);
        set("cascades", "distance", 60);
        set("cascades", "boundsMin", Value::List { -30, -1, -30 });
        set("cascades", "boundsMax", Value::List { 30, 12, 30 });
        shaderBuffer("sun data", "forward", "fragment", "sun", 420, 1000);
        field("sun data", "lightViewProj", "cascades", "matrices");
        field("sun data", "splits", "cascades", "splits");
        field("sun data", "worldTexel", "cascades", "worldTexel");
        field("sun data", "tileTexel", "cascades", "tileTexel");
        field("sun data", "direction", "cascades", "direction");
        field("sun data", "present", "sun", "present");
        field("sun data", "color", "sun color");
        set("sun data", "field.bias", 0.0015);
        set("sun data", "field.pcfRadius", 1.5);
        set("sun data", "field.blend", 0.2);
        field("sun data", "ambient", "sky values", "field.ambient");
        field("sun data", "skyZenith", "zenith");
        field("sun data", "skyHorizon", "horizon");
        field("sun data", "skyGround", "ground");
        field("sun data", "sunCosine", "sun cosine");

        // ---- lights: the point lights, sorted into clusters -----------------------------------------------------------
        // The grid says, for each cluster, which run of the index list is its lights. The counter is how much of the
        // list is claimed: cleared, then added to by each cluster's work group as it claims its run.
        page("lights");
        const int clusters = Grid[0] * Grid[1] * Grid[2];
        const auto buffer = [&](const std::string& id, const int bytes, Value::List usage, const double y) {
            node("Buffer", id, 320, y);
            set(id, "instanceCount", bytes);
            set(id, "usage", Value(std::move(usage)));
        };
        // (eTransferSrc is so that each can be looked at, in an editor.)
        buffer("light grid", clusters * 8, { "eStorage", "eTransferSrc" }, 40);
        buffer("light indices", clusters * MaxLightsPerCluster * 4, { "eStorage", "eTransferSrc" }, 180);
        buffer("light counter", 16, { "eStorage", "eTransferDst", "eTransferSrc" }, 320);
        node("ClearBuffer", "reset counter", 600, 320);
        wire("light counter", "reset counter", "buffer");
        compute("cull lights", "lightCulling", "computeMain", nullptr, 600, 40);
        wire(bring("camera data"), "cull lights", "param.camera");
        wire(bring("light data"), "cull lights", "param.lights");
        wire("light grid", "cull lights", "param.lightGrid");
        wire("light indices", "cull lights", "param.lightIndices");
        wire("light counter", "cull lights", "param.lightCounter");
        set("run cull lights", "groups.x", Grid[0]);
        set("run cull lights", "groups.y", Grid[1]);
        set("run cull lights", "groups.z", Grid[2]);

        // ---- shadows: what the sun sees ----------------------------------------------------------------------------------
        // One depth image, a cascade a quarter. The pipeline and its Draw are in a loop of four turns: each turn's
        // Draw tells its draws which cascade it is (push.cascade), and the shader keeps to that quarter. The first turn's
        // Draw clears the image, and the rest draw over it. Each draws every entity with a Mesh but a lamp's ball and a decal.
        page("shadows");
        image("shadow atlas", "eD32_SFLOAT", { "eDepthStencilAttachment", "eSampled", "eTransferSrc" }, 320, 40, {}, ShadowTile * 2, ShadowTile * 2);
        node("Framebuffer", "shadow target", 760, 40);
        depthTarget("shadow target", "shadow atlas view");
        node("Repeat", "cascade", 320, 240);
        set("cascade", "count", 4);
        node("GraphicsPipeline", "shadow", 760, 240);
        shaders("shadow", "depth", "shadowVertex", "shadowFragment");
        wire(bring("shape"), "shadow", "vertex.layout", "layout");
        wire("shadow target", "shadow", "framebuffer");
        wire(bring("sun data"), "shadow", "param.sun");
        set("shadow", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", true }, { "depthCompareOp", "eLessOrEqual" } });
        node("Not", "after the first", 760, 480);
        wire("cascade", "after the first", "a", "first");
        node("Draw", "cast", 1040, 240);
        wire("shadow", "cast", "pipeline");
        wire("after the first", "cast", "load");
        // What is wholly outside a cascade is not drawn into it: the near ones are small, and take little.
        wire(bring("cascades"), "cast", "cull", "matrices");
        wire("cascade", "cast", "cullIndex", "index");
        wire("cascade", "cast", "push.cascade", "index");
        drawEach("cast", "casters", nullptr, {}, { "PointLight", "Blended" });     // a lamp's own ball casts nothing, and nor does a decal

        // ---- prepass: depth, before anything is shaded -------------------------------------------------------------------
        page("prepass");
        const std::string screen = bring("screen");
        image("depth", "eD32_SFLOAT", { "eDepthStencilAttachment", "eSampled", "eTransferSrc" }, 320, 40, screen);
        node("Framebuffer", "prepass target", 760, 40);
        depthTarget("prepass target", "depth view");
        node("GraphicsPipeline", "prepass", 760, 220);
        shaders("prepass", "depth", "prepassVertex", "prepassFragment");
        wire(bring("shape"), "prepass", "vertex.layout", "layout");
        wire("prepass target", "prepass", "framebuffer");
        wire(bring("camera data"), "prepass", "param.camera");
        node("Draw", "draw depth", 1040, 220);
        wire("prepass", "draw depth", "pipeline");
        wire(bring("seen"), "draw depth", "cull");      // what the camera cannot see is not drawn
        // What has a cut-out is left to the forward pass, which reads its picture: here it would be drawn whole.
        drawEach("draw depth", "solid meshes", nullptr, { "forward" }, { "Masked", "Blended" });

        // ---- forward: every entity with the pipeline's component and a Mesh, lit -------------------------------------
        // Four pictures: the scene as light, and where each pixel's surface is, which way it faces, and how it mirrors.
        // The depth is the pre-pass's, tested against and kept.
        page("forward");
        const std::string forwardScreen = bring("screen");
        const Value::List target { "eColorAttachment", "eSampled", "eStorage", "eTransferSrc" };
        image("hdr", "eRGBA16_SFLOAT", target, 320, 40, forwardScreen);
        image("position", "eRGBA32_SFLOAT", target, 320, 180, forwardScreen);
        image("normal", "eRGBA16_SFLOAT", target, 320, 320, forwardScreen);
        image("specular", "eRGBA8_UNORM", target, 320, 460, forwardScreen);
        node("Framebuffer", "forward target", 760, 40);
        int attachment = 0;
        for (const char* name : { "hdr", "position", "normal", "specular" }) {
            wire(std::string(name) + " view", "forward target", std::format("color[{}].view", attachment));
            set("forward target", std::format("color[{}].name", attachment), name);
            set("forward target", std::format("color[{}].clear", attachment), clearToNothing);
            ++attachment;
        }
        depthTarget("forward target", bring("depth view"));
        node("GraphicsPipeline", "forward", 1040, 40);
        shaders("forward", "forward", "vertex", "fragment");
        wire(bring("shape"), "forward", "vertex.layout", "layout");
        wire("forward target", "forward", "framebuffer");
        wire(bring("camera data"), "forward", "param.camera");
        wire(bring("sun data"), "forward", "param.sun");
        wire(bring("light data"), "forward", "param.lights");
        wire(bring("light grid"), "forward", "param.lightGrid");
        wire(bring("light indices"), "forward", "param.lightIndices");
        wire(bring("shadow atlas view"), "forward", "param.shadowMap");
        wire(bring("nearest"), "forward", "param.shadowMap.sampler");
        // The pictures themselves are left to each draw: each entity's own, in its `forward` component.
        const std::string textures = bring("texture sampler");
        for (const char* map : { "albedoMap", "normalMap", "roughnessMap", "emissiveMap" }) wire(textures, "forward", std::format("param.{}.sampler", map));
        set("forward", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", true }, { "depthCompareOp", "eLessOrEqual" } });
        node("Draw", "draw forward", 1320, 40);
        wire("forward", "draw forward", "pipeline");
        set("draw forward", "depthLoad", "eLoad");
        const std::string seenBy = bring("seen");
        wire(seenBy, "draw forward", "cull");
        drawEach("draw forward", "solids", "forward", {}, { "Blended" });

        // Glass and decals — what has the tag Blended — after everything solid, over it by how opaque each is. The
        // same shaders in a pipeline that blends, writes no depth, and leaves the three pictures beside the first as
        // they are: those go on saying what the solid surface behind is. Its component has the same fields as
        // `forward`'s, and an importer fills both.
        node("GraphicsPipeline", "forward blended", 1040, 420);
        shaders("forward blended", "forward", "vertex", "fragment");
        const auto wired = doc.Graph().Find("forward")->inputs;     // (a copy: wiring makes the graph anew)
        for (const auto& [input, from] : wired)
            if (from.link && (input.starts_with("param.") || input == "vertex.layout" || input == "framebuffer")) wire(from.link->node, "forward blended", input, from.link->output);
        set("forward blended", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", false }, { "depthCompareOp", "eLessOrEqual" } });
        {
            const Value keep = Value::Members { { "blendEnable", false }, { "colorWriteMask", Value::List {} } };
            set("forward blended", "colorBlendState", Value::Members { { "attachments", Value::List { Value::Members {
                { "blendEnable", true }, { "srcColorBlendFactor", "eSrcAlpha" }, { "dstColorBlendFactor", "eOneMinusSrcAlpha" }, { "colorBlendOp", "eAdd" },
                { "srcAlphaBlendFactor", "eOne" }, { "dstAlphaBlendFactor", "eOneMinusSrcAlpha" }, { "alphaBlendOp", "eAdd" },
                { "colorWriteMask", Value::List { "eR", "eG", "eB", "eA" } } }, keep, keep, keep } } });
        }
        node("Draw", "draw blended", 1320, 420);
        wire("forward blended", "draw blended", "pipeline");
        set("draw blended", "load", true);
        wire(seenBy, "draw blended", "cull");
        drawEach("draw blended", "glass", "forward blended", { "Blended" }, {});

        // ---- occlusion: what is in a corner is darker ---------------------------------------------------------------------
        // How occluded each pixel is, worked out of where its surface and its neighbours' are; that blurred, keeping
        // to each surface; and the picture multiplied by it.
        page("occlusion");
        {
            const std::string screen = bring("screen"), camera = bring("camera data"), position = bring("position view");
            const std::string nearest = bring("nearest"), linear = bring("linear");
            image("occlusion raw", "eR8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 40, screen);
            image("occlusion", "eR8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 180, screen);
            compute("occlusion pass", "ambientOcclusion", "occlusion", "occlusionOut", 760, 40);
            wire(camera, "occlusion pass", "param.camera");
            wire(position, "occlusion pass", "param.positionTex");
            wire(nearest, "occlusion pass", "param.positionTex.sampler");
            wire("occlusion raw", "occlusion pass", "param.occlusionOut");
            set("run occlusion pass", "push.radius", 0.5);
            set("run occlusion pass", "push.intensity", 1.5);
            set("run occlusion pass", "push.bias", 0.1);
            compute("blur occlusion", "ambientOcclusion", "blur", "occlusionOut", 760, 240);
            wire(camera, "blur occlusion", "param.camera");
            wire(position, "blur occlusion", "param.positionTex");
            wire(nearest, "blur occlusion", "param.positionTex.sampler");
            wire("occlusion raw view", "blur occlusion", "param.occlusionIn");
            wire(linear, "blur occlusion", "param.occlusionIn.sampler");
            wire("occlusion", "blur occlusion", "param.occlusionOut");
            compute("apply occlusion", "ambientOcclusion", "compose", "hdrImage", 760, 440);
            wire("occlusion view", "apply occlusion", "param.occlusionIn");
            wire(linear, "apply occlusion", "param.occlusionIn.sampler");
            wire(bring("hdr"), "apply occlusion", "param.hdrImage");
        }

        // ---- sky: where nothing was drawn ----------------------------------------------------------------------------------
        page("sky");
        compute("sky", "sky", environment.empty() ? "computeMain" : "environmentMain", "hdrImage", 600, 40);
        if (!environment.empty()) {
            node("ImageFile", "environment", 320, 40);
            set("environment", "file", environment);
            node("Sampler", "environment sampler", 320, 180);       // round the horizon it goes on; at the poles it stops
            for (const char* which : { "minFilter", "magFilter" }) set("environment sampler", which, "eLinear");
            set("environment sampler", "addressModeU", "eRepeat");
            set("environment sampler", "addressModeV", "eClampToEdge");
            wire("environment", "sky", "param.environment");
            wire("environment sampler", "sky", "param.environment.sampler");
            set("run sky", "push.intensity", 1.0);
            set("run sky", "push.rotation", 0.0);
        }
        wire(bring("camera data"), "sky", "param.camera");
        if (environment.empty()) wire(bring("sun data"), "sky", "param.sun");
        wire(bring("depth view"), "sky", "param.depthTex");
        wire(bring("nearest"), "sky", "param.depthTex.sampler");
        wire(bring("hdr"), "sky", "param.hdrImage");

        // ---- reflections: what a polished surface mirrors of the picture ----------------------------------------------
        // After the sky, so that it is mirrored too. The trace reads the picture at other pixels, so what it finds
        // goes into an image of its own, and a second pass adds that to the picture.
        page("reflections");
        {
            const std::string screen = bring("screen"), nearest = bring("nearest"), hdr = bring("hdr");
            image("reflection", "eRGBA16_SFLOAT", { "eStorage", "eSampled", "eTransferSrc" }, 320, 40, screen);
            compute("trace reflections", "reflections", "trace", "reflection", 760, 40);
            wire(bring("camera data"), "trace reflections", "param.camera");
            wire(bring("hdr view"), "trace reflections", "param.hdr");
            wire(bring("linear"), "trace reflections", "param.hdr.sampler");
            for (const char* name : { "position", "normal", "specular" }) {
                wire(bring(std::string(name) + " view"), "trace reflections", std::format("param.{}Tex", name));
                wire(nearest, "trace reflections", std::format("param.{}Tex.sampler", name));
            }
            wire("reflection", "trace reflections", "param.reflection");
            set("run trace reflections", "push.maxDistance", 20);
            set("run trace reflections", "push.thickness", 0.3);
            set("run trace reflections", "push.maxRoughness", 0.5);
            set("run trace reflections", "push.intensity", 1.0);
            compute("add reflections", "reflections", "compose", "hdrImage", 760, 300);
            wire("reflection view", "add reflections", "param.traced");
            wire(nearest, "add reflections", "param.traced.sampler");
            wire(hdr, "add reflections", "param.hdrImage");
        }

        // ---- bloom: what is brighter than white bleeds light round it ----------------------------------------------------
        // Two chains of images, each level half the last: the picture filtered down one, level by level, then back up
        // the other, each level adding the one below it, blurred. A level is a view of one mip of an image. Each way
        // is a loop — a Repeat, and the view, the pipeline and the Dispatch wired from it, there once a turn — in
        // which a Previous is the level the turn before wrote.
        page("bloom");
        {
            const std::string screen = bring("screen"), linear = bring("linear"), hdr = bring("hdr");
            node("Divide", "bloom size", 320, 40);
            wire(screen, "bloom size", "a", "extent");
            set("bloom size", "b", Value::List { 2, 2, 1 });
            image("bloom down", "eRGBA16_SFLOAT", { "eStorage", "eSampled", "eTransferSrc" }, 320, 180, "bloom size", 0, 0, "out");
            image("bloom up", "eRGBA16_SFLOAT", { "eStorage", "eSampled", "eTransferSrc" }, 320, 620, "bloom size", 0, 0, "out");
            set("bloom down", "mipLevels", BloomLevels);
            set("bloom up", "mipLevels", BloomLevels);
            const auto level = [&](const std::string& id, const char* chain, const char* loop, const char* turn, const double x, const double y) {
                node("ImageView", id, x, y);
                wire(chain, id, "image");
                wire(loop, id, "baseMipLevel", turn);
                set(id, "mipLevelCount", 1);
            };

            // The way down: a turn a level. Each reads the level above it — the picture itself, on the first turn —
            // and writes its own.
            node("Repeat", "down levels", 800, 40);
            set("down levels", "count", BloomLevels);
            level("down level", "bloom down", "down levels", "index", 800, 180);
            node("Previous", "level above", 800, 340);
            set("level above", "of", "down level");
            set("level above", "first", bring("hdr view"));
            compute("down", "bloom", "downsample", "target", 1080, 180);
            wire("level above", "down", "param.source");
            wire(linear, "down", "param.source.sampler");
            wire("down level", "down", "param.target");
            wire("down levels", "run down", "push.level", "index");
            set("run down", "push.threshold", 1.0);
            set("run down", "push.knee", 0.5);

            // The way back up, from the smallest level but one: each adds the level below it, blurred, to its own
            // as it came down. The level below the first is the smallest of the way down.
            node("Repeat", "up levels", 800, 620);
            set("up levels", "count", BloomLevels - 1);
            level("up level", "bloom up", "up levels", "reverse", 800, 760);
            node("Element", "smallest", 800, 920);
            set("smallest", "of", "down level");
            node("Previous", "level below", 800, 1060);
            set("level below", "of", "up level");
            set("level below", "first", "smallest");
            node("Element", "as it came down", 800, 1200);
            set("as it came down", "of", "down level");
            wire("up levels", "as it came down", "index", "reverse");
            compute("up", "bloom", "upsample", "target", 1080, 760);
            wire("level below", "up", "param.source");
            wire(linear, "up", "param.source.sampler");
            wire("as it came down", "up", "param.detail");
            wire(linear, "up", "param.detail.sampler");
            wire("up level", "up", "param.target");

            node("Element", "bloom", 1640, 620);
            set("bloom", "of", "up level");
            // Each level of the way up added its own light, so the top holds about as many times it as there are levels.
            compute("add bloom", "bloom", "composite", "hdrImage", 1640, 180);
            wire("bloom", "add bloom", "param.source");
            wire(linear, "add bloom", "param.source.sampler");
            wire(hdr, "add bloom", "param.hdrImage");
            set("run add bloom", "push.strength", 0.5 / BloomLevels);
        }

        // ---- show: the picture as light, made the picture as the display shows it -----------------------------------
        page("show");
        const std::string showScreen = bring("screen");
        image("ldr", "eRGBA8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 40, showScreen);
        compute("tone map", "toneMapping", "computeMain", "ldrImage", 760, 40);
        wire(bring("hdr"), "tone map", "param.hdrImage");
        wire("ldr", "tone map", "param.ldrImage");
        set("run tone map", "push.exposure", 1.0);
        // To look at instead: how many lights each pixel's cluster holds. 'debug view' is 0 for the picture and 1 for
        // that; the pass that makes it is left out while it is not shown.
        node("Number", "debug view", 320, 420);
        set("debug view", "value", 0);
        image("cluster heat", "eRGBA8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 560, showScreen);
        compute("heat", "clusterHeat", "computeMain", "heat", 760, 420);
        wire(bring("camera data"), "heat", "param.camera");
        wire(bring("depth view"), "heat", "param.depthTex");
        wire(bring("nearest"), "heat", "param.depthTex.sampler");
        wire("ldr view", "heat", "param.picture");
        wire(bring("nearest"), "heat", "param.picture.sampler");
        wire(bring("light grid"), "heat", "param.lightGrid");
        wire("cluster heat", "heat", "param.heat");
        set("run heat", "push.most", 24.0);
        wire("debug view", "run heat", "enabled");
        node("Switch", "picture", 1040, 420);
        wire("debug view", "picture", "index");
        wire("ldr", "picture", "option[0]");
        wire("cluster heat", "picture", "option[1]");
        node("Blit", "show", 1320, 420);
        wire("picture", "show", "source");
        wire(showScreen, "show", "destination");
        set("show", "filter", "eLinear");

        if (!ok && error.empty()) error = "a node of the graph could not be set up";
        return ok && AddLampShadows(doc, error);
    }

    /// How big one face of a point light's shadow cube is, and how many lights have one (the shaders' LampShadowMost).
    inline constexpr int LampShadowFace = 512, LampShadowMost = 24;

    /**
     * Adds the point lights' shadows to a graph BuildGraph made: a cube round each of the first LampShadowMost lights,
     * its six faces layers of one image, each drawn by a turn of a loop — what is out of a face's sight left out —
     * and the forward pipelines given the image and the faces' matrices. A pass of its own, after the sun's shadows.
     */
    inline bool AddLampShadows(keditor::Document& doc, std::string& error)
    {
        using kgraph::Value;
        if (doc.Graph().Find("lamp shadow maps")) return true;      // it has them already
        for (const char* needed : { "shape", "nearest", "forward", "forward blended" })
            if (!doc.Graph().Find(needed)) { error = std::format("there is no node '{}': the lamps' shadows are added to the graph BuildGraph makes", needed); return false; }
        const auto fail = [&](const std::string& why) { if (error.empty()) error = why; };
        const auto node = [&](const char* type, const std::string& id, const double x, const double y, const std::string& pass) {
            if (doc.AddNode(type, x, y, id, pass) != id) fail(std::format("'{}' could not be made", id));
        };
        const auto set = [&](const std::string& id, const std::string& input, Value value) { if (!doc.SetInput(id, input, std::move(value))) fail(std::format("'{}' has no '{}'", id, input)); };
        const auto wire = [&](const std::string& from, const std::string& id, const std::string& input, const std::string& output = "out") {
            if (!doc.Connect(from, output, id, input)) fail(std::format("'{}' could not be wired to '{}' of '{}'", from, input, id));
        };
        const auto bring = [&](const std::string& id, const std::string& pass, const double y) { return doc.Import(id, pass, 40, y); };

        // In the frame's pass: the faces' matrices, and the buffer of them the shaders declare.
        const std::string frame = doc.Graph().Find("sun data") ? doc.Graph().Find("sun data")->pass : std::string("frame");
        // Where each lamp is and how far it reaches, every lamp's at once: what the cubes are made round.
        node("Append", "lamp reach", 40, 1640, frame);
        wire("lamps", "lamp reach", "a", "worldPosition");
        wire("lamps", "lamp reach", "b", "field.range");
        node("Collect", "lamp places", 260, 1640, frame);
        wire("lamp reach", "lamp places", "value");
        node("PointShadows", "lamp shadows", 40, 1700, frame);
        wire("lamp places", "lamp shadows", "lights");
        set("lamp shadows", "most", LampShadowMost);
        // The lamps stand still, and so does most of what they light: two lights' cubes are drawn again each frame,
        // one after another, and the rest stay as they were drawn last.
        set("lamp shadows", "each", 2);
        node("ShaderBuffer", "lamp shadow data", 520, 1700, frame);
        set("lamp shadow data", "module", "forward");
        set("lamp shadow data", "entry", "fragment");
        set("lamp shadow data", "parameter", "lampShadows");
        wire("lamp shadows", "lamp shadow data", "field.faces", "matrices");
        wire("lamp shadows", "lamp shadow data", "field.count", "count");
        set("lamp shadow data", "field.texel", 1.0 / LampShadowFace);
        set("lamp shadow data", "field.bias", 0.0004);
        set("lamp shadow data", "field.softness", 1.25);

        // A pass of its own, after the sun's shadows: one image of every face, and a loop that draws a face a turn.
        const std::string pass = "lamp shadows";
        const std::vector<std::string> passes = doc.Passes();
        const auto after = std::ranges::find(passes, "shadows");
        doc.AddPass(pass, after == passes.end() ? -1 : static_cast<int>(after - passes.begin()) + 1);
        node("Image", "lamp shadow maps", 320, 40, pass);
        set("lamp shadow maps", "format", "eD32_SFLOAT");
        set("lamp shadow maps", "usage", Value(Value::List { "eDepthStencilAttachment", "eSampled" }));
        set("lamp shadow maps", "extent.x", LampShadowFace);
        set("lamp shadow maps", "extent.y", LampShadowFace);
        set("lamp shadow maps", "arrayLayers", LampShadowMost * 6);
        node("ImageView", "lamp shadow maps view", 560, 40, pass);
        wire("lamp shadow maps", "lamp shadow maps view", "image");
        set("lamp shadow maps view", "viewType", "e2DArray");
        set("lamp shadow maps view", "arrayLayerCount", LampShadowMost * 6);
        node("Repeat", "lamp face", 320, 260, pass);
        set("lamp face", "count", LampShadowMost * 6);
        node("ImageView", "lamp face view", 560, 260, pass);
        wire("lamp shadow maps", "lamp face view", "image");
        wire("lamp face", "lamp face view", "baseArrayLayer", "index");
        set("lamp face view", "arrayLayerCount", 1);
        node("Framebuffer", "lamp face target", 800, 260, pass);
        wire("lamp face view", "lamp face target", "depthStencil.view");
        set("lamp face target", "depthStencil.which", "Depth");
        set("lamp face target", "depthStencil.name", "depth");
        set("lamp face target", "depthStencil.depth", 1);
        node("GraphicsPipeline", "lamp shadow", 1040, 260, pass);
        set("lamp shadow", "vertex.module", "depth");
        set("lamp shadow", "vertex.entry", "lampVertex");
        set("lamp shadow", "fragment.module", "depth");
        set("lamp shadow", "fragment.entry", "lampFragment");
        wire(bring("shape", pass, 150), "lamp shadow", "vertex.layout", "layout");
        wire("lamp face target", "lamp shadow", "framebuffer");
        wire(bring("lamp shadow data", pass, 260), "lamp shadow", "param.lampShadows");
        set("lamp shadow", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", true }, { "depthCompareOp", "eLessOrEqual" } });
        node("Draw", "cast lamp", 1320, 260, pass);
        wire("lamp shadow", "cast lamp", "pipeline");
        // What is wholly out of a face's sight — past the light's reach, or another way from it — is not drawn into it.
        wire(bring("lamp shadows", pass, 370), "cast lamp", "cull", "matrices");
        wire("lamp face", "cast lamp", "cullIndex", "index");
        wire(bring("lamp shadows", pass, 370), "cast lamp", "idle", "idle");
        // Every entity with a Mesh but a lamp's own ball and a decal, each where it is, told which face it is drawn into.
        node("Filter", "lamp casters", 1320, 480, pass);
        wire(doc.Import("all entities", pass, 1040, 480), "lamp casters", "entities");
        doc.SetInput("lamp casters", "with", Value(Value::List { "Mesh" }));
        doc.SetInput("lamp casters", "without", Value(Value::List { "PointLight", "Blended" }));
        wire("lamp casters", "cast lamp", "mesh", "Mesh.mesh");
        wire("lamp casters", "cast lamp", "push.model", "matrix");
        wire("lamp face", "cast lamp", "push.face", "index");

        // And the pipelines that shade by them.
        // (Copies: bringing a node in makes the graph anew, and what pointed into the old one points nowhere.)
        const std::string shading = doc.Graph().Find("forward")->pass;
        std::string nearest;
        if (const auto& inputs = doc.Graph().Find("forward")->inputs; inputs.contains("param.shadowMap.sampler") && inputs.at("param.shadowMap.sampler").link)
            nearest = inputs.at("param.shadowMap.sampler").link->node;
        const std::string maps = bring("lamp shadow maps view", shading, 1500), data = bring("lamp shadow data", shading, 1610);
        if (nearest.empty()) nearest = bring("nearest", shading, 1720);
        for (const char* pipeline : { "forward", "forward blended" }) {
            wire(maps, pipeline, "param.lampShadowMaps");
            wire(nearest, pipeline, "param.lampShadowMaps.sampler");
            wire(data, pipeline, "param.lampShadows");
        }
        return error.empty();
    }

    /**
     * Adds something for it to draw: a floor, pillars that cast the sun's shadows, a field of spheres from matte to
     * polished and from plastic to metal, and @p lamps point lights of every colour scattered low over them — each a
     * small glowing ball, so that it is seen where it is.
     */
    inline void BuildScene(keditor::Document& doc, const int lamps = 96)
    {
        const auto vec = [](const double x, const double y, const double z) { return Value(Value::List { x, y, z }); };
        const auto drawn = [&](const std::string& name, const char* mesh, const kecs::Vec3 at, const kecs::Vec3 scale, const Value& albedo, const double metallic, const double roughness) {
            const kecs::Entity entity = doc.CreateEntity(name);
            doc.SetPosition(entity, at);
            doc.SetScale(entity, scale);
            doc.AddComponent(entity, "Mesh");
            doc.SetField(entity, "Mesh", "mesh", Value(kgraph::DefaultMeshId(mesh)));
            if (doc.AddComponent(entity, "forward")) {
                // It has no pictures of its own: each is white, which leaves what it is multiplied by as it is.
                for (const char* map : { "albedoMap", "normalMap", "roughnessMap", "emissiveMap" }) doc.SetField(entity, "forward", map, Value("$white"));
                doc.SetField(entity, "forward", "albedo", albedo);
                doc.SetField(entity, "forward", "metallic", Value(metallic));
                doc.SetField(entity, "forward", "roughness", Value(roughness));
            }
            return entity;
        };

        doc.AddTag("Masked");
        doc.AddTag("Blended");
        const kecs::Entity sky = doc.CreateEntity("Sky");
        doc.AddComponent(sky, "Sky");
        doc.SetField(sky, "Sky", "ambient", Value(0.12));
        const kecs::Entity camera = doc.CreateEntity("Camera");
        doc.AddComponent(camera, "Camera");
        doc.SetField(camera, "Camera", "fieldOfView", Value(50));
        doc.SetField(camera, "Camera", "far", Value(200));
        doc.SetPosition(camera, { 0.f, 5.5f, 15.f });
        doc.SetRotation(camera, { -22.f, 0.f, 0.f });
        doc.AddComponent(camera, "FlyCamera");       // hold the right button: the mouse turns it, W A S D E Q move it
        const kecs::Entity sun = doc.CreateEntity("Sun");
        doc.AddComponent(sun, "DirectionalLight");
        doc.SetField(sun, "DirectionalLight", "color", vec(1.0, 0.95, 0.85));
        doc.SetField(sun, "DirectionalLight", "intensity", Value(2.2));
        doc.SetPosition(sun, { 0.f, 10.f, 0.f });
        doc.SetRotation(sun, { -48.f, 35.f, 0.f });

        drawn("Floor", "plane", { 0.f, 0.f, 0.f }, { 48.f, 1.f, 48.f }, vec(0.62, 0.62, 0.64), 0.0, 0.7);
        for (int i = 0; i < 6; ++i) {
            const float x = (i % 2 ? 1.f : -1.f) * 9.f, z = -8.f + static_cast<float>(i / 2) * 8.f;
            drawn(std::format("Pillar {}", i), "cube", { x, 2.5f, z }, { 1.f, 5.f, 1.f }, vec(0.8, 0.78, 0.72), 0.0, 0.85);
        }
        // Rougher across, more of a metal away from the camera.
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 7; ++column) {
                const double hue = (row * 7 + column) / 35.0 * 6.2831853;
                const Value albedo = vec(0.55 + 0.4 * std::cos(hue), 0.55 + 0.4 * std::cos(hue + 2.094), 0.55 + 0.4 * std::cos(hue + 4.189));
                drawn(std::format("Ball {} {}", row, column), "sphere", { (column - 3) * 2.f, 0.75f, 4.f - row * 2.5f }, { 1.5f, 1.5f, 1.5f }, albedo,
                      row / 4.0, 0.08 + column / 6.0 * 0.8);
            }

        // The lamps: where each is, is a count of steps round a circle that never comes back on itself.
        for (int i = 0; i < lamps; ++i) {
            const double turn = i * 2.39996323, away = 2.0 + 16.0 * std::sqrt((i + 0.5) / lamps), hue = i * 0.61803399 * 6.2831853;
            const Value color = vec(0.5 + 0.5 * std::cos(hue), 0.5 + 0.5 * std::cos(hue + 2.094), 0.5 + 0.5 * std::cos(hue + 4.189));
            const kecs::Entity lamp = drawn(std::format("Lamp {}", i), "sphere",
                                            { static_cast<float>(std::cos(turn) * away), 0.45f + 0.9f * static_cast<float>(i % 3), static_cast<float>(std::sin(turn) * away - 2.0) },
                                            { 0.16f, 0.16f, 0.16f }, vec(0, 0, 0), 0.0, 1.0);
            const auto& glow = *color.Items();
            doc.SetField(lamp, "forward", "emission", vec(*glow[0].Number() * 6.0, *glow[1].Number() * 6.0, *glow[2].Number() * 6.0));
            doc.AddComponent(lamp, "PointLight");
            doc.SetField(lamp, "PointLight", "color", color);
            doc.SetField(lamp, "PointLight", "intensity", Value(3.0));
            doc.SetField(lamp, "PointLight", "range", Value(3.5));
        }
    }

    /**
     * Adds a scene made of model files — Sponza and what goes in it, say — in place of BuildScene's: their own things, each with
     * its mesh and its material's pictures (Document::ImportModel), a camera standing in it, a sun, and @p lamps
     * point lights along its length. False, with @p error, when there is no such file. The files are read off the
     * frame: the scene is whole when doc.Reading() is 0.
     */
    inline bool ImportScene(keditor::Document& doc, const std::vector<std::filesystem::path>& models, std::string& error, const int lamps = 64)
    {
        const auto vec = [](const double x, const double y, const double z) { return Value(Value::List { x, y, z }); };
        doc.AddTag("Masked");
        doc.AddTag("Blended");
        const kecs::Entity sky = doc.CreateEntity("Sky");
        doc.AddComponent(sky, "Sky");
        doc.SetField(sky, "Sky", "ambient", Value(0.25));
        const kecs::Entity camera = doc.CreateEntity("Camera");
        doc.AddComponent(camera, "Camera");
        doc.SetField(camera, "Camera", "fieldOfView", Value(60));
        doc.SetField(camera, "Camera", "far", Value(200));
        doc.SetPosition(camera, { -9.5f, 2.2f, -0.4f });
        doc.SetRotation(camera, { 6.f, -90.f, 0.f });
        doc.AddComponent(camera, "FlyCamera");       // hold the right button: the mouse turns it, W A S D E Q move it
        const kecs::Entity sun = doc.CreateEntity("Sun");
        doc.AddComponent(sun, "DirectionalLight");
        doc.SetField(sun, "DirectionalLight", "color", vec(1.0, 0.95, 0.85));
        doc.SetField(sun, "DirectionalLight", "intensity", Value(4.0));
        doc.SetRotation(sun, { -72.f, 25.f, 0.f });
        for (const auto& model : models)
            if (doc.ImportModel(model, &error).IsNull()) return false;

        for (int i = 0; i < lamps; ++i) {
            const double hue = i * 0.61803399 * 6.2831853, along = lamps > 1 ? -10.0 + 20.0 * i / (lamps - 1) : 0.0;
            const Value color = vec(0.5 + 0.5 * std::cos(hue), 0.5 + 0.5 * std::cos(hue + 2.094), 0.5 + 0.5 * std::cos(hue + 4.189));
            const kecs::Entity lamp = doc.CreateEntity(std::format("Lamp {}", i));
            doc.SetPosition(lamp, { static_cast<float>(along), 0.6f + 1.6f * static_cast<float>(i % 3), (i % 2 ? 1.f : -1.f) * (0.6f + 0.9f * static_cast<float>(i % 4)) });
            doc.SetScale(lamp, { 0.1f, 0.1f, 0.1f });
            doc.AddComponent(lamp, "Mesh");
            doc.SetField(lamp, "Mesh", "mesh", Value(kgraph::DefaultMeshId("sphere")));
            if (doc.AddComponent(lamp, "forward")) {
                const auto& glow = *color.Items();
                for (const char* map : { "albedoMap", "normalMap", "roughnessMap", "emissiveMap" }) doc.SetField(lamp, "forward", map, Value("$white"));
                doc.SetField(lamp, "forward", "albedo", vec(0, 0, 0));
                doc.SetField(lamp, "forward", "emission", vec(*glow[0].Number() * 6.0, *glow[1].Number() * 6.0, *glow[2].Number() * 6.0));
            }
            doc.AddComponent(lamp, "PointLight");
            doc.SetField(lamp, "PointLight", "color", color);
            doc.SetField(lamp, "PointLight", "intensity", Value(3.0));
            doc.SetField(lamp, "PointLight", "range", Value(3.5));
        }
        return true;
    }
}
