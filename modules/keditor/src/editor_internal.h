// keditor: what the editor's parts share — Session's helpers. Private to keditor's editor*.cpp.

#pragma once

#include <keditor/editor.h>
#include <keditor/scripting.h>
#include <keditor/viewer.h>


#include <kasset/import.h>
#include <kasset/parameters.h>
#include "shader_icons.h"
#include "widgets.h"
#include "assets_panel.h"

#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdlib>
#include <format>
#include <kui/icons.h>
#include <cctype>
#include <window.h>
#include <chrono>
#include <limits>
#include <tuple>
#include <cstring>
#include <optional>
#include <functional>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>

#include <app.h>
#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef _WIN32
// Where this program is, for the player beside it: declared as Windows has it, rather than all of windows.h here.
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* module, wchar_t* path, unsigned long size);
#endif
#include <builderDescriptions.h>
#include <koralUI.h>
#include <kui/dock.h>
#include <kui/nodes.h>

#include <kgui/layout.h>
#include <koralFileBrowser.h>
#include <keditor/starter.h>
#include <kgraph/scene.h>
#include <kmath/quaternion.h>
#include <kmath/transform.h>
#include <koralSceneView.h>
#include <sstream>
#include <fstream>
#include <capiInterop.h>

#include <kecs/json.h>
#include <kgraph/catalog.h>
#include <koralImageImport.h>
#include <context.h>
#include <deviceFeatures.h>
#include <kscript/declarations.h>
#include <kgraph/meshes.h>
#include <log.h>

namespace keditor
{
    using kgraph::Value;
    namespace kb = kor::builders;
    using namespace widgets;

    namespace editor_detail {

        /** Whether @p component is a camera: the Camera kind of the engine's package, or one of it — a PerspectiveCamera, an OrthographicCamera. */
        inline bool IsCamera(const kecs::World& world, const std::string_view component)
        {
            const kecs::ComponentId camera = world.Find("Camera"), id = world.Find(component);
            return component == "Camera" || (camera != kecs::NoComponent && id != kecs::NoComponent && world.IsOf(id, camera));
        }

        /** Whether two folders are the same one, however they are written. */
        inline bool Same(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            std::error_code ignored;
            return std::filesystem::equivalent(a, b, ignored);
        }

        /** What this user keeps of the editor's, from one run to the next: XDG_CONFIG_HOME's, or ~/.config's. */
        inline std::filesystem::path ConfigDirectory()
        {
            const char* home = std::getenv("HOME");
            const char* config = std::getenv("XDG_CONFIG_HOME");
            return (config && *config ? std::filesystem::path(config) : std::filesystem::path(home && *home ? home : ".") / ".config") / "koral-engine";
        }

        /** Which language a project's scripts are in: 'k', 'c', or 0 when it has none. */
        inline char ScriptLanguage(const Project& project) { return project.Language(); }
    
        /** The designs a theme is in, as the theme file and the Settings menu name them. */
        constexpr std::pair<const char*, kui::ThemeDesign> ThemeDesigns[] { { "koral", kui::ThemeDesign::eKoral }, { "material", kui::ThemeDesign::eMaterial },
                                                                           { "cupertino", kui::ThemeDesign::eCupertino }, { "windows", kui::ThemeDesign::eFluent } };
    
        /** The grid as this user last set it: `<name> <value>` a line, in ~/.config/koral-engine/scene-grid. */
        inline Grid ReadGrid()
        {
            Grid grid;
            if (std::getenv("KORAL_ENGINE_NO_LAYOUT")) return grid;     // a test's editor keeps nothing of this user's
            std::ifstream in(ConfigDirectory() / "scene-grid");
            std::string name;
            float value = 0.f;
            while (in >> name >> value) {
                if (name == "shown") grid.shown = value != 0.f;
                else if (name == "cell" && value > 0.f) grid.cell = value;
                else if (name == "major" && value >= 1.f) grid.major = value;
                else if (name == "fade" && value > 0.f) grid.fade = value;
                else if (name == "opacity") grid.opacity = std::clamp(value, 0.f, 1.f);
                else if (name == "plane" && value >= 0.f && value <= 2.f) grid.plane = static_cast<Grid::Plane>(static_cast<int>(value));
                else if (name == "depthTested") grid.depthTested = value != 0.f;
            }
            return grid;
        }
        inline void SaveGrid(const Grid& grid)
        {
            if (std::getenv("KORAL_ENGINE_NO_LAYOUT")) return;
            std::error_code ignored;
            std::filesystem::create_directories(ConfigDirectory(), ignored);
            std::ofstream(ConfigDirectory() / "scene-grid", std::ios::trunc) << "shown " << (grid.shown ? 1 : 0) << "\ncell " << grid.cell << "\nmajor " << grid.major
                << "\nfade " << grid.fade << "\nopacity " << grid.opacity << "\nplane " << static_cast<int>(grid.plane) << "\ndepthTested " << (grid.depthTested ? 1 : 0) << '\n';
        }

        /** What a channel of a material is called in the Scene panel's list. */
        constexpr std::pair<kgraph::ShaderDebugChannel, const char*> MaterialChannels[] {
            { kgraph::ShaderDebugChannel::eAlbedo, "Unlit (albedo)" }, { kgraph::ShaderDebugChannel::eNormal, "Normals" },
            { kgraph::ShaderDebugChannel::eRoughness, "Roughness" }, { kgraph::ShaderDebugChannel::eMetallic, "Metallic" },
            { kgraph::ShaderDebugChannel::eEmissive, "Emissive" }, { kgraph::ShaderDebugChannel::eOcclusion, "Occlusion" },
        };
    
        constexpr float Radians = 3.14159265358979323846f / 180.f;
    
        /** The box an entity's mesh is in, in its own space; none when it has no mesh, or one of no known box. */
        inline std::optional<std::pair<kor::Vec3, kor::Vec3>> MeshBox(Document& doc, const kecs::Entity entity)
        {
            const kgraph::Value mesh = doc.Field(entity, "Mesh", "mesh");
            const std::string* id = mesh.String();
            if (!id) return std::nullopt;
            const auto held = doc.Runtime().ResourceOf(*id);
            if (!held || !held->bounded) return std::nullopt;
            return std::pair { kor::Vec3(held->bounds[0], held->bounds[1], held->bounds[2]), kor::Vec3(held->bounds[3], held->bounds[4], held->bounds[5]) };
        }

        /** Where a point is in the view's picture, in its pixels; none behind where it is seen from. */
        inline std::optional<kor::Vec2> OnScreen(const kor::Mat4& viewProjection, const kor::Vec3 point, const kor::UVec2 size)
        {
            const kor::Vec4 clip = viewProjection * kor::Vec4(point, 1.f);
            if (clip.w <= 1e-6f) return std::nullopt;
            return kor::Vec2((clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(size.x), (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(size.y));
        }

        /** What a pixel of the picture is in the scene, at a depth as the depth image holds it. */
        inline kor::Vec3 InScene(const kor::Mat4& inverse, const float x, const float y, const kor::UVec2 size, const float depth)
        {
            const kor::Vec4 clip((x + 0.5f) / static_cast<float>(size.x) * 2.f - 1.f, (y + 0.5f) / static_cast<float>(size.y) * 2.f - 1.f, depth, 1.f);
            const kor::Vec4 world = inverse * clip;
            return kor::Vec3(world.x, world.y, world.z) / world.w;
        }
    
        /** Where a scene's bookmarks are kept: the editor's own, not the project's (a view is whoever looks's). */
        inline std::filesystem::path BookmarksFile(const std::string& scene)
        {
            std::string name;
            for (const char c : scene) name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            return ConfigDirectory() / "bookmarks" / name;
        }
    
        /** Whether text is a name code can use: a letter or an underscore, then letters, digits and underscores. */
        inline bool IsIdentifier(const std::string& name)
        {
            if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')) return false;
            return std::ranges::all_of(name, [](const unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
        }

        /** `text` with every `$NAME` made @p name. */
        inline std::string Named(std::string text, const std::string& name)
        {
            for (std::size_t at = text.find("$NAME"); at != std::string::npos; at = text.find("$NAME", at + name.size())) text.replace(at, 5, name);
            return text;
        }

        constexpr const char* ComputeShader = R"slang(// $NAME: a compute shader. Wire an image to a ComputePipeline's `param.target`, and run it with a Dispatch
// whose `cover` is `target`.
module $NAME;

[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

// A value the pipeline's node sets: `constant.strength`.
[vk::constant_id(0)] const float strength = 1.0;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    target[id.xy] = float4(uv * strength, 0.0, 1.0);
}
)slang";

        /** A vertex shader's, and what comes after it: of each entity's mesh, or one triangle over the whole picture. */
        inline std::string VertexShader(const Session::ShaderChoice& c)
        {
            const bool whole = c.wholePicture, tess = c.tessellation;
            std::string said = whole ? "one triangle over the whole picture" : "each entity's mesh, as a Draw of its GraphicsPipeline is given them";
            if (tess) said += ", each triangle cut finer by tessellation";
            if (c.geometry) said += ", passed on by a geometry shader";
            said += c.fragment ? (whole ? ", and a fragment shader that reads another picture" : ", lit by the sun") : ", and no fragment shader: only depth is drawn";
            std::string out = "// $NAME: " + said + ".\n";
            if (whole) out += "// Its pipeline needs no vertex layout, and its Draw no mesh" + std::string(c.fragment ? ": wire the picture it reads to `param.source`.\n" : ".\n");
            else out += "// A Draw is given each one's own from a Filter of an Entities node: `Mesh.mesh` and `matrix`, and for `color` the field of\n"
                        "// the pipeline's component, named after its node. Wire the frame pass's `frame data` to `param.frame`.\n";
            if (tess) out += "// Tessellated, the pipeline's `patchControlPoints` is 3, and `constant.level` says how fine.\n";
            if (!c.fragment) out += "// Without a fragment shader, its framebuffer needs only a depth attachment: a shadow map's, a depth prepass's.\n";
            out += "// Each entry point is called as a pipeline looks for it: vertexMain, fragmentMain and the rest.\nmodule $NAME;\n\n";
            if (!whole) out += "import engineCommon;\n\n";
            if (whole && c.fragment) out += "Texture2D<float4> source;\n\n";
            if (!whole) out += "struct Vertex {\n    float3 position : POSITION;\n    float3 normal : NORMAL;\n};\n\n"
                               "struct DrawData {\n    float4x4 model;         // where the entity is in the world: a Filter's `matrix`, to `push.model`\n"
                               "    float3 color;           // the entity's own: a field of the pipeline's component\n};\n"
                               "[[vk::push_constant]] ConstantBuffer<DrawData> draw;\n\n";
            if (tess) out += "// What each corner of a triangle carries to the tessellation shaders.\nstruct Corner {\n    float3 place;\n    float3 normal;\n    float2 uv;\n};\n\n";
            out += "struct RasterInfo {\n    float4 position : SV_Position;\n    float3 normal;\n    float2 uv;\n};\n\n";
            out += std::string("// Where a place is in the picture.\nfloat4 Placed(float3 place)\n{\n    return ")
                 + (whole ? "float4(place, 1.0);" : "mul(float4(place, 1.0), frame.viewProjection);") + "\n}\n\n";
            const std::string made = tess ? "Corner" : "RasterInfo";
            out += "[shader(\"vertex\")]\n" + made + (whole ? " vertexMain(uint index : SV_VertexID)\n{\n    const float2 corner = float2((index << 1) & 2, index & 2);\n"
                                                              "    const float3 place = float3(corner * 2.0 - 1.0, 0.0);\n    const float3 normal = float3(0.0, 0.0, 1.0);\n    const float2 uv = corner;\n"
                                                            : " vertexMain(Vertex vertex)\n{\n    const float3 place = mul(float4(vertex.position, 1.0), draw.model).xyz;\n"
                                                              "    const float3 normal = mul(float4(vertex.normal, 0.0), draw.model).xyz;\n    const float2 uv = float2(0.0);\n");
            out += tess ? "    Corner made;\n    made.place = place;\n    made.normal = normal;\n    made.uv = uv;\n    return made;\n}\n\n"
                        : "    RasterInfo raster;\n    raster.position = Placed(place);\n    raster.normal = normal;\n    raster.uv = uv;\n    return raster;\n}\n\n";
            if (tess)
                out += "// How fine each triangle is cut: the pipeline's `constant.level`.\n[vk::constant_id(0)] const float level = 4.0;\n\n"
                       "struct Patch {\n    float edges[3] : SV_TessFactor;\n    float inside : SV_InsideTessFactor;\n};\n\n"
                       "Patch patchMain(InputPatch<Corner, 3> corners)\n{\n    Patch patch;\n    patch.edges[0] = level;\n    patch.edges[1] = level;\n    patch.edges[2] = level;\n"
                       "    patch.inside = level;\n    return patch;\n}\n\n"
                       "[shader(\"hull\")]\n[domain(\"tri\")]\n[partitioning(\"integer\")]\n[outputtopology(\"triangle_cw\")]\n[outputcontrolpoints(3)]\n[patchconstantfunc(\"patchMain\")]\n"
                       "Corner tessellationControlMain(InputPatch<Corner, 3> corners, uint i : SV_OutputControlPointID)\n{\n    return corners[i];\n}\n\n"
                       "[shader(\"domain\")]\n[domain(\"tri\")]\nRasterInfo tessellationEvaluationMain(Patch patch, float3 at : SV_DomainLocation, const OutputPatch<Corner, 3> corners)\n{\n"
                       "    const float3 place = corners[0].place * at.x + corners[1].place * at.y + corners[2].place * at.z;\n"
                       "    RasterInfo raster;\n    raster.position = Placed(place);     // move `place` here: a height, a wave\n"
                       "    raster.normal = corners[0].normal * at.x + corners[1].normal * at.y + corners[2].normal * at.z;\n"
                       "    raster.uv = corners[0].uv * at.x + corners[1].uv * at.y + corners[2].uv * at.z;\n    return raster;\n}\n\n";
            if (c.geometry)
                out += "[shader(\"geometry\")]\n[maxvertexcount(3)]\nvoid geometryMain(triangle RasterInfo corners[3], inout TriangleStream<RasterInfo> triangles)\n{\n"
                       "    for (uint i = 0; i < 3; ++i) triangles.Append(corners[i]);     // each triangle passed on as it is: change this\n}\n\n";
            if (c.fragment)
                out += whole ? "[shader(\"fragment\")]\nfloat4 fragmentMain(RasterInfo raster) : SV_Target\n{\n"
                               "    const float4 color = source.Load(int3(int2(raster.position.xy), 0));\n    return float4(1.0 - color.rgb, color.a);    // the picture, inverted: change this\n}\n"
                             : "[shader(\"fragment\")]\nfloat4 fragmentMain(RasterInfo raster) : SV_Target\n{\n"
                               "    const float facing = saturate(dot(normalize(raster.normal), frame.sunDirection.xyz)) * 0.8 + 0.2;\n    return float4(draw.color * facing, 1.0);\n}\n";
            return out;
        }

        /** Mesh shaders' — a task shader's, when asked for — and their fragments: triangles made with no vertex buffer. */
        inline std::string MeshShading(const Session::ShaderChoice& c)
        {
            std::string out = "// $NAME: mesh shaders, a group a triangle";
            out += c.task ? ", as many groups as a task shader says" : "";
            out += c.fragment ? ", and a fragment shader.\n" : ", and no fragment shader: only depth is drawn.\n";
            out += "// A GraphicsPipeline whose `stages` is eMesh names it, and a Draw of it draws its `tasks` (no mesh).\n";
            if (!c.fragment) out += "// Without a fragment shader, its framebuffer needs only a depth attachment.\n";
            out += "module $NAME;\n\nstruct Vertex {\n    float4 position : SV_Position;\n    float3 color;\n};\n\n";
            if (c.task)
                out += "// What the task shader hands each mesh shader group it starts.\nstruct Payload {\n    uint first;\n};\ngroupshared Payload payload;\n\n"
                       "[shader(\"amplification\")]\n[numthreads(1, 1, 1)]\nvoid taskMain(uint3 group : SV_GroupID)\n{\n"
                       "    payload.first = group.x;\n    DispatchMesh(1, 1, 1, payload);      // how many mesh shader groups: leave some out, and they are not drawn\n}\n\n";
            out += std::string("[shader(\"mesh\")]\n[numthreads(1, 1, 1)]\n[outputtopology(\"triangle\")]\nvoid meshMain(uint3 group : SV_GroupID, ")
                 + (c.task ? "in payload Payload given, " : "") + "out indices uint3 triangles[1], out vertices Vertex vertices[3])\n{\n"
                 + "    SetMeshOutputCounts(3, 1);\n    const float shift = float(" + (c.task ? "given.first" : "group.x") + ") * 0.1;\n"
                 + "    vertices[0].position = float4(-0.5 + shift, 0.5, 0.0, 1.0);\n    vertices[1].position = float4(0.5 + shift, 0.5, 0.0, 1.0);\n"
                 + "    vertices[2].position = float4(shift, -0.5, 0.0, 1.0);\n"
                 + "    vertices[0].color = float3(1.0, 0.0, 0.0);\n    vertices[1].color = float3(0.0, 1.0, 0.0);\n    vertices[2].color = float3(0.0, 0.0, 1.0);\n"
                 + "    triangles[0] = uint3(0, 1, 2);\n}\n";
            if (c.fragment) out += "\n[shader(\"fragment\")]\nfloat4 fragmentMain(Vertex vertex) : SV_Target\n{\n    return float4(vertex.color, 1.0);\n}\n";
            return out;
        }

        /** A ray generation shader, a miss shader and a hit group — with an any hit shader and an intersection shader, when asked for. */
        inline std::string RayTracing(const Session::ShaderChoice& c)
        {
            std::string out = "// $NAME: a ray for each pixel of `target`, traced into `scene`: a ray generation shader, a miss shader, and a hit group";
            out += c.intersection ? " of an intersection shader (a sphere in each box)" : " of triangles";
            out += c.anyHit ? " with an any hit shader" : "";
            out += ".\n// A RayTracingPipeline names it as `raygen`, `miss[0]` and `hitGroup[0]`'s, and a TraceRays covers `target`.\n"
                   "// Wire an image to `param.target` and an AccelerationStructure to `param.scene`.\nmodule $NAME;\n\n"
                   "[[vk::image_format(\"rgba8\")]] RWTexture2D<float4> target;\nRaytracingAccelerationStructure scene;\n\n"
                   "struct Payload {\n    float3 color;\n};\n\n";
            const std::string hit = c.intersection ? "Attributes" : "BuiltInTriangleIntersectionAttributes";
            if (c.intersection) out += "// What the intersection shader says of a hit, for the hit shaders.\nstruct Attributes {\n    float3 normal;\n};\n\n";
            out += "[shader(\"raygeneration\")]\nvoid raygenMain()\n{\n    const uint2 at = DispatchRaysIndex().xy;\n"
                   "    const float2 uv = (float2(at) + 0.5) / float2(DispatchRaysDimensions().xy);\n"
                   "    RayDesc ray;\n    ray.Origin = float3(uv * 2.0 - 1.0, -1.0);\n    ray.Direction = float3(0.0, 0.0, 1.0);\n    ray.TMin = 0.001;\n    ray.TMax = 1000.0;\n"
                   "    Payload payload;\n    payload.color = float3(0.0);\n    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);\n"
                   "    target[at] = float4(payload.color, 1.0);\n}\n\n"
                   "[shader(\"miss\")]\nvoid missMain(inout Payload payload)\n{\n    payload.color = float3(0.1, 0.1, 0.2);     // what a ray that hits nothing sees\n}\n\n";
            out += "[shader(\"closesthit\")]\nvoid closestHitMain(inout Payload payload, " + hit + " hit)\n{\n";
            out += c.intersection ? "    payload.color = hit.normal * 0.5 + 0.5;\n}\n"
                                  : "    payload.color = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);\n}\n";
            if (c.anyHit)
                out += "\n[shader(\"anyhit\")]\nvoid anyHitMain(inout Payload payload, " + hit + " hit)\n{\n"
                       "    // Every hit, not only the nearest: IgnoreHit() lets the ray pass on — through what is cut out of a leaf.\n}\n";
            if (c.intersection)
                out += "\n[shader(\"intersection\")]\nvoid intersectionMain()\n{\n    // A sphere of radius 0.5 in the middle of each box: where the ray meets it, the nearest.\n"
                       "    const float3 origin = ObjectRayOrigin(), direction = ObjectRayDirection();\n"
                       "    const float a = dot(direction, direction), b = dot(origin, direction), c = dot(origin, origin) - 0.25;\n"
                       "    const float seen = b * b - a * c;\n    if (seen < 0.0) return;\n    const float t = (-b - sqrt(seen)) / a;\n"
                       "    if (t < RayTMin() || t > RayTCurrent()) return;\n    Attributes attributes;\n"
                       "    attributes.normal = normalize(origin + t * direction);\n    ReportHit(t, 0, attributes);\n}\n";
            return out;
        }

        constexpr const char* CSharpScript = R"cs(// $NAME: a script. Its public fields are a component — shown in the inspector, saved with the scene. It is a Setup,
// initialised once while the scene plays; it takes PerFrame to be called every frame (FixedStep for the game's steady
// step, BeforeDraw to prepare what is drawn). Add it to an entity in the inspector.
public sealed class $NAME : Setup, PerFrame
{
    public float Speed;         // a field starts at zero: set it in the inspector, or in Initialize

    protected override void Initialize()
    {
        if (Speed == 0) Speed = 45;
    }

    public void Update(float delta)
    {
        // Turns its entity about the up axis, so many degrees a second.
        World.SetRotation(Entity, World.RotationOf(Entity) + new Vec3(0, Speed * delta, 0));
    }
}
)cs";

        constexpr const char* KotlinScript = R"kt(// $NAME: a script. Its public vars are a component — shown in the inspector, saved with the scene. It is a Setup,
// initialised once while the scene plays; it takes PerFrame to be called every frame (FixedStep for the game's steady
// step, BeforeDraw to prepare what is drawn). Add it to an entity in the inspector.
import koral.Vec3
import koral.engine.PerFrame
import koral.engine.Setup

class $NAME : Setup(), PerFrame {
    var speed = 0f              // a var starts at zero: set it in the inspector, or in initialize

    override fun initialize() {
        if (speed == 0f) speed = 45f
    }

    override fun update(delta: Float) {
        // Turns its entity about the up axis, so many degrees a second.
        world.setRotation(entity, world.rotationOf(entity) + Vec3(0f, speed * delta, 0f))
    }
}
)kt";

        constexpr const char* CSharpResourceScript = R"cs(// $NAME: makes resources, and fills them on the CPU as they are made. A Script node names it, and gives each
// resource it declares as an output, by its name: made as it is said here — or as the node says otherwise — and
// filled by Init, again whenever it is made again or a field changes.
public sealed class $NAME : ResourceScript
{
    public int Cells = 8;       // a field is the Script node's input, given there or wired

    // What it makes: each argument one of what a node of its kind takes, by name. An ImageView, a BufferView and a
    // Sampler are made as well; a view names what it is of, `image: nameof(Board)`.
    public Image Board = new(format: "eRGBA8_UNORM", width: 256, height: 256);
    public Sampler Nearest = new(minFilter: "eNearest", magFilter: "eNearest");

    protected override void Init()
    {
        // A checkerboard, for an RGBA8 image: four bytes a texel, a uint each.
        var texels = Board.As<uint>();
        for (var y = 0; y < Board.Height; ++y)
            for (var x = 0; x < Board.Width; ++x)
                texels[y * Board.Width + x] = (x * Cells / Board.Width + y * Cells / Board.Height) % 2 == 0 ? 0xFFFFFFFFu : 0xFF000000u;
    }
}
)cs";

        constexpr const char* KotlinResourceScript = R"kt(// $NAME: makes resources, and fills them on the CPU as they are made. A Script node names it, and gives each
// resource it declares as an output, by its name: made as it is said here — or as the node says otherwise — and
// filled by init, again whenever it is made again or a var changes.
import koral.engine.ResourceScript

class $NAME : ResourceScript() {
    var cells = 8               // a var is the Script node's input, given there or wired

    // What it makes: each argument one of what a node of its kind takes, by name. An ImageView, a BufferView and a
    // Sampler are made as well; a view names what it is of, `image = ::board.name`.
    val board = Image(format = "eRGBA8_UNORM", width = 256, height = 256)
    val nearest = Sampler(minFilter = "eNearest", magFilter = "eNearest")

    override fun init() {
        // A checkerboard, for an RGBA8 image: four bytes a texel.
        for (y in 0 until board.height) for (x in 0 until board.width) {
            val white = (x * cells / board.width + y * cells / board.height) % 2 == 0
            board.bytes.putInt((y * board.width + x) * 4, if (white) -1 else 0xFF000000.toInt())
        }
    }
}
)kt";
    
        /** The input of a shader's slot: `vertex.module`; a ComputePass's own, `module`. */
        inline std::string SlotInput(const std::string& slot, const char* part) { return slot.empty() ? std::string(part) : std::format("{}.{}", slot, part); }
    
        /** What a node stands for: a Reference is the node it brings in. */
        inline std::string OriginOf(const kgraph::Graph& graph, std::string id)
        {
            for (int hops = 0; hops < 16; ++hops) {
                const kgraph::Node* node = graph.Find(id);
                if (!node || node->type != "Reference") break;
                const auto target = node->inputs.find("node");
                if (target == node->inputs.end() || !target->second.value.String()) break;
                id = *target->second.value.String();
            }
            return id;
        }

        /**
         * Whether a pass is left off by a choice as the choices are: its `idle` wired from what says a driver is (or is
         * not) an option — through Max or Or, idle for either — and that holds; or its `enabled` from what does not.
         * Nothing when no choice has a say in it.
         */
        inline std::optional<bool> OffByChoice(const kgraph::Graph& graph, const kgraph::Node& pass, const std::map<std::string, int>& chosen)
        {
            const auto holds = [&](const std::string& wired, auto&& self, const int depth) -> std::optional<bool> {
                const kgraph::Node* by = graph.Find(OriginOf(graph, wired));
                if (!by || depth > 4) return std::nullopt;
                if (by->type == "Max" || by->type == "Or") {
                    std::optional<bool> any;
                    for (const char* side : { "a", "b" })
                        if (const auto it = by->inputs.find(side); it != by->inputs.end() && it->second.link)
                            if (const auto found = self(it->second.link->node, self, depth + 1)) any = any.value_or(false) || *found;
                    return any;
                }
                if (by->type != "Equal" && by->type != "NotEqual") return std::nullopt;
                const auto a = by->inputs.find("a"), b = by->inputs.find("b");
                if (a == by->inputs.end() || !a->second.link || b == by->inputs.end() || !b->second.value.Number()) return std::nullopt;
                const auto driver = chosen.find(OriginOf(graph, a->second.link->node));
                if (driver == chosen.end()) return std::nullopt;
                const bool same = driver->second == static_cast<int>(*b->second.value.Number());
                return by->type == "Equal" ? same : !same;
            };
            std::optional<bool> off;
            if (const auto idle = pass.inputs.find("idle"); idle != pass.inputs.end() && idle->second.link)
                if (const auto found = holds(idle->second.link->node, holds, 0)) off = *found;
            if (const auto enabled = pass.inputs.find("enabled"); enabled != pass.inputs.end() && enabled->second.link)
                if (const auto found = holds(enabled->second.link->node, holds, 0)) off = off.value_or(false) || !*found;
            return off;
        }

        inline bool IsPass(const kgraph::Node& node)
        {
            const kgraph::NodeType* type = kgraph::FindType(node.type);
            return type && type->category == "Passes";
        }
    }
    using namespace editor_detail;
}
