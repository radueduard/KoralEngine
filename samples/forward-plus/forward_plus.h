//
// Forward+, with nodes: the renderer of the framework's ForwardPlus project made as a scene's graph, every pass of it
// a page. Nothing in the engine knows of it — it is ordinary nodes, added through the document as an editor's panels
// add them, and the shaders beside this file are what a project of it has in its `shaders` folder.
//
//   frame     the camera, the sun and its cascades, the sky and the point lights, as the buffers the shaders declare
//   lights    the point lights sorted into clusters: a compute pass, a work group a cluster
//   shadows   what the sun sees, four cascades into a quarter each of one depth image
//   prepass   how far everything is from the camera, before anything is shaded: what is in sight of it, in one
//             indirect draw, and which instance each pixel shows
//   culling   what some pixel of the pre-pass showed: all the forward pass draws
//   forward   every entity with a Mesh and the `forward` component, lit by its cluster's lights and the sun: one
//             indirect draw, each instance's pictures by their bindless indices
//   occlusion what is in a corner is darker: horizon-based ambient occlusion, blurred, over the picture
//   sky       the sky, where nothing was drawn
//   reflections  what a polished surface mirrors of the picture, traced through it
//   antialias each frame, moved by a fraction of a pixel, blended into the ones before it
//   bloom     what is brighter than white bleeds light round it
//   show      the picture as light, tone mapped, in the window — or, with `debug view` at 1, how many lights each
//             pixel's cluster holds
//

#pragma once

#include <array>
#include <cmath>
#include <map>
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
    /// The most instances the culls count: the `seen` buffer's room, and the groups the culls are dispatched in (64 each).
    inline constexpr int MostInstances = 65536;
    /// What the shaders read of each vertex: the packs take only these, so that meshes of other files are packed as one.
    inline const kgraph::Value::List Read { "POSITION", "NORMAL", "UV" };

    /** The shaders a project of it needs, by file name. Its bloom, its tone mapping and the sun's shadows are the engine's (koral_bloom, koral_tonemap, koral_shadow). */
    inline constexpr const char* Shaders[] {
        "forwardCommon.slang", "lightCulling.slang", "depth.slang", "forward.slang", "sky.slang",
        "ambientOcclusion.slang", "reflections.slang", "clusterHeat.slang", "cullCommon.slang", "occlusionCull.slang", "taa.slang",
        "meshletTasks.slang", "rayTraced.slang", "emitters.slang",
    };

    inline bool AddLampShadows(keditor::Document& doc, std::string& error);
    inline bool AddBranches(keditor::Document& doc, std::string& error);
    inline bool AddBlendedToBranches(keditor::Document& doc, std::string& error);
    inline void IdleUnchosen(keditor::Document& doc);
    inline void GiveRayMaterials(keditor::Document& doc);
    inline bool AddEmitters(keditor::Document& doc, std::string& error);
    inline bool AddMeshletOcclusion(keditor::Document& doc, std::string& error);

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
        // An image, sized by the window (divided by `shrink`) or as given. Wired as it is where a view of all of it is
        // wanted: what reads or draws into it takes its default view, of the shape the shader declares.
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
            node("ColorConvert", id, 220, y);
            wire(from, id, "color", output);
            set(id, "from", "eGamma22");
            set(id, "to", "eLinear");
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

        // What the frame carries over from the last, for the antialiasing: the camera's matrix this frame and the last,
        // and the fraction of a pixel the pre-pass and the forward pass move the picture by. Advanced once a frame.
        node("Buffer", "frame state", 40, 1700);
        set("frame state", "instanceCount", 256);
        set("frame state", "usage", Value::List { "eStorage", "eTransferSrc" });
        compute("advance frame", "taa", "advance", nullptr, 580, 1700);
        wire("camera data", "advance frame", "param.camera");
        wire("frame state", "advance frame", "param.state");
        for (const char* axis : { "groups.x", "groups.y", "groups.z" }) set("run advance frame", axis, 1);

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
        depthTarget("shadow target", "shadow atlas");
        node("Repeat", "cascade", 320, 240);
        set("cascade", "count", 4);
        node("GraphicsPipeline", "shadow", 760, 240);
        shaders("shadow", "koral_shadow", "shadowVertex", "shadowFragment");
        wire(bring("shape"), "shadow", "vertex.layout", "layout");
        wire("shadow target", "shadow", "framebuffer");
        wire(bring("sun data"), "shadow", "param.cascades");     // its first four matrices are the cascades'
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
        // A cascade is drawn again only when what is in it, or where it is seen from, has changed: not while the camera
        // and everything in sight of the sun hold still.
        set("cast", "whenChanged", true);
        drawEach("cast", "casters", nullptr, {}, { "PointLight", "Blended" });     // a lamp's own ball casts nothing, and nor does a decal

        // ---- prepass: depth, before anything is shaded -------------------------------------------------------------------
        // Every entity with a Mesh but glass and decals, grouped by mesh and drawn by one indirect draw: first what of
        // them is in sight, each instance's box tested against the camera on the GPU and counted into the commands. Each
        // pixel says which instance it shows (one more than its number), for the occlusion cull; a cut-out leaves out
        // where its picture is clear. Its buffer of instances is the forward Draw's, so an instance is one number in both.
        page("prepass");
        const std::string screen = bring("screen");
        image("depth", "eD32_SFLOAT", { "eDepthStencilAttachment", "eSampled", "eTransferSrc" }, 320, 40, screen);
        image("visibility", "eR32_UINT", { "eColorAttachment", "eSampled", "eTransferSrc" }, 320, 180, screen);
        node("Framebuffer", "prepass target", 760, 40);
        depthTarget("prepass target", "depth");
        wire("visibility", "prepass target", "color[0].view");
        set("prepass target", "color[0].name", "visibility");
        set("prepass target", "color[0].clear", Value::Members { { "scalarType", "UInt" }, { "components", 4 }, { "value", Value::Members { { "u", Value::List { 0, 0, 0, 0 } } } } });
        // Not `forward` among its `with`: that is the forward pipeline's component, whose vertex layout comes from this
        // Filter's pack. Every mesh but glass has it.
        node("Filter", "drawn", 1040, 420);
        wire(bring("all entities"), "drawn", "entities");
        set("drawn", "with", Value::List { "Mesh" });
        set("drawn", "without", Value::List { "Blended" });
        set("drawn", "groupBy", "Mesh");
        node("MeshPack", "drawn meshes", 1320, 420);
        wire("drawn", "drawn meshes", "meshes", "Mesh.mesh");
        set("drawn meshes", "attributes", Value(Read));
        node("IndirectDraws", "in sight", 1600, 420);
        set("in sight", "mode", "eGPU");
        wire("drawn meshes", "in sight", "ranges", "ranges");
        wire("drawn", "in sight", "groups", "groups");
        const std::string cameraData = bring("camera data");
        // What is in sight: the engine's CullInSight, seen as the camera sees it without the jitter (`seen`). Its
        // instances are the forward Draw's, wired below.
        node("Function", "frustum", 1320, 640);
        set("frustum", "function", "CullInSight");
        wire("drawn meshes", "frustum", "ranges", "rangeBuffer");
        wire("in sight", "frustum", "commands", "commands");
        wire("in sight", "frustum", "drawCount", "drawCount");
        wire("in sight", "frustum", "visible", "visible");
        wire(bring("seen"), "frustum", "viewProjection");
        node("GraphicsPipeline", "prepass", 760, 220);
        shaders("prepass", "depth", "prepassVertex", "prepassFragment");
        wire("drawn meshes", "prepass", "vertex.layout", "layout");
        wire("prepass target", "prepass", "framebuffer");
        wire(cameraData, "prepass", "param.camera");
        wire(bring("frame state"), "prepass", "param.frameState");
        wire(bring("texture sampler"), "prepass", "param.textureSampler");
        wire("in sight", "prepass", "param.drawnInstances", "visible");
        node("Draw", "draw depth", 1040, 220);
        wire("prepass", "draw depth", "pipeline");
        wire("drawn meshes", "draw depth", "mesh", "mesh");
        set("draw depth", "command", "eDrawIndexedIndirect");
        wire("in sight", "draw depth", "indirect", "commands");
        wire("in sight", "draw depth", "drawCount", "drawCount");

        // ---- culling: what some pixel of the pre-pass showed, counted into the forward pass's commands -----------------
        // `mark` notes each instance a pixel shows; `gather` counts each noted one into its mesh's command, which
        // starts at none each frame, and writes where the forward pass finds it which instance it is.
        page("culling");
        {
            const std::string drawn = bring("drawn"), meshes = bring("drawn meshes");
            node("IndirectDraws", "seen instances", 320, 40);
            set("seen instances", "mode", "eGPU");
            wire(meshes, "seen instances", "ranges", "ranges");
            wire(drawn, "seen instances", "groups", "groups");
            node("Buffer", "seen flags", 320, 260);
            set("seen flags", "instanceCount", MostInstances * 4);
            set("seen flags", "usage", Value::List { "eStorage", "eTransferSrc" });
            compute("mark seen", "occlusionCull", "mark", "visibility", 600, 260);
            wire(bring("visibility"), "mark seen", "param.visibility");
            wire("seen flags", "mark seen", "param.seen");
            compute("gather seen", "occlusionCull", "gather", nullptr, 600, 40);
            wire("seen flags", "gather seen", "param.seen");
            wire("seen instances", "gather seen", "param.commands", "commands");
            wire("seen instances", "gather seen", "param.visible", "visible");
            set("run gather seen", "groups.x", MostInstances / 64);
            for (const char* axis : { "groups.y", "groups.z" }) set("run gather seen", axis, 1);
            wire("seen instances", "run gather seen", "push.commandCount", "drawCount");
        }

        // ---- forward: what was seen, in one indirect draw, lit -----------------------------------------------------------
        // Four pictures: the scene as light, and where each pixel's surface is, which way it faces, and how it mirrors.
        // The depth is the pre-pass's, tested against and not written: so even what it cuts out is tested before it
        // is shaded. Each instance's matrix and the fields of its `forward` component — its pictures by their bindless
        // indices — are in the buffer the Draw makes.
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
            wire(std::string(name), "forward target", std::format("color[{}].view", attachment));
            set("forward target", std::format("color[{}].name", attachment), name);
            set("forward target", std::format("color[{}].clear", attachment), clearToNothing);
            ++attachment;
        }
        depthTarget("forward target", bring("depth"));
        const std::string packed = bring("drawn meshes"), seen = bring("seen instances"), state = bring("frame state");
        node("GraphicsPipeline", "forward", 1040, 40);
        shaders("forward", "forward", "vertex", "fragment");
        wire(packed, "forward", "vertex.layout", "layout");
        wire("forward target", "forward", "framebuffer");
        wire(bring("camera data"), "forward", "param.camera");
        wire(bring("sun data"), "forward", "param.sun");
        wire(bring("light data"), "forward", "param.lights");
        wire(bring("light grid"), "forward", "param.lightGrid");
        wire(bring("light indices"), "forward", "param.lightIndices");
        wire(bring("shadow atlas"), "forward", "param.shadowMap");
        wire(bring("nearest"), "forward", "param.shadowMap.sampler");
        wire(bring("texture sampler"), "forward", "param.textureSampler");
        wire(seen, "forward", "param.drawnInstances", "visible");
        wire(state, "forward", "param.frameState");
        set("forward", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", false }, { "depthCompareOp", "eLessOrEqual" } });
        node("Draw", "draw forward", 1320, 40);
        wire("forward", "draw forward", "pipeline");
        set("draw forward", "depthLoad", "eLoad");
        wire(packed, "draw forward", "mesh", "mesh");
        set("draw forward", "command", "eDrawIndexedIndirect");
        wire(seen, "draw forward", "indirect", "commands");
        wire(seen, "draw forward", "drawCount", "drawCount");
        set("draw forward", "push.culled", 1);
        // The pre-pass draws with the forward Draw's buffer of instances: an instance is the same number in both.
        wire("draw forward", "draw depth", "param.instances", "instances");
        wire("draw forward", "frustum", "instances", "instances");
        wire("draw forward", "frustum", "stride", "instanceStride");

        // Glass and decals — what has the tag Blended — after everything solid, over it by how opaque each is: the same
        // shaders in a pipeline that blends, writes no depth, and leaves the three pictures beside the first as they
        // are, which go on saying what the solid surface behind is. Its own pack, drawn whole: none of it is in the
        // pre-pass to be culled by. Its component has the same fields as `forward`'s, and an importer fills both.
        node("Filter", "glass", 1320, 640);
        wire(bring("all entities"), "glass", "entities");
        set("glass", "with", Value::List { "Mesh", "Blended" });
        set("glass", "groupBy", "Mesh");
        node("MeshPack", "glass meshes", 1600, 640);
        wire("glass", "glass meshes", "meshes", "Mesh.mesh");
        set("glass meshes", "attributes", Value(Read));
        node("IndirectDraws", "glass instances", 1880, 640);
        wire("glass meshes", "glass instances", "ranges", "ranges");
        wire("glass", "glass instances", "groups", "groups");
        node("GraphicsPipeline", "forward blended", 1040, 420);
        shaders("forward blended", "forward", "vertex", "fragment");
        const auto wired = doc.Graph().Find("forward")->inputs;     // (a copy: wiring makes the graph anew)
        for (const auto& [input, from] : wired)
            if (from.link && (input.starts_with("param.") || input == "framebuffer")) wire(from.link->node, "forward blended", input, from.link->output);
        wire("glass meshes", "forward blended", "vertex.layout", "layout");
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
        wire("glass meshes", "draw blended", "mesh", "mesh");
        set("draw blended", "command", "eDrawIndexedIndirect");
        wire("glass instances", "draw blended", "indirect", "commands");
        wire("glass instances", "draw blended", "drawCount", "drawCount");
        set("draw blended", "push.culled", 0);

        // ---- occlusion: what is in a corner is darker ---------------------------------------------------------------------
        // How occluded each pixel is, worked out of where its surface and its neighbours' are; that blurred, keeping
        // to each surface; and the picture multiplied by it.
        page("occlusion");
        {
            const std::string screen = bring("screen"), camera = bring("camera data"), position = bring("position"), normal = bring("normal");
            const std::string nearest = bring("nearest"), linear = bring("linear");
            image("occlusion raw", "eR8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 40, screen);
            image("occlusion", "eR8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 180, screen);
            compute("occlusion pass", "ambientOcclusion", "occlusion", "occlusionOut", 760, 40);
            wire(camera, "occlusion pass", "param.camera");
            wire(position, "occlusion pass", "param.positionTex");
            wire(nearest, "occlusion pass", "param.positionTex.sampler");
            wire(normal, "occlusion pass", "param.normalTex");
            wire(nearest, "occlusion pass", "param.normalTex.sampler");
            wire("occlusion raw", "occlusion pass", "param.occlusionOut");
            set("run occlusion pass", "push.radius", 1.0);
            set("run occlusion pass", "push.intensity", 1.5);
            set("run occlusion pass", "push.bias", 0.1);
            compute("blur occlusion", "ambientOcclusion", "blur", "occlusionOut", 760, 240);
            wire(camera, "blur occlusion", "param.camera");
            wire(position, "blur occlusion", "param.positionTex");
            wire(nearest, "blur occlusion", "param.positionTex.sampler");
            wire(normal, "blur occlusion", "param.normalTex");
            wire(nearest, "blur occlusion", "param.normalTex.sampler");
            wire("occlusion raw", "blur occlusion", "param.occlusionIn");
            wire(linear, "blur occlusion", "param.occlusionIn.sampler");
            wire("occlusion", "blur occlusion", "param.occlusionOut");
            compute("apply occlusion", "ambientOcclusion", "compose", "hdrImage", 760, 440);
            wire("occlusion", "apply occlusion", "param.occlusionIn");
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
        wire(bring("depth"), "sky", "param.depthTex");
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
            wire(bring("hdr"), "trace reflections", "param.hdr");
            wire(bring("linear"), "trace reflections", "param.hdr.sampler");
            for (const char* name : { "position", "normal", "specular" }) {
                wire(bring(std::string(name)), "trace reflections", std::format("param.{}Tex", name));
                wire(nearest, "trace reflections", std::format("param.{}Tex.sampler", name));
            }
            wire("reflection", "trace reflections", "param.reflection");
            set("run trace reflections", "push.maxDistance", 20);
            set("run trace reflections", "push.thickness", 0.3);
            set("run trace reflections", "push.maxRoughness", 0.5);
            set("run trace reflections", "push.intensity", 1.0);
            compute("add reflections", "reflections", "compose", "hdrImage", 760, 300);
            wire("reflection", "add reflections", "param.traced");
            wire(nearest, "add reflections", "param.traced.sampler");
            wire(hdr, "add reflections", "param.hdrImage");
        }

        // ---- antialias: this frame blended into the ones before it -----------------------------------------------------
        // The picture was moved by a fraction of a pixel, another each frame: each pixel is blended into what the frames
        // before came to where it was last frame, that clamped to the colours round it now, so that what has moved
        // leaves no trail. Then the picture is that, and so is the history the next frame blends into.
        page("antialias");
        {
            const std::string screen = bring("screen");
            image("antialiased", "eRGBA16_SFLOAT", { "eStorage", "eSampled", "eTransferSrc" }, 320, 40, screen);
            image("history", "eRGBA16_SFLOAT", { "eStorage", "eSampled", "eTransferSrc" }, 320, 260, screen);
            compute("resolve", "taa", "resolve", "resolved", 760, 40);
            wire(bring("camera data"), "resolve", "param.camera");
            wire(bring("hdr"), "resolve", "param.current");
            wire(bring("position"), "resolve", "param.positionTex");
            wire("history", "resolve", "param.history");
            wire(bring("linear"), "resolve", "param.linearSampler");
            wire(bring("frame state"), "resolve", "param.frameState");
            wire("antialiased", "resolve", "param.resolved");
            set("run resolve", "push.blend", 0.1);
            compute("keep", "taa", "keep", "hdrImage", 760, 260);
            wire("antialiased", "keep", "param.resolvedTex");
            wire(bring("hdr"), "keep", "param.hdrImage");
            wire("history", "keep", "param.historyImage");
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
            set("level above", "first", bring("hdr"));
            compute("down", "koral_bloom", "downsample", "target", 1080, 180);
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
            compute("up", "koral_bloom", "upsample", "target", 1080, 760);
            wire("level below", "up", "param.source");
            wire(linear, "up", "param.source.sampler");
            wire("as it came down", "up", "param.detail");
            wire(linear, "up", "param.detail.sampler");
            wire("up level", "up", "param.target");

            node("Element", "bloom", 1640, 620);
            set("bloom", "of", "up level");
            // Each level of the way up added its own light, so the top holds about as many times it as there are levels.
            compute("add bloom", "koral_bloom", "composite", "hdrImage", 1640, 180);
            wire("bloom", "add bloom", "param.source");
            wire(linear, "add bloom", "param.source.sampler");
            wire(hdr, "add bloom", "param.hdrImage");
            set("run add bloom", "push.strength", 0.5 / BloomLevels);
        }

        // ---- show: the picture as light, made the picture as the display shows it -----------------------------------
        page("show");
        const std::string showScreen = bring("screen");
        image("ldr", "eRGBA8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 40, showScreen);
        compute("tone map", "koral_tonemap", "computeMain", "ldrImage", 760, 40);
        wire(bring("hdr"), "tone map", "param.hdrImage");
        wire("ldr", "tone map", "param.ldrImage");
        set("run tone map", "push.exposure", 1.0);
        // To look at instead, in the editor's scene view: how many lights each pixel's cluster holds. Its pass runs only
        // while the view shows it, and never in the game.
        image("cluster heat", "eRGBA8_UNORM", { "eStorage", "eSampled", "eTransferSrc" }, 320, 560, showScreen);
        node("DebugView", "debug view", 320, 420);
        set("debug view", "name", "Cluster heat");
        wire("cluster heat", "debug view", "picture");
        compute("heat", "clusterHeat", "computeMain", "heat", 760, 420);
        wire(bring("camera data"), "heat", "param.camera");
        wire(bring("depth"), "heat", "param.depthTex");
        wire(bring("nearest"), "heat", "param.depthTex.sampler");
        wire("ldr", "heat", "param.picture");
        wire(bring("nearest"), "heat", "param.picture.sampler");
        wire(bring("light grid"), "heat", "param.lightGrid");
        wire("cluster heat", "heat", "param.heat");
        set("run heat", "push.most", 24.0);
        wire("debug view", "run heat", "enabled", "shown");
        node("Blit", "show", 1320, 420);
        wire("ldr", "show", "source");
        wire(showScreen, "show", "destination");
        set("show", "filter", "eLinear");

        if (!ok && error.empty()) error = "a node of the graph could not be set up";
        return ok && AddLampShadows(doc, error) && AddBranches(doc, error);
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
        // And of those whose turn it is, only a face something in sight of has moved in, or whose light has.
        set("cast lamp", "whenChanged", true);
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
        const std::string maps = bring("lamp shadow maps", shading, 1500), data = bring("lamp shadow data", shading, 1610);
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
    /**
     * How much light a lamp's glowing part gives off — its material's `emission` — for it to light what is round it as
     * much as its point light does: its light's colour, made as bright as a ball of @p radius has to be to give as much
     * a third of its light's @p range away. What the ray tracer lights the scene by, in place of the point light.
     */
    inline Value LampEmission(const Value& color, const double intensity, const double range, const double radius)
    {
        const double away = range / 3.0, falloff = (1.0 - 1.0 / 9.0) * (1.0 - 1.0 / 9.0);
        const double bright = intensity * falloff * away * away / (3.14159265358979 * radius * radius);
        Value::List given;
        for (const auto& channel : *color.Items()) given.push_back(Value(std::pow(std::max(*channel.Number(), 0.0), 2.2) * bright));
        return Value(std::move(given));
    }

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
            // A ball of the sphere's half a unit across, made 0.16 of that.
            doc.SetField(lamp, "forward", "emission", LampEmission(color, 3.0, 3.5, 0.08));
            doc.AddComponent(lamp, "PointLight");
            doc.SetField(lamp, "PointLight", "color", color);
            doc.SetField(lamp, "PointLight", "intensity", Value(3.0));
            doc.SetField(lamp, "PointLight", "range", Value(3.5));
        }
        GiveRayMaterials(doc);
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
                for (const char* map : { "albedoMap", "normalMap", "roughnessMap", "emissiveMap" }) doc.SetField(lamp, "forward", map, Value("$white"));
                doc.SetField(lamp, "forward", "albedo", vec(0, 0, 0));
                doc.SetField(lamp, "forward", "emission", LampEmission(color, 3.0, 3.5, 0.05));
            }
            doc.AddComponent(lamp, "PointLight");
            doc.SetField(lamp, "PointLight", "color", color);
            doc.SetField(lamp, "PointLight", "intensity", Value(3.0));
            doc.SetField(lamp, "PointLight", "range", Value(3.5));
        }
        return true;
    }

    /// How many tasks of up to 32 meshlets the meshlet branch has room for in a frame.
    inline constexpr int MostMeshletTasks = 262144;
    /// The most things that give off light the ray tracer lists: the `emitters` buffer's room.
    inline constexpr int MostEmitters = 4096;
    /// What `renderer` is: which of the three ways the scene is drawn.
    inline constexpr const char* Renderers[] { "vertex", "meshlets", "rays" };

    /**
     * The ray tracer's material — its component, `ray tracer` — given to every entity the forward pass draws that has
     * none yet: each field the two have of the same name as the forward pass's is. A model imported after the graph is
     * made has both from the importer; this is for entities made before it, or by hand.
     */
    inline void GiveRayMaterials(keditor::Document& doc)
    {
        const kecs::World& world = doc.World();
        const kecs::ComponentId rays = world.Find("ray tracer"), forward = world.Find("forward");
        if (rays == kecs::NoComponent || forward == kecs::NoComponent) return;
        std::vector<std::string> shared;
        for (const auto& field : world.Describe(rays).fields)
            if (world.Describe(forward).FindField(field.name)) shared.push_back(field.name);
        for (const kecs::Entity entity : doc.Entities()) {
            const auto has = doc.ComponentsOf(entity);
            if (!std::ranges::contains(has, std::string("forward")) || std::ranges::contains(has, std::string("ray tracer"))) continue;
            doc.AddComponent(entity, "ray tracer");
            for (const auto& field : shared) doc.SetField(entity, "ray tracer", field, doc.Field(entity, "forward", field));
        }
    }

    /**
     * The ray tracer given a material of its own, in a graph whose ray tracer read the forward pass's (a project made
     * before): `trace rays` wired from the entities it traces, and every entity given `ray tracer` as the forward pass's
     * material is. True, and nothing done, when it has one.
     */
    inline bool AddRayMaterial(keditor::Document& doc, std::string& error)
    {
        const kgraph::Node* trace = doc.Graph().Find("trace rays");
        if (!trace) { error = "there is no 'trace rays' to give a material"; return false; }
        if (const auto wired = trace->inputs.find("entities"); wired != trace->inputs.end() && wired->second.link) { GiveRayMaterials(doc); return true; }
        doc.ClearInput("ray tracer", "param.instances");
        const kgraph::Node* drawn = doc.Graph().Find("drawn");
        if (!drawn) { error = "there is no 'drawn' Filter to trace"; return false; }
        const std::string made = doc.Import("drawn", doc.Graph().Find("trace rays")->pass, 40, 1400);
        if (made.empty() || !doc.Connect(made, "out", "trace rays", "entities")) { error = "'trace rays' could not be wired from 'drawn'"; return false; }
        GiveRayMaterials(doc);
        return true;
    }

    /**
     * The ray tracer lit by what gives off light — what has an emissive material — and not by the point lights: what
     * emits listed each frame from its materials (emitters.slang, after the rays are traced: the next frame's are lit by
     * it), and the ray tracer given the list, the point lights' buffers taken off it. Added to a graph that has the rays
     * and not these yet (a project made before them); true, and nothing done, when it has them.
     */
    inline bool AddEmitters(keditor::Document& doc, std::string& error)
    {
        if (const kgraph::Node* gather = doc.Graph().Find("gather emitters")) {
            // One made before it measured each emitter's reach from its corners: given the pack's, as now.
            if (gather->inputs.contains("param.meshIndices")) return true;
            const std::string pass = gather->pass;
            const std::string buffers = doc.Import("pack buffers", pass, 40, 2100);
            if (buffers.empty() || !doc.Connect(buffers, "indices", "gather emitters", "param.meshIndices") || !doc.Connect(buffers, "vertices", "gather emitters", "param.meshPositions")) {
                error = "'gather emitters' could not be given the pack's corners";
                return false;
            }
            return true;
        }
        if (!doc.Graph().Find("trace rays")) { error = "there is no 'trace rays' to light by what gives off light"; return false; }
        bool ok = true;
        const std::string pass = doc.Graph().Find("trace rays")->pass;
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
        double broughtAt = 0;
        const auto bring = [&](const std::string& id) {
            const std::string made = doc.Import(id, pass, 40, 1700 + (broughtAt += 110));
            if (made.empty()) failed(std::format("there is no node '{}' to bring in", id));
            return made;
        };
        // The point lights are the raster passes': the rays have none of them.
        for (const char* gone : { "param.lights", "param.lightGrid", "param.lightIndices" }) doc.ClearInput("ray tracer", gone);

        node("Buffer", "emitters", 320, 1700);
        set("emitters", "instanceCount", MostEmitters * 32);
        set("emitters", "usage", Value(Value::List { "eStorage", "eTransferSrc" }));
        node("Buffer", "emitter count", 320, 1840);
        set("emitter count", "instanceCount", 16);
        set("emitter count", "usage", Value(Value::List { "eStorage", "eTransferDst", "eTransferSrc" }));
        // After the rays: counted from none, and listed.
        node("ClearBuffer", "reset emitters", 600, 1840);
        wire("emitter count", "reset emitters", "buffer");
        node("ComputePipeline", "gather emitters", 600, 1700);
        set("gather emitters", "compute.module", "emitters");
        set("gather emitters", "compute.entry", "gather");
        wire("trace rays", "gather emitters", "param.instances", "instances");
        wire(bring("ray scene"), "gather emitters", "param.meshOf", "meshOf");
        wire(bring("drawn meshes"), "gather emitters", "param.ranges", "rangeBuffer");
        const std::string buffers = bring("pack buffers");
        wire(buffers, "gather emitters", "param.meshIndices", "indices");
        wire(buffers, "gather emitters", "param.meshPositions", "vertices");
        wire("emitters", "gather emitters", "param.emitters");
        wire("emitter count", "gather emitters", "param.emitterCount");
        node("Dispatch", "run gather emitters", 880, 1700);
        wire("gather emitters", "run gather emitters", "pipeline");
        set("run gather emitters", "groups.x", MostInstances / 64);
        for (const char* axis : { "groups.y", "groups.z" }) set("run gather emitters", axis, 1);
        // What the rays are lit by.
        wire("emitters", "ray tracer", "param.emitters");
        wire("emitter count", "ray tracer", "param.emitterCount");
        set("trace rays", "push.emitterCandidates", 32);
        set("trace rays", "push.emitterSamples", 2);

        IdleUnchosen(doc);
        if (!ok && error.empty()) error = "the emitters could not be set up";
        return ok;
    }

    /// The depth pyramid the meshlets are culled by: its first level, whatever the picture's size, and how many it has
    /// (forward.slang's PyramidSize and PyramidLevels).
    inline constexpr int PyramidWidth = 1024, PyramidHeight = 512, PyramidLevels = 10;

    /**
     * The meshlets culled by what hides them, as well as by the camera's sight: drawn twice a frame. The early draw
     * draws what last frame's depth pyramid shows and notes which; the pyramid is made again of what it drew; the late
     * draw draws, over it, what the early one left out that this frame's pyramid shows, and the glass is tested against
     * it too. All on the meshlet forward page, the late draw and the pyramid before the glass in the node list: the
     * three draw into one picture, in that order. Added to a graph that has the meshlets and not this yet (a project
     * made before it); true, and nothing done, when it has it.
     */
    inline bool AddMeshletOcclusion(keditor::Document& doc, std::string& error)
    {
        if (doc.Graph().Find("depth pyramid")) return true;      // it has it already
        if (!doc.Graph().Find("draw meshlets") || !doc.Graph().Find("meshlet blended")) { error = "there are no meshlets, or no meshlet glass, to cull"; return false; }
        bool ok = true;
        const std::string pass = "meshlet forward";
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
        std::vector<std::string> before;
        for (const auto& each : doc.Graph().nodes) before.push_back(each.id);

        // Which meshlets of each task the early draw drew, and the pyramid it is made into.
        node("Buffer", "meshlets drawn early", 320, 2000);
        set("meshlets drawn early", "instanceCount", MostMeshletTasks * 4);
        set("meshlets drawn early", "usage", Value(Value::List { "eStorage", "eTransferSrc" }));
        node("Image", "depth pyramid", 320, 2140);
        set("depth pyramid", "format", "eR32_SFLOAT");
        set("depth pyramid", "extent.x", PyramidWidth);
        set("depth pyramid", "extent.y", PyramidHeight);
        set("depth pyramid", "mipLevels", PyramidLevels);
        set("depth pyramid", "usage", Value(Value::List { "eStorage", "eSampled", "eTransferSrc" }));
        node("ImageView", "meshlet depth view", 320, 2280);
        wire("meshlet depth", "meshlet depth view", "image");

        // The early draw: by last frame's pyramid, noting what it drew.
        set("meshlet forward", "task.entry", "meshletTaskEarly");
        wire("depth pyramid", "meshlet forward", "param.depthPyramid");
        wire("meshlets drawn early", "meshlet forward", "param.meshletsDrawnEarly");

        // The pyramid made again, a level a turn, each of the one before: the first of the early draw's depth.
        node("Repeat", "pyramid levels", 600, 2000);
        set("pyramid levels", "count", PyramidLevels);
        node("ImageView", "pyramid level", 600, 2140);
        wire("depth pyramid", "pyramid level", "image");
        wire("pyramid levels", "pyramid level", "baseMipLevel", "index");
        set("pyramid level", "mipLevelCount", 1);
        node("Previous", "pyramid level below", 600, 2280);
        set("pyramid level below", "of", "pyramid level");
        set("pyramid level below", "first", "meshlet depth view");
        node("ComputePipeline", "reduce depth", 880, 2140);
        set("reduce depth", "compute.module", "meshletTasks");
        set("reduce depth", "compute.entry", "reduce");
        wire("pyramid level below", "reduce depth", "param.finer");
        wire("pyramid level", "reduce depth", "param.coarser");
        node("Dispatch", "run reduce depth", 1140, 2140);
        wire("reduce depth", "run reduce depth", "pipeline");
        set("run reduce depth", "cover", "coarser");

        // The late draw: what the early one left out, by this frame's pyramid, over its picture. Its pipeline is the
        // early one's, wired as it is (a copy of its inputs: wiring makes the graph anew), but for its task shader.
        node("GraphicsPipeline", "meshlet late", 1040, 2420);
        const auto early = doc.Graph().Find("meshlet forward")->inputs;
        const std::string taskCount = doc.Graph().Find("draw meshlets")->inputs.at("indirect").link->node;
        for (const auto& [input, from] : early) {
            if (input == "task.entry") continue;
            if (from.link) wire(from.link->node, "meshlet late", input, from.link->output);
            else set("meshlet late", input, from.value);
        }
        set("meshlet late", "task.entry", "meshletTaskLate");
        node("Draw", "draw late meshlets", 1320, 2420);
        wire("meshlet late", "draw late meshlets", "pipeline");
        set("draw late meshlets", "load", true);
        set("draw late meshlets", "command", "eDrawMeshTasksIndirect");
        wire(taskCount, "draw late meshlets", "indirect");
        set("draw late meshlets", "drawCount", 1);
        set("draw late meshlets", "stride", 12);

        // The glass, by this frame's pyramid. (Its task shader is the others', which name what the early draw drew: given
        // it, and not reading it.)
        set("meshlet blended", "task.entry", "meshletTaskBehind");
        wire("depth pyramid", "meshlet blended", "param.depthPyramid");
        wire("meshlets drawn early", "meshlet blended", "param.meshletsDrawnEarly");
        if (!ok) return false;

        // What was added goes before the glass in the node list: the late draw and the pyramid draw into the picture
        // and its depth after the early draw, and before the glass is laid over them.
        kgraph::Graph graph = doc.Graph();
        std::vector<kgraph::Node> added;
        std::erase_if(graph.nodes, [&](kgraph::Node& each) {
            if (std::ranges::contains(before, each.id)) return false;
            added.push_back(std::move(each));
            return true;
        });
        const auto glass = std::ranges::find_if(graph.nodes, [](const kgraph::Node& each) { return each.id == "meshlet blended" || each.id == "draw meshlet blended"; });
        graph.nodes.insert(glass, std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
        // Idle, with the rest of the page, while the meshlets are not chosen (IdleUnchosen's doing for the passes it had).
        const std::string notChosen = "not meshlets (in meshlet forward)";
        if (!graph.Find(notChosen)) {
            kgraph::Node& reference = graph.Add(notChosen, "Reference").Set("node", "not meshlets");
            reference.pass = pass;
            reference.x = 40;
            reference.y = -120;
        }
        for (const char* id : { "run reduce depth", "draw late meshlets" }) graph.Find(id)->Wire("idle", notChosen);
        doc.SetGraph(std::move(graph));
        return true;
    }

    /**
     * The branches not chosen idle: their passes are there, built, and do nothing. Each page of a branch is given what
     * says it is not chosen (a Reference to it), and each of its passes wired from that — its `idle`, or, for one that
     * cannot idle, its `enabled` (none of those is read by another). A pass wired so already is left as it is: run again
     * after passes were added, only those are wired. One edit for them all.
     */
    inline void IdleUnchosen(keditor::Document& doc)
    {
        kgraph::Graph graph = doc.Graph();
        const auto idleWith = [&](const std::string& page, const std::string& condition) {
            std::vector<std::string> ids;
            for (const auto& each : graph.nodes)
                if (each.pass == page)
                    if (const kgraph::NodeType* type = kgraph::FindType(each.type); type && type->category == "Passes") ids.push_back(each.id);
            if (ids.empty()) return;
            const std::string why = std::format("{} (in {})", condition, page);
            if (!graph.Find(why)) {
                kgraph::Node& reference = graph.Add(why, "Reference").Set("node", condition);
                reference.pass = page;
                reference.x = 40;
                reference.y = -120;
            }
            const auto from = [&](const kgraph::Node& node, const char* input) {
                const auto it = node.inputs.find(input);
                return it != node.inputs.end() && it->second.link ? it->second.link->node : std::string();
            };
            for (const auto& id : ids) {
                const kgraph::Node& pass = *graph.Find(id);
                if (from(pass, "enabled") == why || from(pass, "idle") == why || from(pass, "idle") == id + " or not chosen") continue;
                const kgraph::NodeType* type = kgraph::FindType(pass.type);
                const bool idles = std::ranges::any_of(type->inputs, [](const kgraph::Port& port) { return port.name == "idle"; });
                if (!idles) { graph.Find(id)->Wire("enabled", why); continue; }
                const auto had = pass.inputs.find("idle");
                if (had != pass.inputs.end() && had->second.link) {
                    // Idle already for a reason of its own (a shadow drawn only when it changed): idle for either.
                    const kgraph::Link already = *had->second.link;
                    const double x = pass.x, y = pass.y;
                    kgraph::Node& either = graph.Add(id + " or not chosen", "Max").Wire("a", already.node, already.output).Wire("b", why);
                    either.pass = page;
                    either.x = x;
                    either.y = y + 120;
                    graph.Find(id)->Wire("idle", either.id);
                } else {
                    graph.Find(id)->Wire("idle", why);
                }
            }
        };
        for (const char* page : { "prepass", "culling", "forward", "occlusion", "sky", "reflections" }) idleWith(page, "not vertex");
        for (const char* page : { "meshlet tasks", "meshlet forward", "meshlet occlusion", "meshlet sky", "meshlet reflections" }) idleWith(page, "not meshlets");
        for (const char* page : { "ray scene", "ray trace" }) idleWith(page, "not rays");
        // Rays find their own shadows, and are lit by what gives off light, not by the point lights: the shadow maps and the
        // lights' clusters are for the other two, and rest while rays are traced.
        for (const char* page : { "shadows", "lamp shadows", "lights" }) idleWith(page, "tracing rays");
        doc.SetGraph(std::move(graph));
    }

    /**
     * Glass and decals — what has the tag Blended, the vertex branch's `glass meshes` — drawn by the other two branches
     * as well. The meshlets': their pack cut into meshlets too, its instances in sight listed as tasks as the solid
     * ones' are, and drawn by the same task and mesh shaders over the solid picture by how opaque each is, writing no
     * depth. The rays': a RayScene of their own, which each pixel's ray looks through up to the solid surface it hits —
     * every layer of glass on the way shaded, and the nearest laid over the rest. Added to a graph that has the branches
     * and not these yet (a project made before them); true, and nothing done, when it has them.
     */
    inline bool AddBlendedToBranches(keditor::Document& doc, std::string& error)
    {
        if (doc.Graph().Find("glass meshlets")) return true;       // it has them already
        if (!doc.Graph().Find("renderer") || !doc.Graph().Find("glass meshes")) { error = "there are no branches, or no glass, to draw it in"; return false; }
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
        // Brought in below what the page has: a page that had nodes keeps them where they were.
        double broughtAt = 0;
        const auto bring = [&](const std::string& id) {
            const std::string made = doc.Import(id, pass, 40, 1400 + (broughtAt += 110));
            if (made.empty()) failed(std::format("there is no node '{}' to bring in", id));
            return made;
        };
        const auto onPage = [&](const std::string& name) { pass = name; broughtAt = 0; };
        const auto buffer = [&](const std::string& id, const int bytes, Value::List usage, const double x, const double y) {
            node("Buffer", id, x, y);
            set(id, "instanceCount", bytes);
            set(id, "usage", Value(std::move(usage)));
        };
        const auto compute = [&](const std::string& id, const char* module, const char* entry, const double x, const double y) {
            node("ComputePipeline", id, x, y);
            set(id, "compute.module", module);
            set(id, "compute.entry", entry);
            node("Dispatch", "run " + id, x + 260, y);
            wire(id, "run " + id, "pipeline");
        };
        // What a pipeline is wired with, as a copy: wiring makes the graph anew.
        const auto inputsOf = [&](const std::string& id) { return doc.Graph().Find(id)->inputs; };

        // ---- meshlets: the glass's pack cut, its instances in sight listed as tasks ---------------------------------------
        onPage("meshlet tasks");
        {
            const std::string glass = bring("glass meshes"), instances = bring("glass instances");
            node("Meshlets", "glass meshlets", 320, 1400);
            wire(glass, "glass meshlets", "mesh", "mesh");
            wire(glass, "glass meshlets", "ranges", "ranges");
            node("MeshBuffers", "glass buffers", 320, 1660);
            wire(glass, "glass buffers", "mesh", "mesh");
            buffer("glass task list", MostMeshletTasks * 4, { "eStorage", "eTransferSrc" }, 600, 1660);
            buffer("glass task count", 16, { "eStorage", "eIndirect", "eTransferDst", "eTransferSrc" }, 600, 1800);
            node("ClearBuffer", "reset glass tasks", 880, 1800);
            wire("glass task count", "reset glass tasks", "buffer");
            compute("expand glass meshlets", "meshletTasks", "expand", 880, 1400);
            wire(bring("draw blended"), "expand glass meshlets", "param.instances", "instances");
            wire(glass, "expand glass meshlets", "param.ranges", "rangeBuffer");
            wire(instances, "expand glass meshlets", "param.meshCommands", "commands");
            wire("glass meshlets", "expand glass meshlets", "param.meshRanges", "meshRanges");
            wire("glass task list", "expand glass meshlets", "param.tasks");
            wire("glass task count", "expand glass meshlets", "param.taskArgs");
            wire(bring("frame state"), "expand glass meshlets", "param.frameState");
            set("run expand glass meshlets", "groups.x", MostInstances / 64);
            for (const char* axis : { "groups.y", "groups.z" }) set("run expand glass meshlets", axis, 1);
            wire(instances, "run expand glass meshlets", "push.commandCount", "drawCount");
            compute("bound glass tasks", "meshletTasks", "bound", 880, 1580);
            wire("glass task list", "bound glass tasks", "param.tasks");
            wire("glass task count", "bound glass tasks", "param.taskArgs");
            for (const char* axis : { "groups.x", "groups.y", "groups.z" }) set("run bound glass tasks", axis, 1);
        }
        // ---- meshlets: drawn over the solid picture, as the vertex branch's blended pipeline draws them ------------------
        onPage("meshlet forward");
        {
            node("GraphicsPipeline", "meshlet blended", 1040, 1400);
            set("meshlet blended", "stages", "eMesh");
            for (const char* stage : { "task", "mesh", "fragment" }) set("meshlet blended", std::format("{}.module", stage), "forward");
            set("meshlet blended", "task.entry", "meshletTask");
            set("meshlet blended", "mesh.entry", "meshletMesh");
            set("meshlet blended", "fragment.entry", "fragment");
            // Lit as the solid meshlets are; what says which instances and meshlets, the glass's.
            const std::map<std::string, std::pair<std::string, std::string>> glass {
                { "param.instances", { bring("draw blended"), "instances" } },
                { "param.meshlets", { bring("glass meshlets"), "meshlets" } },
                { "param.meshletVertices", { bring("glass meshlets"), "vertices" } },
                { "param.meshletTriangles", { bring("glass meshlets"), "triangles" } },
                { "param.meshletTasks", { bring("glass task list"), "out" } },
                { "param.meshPositions", { bring("glass buffers"), "vertices" } },
                { "param.meshNormals", { bring("glass buffers"), "normals" } },
                { "param.meshUVs", { bring("glass buffers"), "uvs" } } };
            for (const auto& [input, from] : inputsOf("meshlet forward")) {
                if (!from.link || !(input.starts_with("param.") || input == "framebuffer")) continue;
                if (const auto swapped = glass.find(input); swapped != glass.end()) wire(swapped->second.first, "meshlet blended", input, swapped->second.second);
                else wire(from.link->node, "meshlet blended", input, from.link->output);
            }
            // Blended as the vertex branch's are: over the picture, the three beside it left saying what is behind.
            const auto blended = inputsOf("forward blended");
            for (const char* state : { "depthStencilState", "colorBlendState" })
                if (const auto it = blended.find(state); it != blended.end()) set("meshlet blended", state, it->second.value);
            node("Draw", "draw meshlet blended", 1320, 1400);
            wire("meshlet blended", "draw meshlet blended", "pipeline");
            set("draw meshlet blended", "load", true);
            set("draw meshlet blended", "command", "eDrawMeshTasksIndirect");
            wire(bring("glass task count"), "draw meshlet blended", "indirect");
            set("draw meshlet blended", "drawCount", 1);
            set("draw meshlet blended", "stride", 12);
        }

        // ---- rays: the glass a scene of its own, looked through ----------------------------------------------------------
        onPage("ray scene");
        {
            const std::string glass = bring("glass"), meshes = bring("glass meshes");
            node("RayScene", "glass ray scene", 320, 1400);
            wire(glass, "glass ray scene", "meshes", "Mesh.mesh");
            wire(meshes, "glass ray scene", "mesh", "mesh");
            wire(meshes, "glass ray scene", "ranges", "ranges");
        }
        onPage("ray trace");
        {
            const std::string scene = bring("glass ray scene"), meshes = bring("glass meshes"), buffers = bring("glass buffers");
            wire(bring("draw blended"), "ray tracer", "param.glassInstances", "instances");
            wire(scene, "ray tracer", "param.glassMeshOf", "meshOf");
            wire(scene, "ray tracer", "param.glassScene", "scene");
            wire(meshes, "ray tracer", "param.glassRanges", "rangeBuffer");
            wire(buffers, "ray tracer", "param.glassIndices", "indices");
            wire(buffers, "ray tracer", "param.glassPositions", "vertices");
            wire(buffers, "ray tracer", "param.glassNormals", "normals");
            wire(buffers, "ray tracer", "param.glassUVs", "uvs");
            set("trace rays", "push.glassLayers", 4);
        }

        IdleUnchosen(doc);
        if (!ok && error.empty()) error = "a node of the blended branches could not be set up";
        return ok;
    }

    /**
     * Adds to a graph BuildGraph made — or a project's made from it, of the same names — two more ways of drawing the
     * scene beside the first, each a branch of pages that ends in a lit picture, and a Switch of the three before the
     * pages that make the picture shown (antialias, bloom, show):
     *
     *   vertex    what BuildGraph made: a depth pre-pass, an occlusion cull, a forward pass by vertex shaders over the
     *             pack, then occlusion, the sky and reflections in screen space
     *   meshlets  the same pack cut into meshlets, drawn by task and mesh shaders — what is in sight of the camera found
     *             an instance and then a meshlet at a time — and the same occlusion, sky and reflections after it
     *   rays      the entities as a scene rays are traced through: shaded where a ray a pixel hits, its shadows, its
     *             occlusion and what it mirrors each found by more rays
     *
     * `renderer` (on the frame page: 0, 1 or 2) says which: the Switches take its picture, its depth and where its
     * surfaces are on to the rest, and the passes of the other two branches idle — as do the shadow maps and the lights' clusters while rays
     * are traced, which find their own shadows and are lit by what gives off light. False, with @p error, when a node could not be added, set or wired.
     */
    inline bool AddBranches(keditor::Document& doc, std::string& error)
    {
        if (doc.Graph().Find("renderer")) return true;      // it has them already
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
        double broughtAt = 0;
        const auto bring = [&](const std::string& id) {
            const std::string made = doc.Import(id, pass, 40, 40 + (broughtAt += 110));
            if (made.empty()) failed(std::format("there is no node '{}' to bring in", id));
            return made;
        };
        // A page of a branch, before the pages that make the picture shown.
        const auto page = [&](const std::string& name) {
            pass = name;
            broughtAt = 0;
            const auto& passes = doc.Passes();
            const auto antialias = std::ranges::find(passes, std::string("antialias"));
            doc.AddPass(pass, antialias == passes.end() ? -1 : static_cast<int>(antialias - passes.begin()));
        };
        const auto image = [&](const std::string& id, const char* format, Value::List usage, const double x, const double y, const std::string& sizedBy) {
            node("Image", id, x, y);
            set(id, "format", format);
            set(id, "usage", Value(std::move(usage)));
            doc.ClearInput(id, "extent.x");
            doc.ClearInput(id, "extent.y");
            wire(sizedBy, id, "extent", "extent");
        };
        const auto buffer = [&](const std::string& id, const int bytes, Value::List usage, const double x, const double y) {
            node("Buffer", id, x, y);
            set(id, "instanceCount", bytes);
            set(id, "usage", Value(std::move(usage)));
        };
        const auto compute = [&](const std::string& id, const char* module, const char* entry, const double x, const double y) {
            node("ComputePipeline", id, x, y);
            set(id, "compute.module", module);
            set(id, "compute.entry", entry);
            node("Dispatch", "run " + id, x + 260, y);
            wire(id, "run " + id, "pipeline");
        };
        const Value clearToNothing = Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0, 0, 0, 0 } } } } };

        // ---- which: a number, and whether each branch is not the one it says -------------------------------------------
        pass = "frame";
        node("Int", "renderer", 40, 1900);
        set("renderer", "value", 0);
        for (int branch = 0; branch < 3; ++branch) {
            const std::string id = std::format("not {}", Renderers[branch]);
            node("NotEqual", id, 220, 1900 + branch * 60);
            wire("renderer", id, "a");
            set(id, "b", branch);
        }
        node("Equal", "tracing rays", 220, 2080);
        wire("renderer", "tracing rays", "a");
        set("tracing rays", "b", 2);

        // ---- meshlets: the pack cut into meshlets, what is in sight of them listed, drawn by task and mesh shaders ------
        page("meshlet tasks");
        {
            const std::string drawn = bring("drawn"), meshes = bring("drawn meshes");
            node("Meshlets", "meshlets", 320, 40);
            wire(meshes, "meshlets", "mesh", "mesh");
            wire(meshes, "meshlets", "ranges", "ranges");
            node("MeshBuffers", "pack buffers", 320, 300);
            wire(meshes, "pack buffers", "mesh", "mesh");
            // A command a mesh, each with its instances' first and count: what tells which mesh an instance is.
            node("IndirectDraws", "mesh of instance", 320, 520);
            wire(meshes, "mesh of instance", "ranges", "ranges");
            wire(drawn, "mesh of instance", "groups", "groups");
            buffer("meshlet task list", MostMeshletTasks * 16, { "eStorage", "eTransferSrc" }, 600, 300);
            buffer("meshlet task count", 16, { "eStorage", "eIndirect", "eTransferDst", "eTransferSrc" }, 600, 440);
            node("ClearBuffer", "reset meshlet tasks", 880, 440);
            wire("meshlet task count", "reset meshlet tasks", "buffer");
            compute("expand meshlets", "meshletTasks", "expand", 880, 40);
            wire(bring("draw forward"), "expand meshlets", "param.instances", "instances");
            wire(meshes, "expand meshlets", "param.ranges", "rangeBuffer");
            wire("mesh of instance", "expand meshlets", "param.meshCommands", "commands");
            wire("meshlets", "expand meshlets", "param.meshRanges", "meshRanges");
            wire("meshlet task list", "expand meshlets", "param.tasks");
            wire("meshlet task count", "expand meshlets", "param.taskArgs");
            wire(bring("frame state"), "expand meshlets", "param.frameState");
            set("run expand meshlets", "groups.x", MostInstances / 64);
            for (const char* axis : { "groups.y", "groups.z" }) set("run expand meshlets", axis, 1);
            wire("mesh of instance", "run expand meshlets", "push.commandCount", "drawCount");
            compute("bound meshlet tasks", "meshletTasks", "bound", 880, 220);
            wire("meshlet task list", "bound meshlet tasks", "param.tasks");
            wire("meshlet task count", "bound meshlet tasks", "param.taskArgs");
            for (const char* axis : { "groups.x", "groups.y", "groups.z" }) set("run bound meshlet tasks", axis, 1);
        }
        page("meshlet forward");
        {
            const std::string screen = bring("screen");
            const Value::List target { "eColorAttachment", "eSampled", "eStorage", "eTransferSrc" };
            image("meshlet hdr", "eRGBA16_SFLOAT", target, 320, 40, screen);
            image("meshlet position", "eRGBA32_SFLOAT", target, 320, 180, screen);
            image("meshlet normal", "eRGBA16_SFLOAT", target, 320, 320, screen);
            image("meshlet specular", "eRGBA8_UNORM", target, 320, 460, screen);
            image("meshlet depth", "eD32_SFLOAT", { "eDepthStencilAttachment", "eSampled", "eTransferSrc" }, 320, 600, screen);
            node("Framebuffer", "meshlet target", 760, 40);
            int attachment = 0;
            for (const char* name : { "hdr", "position", "normal", "specular" }) {
                wire(std::format("meshlet {}", name), "meshlet target", std::format("color[{}].view", attachment));
                set("meshlet target", std::format("color[{}].name", attachment), name);
                set("meshlet target", std::format("color[{}].clear", attachment), clearToNothing);
                ++attachment;
            }
            wire("meshlet depth", "meshlet target", "depthStencil.view");
            set("meshlet target", "depthStencil.which", "Depth");
            set("meshlet target", "depthStencil.name", "depth");
            set("meshlet target", "depthStencil.depth", 1);
            node("GraphicsPipeline", "meshlet forward", 1040, 40);
            set("meshlet forward", "stages", "eMesh");
            for (const char* stage : { "task", "mesh", "fragment" }) set("meshlet forward", std::format("{}.module", stage), "forward");
            set("meshlet forward", "task.entry", "meshletTask");
            set("meshlet forward", "mesh.entry", "meshletMesh");
            set("meshlet forward", "fragment.entry", "fragment");
            wire("meshlet target", "meshlet forward", "framebuffer");
            // Lit as the forward pass is: what its pipeline is given that this one's shaders read too.
            std::vector<std::string> takes;
            for (const auto& port : doc.Runtime().PortsOf("meshlet forward")) takes.push_back(port.name);
            const auto wired = doc.Graph().Find("forward")->inputs;     // (a copy: wiring makes the graph anew)
            for (const auto& [input, from] : wired) {
                if (!from.link || !input.starts_with("param.") || std::ranges::find(takes, input) == takes.end()) continue;
                const kgraph::Node* source = doc.Graph().Find(from.link->node);
                // What the forward page brought in, brought in here; what it made itself, brought in from it.
                std::string id = from.link->node;
                if (source && source->type == "Reference") if (const auto it = source->inputs.find("node"); it != source->inputs.end() && it->second.value.String()) id = *it->second.value.String();
                // A Function's call is written out before the graph runs: no Reference can stand for it, so it is wired
                // as the forward pipeline has it, straight.
                const kgraph::Node* origin = doc.Graph().Find(id);
                if (origin && origin->type == "Function") wire(id, "meshlet forward", input, from.link->output);
                else wire(bring(id), "meshlet forward", input, from.link->output);
            }
            const std::string meshlets = bring("meshlets"), buffers = bring("pack buffers");
            wire(bring("draw forward"), "meshlet forward", "param.instances", "instances");
            wire(meshlets, "meshlet forward", "param.meshlets", "meshlets");
            wire(meshlets, "meshlet forward", "param.meshletVertices", "vertices");
            wire(meshlets, "meshlet forward", "param.meshletTriangles", "triangles");
            wire(bring("meshlet task list"), "meshlet forward", "param.meshletTasks");
            wire(buffers, "meshlet forward", "param.meshPositions", "vertices");
            wire(buffers, "meshlet forward", "param.meshNormals", "normals");
            wire(buffers, "meshlet forward", "param.meshUVs", "uvs");
            set("meshlet forward", "depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", true }, { "depthCompareOp", "eLess" } });
            node("Draw", "draw meshlets", 1320, 40);
            wire("meshlet forward", "draw meshlets", "pipeline");
            set("draw meshlets", "command", "eDrawMeshTasksIndirect");
            wire(bring("meshlet task count"), "draw meshlets", "indirect");
            set("draw meshlets", "drawCount", 1);
            set("draw meshlets", "stride", 12);
        }
        // Occlusion, the sky and reflections, as the vertex branch has them, over the meshlet branch's pictures: its
        // pages copied, what they bring in of the forward pass's brought in of this one's instead.
        const std::map<std::string, std::string> meshletImages {
            { "hdr", "meshlet hdr" }, { "position", "meshlet position" }, { "normal", "meshlet normal" }, { "specular", "meshlet specular" }, { "depth", "meshlet depth" } };
        std::map<std::string, std::vector<std::string>> copies;
        for (const char* from : { "occlusion", "sky", "reflections" }) {
            std::vector<kgraph::Node> nodes;
            for (const auto& each : doc.Graph().nodes)
                if (each.pass == from) nodes.push_back(each);
            for (auto& each : nodes) {
                if (each.type == "Reference")
                    if (auto it = each.inputs.find("node"); it != each.inputs.end() && it->second.value.String())
                        if (const auto swapped = meshletImages.find(*it->second.value.String()); swapped != meshletImages.end()) it->second.value = Value(swapped->second);
                for (auto& [name, input] : each.inputs)
                    if (input.link)
                        if (const auto swapped = meshletImages.find(input.link->node); swapped != meshletImages.end()) input.link->node = swapped->second;
            }
            page(std::format("meshlet {}", from));
            const std::string into = pass;
            copies[from] = doc.PasteNodes(std::move(nodes), 40, 40, &into);
            if (copies[from].empty()) failed(std::format("the page '{}' could not be copied", from));
        }

        // ---- rays: the entities as a scene, a ray a pixel traced through it ---------------------------------------------
        page("ray scene");
        {
            const std::string drawn = bring("drawn"), meshes = bring("drawn meshes");
            node("RayScene", "ray scene", 320, 40);
            wire(drawn, "ray scene", "meshes", "Mesh.mesh");
            wire(meshes, "ray scene", "mesh", "mesh");
            wire(meshes, "ray scene", "ranges", "ranges");
            set("ray scene", "masked", "Masked");
            set("ray scene", "noShadow", "PointLight");     // a lamp's own ball casts nothing, as in the shadow maps
        }
        page("ray trace");
        {
            const std::string screen = bring("screen");
            image("ray hdr", "eRGBA16_SFLOAT", { "eStorage", "eSampled", "eTransferSrc" }, 320, 40, screen);
            image("ray position", "eRGBA32_SFLOAT", { "eStorage", "eSampled", "eTransferSrc" }, 320, 180, screen);
            node("RayTracingPipeline", "ray tracer", 760, 40);
            set("ray tracer", "raygen.module", "rayTraced");
            set("ray tracer", "raygen.entry", "raygen");
            set("ray tracer", "miss[0].module", "rayTraced");
            set("ray tracer", "miss[0].entry", "missed");
            set("ray tracer", "miss[1].module", "rayTraced");
            set("ray tracer", "miss[1].entry", "shadowMissed");
            set("ray tracer", "hitGroup[0].closestHit.module", "rayTraced");
            set("ray tracer", "hitGroup[0].closestHit.entry", "hit");
            set("ray tracer", "hitGroup[0].anyHit.module", "rayTraced");
            set("ray tracer", "hitGroup[0].anyHit.entry", "cutOut");
            const std::string scene = bring("ray scene"), meshes = bring("drawn meshes"), buffers = bring("pack buffers");
            wire(bring("camera data"), "ray tracer", "param.camera");
            wire(bring("sun data"), "ray tracer", "param.sun");
            wire(scene, "ray tracer", "param.meshOf", "meshOf");
            wire(scene, "ray tracer", "param.scene", "scene");
            wire(meshes, "ray tracer", "param.ranges", "rangeBuffer");
            wire(buffers, "ray tracer", "param.meshIndices", "indices");
            wire(buffers, "ray tracer", "param.meshPositions", "vertices");
            wire(buffers, "ray tracer", "param.meshNormals", "normals");
            wire(buffers, "ray tracer", "param.meshUVs", "uvs");
            wire(bring("texture sampler"), "ray tracer", "param.textureSampler");
            wire(bring("frame state"), "ray tracer", "param.frameState");
            wire("ray hdr", "ray tracer", "param.picture");
            wire("ray position", "ray tracer", "param.positions");
            node("TraceRays", "trace rays", 1040, 40);
            wire("ray tracer", "trace rays", "pipeline");
            // Its material each entity's own, as the RayScene numbers them: its component, `ray tracer`.
            wire(bring("drawn"), "trace rays", "entities");
            set("trace rays", "cover", "picture");
            set("trace rays", "push.occlusionRadius", 1.0);
            set("trace rays", "push.occlusionRays", 2);
            set("trace rays", "push.reflectionRoughness", 0.6);
            set("trace rays", "push.shadowsFrom", 0.01);
        }

        // ---- the three as one: what the pages after them use ----------------------------------------------------------
        page("branches");
        {
            const auto choose = [&](const std::string& id, const std::array<std::string, 3>& options, const double y) {
                node("Switch", id, 600, y);
                wire(bring("renderer"), id, "index");
                for (int i = 0; i < 3; ++i) wire(bring(options[static_cast<std::size_t>(i)]), id, std::format("option[{}]", i));
            };
            choose("final picture", { "hdr", "meshlet hdr", "ray hdr" }, 40);
            choose("final position", { "position", "meshlet position", "ray position" }, 260);
            // Rays make no depth: the cluster heat, the only page after that reads one, reads the vertex branch's.
            choose("final depth", { "depth", "meshlet depth", "depth" }, 480);
        }
        // What the pages after the branches brought in of the vertex branch's pictures is the chosen branch's now.
        const std::map<std::string, std::string> finals { { "hdr", "final picture" }, { "position", "final position" }, { "depth", "final depth" } };
        const auto& passes = doc.Passes();
        const auto after = std::ranges::find(passes, std::string("branches"));
        const std::vector<std::string> later(after == passes.end() ? passes.end() : after + 1, passes.end());
        std::vector<std::pair<std::string, std::string>> retarget;
        for (const auto& each : doc.Graph().nodes) {
            if (each.type != "Reference" || std::ranges::find(later, each.pass) == later.end()) continue;
            const auto target = each.inputs.find("node");
            if (target == each.inputs.end() || !target->second.value.String()) continue;
            if (const auto chosen = finals.find(*target->second.value.String()); chosen != finals.end()) retarget.emplace_back(each.id, chosen->second);
        }
        for (const auto& [reference, chosen] : retarget) set(reference, "node", chosen);

        IdleUnchosen(doc);
        if (!ok && error.empty()) error = "a node of the branches could not be set up";
        return ok && AddBlendedToBranches(doc, error) && AddEmitters(doc, error) && AddMeshletOcclusion(doc, error);
    }
}
