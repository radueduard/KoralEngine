#pragma once

/**
 * @file runtime.h
 * @brief A graph, running: its nodes built into GPU objects, and its passes in a frame graph.
 *
 * @code
 * kgraph::Runtime runtime(scene.Graph());       // the scene's frame graph
 * runtime.Set(graph);                           // builds what is new or changed, and nothing else
 * // each frame, before the frame graph runs:
 * runtime.Update();                             // picks up shaders edited since
 * @endcode
 *
 * Set works out every node in the order its wires need, and builds a node again only when what goes into it has
 * changed — its inputs' values, or an object it is wired to having been rebuilt. A node that cannot be built says
 * why (Status), and so does everything that needed it; the rest of the graph runs.
 *
 * Passes run in the order their resources need, not the order they are in: one that writes an image runs before
 * one that reads it. Each resource a pass uses is imported into the frame graph under its node's id.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <kmath/vector.h>
#include <optional>
#include <set>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <image.h>
#include <resource.h>

#include <kecs/kecs.h>

#include "assets.h"
#include "catalog.h"
#include "graph.h"
#include "meshes.h"
#include "view.h"

struct KoralResource;
namespace kor { class FrameGraph; class DebugDraw; }

namespace kgraph
{
    /**
     * @brief A GPU object a node made. Shared by whoever uses it — the nodes wired to it, the passes that run with
     *        it — and freed when none does.
     */
    struct Resource {
        KoralResource* handle = nullptr;    ///< Owned. Null for the screen.
        std::string kind;                   ///< "Image", "Shader", "ComputePipeline"...
        std::string node;                   ///< The node that made it.
        bool screen = false;                ///< The scene's window, which is the frame graph's to give.
        std::string label;                  ///< What a person calls it, when it has such a name: a model's mesh, as its file names it.
        std::shared_ptr<Resource> source;   ///< For a view: what it is a view of.
        /// What its node made in its place, when the node was built again: whoever holds this one is after that one.
        std::shared_ptr<Resource> replacedBy;
        /// A mesh's: the box it is in, least corner then greatest — when it is known (one made of numbers: a default
        /// mesh, a model's). What a Draw's `cull` leaves out by.
        float bounds[6] { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
        bool bounded = false;
        /// A mesh that is a part of another — a model's mesh, of the one mesh its model is on the device (source):
        /// which of that mesh's indices are its, and what they count from. It has no handle of its own.
        bool part = false;
        /// It stands in for a file that is still being read: a white texel for a picture, nothing for a mesh.
        bool pending = false;
        std::uint32_t firstIndex = 0, indexCount = 0;
        std::int32_t vertexOffset = 0;
        std::uint32_t vertexCount = 0;      ///< A part's: how many of the whole's vertices, from vertexOffset, are its.

        Resource() = default;
        Resource(const Resource&) = delete;
        Resource& operator=(const Resource&) = delete;
        ~Resource();
    };

    /**
     * @brief A node an editor offers for one of the world's components, made of a node type there is: `PointLight`, a
     *        ForEach of it; `PointLight Buffer`, a BufferBuilder laid out as an element of that ForEach is. Nothing of
     *        it is written in the engine: there is one for every component with a number or vector field — the
     *        engine's package's, a project's scripts', a pipeline's.
     */
    struct Preset {
        std::string name;           ///< What it is offered as, and the title a node made from it has.
        std::string type;           ///< The node type it is: "ForEach", "BufferBuilder".
        std::string component;      ///< The component it is of.
        std::string doc;
        std::vector<std::pair<std::string, Value>> inputs;  ///< What the node is given to start with.
    };

    /** @brief How a node fared when the graph was last set. */
    struct NodeStatus {
        bool ok = false;
        std::string error;          ///< Why not, when not ok.
        std::uint64_t builds = 0;   ///< How many times it has been built: unchanged by a Set that changed nothing of it.
    };

    /** @brief What a pass that runs for each entity did, the last frame it was readied for. */
    struct PassReport {
        std::size_t ran = 0;        ///< Entities it runs for.
        std::size_t skipped = 0;    ///< Entities that have what it asks for, and that it cannot run for.
        std::size_t culled = 0;     ///< Entities a Draw left out for being out of sight (its `cull`).
        /// Which entities it could not run for, and why each: the first few hundred of them.
        std::vector<std::pair<kecs::Entity, std::string>> passedBy;
        std::string why;            ///< Why the first of those: a parameter with nothing in it, a mesh that does not fit.
    };

    /** @brief What a node made, copied out where it can be looked at: an image's texels, or a buffer's bytes. */
    struct Peek {
        /** One value of a buffer's element, for a buffer whose shader says what it is made of (a ShaderBuffer). */
        struct Field {
            std::string name;
            std::uint32_t offset = 0;       ///< In bytes, from the start of an element.
            char scalar = 'f';              ///< 'f' a float, 'i' an int, 'u' a uint, 'b' a bool: four bytes each.
            std::uint32_t numbers = 1;      ///< How many of them, one after another.
        };
        bool image = false;                 ///< An image; a buffer when not.
        std::string format;                 ///< An image's format, by name: `eRGBA16_SFLOAT`.
        kor::UVec3 extent { 0, 0, 0 };      ///< The size of what was copied: the image's, at @ref mip.
        // An image's, all of it: what it is besides the part that was copied.
        kor::UVec3 fullExtent { 0, 0, 0 };  ///< At its first level (of what a view sees: from its first level).
        std::uint32_t mips = 1;             ///< Its levels (a view's: from its first).
        std::uint32_t layers = 1;           ///< Its array layers: six for a cube.
        std::uint32_t samples = 1;
        std::uint32_t mip = 0, layer = 0;   ///< Which level and layer were copied.
        std::string type;                   ///< "1D", "2D" or "3D".
        std::vector<std::string> usage;     ///< Its usage flags, by name: `eSampled`.
        std::uint64_t byteSize = 0;         ///< A buffer's size; an image's, every level and layer of it.
        std::uint32_t stride = 0;           ///< A ShaderBuffer's: the bytes of one element. 0 for any other buffer.
        std::uint32_t count = 0;            ///< A ShaderBuffer's: how many elements it holds.
        std::vector<Field> fields;
        std::vector<std::byte> bytes;       ///< As they were a frame or two ago.
    };

    /** @brief How a format's texels are laid out, from its name: `eRGBA16_SFLOAT` is four channels of sixteen bits, floats. */
    struct TexelLayout {
        int channels = 0;                   ///< 0 for a format that is not one channel after another of one size.
        int bits = 0;                       ///< Of each channel.
        char kind = 'u';                    ///< 'u' 0 to 1, 's' -1 to 1, 'U' a whole number, 'I' a signed one, 'f' a float.
        bool depth = false;
        [[nodiscard]] std::size_t Bytes() const { return static_cast<std::size_t>(channels * bits / 8); }
    };
    [[nodiscard]] TexelLayout TexelLayoutOf(std::string_view format);

    class Runtime {
    public:
        /**
         * @brief Runs its passes in @p frameGraph, which must outlive it.
         * @param world Where the entities are that pipelines run for, and where each pipeline's component is
         *              registered. It must outlive the runtime too. With none, a pass that runs per entity is an error.
         */
        explicit Runtime(kor::FrameGraph& frameGraph, kecs::World* world = nullptr);
        /**
         * @brief Another view of what @p source runs: the same graph, Set from source.Current() whenever its Revision
         *        goes up, over the same world, into @p frameGraph — seen from where SetView says, with what SetSelected
         *        says picked. Every pass and every resource of a node is its own: a picture the size of its screen,
         *        what a pass works out from where it is seen. What is in files, the default meshes and pictures and
         *        the meshes made with MakeMesh are @p source's — asked for here, they are asked of it, and nothing is
         *        read twice. @p source must outlive it.
         */
        Runtime(kor::FrameGraph& frameGraph, kecs::World* world, Runtime& source);
        ~Runtime();
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        /** @brief Makes @p graph what runs. On the frame graph's thread, between frames. */
        void Set(Graph graph);
        /** @brief The graph as it was set: with its loops as loops. What runs has each written out, a node a turn. */
        [[nodiscard]] const Graph& Current() const { return _graph; }
        /** @brief Goes up each time a graph is Set: what a runtime that follows this one is Set again by. */
        [[nodiscard]] std::uint64_t Revision() const { return _revision; }
        /**
         * @brief What a node of a loop — one wired from a Repeat — is on each of its turns, by id: `down [0]`,
         *        `down [1]`. Each is asked about like any node; asked about by its own id, the node answers for the
         *        first of them. Empty for a node that is in no loop.
         */
        [[nodiscard]] std::vector<std::string> TurnsOf(std::string_view id) const;
        /**
         * @brief The ForEach (or Filter, or Entities node) a node is worked out once an element of, by id: it itself, a
         *        value worked out of one, a buffer (a BufferBuilder, a ShaderBuffer) with an element for each of its
         *        elements, or a Draw or a Dispatch that draws each. Empty for a node that follows none.
         */
        [[nodiscard]] std::string EachOf(std::string_view id) const;
        /** @brief The nodes there are for the world's components, by name: two each (see Preset). None without a world. */
        [[nodiscard]] std::vector<Preset> Presets() const;
        /**
         * @brief Each frame, before the frame graph runs. Builds again what depends on a shader that was edited,
         *        and readies the passes that run per entity: which entities, and each one's descriptor set.
         */
        void Update();

        /**
         * @brief Keeps @p resource until the frame that may be drawing with it is done: for whoever takes one from
         *        where a pass could find it — an entity's field — in the middle of a frame. What a graph's own edits
         *        replace is kept so already.
         */
        void Retire(std::shared_ptr<Resource> resource);

        // ---- what is in files ---------------------------------------------------------------------------------------

        /** @brief Where a file named without its whole path is looked for: a project's `assets` folder. */
        void SetAssetDirectory(std::filesystem::path directory);
        [[nodiscard]] const std::filesystem::path& AssetDirectory() const;
        /**
         * @brief What a file — a picture, or with @p mesh a mesh of a model — is found by, as ResourceOf takes it:
         *        `@wall.png`, `@sponza/Sponza.gltf#3`. Under the asset directory it is named from there, so that a
         *        project moved keeps its files; anywhere else, by its whole path.
         */
        [[nodiscard]] std::string AssetId(const std::filesystem::path& file, int mesh = -1) const;
        /**
         * @brief What the model file at @p path holds, once it is read: asked for the first time, it starts being
         *        read off the frame, and this is null until it is. Null with @p error saying why, once it is known to
         *        be no model.
         */
        [[nodiscard]] std::shared_ptr<const Model> ModelOf(const std::filesystem::path& path, std::string* error = nullptr) const;

        /**
         * @brief Makes a mesh of @p data — any attributes, in the buffers it says — found by @p id from now on (a
         *        script's are `$script/<name>`). One there already by that id is replaced: what held it holds this one.
         *        False, and @p error says why, when @p data is no mesh.
         */
        /**
         * @brief What a model file's meshes have, every attribute, read or not — before it is imported: the file is
         *        read off the frame for it, and nothing of it is made. Nothing until that is done (asked again, it is not
         *        read again); and nothing, with @p error saying why, for what is no model. Of a file read already, at once.
         */
        [[nodiscard]] std::optional<std::vector<Model::Attribute>> ModelAttributes(const std::filesystem::path& path, std::string* error = nullptr) const;
        bool MakeMesh(const std::string& id, const VertexData& data, std::string* error = nullptr);
        /**
         * @brief A mesh's numbers, read back from the device: every attribute it has (of four-byte numbers), in the
         *        buffers it has them in, and its indices — a part of a model's, only its own. Waits for the device.
         *        Nothing, and @p error says why, for what is no mesh, or is still being read.
         */
        [[nodiscard]] std::optional<VertexData> ReadMesh(std::string_view id, std::string* error = nullptr) const;
        /**
         * @brief How many files are still being read. A file is read off the frame: until it is, a picture asked for
         *        is a white texel and a mesh is nothing, so what is drawn with them fills in as they arrive.
         */
        [[nodiscard]] std::size_t Reading() const;
        /**
         * @brief Every picture and mesh of a file there is so far, by the name it is found by (`@...`): what entities
         *        and nodes asked for, and every mesh of a model that was read. Each with ResourceOf, which is a
         *        stand-in (Resource::pending) while the file is being read.
         */
        [[nodiscard]] std::vector<std::string> Assets() const;
        /** @brief Why each file that could not be read could not. */
        [[nodiscard]] std::vector<std::string> Unread() const;

        // ---- looking at what a node made ----------------------------------------------------------------------------

        /**
         * @brief From now on, copies what node @p id makes — its image or its buffer — out each frame, to be read
         *        with Watched. An image needs eTransferSrc among its usage, and so does a Buffer node's buffer.
         *        It costs a copy a frame while it is watched: Unwatch when done.
         */
        void Watch(std::string_view id);
        /**
         * @brief Watches an image's level @p mip and array layer @p layer, rather than its first: watched already, the
         *        copy is made of those from a frame or two on. A level or layer it has not is its last.
         */
        void Watch(std::string_view id, std::uint32_t mip, std::uint32_t layer);
        /**
         * @brief Copies a watched node's image or buffer out once every @p frames frames, rather than every frame: what a
         *        thumbnail needs, at a fraction of the cost. Watches spread out over those frames, so that many of them
         *        are not all copied on the same one. Changing how often builds nothing again. 1: every frame.
         */
        void WatchEvery(std::string_view id, std::uint32_t frames);
        void Unwatch(std::string_view id);
        /** @brief What a watched node held a frame or two ago; nothing when it is not watched, or not copied yet. */
        [[nodiscard]] std::optional<Peek> Watched(std::string_view id) const;
        /** @brief Why a watched node cannot be looked at; empty when it can. */
        [[nodiscard]] std::string WatchProblem(std::string_view id) const;

        // ---- the frame, as whoever runs the graph has it ------------------------------------------------------

        /**
         * @brief What the main View sees from, and the lens of a Perspective wired from it, in place of what the graph
         *        gives them: an editor's view. With none (the default), what the graph gives.
         */
        void SetView(std::optional<View> view);
        /**
         * @brief Of a runtime that follows another: the Buffer nodes, by id, whose buffer is the source's own rather than
         *        one of its own — those something outside the graph writes into (a graphics script), so that what it
         *        writes reaches both. Taken again whenever the source builds another. Nothing for one that follows none.
         */
        void Share(const std::set<std::string>& nodes);
        /** @brief The entity that is picked: what a push constant named `selected` is given (its number; 0 for none). */
        void SetSelected(kecs::Entity entity);
        /** @brief How big the scene's window is, each frame: what a Screen node gives as its extent. Sized by it, an image follows the window. */
        void SetScreenExtent(kor::UVec2 extent);
        /** @brief Where a DebugDraw node's shapes come from: the scene's. Null, the default, draws none. */
        void SetDebugDraw(kor::DebugDraw* debug);
        /**
         * @brief What the scene was last seen through: the main View's matrix and the lens wired from it — its field of
         *        view, near and far when it is a Perspective — the two as one, and its picture's aspect.
         */
        [[nodiscard]] View CurrentView() const;
        [[nodiscard]] kor::Mat4 ViewProjection() const;
        [[nodiscard]] float Aspect() const;
        /**
         * @brief Asks which entity is drawn at a place in the picture — @p u across and @p v down, each from 0 to
         *        1 — through the graph's Pick node. @p then hears, a few frames on: it, or NullEntity. False when
         *        the graph has no Pick node that is built.
         */
        bool Pick(float u, float v, std::function<void(kecs::Entity)> then);

        /** @brief How node @p id fared, or null when there is none. */
        [[nodiscard]] const NodeStatus* Status(std::string_view id) const;
        /** @brief Whether every node is ok. */
        [[nodiscard]] bool Ok() const;
        /** @brief Every node that is not ok, with why: "id: error". */
        [[nodiscard]] std::vector<std::string> Problems() const;

        /**
         * @brief What node @p id 's @p output is, when it is a resource; null otherwise. A default mesh is found
         *        the same way, with no node for it: `$cube`, `$sphere` (kgraph::DefaultMeshId), made when first asked for.
         *        So are three pictures of one texel — `$white`, `$black`, `$normal` — and what is in a file: `@wall.png`
         *        is that picture, and `@model.gltf#3` the fourth mesh of that model. @see assets.h
         */
        [[nodiscard]] std::shared_ptr<Resource> ResourceOf(std::string_view id, std::string_view output = "out") const;
        /** @brief The same, as the image it is; empty when it is not one. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> ImageOf(std::string_view id, std::string_view output = "out") const;
        /** @brief What node @p id 's @p output is, when it is a value; null otherwise. */
        [[nodiscard]] Value ValueOf(std::string_view id, std::string_view output = "out") const;

        /**
         * @brief Every input node @p id has: its type's, and for a pipeline those its shaders give it — `param.<name>`
         *        and `constant.<name>` — once they are built. A ShaderBuffer has a `field.<name>` for each field of
         *        the struct its shader declares the buffer of.
         */
        [[nodiscard]] std::vector<Port> PortsOf(std::string_view id) const;
        /**
         * @brief Every output node @p id has: its type's, and for an Entities node a `field.<name>` for each field
         *        of its component, once it is built.
         */
        [[nodiscard]] std::vector<Port> OutputsOf(std::string_view id) const;
        /**
         * @brief A pipeline node's parameters nobody wired: the ones each entity that runs it will supply.
         *        Empty when everything is set, and the pipeline runs once.
         */
        [[nodiscard]] std::vector<std::string> PerEntityParameters(std::string_view id) const;

        /**
         * @brief The component generated for pipeline node @p id: what an entity has, to be run by it. It is named
         *        after the node, with a field for each parameter left unwired and each push constant. NoComponent
         *        when the pipeline has neither, and so runs once.
         */
        [[nodiscard]] kecs::ComponentId ComponentOf(std::string_view id) const;
        /** @brief What became of that component's fields when the pipeline last changed its shape; null when it never did. */
        [[nodiscard]] const kecs::Migration* MigrationOf(std::string_view id) const;
        /**
         * @brief Every entity a pass could not run for this frame, with which pass and why: "draw forward: 'albedoMap'
         *        has nothing in it". What an editor says on the entity itself, where it is mended.
         */
        [[nodiscard]] std::vector<std::pair<kecs::Entity, std::string>> PassedBy() const;
        /**
         * @brief How a Draw or a Dispatch wired from a ForEach fared this frame: a draw an element. Null for another kind
         *        of node, and for one that draws once and drew.
         */
        [[nodiscard]] const PassReport* ReportOf(std::string_view id) const;
        /**
         * @brief The inputs taken off pipeline nodes because an edited shader no longer has them, or has them as
         *        another kind: "node.input", in the order it happened. Current() no longer has them. Emptied by the call.
         */
        [[nodiscard]] std::vector<std::string> TakeReset();

    private:
        struct Impl;
        void Run();
        std::unique_ptr<Impl> _impl;
        Graph _graph;
        Graph _running;     // the graph with its loops written out: what is evaluated
        std::uint64_t _revision = 0;
    };
}
