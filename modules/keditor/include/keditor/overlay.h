//
// keditor: what an editor draws over a scene, and reads back of it, whatever the scene's own graph is. Which entity
// is under the pointer, the line round the one that is picked, and the editor's shapes — a gizmo, a camera's reach —
// are no part of a scene: they are made here, by a few passes of the editor's own that run beside the scene's, from
// every entity with a Mesh as the editor's view sees it. So a scene whose graph was written without a thought for an
// editor is picked from and outlined all the same, and what a project runs has nothing of the editor in it.
//

#pragma once

#include <functional>
#include <memory>
#include <optional>
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
        /**
         * @brief Asks which entity is drawn at a place in the picture, @p u across and @p v down, each from 0 to 1.
         *        @p then hears a few frames on: it, or NullEntity. False when the passes could not be made.
         */
        bool Pick(float u, float v, std::function<void(kecs::Entity)> then);

        /** @brief What goes over the scene's picture: the line round what is picked and the editor's shapes, on nothing. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> Picture() const;
        /** @brief The same, as something to hold: whoever shows the picture keeps it until it has stopped showing it. */
        [[nodiscard]] std::shared_ptr<kgraph::Resource> PictureHeld() const;
        /** @brief The view it last drew with, as one matrix, and its picture's aspect: what a gizmo is placed by. */
        [[nodiscard]] kor::Mat4 ViewProjection() const;
        [[nodiscard]] float Aspect() const;
        /** @brief Why its passes could not be made; empty when they were. */
        [[nodiscard]] std::string Problem() const;

    private:
        std::unique_ptr<kgraph::Runtime> _runtime;
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

        /** @brief Asks for a picture of @p mesh, to be found by @p id. Asked again, nothing more is done. */
        void Want(const std::string& id, std::shared_ptr<kgraph::Resource> mesh);
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
