//
// keditor: what an editor draws over a scene, and reads back of it, whatever the scene's own graph is. Which entity
// is under the pointer, the line round the one that is picked, and the editor's shapes — a gizmo, a camera's reach —
// are no part of a scene: they are made here, by a few passes of the editor's own that run beside the scene's, from
// every entity with a Mesh as the editor's view sees it. So a scene whose graph was written without a thought for an
// editor is picked from and outlined all the same, and what a project runs has nothing of the editor in it.
//
// So are the editor's other ways of looking at a scene — its edges, its shape in clay, its normals, a grid under it —
// but those are graphs in files (packages/koral/editor/scene-view.kgraph, and a project's `editor/*.kgraph`), each
// DebugView of them one more way the Scene panel can show the scene.
//

#pragma once

#include <kgraph/materials.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <image.h>
#include <resource.h>

#include <kecs/kecs.h>
#include <kgraph/runtime.h>
#include <kgraph/view.h>

namespace kor { class FrameGraph; class DebugDraw; }

namespace keditor
{
    /** @brief The grid the editor draws in the scene, as its settings have it: a plane through the origin. */
    struct Grid {
        bool shown = true;
        float cell = 1.f;           ///< Metres between its lines.
        float major = 10.f;         ///< A stronger line every so many cells.
        float fade = 80.f;          ///< Metres off at which it is gone.
        float opacity = 1.f;
        enum class Plane : int { eXZ = 0, eXY = 1, eYZ = 2 } plane = Plane::eXZ;
        bool depthTested = true;    ///< Hidden behind what is drawn in front of it.

        bool operator==(const Grid&) const = default;
    };

    class Overlay {
    public:
        /** @brief Runs in @p frameGraph, beside the scene's passes, over the entities of @p world. Both must outlive it. */
        Overlay(kor::FrameGraph& frameGraph, kecs::World& world);
        ~Overlay();
        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        /** @brief Each frame, before the frame graph runs: how big the picture is, and whose shapes are drawn over it. */
        void Update(kor::UVec2 extent, kor::DebugDraw* debug);
        /** @brief Where the scene is looked at from: the editor's view, or — with none — the scene's own camera. */
        void SetView(std::optional<kgraph::View> view);
        /** @brief The entity a line is drawn round. */
        void SetSelected(kecs::Entity entity);
        /** @brief Entities neither drawn for picking nor picked: hidden in the editor's view, or being dragged onto a surface. */
        void SetHidden(std::set<kecs::Entity> hidden);
        /**
         * @brief Asks which entity is drawn at a place in the picture, @p u across and @p v down, each from 0 to 1.
         *        @p then hears a few frames on: it, or NullEntity. False when the passes could not be made.
         */
        bool Pick(float u, float v, std::function<void(kecs::Entity)> then);
        /** @brief Pick, with the depth round the pixel: where on the surface there it is (SurfaceOf). */
        bool PickAt(float u, float v, std::function<void(const kgraph::Runtime::PickHit&)> then);

        /** @brief What goes over the scene's picture: the line round what is picked and the editor's shapes, on nothing. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> Picture() const;
        /** @brief The same, as something to hold: whoever shows the picture keeps it until it has stopped showing it. */
        [[nodiscard]] std::shared_ptr<kgraph::Resource> PictureHeld() const;
        /** @brief The view it last drew with, as one matrix, and its picture's aspect: what a gizmo is placed by. */
        [[nodiscard]] kor::Mat4 ViewProjection() const;
        [[nodiscard]] float Aspect() const;
        /** @brief Why its passes could not be made; empty when they were. */
        [[nodiscard]] std::string Problem() const;

        // ---- the editor's ways of looking at the scene: graphs in files ----------------------------------------

        /** @brief The engine's own: packages/koral/editor/scene-view.kgraph, which is read first, and has the grid. */
        [[nodiscard]] static std::filesystem::path EngineViews();
        /**
         * @brief Runs, besides the engine's, every `*.kgraph` in @p directory (a project's `editor` folder): each
         *        DebugView of them is listed after the engine's. Another directory, or none, takes those it ran away.
         */
        void SetProjectViews(const std::filesystem::path& directory);
        /** @brief One of the DebugViews of those graphs: which graph — its file — its node and its name. */
        struct View {
            std::filesystem::path file;
            std::string id;
            std::string name;
        };
        /** @brief Every DebugView of the graphs it runs: the engine's, then the project's, each in its graph's order. */
        [[nodiscard]] std::vector<View> Views() const;
        /** @brief Shows the DebugView @p id of the graph read from @p file; empty, none. */
        void ShowView(const std::filesystem::path& file, const std::string& id);
        /** @brief The picture of the DebugView shown, to hold while it is shown; null with none. */
        [[nodiscard]] std::shared_ptr<kgraph::Resource> ViewPictureHeld() const;
        /** @brief How the grid is drawn: the engine's graph's `grid shown` and its grid Draw's push constants. */
        void SetGrid(const Grid& grid);
        /** @brief The grid, on nothing, as big as the scene's picture: to go over it. Null while it is not shown. */
        [[nodiscard]] std::shared_ptr<kgraph::Resource> GridHeld() const;
        /** @brief What is wrong with those graphs, each said with its file: one not read, a node that failed. */
        [[nodiscard]] std::vector<std::string> ViewProblems() const;

    private:
        struct ViewGraph;
        void ReadViews(ViewGraph& graph);
        void ApplyGrid();

        kor::FrameGraph& _frameGraph;
        kecs::World& _world;
        std::unique_ptr<kgraph::Runtime> _runtime;
        std::vector<std::unique_ptr<ViewGraph>> _views;     // the engine's first
        std::vector<std::pair<std::unique_ptr<ViewGraph>, int>> _retired;   // until the frames that drew with them are done
        std::optional<std::filesystem::path> _projectViews;                  // asked for, taken up at the next Update
        std::filesystem::path _projectViewsNow;
        std::optional<kgraph::View> _view;
        Grid _grid;
        bool _gridAsked = false;
        std::uint32_t _frames = 0;
    };

    /** @brief A material to be drawn: its kind, and the values it has of its fields. What it has not is as the kind starts. */
    struct MaterialShown {
        kgraph::MaterialKind kind;
        std::vector<std::pair<std::string, kgraph::Value>> values;
        /// Where the kinds' modules are (kgraph::Runtime::GeneratedShaderDirectory): the scene's, and not another folder of
        /// the same, or every shader that imports one would find it in a new place, and be built again.
        std::filesystem::path modules;
    };

    /**
     * @brief A material on a mesh, in the middle of a cube map's light, looked at from wherever it is orbited to: what the
     *        material inspector shows, and what small pictures of materials are drawn by. Any kind that has a shading
     *        module (`<Kind>Shading`, see koral_shading.slang) is drawn: a module of a few lines is generated that names it
     *        and the shading, and the rest is koral_material_preview. Its passes and its world are its own, as MeshViewer's.
     */
    class MaterialPreview {
    public:
        /** @brief What gives the resource of an id that is no file's, nor one of the engine's: a node of the scene's graph. */
        using Find = std::function<std::shared_ptr<kgraph::Resource>(const std::string&)>;

        /** @brief @p showAround: what is behind the mesh is the cube map; otherwise nothing, for a small picture on a panel. */
        MaterialPreview(kor::FrameGraph& frameGraph, kor::UVec2 extent, bool showAround);
        ~MaterialPreview();
        MaterialPreview(const MaterialPreview&) = delete;
        MaterialPreview& operator=(const MaterialPreview&) = delete;

        /** @brief Shows this material; nothing shows nothing. Its passes are made by Update, as MeshViewer's are. */
        void Show(std::optional<MaterialShown> shown);
        /** @brief What it is on; null, the sphere. */
        void SetMesh(std::shared_ptr<kgraph::Resource> mesh);
        void SetFind(Find find) { _find = std::move(find); }
        void SetAssetDirectory(std::filesystem::path directory);
        void SetExtent(kor::UVec2 extent);
        /** @brief Orbits it, by @p across (about the up axis) and @p up (over it), in radians. */
        void Turn(float across, float up);
        /** @brief Nearer (more than 0) or further. */
        void Zoom(float steps);
        /** @brief What the material shown holds of @p field, as it is drawn: what it was given, or what its kind starts it as. */
        [[nodiscard]] kgraph::Value FieldOf(std::string_view field) const;
        /** @brief The picture of @p id as this reads it: for a field that holds one to show it. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> PictureOf(std::string_view id) const;
        /** @brief Each frame, before the frame graph runs. */
        void Update();
        /** @brief Whether what was asked for is what is being drawn: built, and its pictures read. */
        [[nodiscard]] bool Settled() const;
        [[nodiscard]] kor::ResourceRef<const kor::Image> Picture() const;
        [[nodiscard]] std::shared_ptr<kgraph::Resource> PictureHeld() const;
        /** @brief The picture's texels, as last read back, four bytes each; null until they are. Read back only with SetReadBack. */
        [[nodiscard]] std::optional<std::vector<std::byte>> Texels() const;
        void SetReadBack(bool read) { _readBack = read; }
        [[nodiscard]] std::string Problem() const;

    private:
        void Build();
        void Apply();

        struct Cube;
        kor::FrameGraph& _frameGraph;
        kecs::World _world;
        kecs::Entity _entity = kecs::NullEntity;
        std::unique_ptr<kgraph::Runtime> _runtime;
        std::shared_ptr<Cube> _cube;
        std::optional<MaterialShown> _shown;
        std::shared_ptr<kgraph::Resource> _mesh, _drawn;
        std::vector<std::shared_ptr<kgraph::Resource>> _pictures;
        std::filesystem::path _assets;
        Find _find;
        kor::UVec2 _extent, _wanted;
        int _settling = 0;      // frames the size asked for has held: a panel being dragged is not made again at each
        float _yaw = 0.785f, _pitch = 0.5f, _distance = 1.f;
        bool _showAround, _readBack = false, _dirty = true, _changed = false, _emptied = false;
        std::string _kind;
        std::string _problem;
    };

    /**
     * @brief Small pictures of meshes, for a list of them to show: each mesh drawn by itself, seen from a corner far
     *        enough off to see all of it, a few frames a mesh while there are any still to draw. The editor's own
     *        passes, like Overlay's, in a frame graph it is given.
     */
    class Thumbnails {
    public:
        explicit Thumbnails(kor::FrameGraph& frameGraph);
        ~Thumbnails();
        Thumbnails(const Thumbnails&) = delete;
        Thumbnails& operator=(const Thumbnails&) = delete;

        /**
         * @brief Asks for a picture of @p mesh, to be found by @p id. Asked again, nothing more is done. With @p kept — a
         *        key of what the mesh is made of — and a cache (SetCache), it is read from there when it was drawn
         *        before, by this editor or another, and kept there once it is drawn.
         */
        void Want(const std::string& id, std::shared_ptr<kgraph::Resource> mesh, const std::string& kept = {});
        /** @brief Where pictures are kept between runs: a project's `.koral/cache/thumbnails`. Empty: nowhere. */
        void SetCache(std::filesystem::path directory);
        /** @brief Where a file named without its whole path is: a project's `assets` folder. What is read for a picture is read from here, apart from the scene. */
        void SetAssetDirectory(std::filesystem::path directory);
        /**
         * @brief Asks for a picture of a sphere (@p sphere, its mesh) of the material @p shown, found by @p id; kept as Want
         *        keeps one. Drawn by the material's kind's own shading, as the material inspector draws it.
         */
        void WantMaterial(const std::string& id, std::shared_ptr<kgraph::Resource> sphere, MaterialShown shown, const std::string& kept = {});
        /**
         * @brief Asks for a picture of all of the model file @p file, seen from a corner; kept as Want keeps one. Read for
         *        it here, and let go: the scene has nothing of it.
         */
        void WantModel(const std::string& id, std::filesystem::path file, const std::string& kept = {});
        /** @brief Asks for a picture of the picture file @p file, read off the frame and made small; kept as Want keeps one. */
        void WantFile(const std::string& id, std::filesystem::path file, const std::string& kept = {});
        /** @brief Whether @p id has a picture, or one was kept under @p kept (which it now has): nothing has to be read to show it. */
        bool Has(const std::string& id, const std::string& kept);
        /** @brief Each frame, before the frame graph runs. True when a picture was made since the last call. */
        bool Update();
        /** @brief The picture of the mesh asked for by @p id; empty until it is made. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> Of(std::string_view id) const;

    private:
        struct State;
        std::unique_ptr<State> _state;
    };

    /**
     * @brief One mesh by itself, drawn as big as it is shown and turned about as whoever looks at it likes: what a
     *        mesh is looked at in. Its own passes, like Thumbnails', in a frame graph it is given; they run only while
     *        it has a mesh to show. Its pipeline is made for the mesh's own vertex layout, so any of the mesh's
     *        attributes can be what is shown, and its edges are drawn by the triangles themselves, smooth and as wide
     *        as asked.
     */
    class MeshViewer {
    public:
        /** @brief What colours the mesh: light on it, nothing, or one of its attributes (ShowAttribute). */
        enum class Shading : std::uint8_t { eLit, ePlain, eAttribute };
        /** @brief How an attribute's values are a colour: 0 to 1 as they are, -1 to 1 (a normal, a tangent), or a checker of a UV. */
        enum class Mapping : std::uint8_t { eUnit, eSigned, eChecker };
        /** @brief One attribute of a vertex the mesh has: what it is, its channels' type, and how many. */
        struct Attribute {
            std::string semantic;
            std::string type;           ///< "float", "int" or "uint".
            std::uint32_t count = 0;
        };

        explicit MeshViewer(kor::FrameGraph& frameGraph);
        ~MeshViewer();
        MeshViewer(const MeshViewer&) = delete;
        MeshViewer& operator=(const MeshViewer&) = delete;

        /**
         * @brief Shows @p mesh, seen whole; null shows nothing. The same mesh again changes nothing. Its passes are
         *        made and taken away by Update, never here: this may be called while a frame is being recorded (a
         *        button pressed), and what that frame has recorded of them must still be there when it is sent. Taken
         *        away, they are given nothing to run rather than destroyed, so what they made outlives the frames using it.
         */
        void Show(std::shared_ptr<kgraph::Resource> mesh);
        [[nodiscard]] const std::shared_ptr<kgraph::Resource>& Shown() const { return _mesh; }
        /** @brief The attributes of the mesh shown that can be shown: those with a semantic, of one to four numbers. */
        [[nodiscard]] std::vector<Attribute> Attributes() const;
        /** @brief How big the picture is drawn: as big as it is shown. */
        void SetExtent(kor::UVec2 extent);
        /** @brief Turns it about its middle, by @p across (about the up axis) and @p up (over it), in radians. */
        void Turn(float across, float up);
        /** @brief Nearer (more than 0) or further: each step a seventh of the way. */
        void Zoom(float steps);
        /** @brief Seen whole again, from the corner it is first seen from. */
        void Frame();
        /** @brief Lit or plain; eAttribute shows the attribute ShowAttribute last named. */
        void SetShading(Shading shading);
        /** @brief Colours it by its attribute @p semantic (`UV`, `TANGENT`), mapped as suits it: -1 to 1 for a direction, a checker for a UV. */
        void ShowAttribute(std::string semantic);
        void SetMapping(Mapping mapping);
        void SetWireframe(bool wireframe);
        /** @brief How wide its edges are drawn, in pixels. */
        void SetEdgeWidth(float pixels);
        [[nodiscard]] Shading CurrentShading() const { return _shading; }
        [[nodiscard]] const std::string& ShownAttribute() const { return _attribute; }
        [[nodiscard]] Mapping CurrentMapping() const { return _mapping; }
        [[nodiscard]] bool Wireframe() const { return _wireframe; }
        [[nodiscard]] float EdgeWidth() const { return _edgeWidth; }
        /** @brief How near it is: 1 seen whole, more nearer. */
        [[nodiscard]] float Nearness() const { return 1.f / _distance; }

        /** @brief Each frame, before the frame graph runs: makes its passes again when asked, or takes them away with nothing to show. */
        void Update();
        [[nodiscard]] kor::ResourceRef<const kor::Image> Picture() const;
        /** @brief The picture, to hold while it is shown. */
        [[nodiscard]] std::shared_ptr<kgraph::Resource> PictureHeld() const;
        [[nodiscard]] std::string Problem() const;

    private:
        void Build();

        kor::FrameGraph& _frameGraph;
        kecs::World _world;
        kecs::Entity _entity = kecs::NullEntity;
        std::unique_ptr<kgraph::Runtime> _runtime;
        std::shared_ptr<kgraph::Resource> _mesh;
        kor::UVec2 _extent { 512, 384 };
        float _yaw = 0.785f, _pitch = 0.5f, _distance = 1.f;
        Shading _shading = Shading::eLit;
        Mapping _mapping = Mapping::eUnit;
        std::string _attribute;
        float _edgeWidth = 1.f;
        bool _wireframe = false, _dirty = true;
        bool _emptied = false;      // its runtime has been given nothing to run, with nothing to show
    };
}
