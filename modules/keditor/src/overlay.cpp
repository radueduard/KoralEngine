//
// keditor: what an editor draws over a scene, and reads back of it. @see overlay.h
//
// It is a graph like a scene's, run by a runtime of its own in the scene's frame graph: an image of which entity is
// where, drawn from every entity with a Mesh on a frame a pick is asked of, and from the picked one alone otherwise; a
// Pick of it; a line round the picked one and the editor's shapes, into
// a picture that is nothing wherever neither is.
//

#include <keditor/overlay.h>
#include <functional>
#include <mesh.h>
#include <capiInterop.h>

#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <filesystem>
#include <format>
#include <fstream>

#include <shader.h>
#include <kmath/transform.h>

#include <kgraph/entities.h>
#include <kscript/declarations.h>
#include <log.h>

namespace keditor
{
    namespace {
        using kgraph::Value;

        constexpr const char* IdsShader = R"slang(// The editor's: which entity is where. Every entity with a Mesh, as the editor's view sees it, drawn as its number.
// `model` and `entityId` are wired to the Draw from a ForEach of Mesh, so no entity needs anything to be drawn here.

struct View {
    float4x4 viewProjection;
};
ConstantBuffer<View> view;

struct Object {
    float4x4 model;
    uint entityId;
};
[[vk::push_constant]] ConstantBuffer<Object> object;

[shader("vertex")]
float4 vertex(float3 position : POSITION) : SV_Position
{
    return mul(mul(float4(position, 1.0), object.model), view.viewProjection);
}

[shader("fragment")]
uint fragment() : SV_Target
{
    return object.entityId;
}
)slang";

        constexpr const char* OutlineShader = R"slang(// The editor's: a line round the picked entity. A pixel that is not the entity's, but is within `thickness` pixels of
// one that is, is the line's colour; every other pixel is left as nothing. `selected` is wired from a Selected node.

Texture2D<uint> ids;

[shader("vertex")]
float4 vertex(uint index : SV_VertexID) : SV_Position
{
    float2 corner = float2((index << 1) & 2, index & 2);
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 fragment(float4 position : SV_Position, uniform uint selected, uniform float4 outlineColor, uniform int thickness) : SV_Target
{
    if (selected == 0) discard;
    int2 pixel = int2(position.xy);
    uint width, height;
    ids.GetDimensions(width, height);
    if (ids.Load(int3(pixel, 0)) == selected) discard;
    for (int y = -thickness; y <= thickness; ++y)
        for (int x = -thickness; x <= thickness; ++x) {
            if (x * x + y * y > thickness * thickness) continue;
            int2 at = clamp(pixel + int2(x, y), int2(0, 0), int2(width, height) - 1);
            if (ids.Load(int3(at, 0)) == selected) return outlineColor;
        }
    discard;
    return float4(0.0, 0.0, 0.0, 0.0);
}
)slang";

        constexpr const char* ThumbShader = R"slang(// The editor's: one mesh by itself, lit from over the shoulder, for a small picture of it in a list.

struct View {
    float4x4 viewProjection;
};
ConstantBuffer<View> view;

struct Object {
    float4x4 model;
};
[[vk::push_constant]] ConstantBuffer<Object> object;

struct Raster {
    float4 position : SV_Position;
    float3 normal;
};

[shader("vertex")]
Raster vertex(float3 position : POSITION, float3 normal : NORMAL)
{
    Raster raster;
    raster.position = mul(mul(float4(position, 1.0), object.model), view.viewProjection);
    raster.normal = normal;
    return raster;
}

[shader("fragment")]
float4 fragment(Raster raster) : SV_Target
{
    float3 normal = normalize(raster.normal);
    float light = 0.28 + 0.72 * saturate(dot(normal, normalize(float3(0.45, 0.8, 0.4))) * 0.5 + 0.5);
    return float4(float3(0.80, 0.83, 0.90) * light, 1.0);
}

// A material on a sphere: its colour and picture, how metal and how rough, what it gives off — under the same light, and
// a sky for what shines to show. The editor's own look at its values: a project's shader needs what its graph makes.
struct Look {
    float4x4 model;
    float4 tint;
    float4 surface;     // x: metallic, y: roughness
    float4 glow;
};
[[vk::push_constant]] ConstantBuffer<Look> look;
Texture2D<float4> picture;
SamplerState smooth;

struct MaterialRaster {
    float4 position : SV_Position;
    float3 normal;
    float2 uv;
};

[shader("vertex")]
MaterialRaster materialVertex(float3 position : POSITION, float3 normal : NORMAL, float2 uv : UV)
{
    MaterialRaster raster;
    raster.position = mul(mul(float4(position, 1.0), look.model), view.viewProjection);
    raster.normal = normal;
    raster.uv = uv;
    return raster;
}

[shader("fragment")]
float4 materialFragment(MaterialRaster raster) : SV_Target
{
    const float3 normal = normalize(raster.normal), toLight = normalize(float3(0.45, 0.8, 0.4)), toEye = normalize(float3(1.0, 0.75, 1.0));
    const float3 base = picture.Sample(smooth, raster.uv).rgb * look.tint.rgb;
    const float metallic = saturate(look.surface.x), rough = clamp(look.surface.y, 0.04, 1.0);
    const float3 shine = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    const float diffuse = 0.28 + 0.72 * saturate(dot(normal, toLight) * 0.5 + 0.5);
    const float power = clamp(2.0 / (rough * rough * rough * rough) - 2.0, 1.0, 2048.0);
    const float highlight = pow(saturate(dot(normal, normalize(toLight + toEye))), power) * (power + 8.0) / 25.0;
    const float3 mirrored = reflect(-toEye, normal);
    const float sky = lerp(0.25, 0.95, saturate(mirrored.y * 0.5 + 0.5)) * (1.0 - 0.5 * rough);
    return float4(base * (1.0 - metallic) * diffuse + shine * (sky + min(highlight, 4.0)) + look.glow.rgb, 1.0);
}
)slang";

        /** The editor's own shaders, where the framework finds them: written once, beside whatever a project has. */
        void WriteShaders()
        {
            static bool written = false;
            if (std::exchange(written, true)) return;
            std::error_code ec;
            const std::filesystem::path directory = std::filesystem::temp_directory_path(ec) / "koral-engine" / "editor-shaders";
            std::filesystem::create_directories(directory, ec);
            for (const auto& [name, text] : { std::pair { "koralEditorIds.slang", IdsShader }, std::pair { "koralEditorOutline.slang", OutlineShader },
                                             std::pair { "koralEditorThumb.slang", ThumbShader } }) {
                const std::filesystem::path file = directory / name;
                std::ifstream in(file, std::ios::binary);
                const std::string now((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (now != text) std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
            }
            kor::Shader::AddSearchPath(directory);
        }

        kgraph::Graph Passes()
        {
            kgraph::Graph graph;
            const auto image = [&](const char* id, const char* format, Value::List usage) {
                graph.Add(id, "Image").Set("format", format).Set("usage", Value(std::move(usage))).Wire("extent", "screen", "extent");
            };
            const auto clear = [](const char* scalar, const char* member) {
                return Value(Value::Members { { "scalarType", scalar }, { "components", 4 }, { "value", Value::Members { { member, Value::List { 0, 0, 0, 0 } } } } });
            };
            graph.Add("screen", "Screen");
            graph.Add("shape", "Primitive");
            graph.Add("camera", "View").Set("main", true);     // the editor's view, and its lens
            graph.Add("lens", "Perspective").Set("main", true).Wire("aspect", "screen", "aspect");
            graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "camera");      // the projection after the view
            graph.Add("view", "ShaderBuffer").Set("module", "koralEditorIds").Set("entry", "vertex").Set("parameter", "view")
                .Wire("field.viewProjection", "seen");

            // Which entity is where: its number, nearest first.
            image("ids", "eR32_UINT", { "eColorAttachment", "eSampled", "eTransferSrc" });
            image("depth", "eD32_SFLOAT", { "eDepthStencilAttachment", "eTransferSrc" });
            graph.Add("ids target", "Framebuffer").Wire("color[0].view", "ids").Set("color[0].name", "ids").Set("color[0].clear", clear("UInt", "u"))
                .Wire("depthStencil.view", "depth").Set("depthStencil.which", "Depth").Set("depthStencil.name", "depth").Set("depthStencil.depth", 1);
            graph.Add("entities", "GraphicsPipeline").Set("vertex.module", "koralEditorIds").Set("vertex.entry", "vertex")
                .Set("fragment.module", "koralEditorIds").Set("fragment.entry", "fragment").Wire("vertex.layout", "shape", "layout")
                .Wire("framebuffer", "ids target").Wire("param.view", "view");
            // Every entity with a Mesh, drawn only on a frame a pick is asked of: what is under the pointer is found among
            // them all. Otherwise idle: with thousands of entities, a draw each, every frame, is what the editor spent most on.
            graph.Add("all", "Entities");
            graph.Add("drawn", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" });
            graph.Add("picked", "Selected");
            graph.Add("not picking", "LessOrEqual").Wire("a", "picked", "picking").Set("b", 0);
            graph.Add("draw ids", "Draw").Wire("pipeline", "entities").Wire("cull", "seen").Wire("idle", "not picking")
                .Wire("mesh", "drawn", "Mesh.mesh").Wire("push.model", "drawn", "matrix").Wire("push.entityId", "drawn", "entityId");
            // The picked one alone, every other frame it is picked: all its outline needs. Its line goes round all of it,
            // what is in front of it as well (as the ids of everything else are not drawn to hide it).
            graph.Add("chosen", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Wire("entityId", "picked", "entityId");
            graph.Add("chosen wanted", "Subtract").Wire("a", "picked", "present").Wire("b", "picked", "picking");
            graph.Add("chosen unwanted", "LessOrEqual").Wire("a", "chosen wanted").Set("b", 0);
            graph.Add("draw chosen", "Draw").Wire("pipeline", "entities").Wire("cull", "seen").Wire("idle", "chosen unwanted")
                .Wire("mesh", "chosen", "Mesh.mesh").Wire("push.model", "chosen", "matrix").Wire("push.entityId", "chosen", "entityId");
            // (With the depth round the pixel: where on a surface it is, for what is placed or measured there.)
            graph.Add("pick", "Pick").Wire("ids", "ids").Wire("depth", "depth");

            // Over the scene's picture: the line round what is picked, then the editor's shapes.
            image("over", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferSrc" });
            graph.Add("over target", "Framebuffer").Wire("color[0].view", "over").Set("color[0].name", "color").Set("color[0].clear", clear("Float", "f"));
            graph.Add("outline", "GraphicsPipeline").Set("vertex.module", "koralEditorOutline").Set("vertex.entry", "vertex")
                .Set("fragment.module", "koralEditorOutline").Set("fragment.entry", "fragment").Wire("framebuffer", "over target").Wire("param.ids", "ids")
                .Set("depthStencilState", Value::Members { { "depthTestEnable", false }, { "depthWriteEnable", false } });
            graph.Add("draw outline", "Draw").Wire("pipeline", "outline").Wire("push.selected", "picked", "entityId")
                .Set("push.outlineColor", Value::List { 1, 0.55, 0.15, 1 }).Set("push.thickness", 2);
            graph.Add("shapes", "DebugDraw").Wire("image", "over");
            return graph;
        }
    }

    /** A graph of the editor's ways of looking at the scene, read from its file, and read again when the file changes. */
    struct Overlay::ViewGraph {
        std::filesystem::path file;
        std::filesystem::file_time_type written {};
        std::unique_ptr<kgraph::Runtime> runtime;
        std::string problem;        // why the file could not be read
    };

    Overlay::Overlay(kor::FrameGraph& frameGraph, kecs::World& world) : _frameGraph(frameGraph), _world(world)
    {
        WriteShaders();
        _runtime = std::make_unique<kgraph::Runtime>(frameGraph, &world);
        _runtime->Set(Passes());
        auto engine = std::make_unique<ViewGraph>();
        engine->file = EngineViews();
        ReadViews(*engine);
        _views.push_back(std::move(engine));
        ApplyGrid();
    }
    Overlay::~Overlay() = default;

    std::filesystem::path Overlay::EngineViews() { return kscript::PackageDirectory() / "editor" / "scene-view.kgraph"; }

    void Overlay::ReadViews(ViewGraph& graph)
    {
        std::error_code ec;
        graph.written = std::filesystem::last_write_time(graph.file, ec);
        std::ifstream in(graph.file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        kgraph::Graph read;
        std::string error;
        if (text.empty() || !kgraph::Graph::FromJson(text, read, error)) {
            graph.problem = std::format("{}: {}", graph.file.filename().string(), text.empty() ? "could not be read" : error);
            return;
        }
        graph.problem.clear();
        if (!graph.runtime) {
            graph.runtime = std::make_unique<kgraph::Runtime>(_frameGraph, &_world);
            graph.runtime->SetEditorView(true);
        }
        graph.runtime->Set(std::move(read));
        _gridAsked = true;      // what it was set to is the file's: the grid as the editor has it, again
    }

    void Overlay::Update(const kor::UVec2 extent, kor::DebugDraw* debug)
    {
        std::erase_if(_retired, [](auto& kept) { return --kept.second <= 0; });
        // A project's graphs, taken up between frames: those of the one before go once nothing draws with them.
        if (_projectViews) {
            const std::filesystem::path directory = *std::exchange(_projectViews, std::nullopt);
            if (directory != _projectViewsNow) {
                _projectViewsNow = directory;
                while (_views.size() > 1) { _retired.emplace_back(std::move(_views.back()), 4); _views.pop_back(); }
                std::error_code ec;
                std::vector<std::filesystem::path> files;
                if (!directory.empty())
                    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
                        if (entry.is_regular_file() && entry.path().extension() == ".kgraph") files.push_back(entry.path());
                std::ranges::sort(files);
                for (auto& file : files) {
                    auto graph = std::make_unique<ViewGraph>();
                    graph->file = std::move(file);
                    ReadViews(*graph);
                    _views.push_back(std::move(graph));
                }
            }
        }
        // A file written since it was read is read again: a view is worked on while it is looked at.
        if (++_frames % 30 == 0)
            for (auto& graph : _views) {
                std::error_code ec;
                if (const auto written = std::filesystem::last_write_time(graph->file, ec); !ec && written != graph->written) ReadViews(*graph);
            }
        if (std::exchange(_gridAsked, false)) ApplyGrid();
        _runtime->SetScreenExtent(extent);
        _runtime->SetDebugDraw(debug);
        _runtime->Update();
        for (auto& graph : _views) {
            if (!graph->runtime) continue;
            graph->runtime->SetScreenExtent(extent);
            graph->runtime->SetView(_view);
            graph->runtime->Update();
        }
    }
    void Overlay::SetView(std::optional<kgraph::View> view)
    {
        _view = view;
        _runtime->SetView(std::move(view));
    }

    void Overlay::SetProjectViews(const std::filesystem::path& directory) { _projectViews = directory; }

    std::vector<Overlay::View> Overlay::Views() const
    {
        std::vector<View> views;
        for (const auto& graph : _views)
            if (graph->runtime)
                for (auto& view : graph->runtime->DebugViews()) views.push_back({ graph->file, std::move(view.id), std::move(view.name) });
        return views;
    }

    void Overlay::ShowView(const std::filesystem::path& file, const std::string& id)
    {
        for (const auto& graph : _views)
            if (graph->runtime) graph->runtime->ShowDebugView(graph->file == file ? id : std::string());
    }

    std::shared_ptr<kgraph::Resource> Overlay::ViewPictureHeld() const
    {
        for (const auto& graph : _views)
            if (graph->runtime && !graph->runtime->ShownDebugView().empty()) return graph->runtime->ResourceOf(graph->runtime->ShownDebugView());
        return nullptr;
    }

    void Overlay::SetGrid(const Grid& grid)
    {
        if (grid == _grid) return;
        _grid = grid;
        _gridAsked = true;      // set between frames, as a graph is
    }

    void Overlay::ApplyGrid()
    {
        if (_views.empty() || !_views.front()->runtime) return;
        const Grid& grid = _grid;
        kgraph::Runtime& runtime = *_views.front()->runtime;
        // What it is set to is the graph's own nodes': `grid shown`, and the grid Draw's push constants.
        kgraph::Graph graph = runtime.Current();
        kgraph::Node* shown = graph.Find("grid shown");
        kgraph::Node* draw = graph.Find("draw grid");
        if (!shown || !draw) return;
        const kgraph::Graph was = graph;
        shown->Set("value", grid.shown);
        draw->Set("push.cell", grid.cell).Set("push.major", grid.major).Set("push.fade", grid.fade).Set("push.opacity", grid.opacity)
            .Set("push.plane", static_cast<int>(grid.plane)).Set("push.depthTested", grid.depthTested ? 1 : 0);
        if (graph != was) runtime.Set(std::move(graph));
    }

    std::shared_ptr<kgraph::Resource> Overlay::GridHeld() const
    {
        if (!_grid.shown || _views.empty() || !_views.front()->runtime) return nullptr;
        return _views.front()->runtime->ResourceOf("grid");
    }

    std::vector<std::string> Overlay::ViewProblems() const
    {
        std::vector<std::string> problems;
        for (const auto& graph : _views) {
            if (!graph->problem.empty()) problems.push_back(graph->problem);
            if (graph->runtime)
                for (const auto& problem : graph->runtime->Problems()) problems.push_back(std::format("{}: {}", graph->file.filename().string(), problem));
        }
        return problems;
    }
    void Overlay::SetSelected(const kecs::Entity entity) { _runtime->SetSelected(entity); }
    void Overlay::SetHidden(std::set<kecs::Entity> hidden) { _runtime->SetHidden(std::move(hidden)); }
    bool Overlay::Pick(const float u, const float v, std::function<void(kecs::Entity)> then) { return _runtime->Pick(u, v, std::move(then)); }
    bool Overlay::PickAt(const float u, const float v, std::function<void(const kgraph::Runtime::PickHit&)> then) { return _runtime->PickAt(u, v, std::move(then)); }
    kor::ResourceRef<const kor::Image> Overlay::Picture() const { return _runtime->ImageOf("over"); }
    std::shared_ptr<kgraph::Resource> Overlay::PictureHeld() const { return _runtime->ResourceOf("over"); }
    kor::Mat4 Overlay::ViewProjection() const { return _runtime->ViewProjection(); }
    float Overlay::Aspect() const { return _runtime->Aspect(); }
    std::string Overlay::Problem() const
    {
        const auto problems = _runtime->Problems();
        return problems.empty() ? std::string() : problems.front();
    }

    // ---- small pictures of meshes ---------------------------------------------------------------------------------

    namespace {
        constexpr int ThumbSize = 96;
        constexpr int ThumbFrames = 5;      // from a mesh being set to its picture being read back

        kgraph::Graph ThumbPasses()
        {
            kgraph::Graph graph;
            const auto image = [&](const char* id, const char* format, Value::List usage) {
                graph.Add(id, "Image").Set("format", format).Set("usage", Value(std::move(usage))).Set("extent.x", ThumbSize).Set("extent.y", ThumbSize);
            };
            graph.Add("shape", "Primitive");
            graph.Add("camera", "View").Set("main", true);
            graph.Add("lens", "Perspective").Set("main", true);     // square: an aspect of 1
            graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "camera");
            graph.Add("view", "ShaderBuffer").Set("module", "koralEditorThumb").Set("entry", "vertex").Set("parameter", "view")
                .Wire("field.viewProjection", "seen");
            image("thumb", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferSrc" });
            image("depth", "eD32_SFLOAT", { "eDepthStencilAttachment" });
            graph.Add("target", "Framebuffer").Wire("color[0].view", "thumb").Set("color[0].name", "color")
                .Set("color[0].clear", Value(Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0, 0, 0, 0 } } } } }))
                .Wire("depthStencil.view", "depth").Set("depthStencil.which", "Depth").Set("depthStencil.name", "depth").Set("depthStencil.depth", 1);
            graph.Add("meshes", "GraphicsPipeline").Set("vertex.module", "koralEditorThumb").Set("vertex.entry", "vertex")
                .Set("fragment.module", "koralEditorThumb").Set("fragment.entry", "fragment").Wire("vertex.layout", "shape", "layout")
                .Wire("framebuffer", "target").Wire("param.view", "view");
            graph.Add("all", "Entities");
            graph.Add("drawn", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("without", Value::List { "material" });
            graph.Add("draw", "Draw").Wire("pipeline", "meshes").Wire("mesh", "drawn", "Mesh.mesh").Wire("push.model", "drawn", "matrix");
            // A material, on what it is shown on: what it looks like the entity's, as a pipeline's parameters are.
            graph.Add("smooth", "Sampler").Set("minFilter", "eLinear").Set("magFilter", "eLinear").Set("mipmapMode", "eLinear");
            graph.Add("material", "GraphicsPipeline").Set("vertex.module", "koralEditorThumb").Set("vertex.entry", "materialVertex")
                .Set("fragment.module", "koralEditorThumb").Set("fragment.entry", "materialFragment").Wire("vertex.layout", "shape", "layout")
                .Wire("framebuffer", "target").Wire("param.view", "view").Wire("param.smooth", "smooth");
            graph.Add("looked", "ForEach").Set("component", "material").Set("with", Value::List { "Mesh" });
            graph.Add("draw material", "Draw").Wire("pipeline", "material").Wire("mesh", "looked", "Mesh.mesh").Wire("push.model", "looked", "matrix")
                .Wire("push.tint", "looked", "field.tint").Wire("push.surface", "looked", "field.surface").Wire("push.glow", "looked", "field.glow")
                .Wire("param.picture", "looked", "field.picture");
            return graph;
        }
    }

    struct Thumbnails::State {
        kor::FrameGraph& frameGraph;
        kecs::World world;
        std::unique_ptr<kgraph::Runtime> runtime;      // there while there is something to draw
        kecs::Entity entity = kecs::NullEntity;
        struct Waiting { std::string id; std::shared_ptr<kgraph::Resource> mesh; std::optional<Look> look; };
        std::vector<Waiting> waiting;
        std::set<std::string, std::less<>> asked;
        std::map<std::string, kor::Resource<kor::Image>, std::less<>> made;
        std::map<std::string, std::vector<kor::U8Vec4>, std::less<>> pixels;    // each picture made, as it was read back
        std::map<std::string, std::string, std::less<>> keptAs;                 // a picture's key in the cache, by its id
        std::filesystem::path cache;
        std::string drawing;
        int frames = 0;
        explicit State(kor::FrameGraph& graph) : frameGraph(graph) {}
    };

    Thumbnails::Thumbnails(kor::FrameGraph& frameGraph) : _state(std::make_unique<State>(frameGraph)) { WriteShaders(); }
    Thumbnails::~Thumbnails() { _state->runtime.reset(); }

    namespace {
        kor::Resource<kor::Image> ImageOfPixels(const std::vector<kor::U8Vec4>& pixels)
        {
            return kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(ThumbSize, ThumbSize))
                .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                .SetData(std::span<const kor::U8Vec4>(pixels)).Build();
        }
    }

    void Thumbnails::SetCache(std::filesystem::path directory) { _state->cache = std::move(directory); }

    void Thumbnails::Want(const std::string& id, std::shared_ptr<kgraph::Resource> mesh, const std::string& kept)
    {
        if (!mesh || mesh->pending || mesh->kind != "Mesh" || !_state->asked.insert(id).second) return;
        // Drawn before, by what it is made of: read, not drawn.
        if (!kept.empty() && !_state->cache.empty()) {
            _state->keptAs[id] = kept;
            std::ifstream in(_state->cache / (kept + ".kthumb"), std::ios::binary);
            std::vector<kor::U8Vec4> pixels(static_cast<std::size_t>(ThumbSize * ThumbSize));
            if (in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 4)))
                if (auto image = ImageOfPixels(pixels); image.Valid()) {
                    _state->made[id] = std::move(image);
                    _state->pixels[id] = std::move(pixels);
                    return;
                }
        }
        _state->waiting.push_back({ id, std::move(mesh), std::nullopt });
    }

    void Thumbnails::WantMaterial(const std::string& id, std::shared_ptr<kgraph::Resource> mesh, Look look, const std::string& kept)
    {
        if (!mesh || mesh->pending || mesh->kind != "Mesh") return;
        Want(id, std::move(mesh), kept);
        // Asked just now, and not read from the cache: drawn as it looks.
        if (!_state->waiting.empty() && _state->waiting.back().id == id) _state->waiting.back().look = std::move(look);
    }

    kor::ResourceRef<const kor::Image> Thumbnails::Of(const std::string_view id) const
    {
        const auto it = _state->made.find(id);
        return it == _state->made.end() || !it->second.Valid() ? kor::ResourceRef<const kor::Image>() : kor::ResourceRef<const kor::Image>(it->second);
    }

    bool Thumbnails::Update()
    {
        State& state = *_state;
        // Nothing to draw: its passes are left as they are, drawing the last mesh again into a picture nobody reads —
        // a mesh in 96 pixels — so that the next mesh asked for is drawn without anything being made again.
        if (state.drawing.empty() && state.waiting.empty()) return false;
        if (!state.runtime) {
            state.runtime = std::make_unique<kgraph::Runtime>(state.frameGraph, &state.world);
            state.runtime->Set(ThumbPasses());
            state.runtime->Watch("thumb");
            if (state.entity.IsNull()) {
                state.entity = state.world.Create();
                state.world.Add<kgraph::Mesh>(state.entity, {});
            }
        }
        bool made = false;
        if (!state.drawing.empty() && ++state.frames >= ThumbFrames) {
            // Its picture, as it was read back: kept as an image of its own, which the next mesh does not draw over.
            if (const auto peek = state.runtime->Watched("thumb"); peek && peek->bytes.size() >= static_cast<std::size_t>(ThumbSize * ThumbSize * 4)) {
                std::vector<kor::U8Vec4> pixels(static_cast<std::size_t>(ThumbSize * ThumbSize));
                std::memcpy(pixels.data(), peek->bytes.data(), pixels.size() * 4);
                auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(ThumbSize, ThumbSize))
                    .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                    .SetData(std::span<const kor::U8Vec4>(pixels)).Build();
                if (image.Valid()) {
                    state.made[state.drawing] = std::move(image);
                    made = true;
                    // Kept for the next run, by what it is made of.
                    if (const auto key = state.keptAs.find(state.drawing); key != state.keptAs.end() && !state.cache.empty()) {
                        std::error_code ec;
                        std::filesystem::create_directories(state.cache, ec);
                        std::ofstream(state.cache / (key->second + ".kthumb"), std::ios::binary | std::ios::trunc)
                            .write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 4));
                    }
                    state.pixels[state.drawing] = std::move(pixels);
                }
                state.drawing.clear();
            } else if (state.frames > ThumbFrames * 8) {
                state.drawing.clear();      // it could not be read back: the list shows what it showed before
            }
        }
        if (state.drawing.empty() && !state.waiting.empty()) {
            auto [id, mesh, look] = std::move(state.waiting.back());
            state.waiting.pop_back();
            // Seen from a corner, far enough off to see all of the box it is in.
            kor::Vec3 lower(-0.5f), upper(0.5f);
            if (mesh->bounded) { lower = { mesh->bounds[0], mesh->bounds[1], mesh->bounds[2] }; upper = { mesh->bounds[3], mesh->bounds[4], mesh->bounds[5] }; }
            const kor::Vec3 centre = (lower + upper) * 0.5f;
            const float radius = std::max(kor::Length(upper - lower) * 0.5f, 1e-4f);
            kgraph::View view;
            view.fieldOfView = 0.6f;
            view.near = radius * 0.05f;
            view.far = radius * 12.f;
            view.view = kor::LookAt(centre + kor::Normalize(kor::Vec3(1.f, 0.75f, 1.f)) * (radius / std::sin(view.fieldOfView * 0.5f) * 1.05f), centre);
            state.runtime->SetView(view);
            state.world.Mutable<kgraph::Mesh>(state.entity)->mesh = std::move(mesh);
            // A material: drawn by what looks at one, with its values. A mesh: by the plain light, as it is.
            const kecs::ComponentId material = state.runtime->ComponentOf("material");
            if (look && material != kecs::NoComponent) {
                if (!state.world.Has(state.entity, material)) state.world.Add(state.entity, material);
                kgraph::Parameters values(state.world, state.entity, material);
                values.Set("tint", look->colour).Set("surface", kor::Vec4(look->metallic, look->roughness, 0.f, 0.f))
                    .Set("glow", kor::Vec4(look->emission.x, look->emission.y, look->emission.z, 0.f))
                    .Set("picture", look->picture ? look->picture : state.runtime->ResourceOf("$white"));
            } else if (material != kecs::NoComponent && state.world.Has(state.entity, material)) {
                state.world.Remove(state.entity, material);
            }
            state.drawing = std::move(id);
            state.frames = 0;
        }
        state.runtime->Update();
        return made;
    }

    // ---- a mesh, looked at -----------------------------------------------------------------------------------------

    namespace {
        /** The device's mesh a viewed mesh is drawn from: its own, or — for a part of a model's — the whole one's. */
        kor::ResourceRef<const kor::Mesh> DeviceMesh(const kgraph::Resource& held)
        {
            const KoralResource* handle = held.part && held.source ? held.source->handle : held.handle;
            return handle ? kor::capi::MeshOf(const_cast<KoralResource*>(handle)) : kor::ResourceRef<const kor::Mesh>();
        }

        const char* ChannelTypeName(const kor::ChannelType type)
        {
            switch (type) {
            case kor::ChannelType::eFloat: return "eFloat";
            case kor::ChannelType::eInt: return "eInt";
            case kor::ChannelType::eUInt: return "eUInt";
            case kor::ChannelType::eShort: return "eShort";
            case kor::ChannelType::eUShort: return "eUShort";
            case kor::ChannelType::eByte: return "eByte";
            case kor::ChannelType::eUByte: return "eUByte";
            case kor::ChannelType::eDouble: return "eDouble";
            }
            return "eFloat";
        }
        /** What a shader reads a channel of this type as. */
        const char* ShaderScalar(const kor::ChannelType type)
        {
            switch (type) {
            case kor::ChannelType::eInt: case kor::ChannelType::eShort: case kor::ChannelType::eByte: return "int";
            case kor::ChannelType::eUInt: case kor::ChannelType::eUShort: case kor::ChannelType::eUByte: return "uint";
            default: return "float";
            }
        }
        bool Showable(const kor::VertexLayout::Attribute& attribute)
        {
            return !attribute.semantic.empty() && attribute.locations == 1 && attribute.channelCount >= 1 && attribute.channelCount <= 4
                && attribute.channelType != kor::ChannelType::eDouble;
        }

        /** A pipeline's `vertex.layout`, as the mesh lays its vertices out: exactly, whatever is between them. */
        Value LayoutValueOf(const kor::VertexLayout& layout)
        {
            Value::List bindings, attributes;
            for (const auto& binding : layout.bindings)
                bindings.emplace_back(Value::Members { { "binding", binding.binding }, { "stride", binding.stride },
                                                       { "inputRate", binding.inputRate == kor::VertexInputRate::eInstance ? "eInstance" : "eVertex" } });
            double position = -1;
            for (const auto& attribute : layout.attributes) {
                if (attribute.semantic == "POSITION" && position < 0) position = static_cast<double>(attributes.size());
                attributes.emplace_back(Value::Members {
                    { "semantic", attribute.semantic }, { "semanticNamespace", attribute.semanticNamespace }, { "binding", attribute.binding },
                    { "offset", attribute.offset }, { "channelType", ChannelTypeName(attribute.channelType) }, { "channelCount", attribute.channelCount },
                    { "location", attribute.location ? static_cast<double>(*attribute.location) : -1.0 }, { "locations", attribute.locations },
                    { "locationStride", attribute.locationStride } });
            }
            return Value(Value::Members { { "bindings", std::move(bindings) }, { "attributes", std::move(attributes) }, { "positionAttribute", position } });
        }

        /**
         * The shader a mesh is looked at with, for what its vertices have and which of them is shown: its position, its
         * normal when it has one (otherwise each triangle's own), and the attribute shown, read as the mesh has it. A
         * geometry stage gives each corner of a triangle a third of where it is, so that its edges are drawn by the
         * triangle, smooth and as wide as asked. Written once a kind, under a name its text gives it.
         */
        std::string MeshModule(const kor::VertexLayout* layout, const std::string& shown)
        {
            const auto find = [&](const std::string_view semantic) -> const kor::VertexLayout::Attribute* {
                if (!layout) return nullptr;
                const auto it = std::ranges::find_if(layout->attributes, [&](const auto& a) { return a.semantic == semantic && Showable(a); });
                return it == layout->attributes.end() ? nullptr : &*it;
            };
            const auto* normal = layout ? find("NORMAL") : nullptr;
            const bool hasNormal = !layout || (normal && normal->channelCount == 3 && normal->channelType == kor::ChannelType::eFloat);
            std::string inputs = "float3 position : POSITION", value = "float4(0.0)";
            if (hasNormal) inputs += ", float3 normal : NORMAL";
            if (shown == "POSITION") value = "float4(position, 0.0)";
            else if (shown == "NORMAL" && hasNormal) value = "float4(normal, 0.0)";
            else if (const auto* attribute = find(shown)) {
                const std::string scalar = ShaderScalar(attribute->channelType);
                const std::uint32_t n = attribute->channelCount;
                inputs += std::format(", {}{} viewed : {}", scalar, n == 1 ? std::string() : std::to_string(n), attribute->semantic);
                value = n == 1 ? "float4(float(viewed), 0.0, 0.0, 0.0)" : n == 2 ? "float4(float2(viewed), 0.0, 0.0)" : n == 3 ? "float4(float3(viewed), 0.0)" : "float4(viewed)";
            } else if (!layout && shown == "UV") {
                inputs += ", float2 viewed : UV";
                value = "float4(viewed, 0.0, 0.0)";
            }
            const std::string text = std::format(R"slang(// The editor's: one mesh by itself, to be looked at — lit, plain, or by one of its attributes — and its edges.

struct View {{
    float4x4 viewProjection;
    uint shading;           // 0 lit, 1 plain, 2 the attribute
    uint mapping;           // the attribute: 0 from 0 to 1, 1 from -1 to 1, 2 a checker of it as a UV
    uint count;             // how many numbers the attribute has
    uint edges;             // whether its edges are drawn (the geometry stage is there)
    float edgeWidth;        // in pixels
}};
ConstantBuffer<View> view;

struct Object {{
    float4x4 model;
}};
[[vk::push_constant]] ConstantBuffer<Object> object;

struct Raster {{
    float4 position : SV_Position;
    float3 world;
    float3 normal;
    float4 value;
    float3 corner;          // a third of where in its triangle: 1 at a corner, 0 along the edge across from it
}};

[shader("vertex")]
Raster vertex({0})
{{
    Raster raster;
    float4 world = mul(float4(position, 1.0), object.model);
    raster.position = mul(world, view.viewProjection);
    raster.world = world.xyz;
    raster.normal = {1};
    raster.value = {2};
    raster.corner = float3(0.0);
    return raster;
}}

[shader("geometry")]
[maxvertexcount(3)]
void edges(triangle Raster corners[3], inout TriangleStream<Raster> output)
{{
    for (uint i = 0; i < 3; ++i) {{
        Raster corner = corners[i];
        corner.corner = float3(i == 0 ? 1.0 : 0.0, i == 1 ? 1.0 : 0.0, i == 2 ? 1.0 : 0.0);
        output.Append(corner);
    }}
}}

[shader("fragment")]
float4 fragment(Raster raster, bool front : SV_IsFrontFace) : SV_Target
{{
    float3 flat = normalize(cross(ddx(raster.world), ddy(raster.world)));
    float3 normal = {3} ? normalize(raster.normal) : flat;
    float3 colour;
    if (view.shading == 0) {{
        // Lit from over the shoulder, and a little from below; a face seen from behind, darker and redder.
        float3 light = normalize(float3(0.45, 0.8, 0.4)), under = normalize(float3(-0.5, -0.3, -0.6));
        float key = {3} ? saturate(dot(normal, light)) : abs(dot(normal, light)), fill = {3} ? saturate(dot(normal, under)) : abs(dot(normal, under));
        float3 albedo = front ? float3(0.80, 0.83, 0.90) : float3(0.55, 0.32, 0.30);
        colour = albedo * (0.18 + 0.7 * key + 0.15 * fill);
    }} else if (view.shading == 1) {{
        colour = float3(0.78, 0.80, 0.86);
    }} else {{
        float4 v = raster.value;
        float3 numbers = view.count == 1 ? v.xxx : view.count == 2 ? float3(v.xy, 0.0) : v.xyz;
        if (view.mapping == 2) {{
            int2 cell = int2(floor(v.xy * 8.0));
            float check = ((cell.x + cell.y) & 1) != 0 ? 0.85 : 0.25;
            colour = lerp(float3(check), float3(frac(v.xy), 0.0), 0.35);
        }} else {{
            colour = saturate(view.mapping == 1 ? numbers * 0.5 + 0.5 : numbers);
        }}
        if (any(isnan(v)) || any(isinf(v))) colour = float3(1.0, 0.0, 1.0);
    }}
    if (view.edges != 0) {{
        // How far from the nearest edge, in pixels: the edge as wide as asked, its sides blended over a pixel.
        float3 pixels = raster.corner / max(fwidth(raster.corner), float3(1e-6));
        float near = min(pixels.x, min(pixels.y, pixels.z));
        float edge = 1.0 - smoothstep(view.edgeWidth * 0.5 - 0.5, view.edgeWidth * 0.5 + 0.5, near);
        colour = lerp(colour, float3(0.98, 0.70, 0.25), edge);
    }}
    return float4(colour, 1.0);
}}
)slang", inputs, hasNormal ? "mul(float4(normal, 0.0), object.model).xyz" : "float3(0.0)", value, hasNormal ? "true" : "false");
            const std::string name = std::format("koralEditorMesh{:016x}", std::hash<std::string> {}(text));
            std::error_code ec;
            const std::filesystem::path file = std::filesystem::temp_directory_path(ec) / "koral-engine" / "editor-shaders" / (name + ".slang");
            if (!std::filesystem::exists(file, ec)) {
                const std::filesystem::path partial = file.string() + ".part";
                std::ofstream(partial, std::ios::binary | std::ios::trunc) << text;
                std::filesystem::rename(partial, file, ec);
            }
            return name;
        }
    }

    MeshViewer::MeshViewer(kor::FrameGraph& frameGraph) : _frameGraph(frameGraph) { WriteShaders(); }
    MeshViewer::~MeshViewer() { _runtime.reset(); }

    void MeshViewer::Show(std::shared_ptr<kgraph::Resource> mesh)
    {
        if (mesh == _mesh) return;
        const bool another = mesh && _mesh && mesh->node != _mesh->node;
        _mesh = std::move(mesh);
        if (another) Frame();
        _dirty = true;      // another mesh may lay its vertices out otherwise, and have other attributes
        // An attribute the mesh has not is not shown.
        if (_shading == Shading::eAttribute && std::ranges::none_of(Attributes(), [&](const Attribute& a) { return a.semantic == _attribute; }))
            _shading = Shading::eLit;
    }

    std::vector<MeshViewer::Attribute> MeshViewer::Attributes() const
    {
        std::vector<Attribute> out;
        if (!_mesh || _mesh->pending) return out;
        const auto mesh = DeviceMesh(*_mesh);
        if (!mesh.Get()) return out;
        for (const auto& attribute : mesh->Layout().attributes)
            if (Showable(attribute)) out.push_back({ attribute.semantic, ShaderScalar(attribute.channelType), attribute.channelCount });
        return out;
    }

    void MeshViewer::SetExtent(const kor::UVec2 extent)
    {
        const kor::UVec2 kept { std::clamp(extent.x, 16u, 4096u), std::clamp(extent.y, 16u, 4096u) };
        if (kept == _extent) return;
        _extent = kept;
        _dirty = true;
    }

    void MeshViewer::Turn(const float across, const float up)
    {
        _yaw += across;
        _pitch = std::clamp(_pitch + up, -1.55f, 1.55f);
    }
    void MeshViewer::Zoom(const float steps) { _distance = std::clamp(_distance * std::pow(6.f / 7.f, steps), 0.02f, 20.f); }
    void MeshViewer::Frame() { _yaw = 0.785f; _pitch = 0.5f; _distance = 1.f; }
    void MeshViewer::SetShading(const Shading shading) { if (std::exchange(_shading, shading) != shading) _dirty = true; }
    void MeshViewer::ShowAttribute(std::string semantic)
    {
        // A direction is from -1 to 1; a UV is seen best as a checker over it; anything else as it is.
        _mapping = semantic == "NORMAL" || semantic.starts_with("TANGENT") || semantic.starts_with("BITANGENT") || semantic == "POSITION" ? Mapping::eSigned
                 : semantic.starts_with("UV") || semantic.starts_with("TEXCOORD") ? Mapping::eChecker : Mapping::eUnit;
        _attribute = std::move(semantic);
        _shading = Shading::eAttribute;
        _dirty = true;
    }
    void MeshViewer::SetMapping(const Mapping mapping) { if (std::exchange(_mapping, mapping) != mapping) _dirty = true; }
    void MeshViewer::SetWireframe(const bool wireframe) { if (std::exchange(_wireframe, wireframe) != wireframe) _dirty = true; }
    void MeshViewer::SetEdgeWidth(const float pixels)
    {
        const float kept = std::clamp(pixels, 0.5f, 8.f);
        if (std::exchange(_edgeWidth, kept) != kept) _dirty = true;
    }

    void MeshViewer::Build()
    {
        // The mesh's own layout, when the device has it: what the pipeline is made for, and what its shader reads.
        std::optional<kor::VertexLayout> layout;
        if (_mesh && !_mesh->pending)
            if (const auto mesh = DeviceMesh(*_mesh); mesh.Get() && !mesh->Layout().attributes.empty()) layout = mesh->Layout();
        const std::string shown = _shading == Shading::eAttribute ? _attribute : std::string();
        const std::string module = MeshModule(layout ? &*layout : nullptr, shown);
        std::uint32_t count = 4;
        if (shown == "POSITION" || shown == "NORMAL") count = 3;
        else if (layout)
            for (const auto& attribute : layout->attributes) if (attribute.semantic == shown) { count = attribute.channelCount; break; }
        if (!layout && shown == "UV") count = 2;

        kgraph::Graph graph;
        const auto image = [&](const char* id, const char* format, Value::List usage) {
            graph.Add(id, "Image").Set("format", format).Set("usage", Value(std::move(usage))).Set("extent.x", _extent.x).Set("extent.y", _extent.y);
        };
        graph.Add("camera", "View").Set("main", true);
        graph.Add("lens", "Perspective").Set("main", true).Set("aspect", static_cast<double>(_extent.x) / std::max(_extent.y, 1u));
        graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "camera");
        graph.Add("view", "ShaderBuffer").Set("module", module).Set("entry", "vertex").Set("parameter", "view")
            .Wire("field.viewProjection", "seen").Set("field.shading", static_cast<int>(_shading)).Set("field.mapping", static_cast<int>(_mapping))
            .Set("field.count", static_cast<int>(count)).Set("field.edges", _wireframe ? 1 : 0).Set("field.edgeWidth", _edgeWidth);
        image("shot", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferSrc" });
        image("depth", "eD32_SFLOAT", { "eDepthStencilAttachment" });
        graph.Add("target", "Framebuffer").Wire("color[0].view", "shot").Set("color[0].name", "color")
            .Set("color[0].clear", Value(Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0.075, 0.078, 0.09, 1 } } } } }))
            .Wire("depthStencil.view", "depth").Set("depthStencil.which", "Depth").Set("depthStencil.name", "depth").Set("depthStencil.depth", 1);
        auto& pipeline = graph.Add("solid", "GraphicsPipeline").Set("vertex.module", module).Set("vertex.entry", "vertex")
            .Set("fragment.module", module).Set("fragment.entry", "fragment").Wire("framebuffer", "target").Wire("param.view", "view");
        if (layout) pipeline.Set("vertex.layout", LayoutValueOf(*layout));
        else {
            graph.Add("shape", "Primitive");
            graph.Find("solid")->Wire("vertex.layout", "shape", "layout");
        }
        // Its edges: drawn by its triangles, each corner told where in its triangle it is.
        if (_wireframe) graph.Find("solid")->Set("geometry.module", module).Set("geometry.entry", "edges");
        graph.Add("all", "Entities");
        graph.Add("drawn", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" });
        graph.Add("draw", "Draw").Wire("pipeline", "solid").Wire("mesh", "drawn", "Mesh.mesh").Wire("push.model", "drawn", "matrix");
        _runtime->Set(graph);
    }

    void MeshViewer::Update()
    {
        // Nothing to show: its passes are taken away by giving it nothing to run. The runtime is kept: what it made is
        // retired, and let go once the frames that use it are done, where a runtime destroyed would free it at once —
        // under a frame already planned with its passes in it.
        if (!_mesh) {
            if (_runtime && !_emptied) {
                _runtime->Set(kgraph::Graph {});
                if (!_entity.IsNull()) _world.Mutable<kgraph::Mesh>(_entity)->mesh = nullptr;
                _emptied = true;
            }
            if (_runtime) _runtime->Update();
            return;
        }
        _emptied = false;
        if (_entity.IsNull()) {
            _entity = _world.Create();
            _world.Add<kgraph::Mesh>(_entity, {});
        }
        _world.Mutable<kgraph::Mesh>(_entity)->mesh = _mesh;
        if (!_runtime) { _runtime = std::make_unique<kgraph::Runtime>(_frameGraph, &_world); _dirty = true; }
        if (std::exchange(_dirty, false)) Build();
        // Seen from about its middle, as far off as sees all of the box it is in, at the distance it is zoomed to.
        kor::Vec3 lower(-0.5f), upper(0.5f);
        if (_mesh->bounded) { lower = { _mesh->bounds[0], _mesh->bounds[1], _mesh->bounds[2] }; upper = { _mesh->bounds[3], _mesh->bounds[4], _mesh->bounds[5] }; }
        const kor::Vec3 centre = (lower + upper) * 0.5f;
        const float radius = std::max(kor::Length(upper - lower) * 0.5f, 1e-4f);
        kgraph::View view;
        view.fieldOfView = 0.7f;
        const float aspect = static_cast<float>(_extent.x) / static_cast<float>(std::max(_extent.y, 1u));
        const float fit = radius / std::sin(view.fieldOfView * 0.5f * std::min(aspect, 1.f)) * 1.05f * _distance;
        const kor::Vec3 from = centre + kor::Vec3(std::cos(_pitch) * std::sin(_yaw), std::sin(_pitch), std::cos(_pitch) * std::cos(_yaw)) * fit;
        view.near = std::max(fit - radius * 1.5f, fit * 0.002f);
        view.far = fit + radius * 2.f;
        view.view = kor::LookAt(from, centre);
        _runtime->SetView(view);
        _runtime->Update();
    }

    kor::ResourceRef<const kor::Image> MeshViewer::Picture() const { return _runtime && _mesh ? _runtime->ImageOf("shot") : kor::ResourceRef<const kor::Image>(); }
    std::shared_ptr<kgraph::Resource> MeshViewer::PictureHeld() const { return _runtime && _mesh ? _runtime->ResourceOf("shot") : nullptr; }
    std::string MeshViewer::Problem() const
    {
        if (!_runtime) return {};
        const auto problems = _runtime->Problems();
        return problems.empty() ? std::string() : problems.front();
    }
}
