//
// keditor: what an editor draws over a scene, and reads back of it. @see overlay.h
//
// It is a graph like a scene's, run by a runtime of its own in the scene's frame graph: an image of which entity is
// where, drawn from every entity with a Mesh on a frame a pick is asked of, and from the picked one alone otherwise; a
// Pick of it; a line round the picked one and the editor's shapes, into
// a picture that is nothing wherever neither is.
//

#include <keditor/overlay.h>
#include <algorithm>
#include <bit>
#include <keditor/document.h>
#include <buffer.h>
#include <commandBuffer.h>
#include <bindless.h>
#include <kasset/cook.h>
#include <chrono>
#include <deque>
#include <future>
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
            graph.Add("drawn", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" });
            graph.Add("draw", "Draw").Wire("pipeline", "meshes").Wire("mesh", "drawn", "Mesh.mesh").Wire("push.model", "drawn", "matrix");
            return graph;
        }
    }

    namespace {
        /** One thing of a model, for a picture of all of them: what it draws, and where it is, under which of those before it. */
        struct Part {
            std::vector<std::shared_ptr<kgraph::Resource>> meshes;
            kor::Vec3 position { 0.f, 0.f, 0.f }, rotation { 0.f, 0.f, 0.f }, scale { 1.f, 1.f, 1.f };
            int parent = -1;
        };
    }

    struct Thumbnails::State {
        kor::FrameGraph& frameGraph;
        kecs::World world;
        std::unique_ptr<kgraph::Runtime> runtime;      // there while there is something to draw
        kecs::Entity entity = kecs::NullEntity;
        struct Waiting { std::string id; std::shared_ptr<kgraph::Resource> mesh; std::filesystem::path model; };
        // Materials, drawn one after another by what shows one.
        struct WaitingMaterial { std::string id; std::shared_ptr<kgraph::Resource> sphere; MaterialShown shown; };
        std::deque<WaitingMaterial> materials;
        std::unique_ptr<MaterialPreview> preview;
        std::string previewing;
        int previewFrames = 0, previewGiveUp = 0;
        std::vector<Waiting> waiting;
        std::vector<kecs::Entity> drawn;                                        // a model's things, while its picture is drawn
        struct Reading { std::string id; std::future<std::optional<std::vector<kor::U8Vec4>>> pixels; };
        std::deque<std::pair<std::string, std::filesystem::path>> files;        // picture files to be read, one at a time
        std::optional<Reading> reading;
        std::set<std::string, std::less<>> asked;
        std::map<std::string, kor::Resource<kor::Image>, std::less<>> made;
        std::map<std::string, std::vector<kor::U8Vec4>, std::less<>> pixels;    // each picture made, as it was read back
        std::map<std::string, std::string, std::less<>> keptAs;                 // a picture's key in the cache, by its id
        std::filesystem::path cache, assets;
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

    void Thumbnails::SetAssetDirectory(std::filesystem::path directory)
    {
        if (_state->assets == directory) return;
        _state->assets = std::move(directory);
        if (_state->runtime) _state->runtime->SetAssetDirectory(_state->assets);
    }

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
        _state->waiting.push_back({ id, std::move(mesh), {} });
    }

    bool Thumbnails::Has(const std::string& id, const std::string& kept)
    {
        if (_state->made.contains(id)) return true;
        if (kept.empty() || _state->cache.empty()) return false;
        _state->keptAs[id] = kept;
        std::ifstream in(_state->cache / (kept + ".kthumb"), std::ios::binary);
        std::vector<kor::U8Vec4> pixels(static_cast<std::size_t>(ThumbSize * ThumbSize));
        if (!in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 4))) return false;
        auto image = ImageOfPixels(pixels);
        if (!image.Valid()) return false;
        _state->made[id] = std::move(image);
        _state->pixels[id] = std::move(pixels);
        _state->asked.insert(id);
        return true;
    }

    void Thumbnails::WantModel(const std::string& id, std::filesystem::path file, const std::string& kept)
    {
        if (Has(id, kept) || !_state->asked.insert(id).second) return;
        if (!kept.empty()) _state->keptAs[id] = kept;
        _state->waiting.push_back({ id, nullptr, std::move(file) });
    }

    void Thumbnails::WantFile(const std::string& id, std::filesystem::path file, const std::string& kept)
    {
        if (Has(id, kept) || !_state->asked.insert(id).second) return;
        if (!kept.empty()) _state->keptAs[id] = kept;
        _state->files.emplace_back(id, std::move(file));
    }

    namespace {
        /** A picture file as a small picture: all of it, as big as fits and in the middle, what is left over nothing. Off the frame. */
        std::optional<std::vector<kor::U8Vec4>> SmallPictureOf(const std::filesystem::path& file)
        {
            const std::string name = "@" + file.generic_string();
            const auto read = kasset::cook::Pixels({ name }, [&](std::string_view) { return file; });
            if (!read) return std::nullopt;
            const auto width = static_cast<int>(read->extent.x), height = static_cast<int>(read->extent.y);
            if (width <= 0 || height <= 0) return std::nullopt;
            const float fit = static_cast<float>(ThumbSize) / static_cast<float>(std::max(width, height));
            const int shownWidth = std::max(static_cast<int>(static_cast<float>(width) * fit), 1), shownHeight = std::max(static_cast<int>(static_cast<float>(height) * fit), 1);
            const int left = (ThumbSize - shownWidth) / 2, top = (ThumbSize - shownHeight) / 2;
            std::vector<kor::U8Vec4> pixels(static_cast<std::size_t>(ThumbSize * ThumbSize), kor::U8Vec4(0, 0, 0, 0));
            for (int y = 0; y < shownHeight; ++y)
                for (int x = 0; x < shownWidth; ++x) {
                    // The average of what falls in the texel: a big picture does not shimmer small.
                    const int x0 = x * width / shownWidth, x1 = std::max((x + 1) * width / shownWidth, x0 + 1);
                    const int y0 = y * height / shownHeight, y1 = std::max((y + 1) * height / shownHeight, y0 + 1);
                    std::uint32_t sum[4] { 0, 0, 0, 0 }, count = 0;
                    for (int v = y0; v < y1; ++v)
                        for (int u = x0; u < x1; ++u, ++count) {
                            const unsigned char* texel = &read->pixels[(static_cast<std::size_t>(v) * width + u) * 4];
                            for (int c = 0; c < 4; ++c) sum[c] += texel[c];
                        }
                    pixels[static_cast<std::size_t>((top + y) * ThumbSize + left + x)] = kor::U8Vec4(sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count);
                }
            return pixels;
        }
    }

    void Thumbnails::WantMaterial(const std::string& id, std::shared_ptr<kgraph::Resource> sphere, MaterialShown shown, const std::string& kept)
    {
        State& state = *_state;
        if (!state.asked.insert(id).second) return;
        if (!kept.empty()) state.keptAs[id] = kept;
        if (Has(id, kept)) return;
        state.materials.push_back({ id, std::move(sphere), std::move(shown) });
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
        // Picture files, read off the frame one after another.
        bool read = false;
        if (state.reading && state.reading->pixels.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            if (auto pixels = state.reading->pixels.get()) {
                if (auto image = ImageOfPixels(*pixels); image.Valid()) {
                    state.made[state.reading->id] = std::move(image);
                    if (const auto key = state.keptAs.find(state.reading->id); key != state.keptAs.end() && !state.cache.empty()) {
                        std::error_code ec;
                        std::filesystem::create_directories(state.cache, ec);
                        std::ofstream(state.cache / (key->second + ".kthumb"), std::ios::binary | std::ios::trunc)
                            .write(reinterpret_cast<const char*>(pixels->data()), static_cast<std::streamsize>(pixels->size() * 4));
                    }
                    read = true;
                }
            }
            state.reading.reset();
        }
        if (!state.reading && !state.files.empty()) {
            auto [id, file] = std::move(state.files.front());
            state.files.pop_front();
            state.reading = State::Reading { std::move(id), std::async(std::launch::async, SmallPictureOf, std::move(file)) };
        }
        // A material: drawn by what shows one, and read back. One that cannot be drawn (its kind has no shading) stays
        // as the list shows it until then.
        if (!state.previewing.empty() || !state.materials.empty()) {
            if (!state.preview) {
                state.preview = std::make_unique<MaterialPreview>(state.frameGraph, kor::UVec2(ThumbSize, ThumbSize), false);
                state.preview->SetAssetDirectory(state.assets);
                state.preview->SetReadBack(true);
            }
            if (state.previewing.empty()) {
                auto next = std::move(state.materials.front());
                state.materials.pop_front();
                state.preview->SetMesh(std::move(next.sphere));
                state.preview->Show(std::move(next.shown));
                state.previewing = std::move(next.id);
                state.previewFrames = 0;
            }
        }
        if (state.preview) {
            state.preview->Update();
            if (!state.previewing.empty()) {
                const int frames = state.preview->Settled() ? ++state.previewFrames : state.previewFrames;
                const auto texels = frames >= ThumbFrames ? state.preview->Texels() : std::nullopt;
                if (texels && texels->size() >= static_cast<std::size_t>(ThumbSize * ThumbSize * 4)) {
                    std::vector<kor::U8Vec4> pixels(static_cast<std::size_t>(ThumbSize * ThumbSize));
                    std::memcpy(pixels.data(), texels->data(), pixels.size() * 4);
                    if (auto image = ImageOfPixels(pixels); image.Valid()) {
                        state.made[state.previewing] = std::move(image);
                        read = true;
                        if (const auto key = state.keptAs.find(state.previewing); key != state.keptAs.end() && !state.cache.empty()) {
                            std::error_code ec;
                            std::filesystem::create_directories(state.cache, ec);
                            std::ofstream(state.cache / (key->second + ".kthumb"), std::ios::binary | std::ios::trunc)
                                .write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 4));
                        }
                        state.pixels[state.previewing] = std::move(pixels);
                    }
                    state.previewing.clear();
                } else if (!state.preview->Problem().empty() && ++state.previewFrames > ThumbFrames * 4) {
                    kor::log::Warn("A material's picture could not be drawn: {}", state.preview->Problem());
                    state.previewing.clear();
                } else if (++state.previewGiveUp > 1200) {
                    state.previewing.clear();
                }
                if (state.previewing.empty()) state.previewGiveUp = 0;
            }
        }
        if (state.drawing.empty() && state.waiting.empty()) return read;
        if (!state.runtime) {
            state.runtime = std::make_unique<kgraph::Runtime>(state.frameGraph, &state.world);
            state.runtime->SetAssetDirectory(state.assets);
            state.runtime->Set(ThumbPasses());
            state.runtime->Watch("thumb");
            if (state.entity.IsNull()) {
                state.entity = state.world.Create();
                state.world.Add<kgraph::Mesh>(state.entity, {});
            }
        }
        bool made = read;
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
            // A model's things go with its picture.
            if (state.drawing.empty()) {
                for (const kecs::Entity entity : state.drawn) state.world.Destroy(entity);
                state.drawn.clear();
            }
        }
        if (state.drawing.empty() && !state.waiting.empty()) {
            // What it needs read first, by what is here alone: not the scene's. Looked at again next frame until it is.
            State::Waiting& next = state.waiting.back();
            const auto ready = [&](std::shared_ptr<kgraph::Resource>& held) {
                while (held && held->replacedBy) held = held->replacedBy;
                return held && !held->pending;
            };
            std::vector<Part> parts;
            bool waits = false;
            std::string failed;
            if (!next.model.empty()) {
                const auto model = state.runtime->ModelOf(next.model, &failed);
                waits = !model;
                if (model) for (const kgraph::ModelNode& node : model->nodes) {
                    Part part;
                    part.position = { node.position[0], node.position[1], node.position[2] };
                    part.rotation = { node.rotation[0], node.rotation[1], node.rotation[2] };
                    part.scale = { node.scale[0], node.scale[1], node.scale[2] };
                    part.parent = node.parent;
                    for (const auto& each : node.parts) {
                        auto held = state.runtime->ResourceOf(each.mesh);
                        if (!ready(held)) { waits = true; continue; }
                        part.meshes.push_back(std::move(held));
                    }
                    parts.push_back(std::move(part));
                }
            }
            if (!failed.empty()) { state.waiting.pop_back(); state.runtime->Update(); return made; }     // shown by its kind
            if (waits) { state.runtime->Update(); return made; }
            auto [id, mesh, model] = std::move(state.waiting.back());
            state.waiting.pop_back();
            // Seen from a corner, far enough off to see all of the box it is in.
            kor::Vec3 lower(-0.5f), upper(0.5f);
            if (mesh && mesh->bounded) { lower = { mesh->bounds[0], mesh->bounds[1], mesh->bounds[2] }; upper = { mesh->bounds[3], mesh->bounds[4], mesh->bounds[5] }; }
            // A model: each of its things where the file has it, and the box of them all.
            if (!parts.empty()) {
                std::vector<kecs::Entity> nodes;
                kor::Vec3 low(1e30f), high(-1e30f);
                for (const Part& part : parts) {
                    const kecs::Entity node = state.world.Create();
                    if (part.parent >= 0 && part.parent < static_cast<int>(nodes.size())) state.world.SetParent(node, nodes[static_cast<std::size_t>(part.parent)], -1, false);
                    state.world.SetPosition(node, { part.position.x, part.position.y, part.position.z });
                    state.world.SetRotation(node, { part.rotation.x, part.rotation.y, part.rotation.z });
                    state.world.SetScale(node, { part.scale.x, part.scale.y, part.scale.z });
                    nodes.push_back(node);
                    state.drawn.push_back(node);
                    for (const auto& each : part.meshes) {
                        if (!each) continue;
                        const kecs::Entity thing = state.world.Create();
                        state.world.SetParent(thing, node, -1, false);
                        state.world.Add<kgraph::Mesh>(thing, { each });
                        state.drawn.push_back(thing);
                        if (!each->bounded) continue;
                        const kecs::Mat4 world = state.world.WorldMatrix(thing);
                        for (int corner = 0; corner < 8; ++corner) {
                            const float p[3] { each->bounds[corner & 1 ? 3 : 0], each->bounds[corner & 2 ? 4 : 1], each->bounds[corner & 4 ? 5 : 2] };
                            for (int axis = 0; axis < 3; ++axis) {
                                const float at = world.m[axis] * p[0] + world.m[4 + axis] * p[1] + world.m[8 + axis] * p[2] + world.m[12 + axis];
                                low[axis] = std::min(low[axis], at);
                                high[axis] = std::max(high[axis], at);
                            }
                        }
                    }
                }
                if (low.x <= high.x) { lower = low; upper = high; }
                else { for (const kecs::Entity entity : state.drawn) state.world.Destroy(entity); state.drawn.clear(); state.runtime->Update(); return made; }
            }
            const kor::Vec3 centre = (lower + upper) * 0.5f;
            const float radius = std::max(kor::Length(upper - lower) * 0.5f, 1e-4f);
            kgraph::View view;
            view.fieldOfView = 0.6f;
            view.near = radius * 0.05f;
            view.far = radius * 12.f;
            view.view = kor::LookAt(centre + kor::Normalize(kor::Vec3(1.f, 0.75f, 1.f)) * (radius / std::sin(view.fieldOfView * 0.5f) * 1.05f), centre);
            state.runtime->SetView(view);
            // The one thing a mesh is shown on; a model has its own things, and that shows nothing.
            if (mesh && !state.world.Has<kgraph::Mesh>(state.entity)) state.world.Add<kgraph::Mesh>(state.entity, {});
            if (mesh) state.world.Mutable<kgraph::Mesh>(state.entity)->mesh = std::move(mesh);
            else if (state.world.Has<kgraph::Mesh>(state.entity)) state.world.Remove<kgraph::Mesh>(state.entity);
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

    // ---- a material, looked at -----------------------------------------------------------------------------------------

    namespace {
        constexpr std::uint32_t CubeSize = 256, CubeMips = 7;

        /** A half, of a float. */
        std::uint16_t Half(const float value)
        {
            const float v = std::clamp(value, 0.f, 65000.f);
            if (v < 6.2e-5f) return 0;
            const std::uint32_t bits = std::bit_cast<std::uint32_t>(v);
            const int exponent = static_cast<int>(bits >> 23) - 127 + 15;
            return static_cast<std::uint16_t>((exponent << 10) | ((bits >> 13) & 0x3FF));
        }

        float Smooth(const float from, const float to, const float at)
        {
            const float t = std::clamp((at - from) / (to - from), 0.f, 1.f);
            return t * t * (3.f - 2.f * t);
        }

        /** How much of a span @p half wide about 0 is at @p at, seen through a blur @p blur wide: its edges a slope, its light kept. */
        float Spread(const float at, const float half, const float blur)
        {
            return Smooth(half + blur, std::max(half - blur, 0.f) - 1e-4f, std::abs(at)) * std::min(half / blur, 1.f);
        }

        /** A rectangle of light hung towards @p towards, as seen along @p direction through a blur @p blur wide. */
        float Panel(const kor::Vec3 direction, const kor::Vec3 towards, const float width, const float height, const float blur)
        {
            const kor::Vec3 n = kor::Normalize(towards);
            const float along = kor::Dot(direction, n);
            if (along <= 0.05f) return 0.f;
            const kor::Vec3 across = kor::Normalize(kor::Cross(kor::Vec3(0.f, 1.f, 0.f), n)), up = kor::Cross(n, across);
            const kor::Vec3 at = direction * (1.f / along) - n;
            return Spread(kor::Dot(at, across), width, blur) * Spread(kor::Dot(at, up), height, blur) * Smooth(0.05f, 0.4f, along + blur * 0.2f);
        }

        /**
         * A photographer's room: dim walls, a dark floor, and panels of light for a highlight to be the shape of — as a
         * surface of @p roughness sees it. What is in it is known, so it is blurred by widening its edges as far as such a
         * surface's reflection spreads, a texel at a time, rather than by sampling it.
         */
        kor::Vec3 Studio(const kor::Vec3 direction, const float roughness)
        {
            const float blur = std::max(2.f * roughness * roughness, 0.015f);
            const float height = direction.y * 0.5f + 0.5f;
            const kor::Vec3 wall = kor::Vec3(0.03f, 0.033f, 0.04f) * (1.f - height) + kor::Vec3(0.2f, 0.22f, 0.26f) * height;
            const kor::Vec3 floor = kor::Vec3(0.06f, 0.058f, 0.056f) + kor::Vec3(0.05f) * std::max(1.f + direction.y * 1.4f, 0.f);
            const float seam = Smooth(-0.04f - blur, 0.01f + blur, direction.y);
            const kor::Vec3 room = floor * (1.f - seam) + wall * seam;
            const float key = Panel(direction, { -0.55f, 0.75f, 0.45f }, 0.62f, 0.42f, blur) * 13.f;
            const float strip = Panel(direction, { 0.85f, 0.18f, 0.35f }, 0.14f, 0.75f, blur) * 7.f;
            const float rim = Panel(direction, { 0.15f, 0.3f, -0.95f }, 0.7f, 0.16f, blur) * 8.f;
            return room + kor::Vec3(1.f, 0.96f, 0.9f) * key + kor::Vec3(0.8f, 0.9f, 1.f) * strip + kor::Vec3(1.f) * rim;
        }

        /** Which way a texel of a face of a cube looks, the faces in a cube map's order: +x, -x, +y, -y, +z, -z. */
        kor::Vec3 CubeDirection(const std::uint32_t face, const float u, const float v)
        {
            switch (face) {
            case 0: return kor::Normalize(kor::Vec3(1.f, -v, -u));
            case 1: return kor::Normalize(kor::Vec3(-1.f, -v, u));
            case 2: return kor::Normalize(kor::Vec3(u, 1.f, v));
            case 3: return kor::Normalize(kor::Vec3(u, -1.f, -v));
            case 4: return kor::Normalize(kor::Vec3(u, -v, 1.f));
            default: return kor::Normalize(kor::Vec3(-u, -v, -1.f));
            }
        }
    }

    /** The room, as a cube map: each mip level as a rougher surface sees it. One, shared by every preview there is. */
    struct MaterialPreview::Cube {
        kor::Resource<kor::Image> image;
        std::uint32_t index = 0;

        Cube()
        {
            image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA16_SFLOAT).SetExtent(kor::UVec2(CubeSize, CubeSize))
                .SetArrayLayers(6).SetMipLevels(CubeMips)
                .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst | kor::Image::Usage::eTransferSrc).Build();
            if (!image.Valid()) return;
            std::vector<std::uint16_t> texels;
            std::vector<kor::Copy> copies;
            for (std::uint32_t mip = 0; mip < CubeMips; ++mip) {
                const std::uint32_t size = CubeSize >> mip;
                const float t = static_cast<float>(mip) / static_cast<float>(CubeMips - 1), roughness = t * t;
                for (std::uint32_t face = 0; face < 6; ++face) {
                    copies.push_back(kor::Copy { .bufferOffset = texels.size() * sizeof(std::uint16_t), .imageOffset = { 0, 0, 0 },
                                                 .imageExtent = kor::IVec3(static_cast<int>(size), static_cast<int>(size), 1),
                                                 .imageBaseArrayLayer = face, .imageLayerCount = 1, .imageMipLevel = mip });
                    for (std::uint32_t y = 0; y < size; ++y)
                        for (std::uint32_t x = 0; x < size; ++x) {
                            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.f - 1.f, v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.f - 1.f;
                            const kor::Vec3 light = Studio(CubeDirection(face, u, v), roughness);
                            texels.insert(texels.end(), { Half(light.x), Half(light.y), Half(light.z), Half(1.f) });
                        }
                }
            }
            const auto staging = kor::Buffer::Builder<std::uint16_t>().SetDataView(std::span<const std::uint16_t>(texels))
                .SetUsage(kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eStaging).Build();
            const kor::ResourceRef<const kor::Image> ref(image);
            (void)kor::CommandBuffer::Upload([&](kor::CommandBuffer& commandBuffer) {
                for (const kor::Copy& copy : copies) commandBuffer.CopyBufferToImage(staging, ref, copy);
                commandBuffer.Barrier({}, { { ref, kor::ResourceAccess::eAllShaderRead } });
            }, kor::CommandBuffer::Usage::eGraphics);
            index = kor::Bindless::Sampled(ref, kor::ImageShape::eCube).index;
        }
    };

    namespace {
        /**
         * The shader a kind's materials are drawn with on a mesh laid out so: a few lines, naming the kind, its shading module
         * and what the mesh's vertices have. Everything else is koral_material_preview. Written once, under a name its text gives it.
         */
        std::string MaterialModule(const std::string& kind, const kor::VertexLayout* layout)
        {
            const auto has = [&](const std::string_view semantic, const std::uint32_t count) {
                return !layout || std::ranges::any_of(layout->attributes, [&](const auto& a) { return a.semantic == semantic && a.channelCount == count && a.channelType == kor::ChannelType::eFloat; });
            };
            const bool normal = has("NORMAL", 3), uv = has("UV", 2);
            const std::string text = std::format(R"slang(// The editor's: {0} on a mesh. Generated.

import entity;
import koral_material_preview;
import {0}Shading;

[entity("PER_INSTANCE")] StructuredBuffer<{0}> materials;

[shader("vertex")]
KoralPreviewRaster vertex(float3 position : POSITION{1}{2}, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{{
    return KoralPreviewVertex(position, {3}, {4}, first + index);
}}

[shader("fragment")]
float4 fragment(KoralPreviewRaster raster, bool front : SV_IsFrontFace) : SV_Target
{{
    return KoralPreviewColour(materials[raster.instance], raster, front);
}}
)slang", kind, normal ? ", float3 normal : NORMAL" : "", uv ? ", float2 uv : UV" : "", normal ? "normal" : "normalize(position)", uv ? "uv" : "float2(0.0)");
            const std::string name = std::format("koralEditorMaterial{:016x}", std::hash<std::string> {}(text));
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

    MaterialPreview::MaterialPreview(kor::FrameGraph& frameGraph, const kor::UVec2 extent, const bool showAround)
        : _frameGraph(frameGraph), _extent(extent), _wanted(extent), _showAround(showAround)
    {
        if (_showAround) _pitch = 0.16f;    // seeing the room behind it, not its floor
        WriteShaders();
        static std::weak_ptr<Cube> shared;
        _cube = shared.lock();
        if (!_cube) shared = _cube = std::make_shared<Cube>();
    }
    MaterialPreview::~MaterialPreview() { _runtime.reset(); }

    void MaterialPreview::Show(std::optional<MaterialShown> shown)
    {
        if (shown && shown->kind.name != _kind) _dirty = true;
        _shown = std::move(shown);
        _changed = true;
    }
    void MaterialPreview::SetMesh(std::shared_ptr<kgraph::Resource> mesh)
    {
        if (mesh == _mesh) return;
        _mesh = std::move(mesh);
        _dirty = true;
    }
    void MaterialPreview::SetAssetDirectory(std::filesystem::path directory)
    {
        _assets = std::move(directory);
        if (_runtime) _runtime->SetAssetDirectory(_assets);
    }
    void MaterialPreview::SetExtent(const kor::UVec2 extent)
    {
        const kor::UVec2 kept { std::clamp(extent.x, 16u, 4096u), std::clamp(extent.y, 16u, 4096u) };
        if (kept == _wanted) return;
        _wanted = kept;
        _settling = 0;
    }
    void MaterialPreview::Turn(const float across, const float up)
    {
        _yaw += across;
        _pitch = std::clamp(_pitch + up, -1.55f, 1.55f);
    }
    void MaterialPreview::Zoom(const float steps) { _distance = std::clamp(_distance * std::pow(6.f / 7.f, steps), 0.2f, 8.f); }

    void MaterialPreview::Build()
    {
        std::optional<kor::VertexLayout> layout;
        if (_drawn && !_drawn->pending)
            if (const auto mesh = DeviceMesh(*_drawn); mesh.Get() && !mesh->Layout().attributes.empty()) layout = mesh->Layout();
        const std::string module = MaterialModule(_shown->kind.name, layout ? &*layout : nullptr);

        kgraph::Graph graph;
        const auto image = [&](const char* id, const char* format, Value::List usage) {
            graph.Add(id, "Image").Set("format", format).Set("usage", Value(std::move(usage))).Set("extent.x", _extent.x).Set("extent.y", _extent.y);
        };
        graph.Add("camera", "View").Set("main", true);
        graph.Add("lens", "Perspective").Set("main", true).Set("aspect", static_cast<double>(_extent.x) / std::max(_extent.y, 1u));
        graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "camera");
        graph.Add("unseen", "MatrixInverse").Wire("a", "seen");
        graph.Add("eye", "MatrixInverse").Wire("a", "camera");
        graph.Add("view", "ShaderBuffer").Set("module", module).Set("entry", "vertex").Set("parameter", "view")
            .Wire("field.viewProjection", "seen").Wire("field.inverseViewProjection", "unseen").Wire("field.inverseView", "eye")
            .Set("field.environment", static_cast<int>(_cube->index)).Set("field.mips", static_cast<int>(CubeMips)).Set("field.showAround", _showAround ? 1 : 0);
        image("shot", "eRGBA8_UNORM", { "eColorAttachment", "eSampled", "eTransferSrc" });
        image("depth", "eD32_SFLOAT", { "eDepthStencilAttachment" });
        graph.Add("target", "Framebuffer").Wire("color[0].view", "shot").Set("color[0].name", "color")
            .Set("color[0].clear", Value(Value::Members { { "scalarType", "Float" }, { "components", 4 }, { "value", Value::Members { { "f", Value::List { 0, 0, 0, 0 } } } } }))
            .Wire("depthStencil.view", "depth").Set("depthStencil.which", "Depth").Set("depthStencil.name", "depth").Set("depthStencil.depth", 1);
        graph.Add("smooth", "Sampler").Set("minFilter", "eLinear").Set("magFilter", "eLinear").Set("mipmapMode", "eLinear").Set("maxLod", 16);
        if (_showAround) {
            graph.Add("sky", "GraphicsPipeline").Set("vertex.module", "koral_material_preview").Set("vertex.entry", "skyVertex")
                .Set("fragment.module", "koral_material_preview").Set("fragment.entry", "skyFragment")
                .Wire("framebuffer", "target").Wire("param.view", "view").Wire("param.smooth", "smooth");
            graph.Add("preview sky", "Draw").Wire("pipeline", "sky").Set("vertexCount", 3);
        }
        graph.Add("all", "Entities");
        graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh", _shown->kind.name }).Set("groupBy", "Mesh");
        graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
        graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
        graph.Add("material", "GraphicsPipeline").Set("vertex.module", module).Set("vertex.entry", "vertex").Wire("vertex.layout", "pack", "layout")
            .Set("fragment.module", module).Set("fragment.entry", "fragment").Wire("framebuffer", "target").Wire("param.view", "view").Wire("param.smooth", "smooth");
        graph.Add("preview material", "Draw").Set("load", _showAround).Wire("pipeline", "material").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
            .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
        _runtime->Set(graph);
        if (_readBack) _runtime->Watch("shot");
    }

    void MaterialPreview::Apply()
    {
        const kecs::ComponentId id = _world.Find(_shown->kind.name);
        if (id == kecs::NoComponent) return;
        if (_world.Has(_entity, id)) _world.Remove(_entity, id);
        _world.Add(_entity, id);
        _pictures.clear();
        const auto resolve = [this](const std::string& name) {
            auto held = name.starts_with('@') || name.starts_with('$') ? _runtime->ResourceOf(name) : nullptr;
            if (!held && _find) held = _find(name);
            if (held) _pictures.push_back(held);
            return held;
        };
        const auto retire = [this](std::shared_ptr<kgraph::Resource> old) { _runtime->Retire(std::move(old)); };
        for (const auto& [field, value] : _shown->values) {
            std::string error;
            if (!Document::WriteField(_world, _entity, id, field, value, resolve, retire, &error)) kor::log::Warn("A material's preview left '{}' as it starts: {}", field, error);
        }
    }

    void MaterialPreview::Update()
    {
        if (!_shown) {
            if (_runtime && !_emptied) {
                _runtime->Set(kgraph::Graph {});
                _emptied = true;
                _kind.clear();
            }
            if (_runtime) _runtime->Update();
            return;
        }
        _emptied = false;
        if (!_runtime) {
            _runtime = std::make_unique<kgraph::Runtime>(_frameGraph, &_world);
            _runtime->SetAssetDirectory(_assets);
            _dirty = true;
        }
        if (!_shown->modules.empty()) _runtime->SetGeneratedShaderDirectory(_shown->modules, false);
        if (_entity.IsNull()) {
            _entity = _world.Create();
            _world.Add<kgraph::Mesh>(_entity, {});
        }
        if (_wanted != _extent && ++_settling >= 8) {
            _extent = _wanted;
            _settling = 0;
            _dirty = true;
        }
        if (_kind != _shown->kind.name) {
            std::string error;
            if (_runtime->DeclareMaterialKind(_shown->kind, &error) == kecs::NoComponent) _problem = error;
            else _problem.clear();
            _kind = _shown->kind.name;
        }
        auto drawn = _mesh ? _mesh : _runtime->ResourceOf("$sphere");
        while (drawn && drawn->replacedBy) drawn = drawn->replacedBy;
        if (!drawn || drawn->pending) { _runtime->Update(); return; }
        if (drawn != _drawn) {
            _drawn = std::move(drawn);
            _world.Mutable<kgraph::Mesh>(_entity)->mesh = _drawn;
            _dirty = true;
        }
        if (std::exchange(_changed, false)) Apply();
        if (std::exchange(_dirty, false)) Build();
        kor::Vec3 lower(-0.5f), upper(0.5f);
        if (_drawn->bounded) { lower = { _drawn->bounds[0], _drawn->bounds[1], _drawn->bounds[2] }; upper = { _drawn->bounds[3], _drawn->bounds[4], _drawn->bounds[5] }; }
        const kor::Vec3 centre = (lower + upper) * 0.5f;
        // A sphere fills its box less than anything else fills one; with nothing round it, it fills the picture.
        const float radius = std::max(kor::Length(upper - lower) * (_showAround ? 0.5f : 0.31f), 1e-4f);
        kgraph::View view;
        view.fieldOfView = 0.6f;
        const float aspect = static_cast<float>(_extent.x) / static_cast<float>(std::max(_extent.y, 1u));
        const float fit = radius / std::sin(view.fieldOfView * 0.5f * std::min(aspect, 1.f)) * 1.05f * _distance;
        const kor::Vec3 from = centre + kor::Vec3(std::cos(_pitch) * std::sin(_yaw), std::sin(_pitch), std::cos(_pitch) * std::cos(_yaw)) * fit;
        view.near = std::max(fit - radius * 3.f, fit * 0.01f);
        view.far = fit + radius * 4.f;
        view.view = kor::LookAt(from, centre);
        _runtime->SetView(view);
        _runtime->Update();
    }

    kgraph::Value MaterialPreview::FieldOf(const std::string_view field) const
    {
        const kecs::ComponentId id = _shown ? _world.Find(_shown->kind.name) : kecs::NoComponent;
        return id == kecs::NoComponent || _entity.IsNull() || !_world.Has(_entity, id) ? kgraph::Value {} : Document::ReadField(_world, _entity, id, field);
    }

    kor::ResourceRef<const kor::Image> MaterialPreview::PictureOf(const std::string_view id) const
    {
        return _runtime ? _runtime->ImageOf(id) : kor::ResourceRef<const kor::Image>();
    }

    bool MaterialPreview::Settled() const
    {
        if (!_runtime || !_shown || _dirty || _changed || !_runtime->Ok()) return false;
        return std::ranges::none_of(_pictures, [](const std::shared_ptr<kgraph::Resource>& held) {
            const kgraph::Resource* now = held.get();
            while (now && now->replacedBy) now = now->replacedBy.get();
            return now && now->pending;
        });
    }
    kor::ResourceRef<const kor::Image> MaterialPreview::Picture() const { return _runtime && _shown ? _runtime->ImageOf("shot") : kor::ResourceRef<const kor::Image>(); }
    std::shared_ptr<kgraph::Resource> MaterialPreview::PictureHeld() const { return _runtime && _shown ? _runtime->ResourceOf("shot") : nullptr; }
    std::optional<std::vector<std::byte>> MaterialPreview::Texels() const
    {
        if (!_runtime) return std::nullopt;
        auto peek = _runtime->Watched("shot");
        if (!peek) return std::nullopt;
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(peek->bytes.data()), reinterpret_cast<const std::byte*>(peek->bytes.data()) + peek->bytes.size());
    }
    std::string MaterialPreview::Problem() const
    {
        if (!_problem.empty() || !_runtime) return _problem;
        const auto problems = _runtime->Problems();
        return problems.empty() ? std::string() : problems.front();
    }
}
