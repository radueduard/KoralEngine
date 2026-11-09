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
#include <span>
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
#include "materials.h"
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
        /// A transient Image's or Buffer's: the frame graph makes it for the frame, sharing its memory with others never
        /// needed at the same time. What its node first made stands in for it until the frame graph has made it.
        bool transient = false;
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
        /// A mesh's (one of numbers, not a part): its vertex buffers, one for each binding, and its index buffer last when
        /// it has one — each a resource of its own, which a compute shader can read (eStorage). @see the MeshBuffers node
        std::vector<std::shared_ptr<Resource>> buffers;

        Resource() = default;
        Resource(const Resource&) = delete;
        Resource& operator=(const Resource&) = delete;
        ~Resource();
    };

    /**
     * @brief A node an editor offers for one of the world's components, made of a node type there is: `PointLight`, a
     *        ForEach of it; `PointLight Buffer`, a BufferBuilder laid out as an element of that ForEach is. Nothing of
     *        it is written in the engine: there is one for every component with a number or vector field — the
     *        engine's package's, a project's scripts', a pipeline's — and a Script node for each ResourceScript.
     */
    struct Preset {
        std::string name;           ///< What it is offered as, and the title a node made from it has.
        std::string type;           ///< The node type it is: "ForEach", "BufferBuilder".
        std::string component;      ///< The component it is of. Empty for a Script node of a ResourceScript.
        std::string doc;
        std::vector<std::pair<std::string, Value>> inputs;  ///< What the node is given to start with.
        std::string group;          ///< What it is for, to be offered under: a function's purpose ("Physics"). Empty: by its kind.
        std::string keywords;       ///< More words it is found by: a function's tags.
    };

    /** @brief How a node fared when the graph was last set. */
    struct NodeStatus {
        bool ok = false;
        std::string error {};          ///< Why not, when not ok.
        std::uint64_t builds = 0;   ///< How many times it has been built: unchanged by a Set that changed nothing of it.
        std::string note {};           ///< Something said of it while it is ok: what it waits for — a script nothing runs yet.
    };

    /** @brief What a resource script fills: one of its resources as it was made, and its contents, to be written. */
    struct Contents {
        std::string name;               ///< The resource, as the script declares it: the field it is.
        std::string node;               ///< The Script node it is of.
        std::string kind;               ///< "Image" or "Buffer".
        std::string format;             ///< An image's, by name: "eRGBA8_UNORM".
        kor::UVec3 extent { 1, 1, 1 };  ///< An image's, of its first level.
        std::uint32_t layers = 1;       ///< An image's array layers.
        std::uint32_t texelBytes = 0;   ///< An image's: the bytes of one texel. @see TexelLayoutOf
        /// What to fill, zeros to begin with. An image's first level — a layer after another, a row after another, a texel
        /// after another, from the top left — its smaller levels made from it. A buffer's every byte.
        std::span<std::byte> bytes;
    };

    /**
     * @brief One of the resources a resource script makes: an output of the Script node that runs it, by its name. It is
     *        made as a node of its kind would be, of what the script's source gives it, and of what the Script node is
     *        given for it as `<name>.<input>`.
     */
    struct ScriptResource {
        std::string name;               ///< The field it is: the output, and what the node's inputs for it start with.
        std::string kind;               ///< The node type it is made as: "Image", "Buffer", "ImageView", "Sampler" — any builder's.
        /// What the source makes it with, by its kind's inputs: `format`, `extent`, `usage`. One that takes a resource —
        /// an ImageView's `image` — names another of the script's resources.
        std::vector<std::pair<std::string, Value>> settings;
    };

    /**
     * @brief A script that makes resources and fills them on the CPU: Images, Buffers, views of them, Samplers — what its
     *        source declares — the images' texels and the buffers' bytes written by it as they are made. A Script node
     *        names one, and gives each of its resources as an output. @see Runtime::SetResourceScript
     */
    struct ResourceScript {
        std::string name;               ///< Its class: what a Script node names it by.
        std::vector<ScriptResource> resources;          ///< What it makes, in the order it declares them.
        std::vector<kecs::FieldDescription> fields;     ///< Its settings: a `field.<name>` input of the Script node each.
        std::vector<std::byte> initial; ///< What its settings are when a node gives none, as its source says. Empty: zeros.
        std::size_t size = 0;           ///< Of its settings' bytes.
        /// Fills @p contents — each of its Images and Buffers, by name — given @p settings: its fields' bytes, as `fields`
        /// lays them out. Empty when it did, or why it could not. Null for a script declared, that nothing runs: what it
        /// makes is made as it is, empty, and the node says so.
        std::function<std::string(std::span<Contents> contents, std::span<const std::byte> settings)> fill;
    };

    /** @brief What a pass that runs for each entity did, the last frame it was readied for. */
    struct PassReport {
        std::size_t ran = 0;        ///< Entities it runs for.
        std::size_t skipped = 0;    ///< Entities that have what it asks for, and that it cannot run for.
        std::size_t culled = 0;     ///< Entities a Draw left out for being out of sight (its `cull`).
        bool unchanged = false;     ///< A Draw `whenChanged` that drew nothing this frame: what it would draw is what it drew last.
        /// Which entities it could not run for, and why each: the first few hundred of them.
        std::vector<std::pair<kecs::Entity, std::string>> passedBy;
        std::string why;            ///< Why the first of those: a parameter with nothing in it, a mesh that does not fit.
    };

    /** @brief A pass node's use of a resource: which node, on which page, and whether it reads it, writes it or both. */
    struct PassUse {
        std::string node;           ///< As it runs: a node of a loop is each of its turns, `down [0]`.
        std::string pass;           ///< The page it is on.
        bool read = false, write = false;
        bool operator==(const PassUse&) const = default;
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

    /** @brief The specialization constant a shader declares to show a channel of its material (koral_debug.slang). */
    inline constexpr const char* ShaderDebugConstant = "koralDebugView";
    /** @brief What `koralDebugView` is, as koral_debug.slang names them: 0 is the lit colour, as without it. */
    enum class ShaderDebugChannel : int {
        eLit = 0, eAlbedo = 1, eNormal = 2, eRoughness = 3, eMetallic = 4, eEmissive = 5, eOcclusion = 6, eLighting = 7,
    };

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
         * @brief Makes a picture of @p width × @p height texels, four bytes each (red, green, blue, alpha), its smaller
         *        levels made of it — found by @p id from now on (a script's are `$script/<name>`). One there already by
         *        that id is replaced: what held it holds this one. A generator's (`$script/gen/...`) is kept in the
         *        project's cache, and is there when it is asked for again with nothing made of it.
         */
        bool MakeImage(const std::string& id, std::uint32_t width, std::uint32_t height, std::span<const std::byte> rgba, std::string* error = nullptr);
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

        // ---- pictures kept block-compressed ------------------------------------------------------------------------

        /** @brief Every picture file it reads, and every pack (Graph::texturePacks) it was asked for: by asset id. */
        [[nodiscard]] std::vector<std::string> Pictures() const;
        /**
         * @brief Whether picture @p id has a file cooked of it — of its file as it is now, with the graph's settings as
         *        they are now — in TextureCache: what is read in its place. False for one kept as it is read.
         */
        [[nodiscard]] bool Cooked(std::string_view id) const;
        /** @brief What cooking is at: of those asked since it last had nothing to do, how many are done, which is being cooked, and what could not be. */
        struct CookProgress {
            std::size_t done = 0, total = 0;
            std::string current;
            std::vector<std::string> failed;    ///< Why each that could not be cooked could not.
        };
        /**
         * @brief Cooks pictures @p ids — files and packs the graph keeps in a block format — into TextureCache, one
         *        after another off the frame; each is read again from what was cooked of it as soon as it is. Those kept
         *        as they are read, and those already asked for, are passed over.
         */
        void Cook(const std::vector<std::string>& ids);
        [[nodiscard]] const CookProgress& Cooking() const;
        /** @brief Where pictures are cooked to: `.koral/cache/textures` in the folder the asset directory is in. Empty without one. */
        [[nodiscard]] std::filesystem::path TextureCache() const;
        /** @brief Why each file that could not be read could not. */
        [[nodiscard]] std::vector<std::string> Unread() const;

        // ---- looking at what a node made ----------------------------------------------------------------------------

        /**
         * @brief From now on, copies what node @p id makes — its image or its buffer — out each frame, to be read
         *        with Watched. An image needs eTransferSrc among its usage, and so does a Buffer node's buffer — which
         *        an Image or a Buffer node's always has.
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
        /** @brief Watches node @p id 's @p output, rather than the one it is shown by otherwise: another of a Script node's. */
        void WatchOutput(std::string_view id, std::string output);
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
         * @brief Entities left out of what is drawn, in this runtime alone: an editor's view hiding some, or showing
         *        some by themselves. Each list of entities with a Mesh leaves them out; what lights them, and the
         *        world, are as they were. Empty (the default), none.
         */
        void SetHidden(std::set<kecs::Entity> hidden);
        /**
         * @brief Of a runtime that follows another: the Buffer nodes, by id, whose buffer is the source's own rather than
         *        one of its own — those something outside the graph writes into (a graphics script), so that what it
         *        writes reaches both. Taken again whenever the source builds another. Nothing for one that follows none.
         */
        void Share(const std::set<std::string>& nodes);
        /**
         * @brief Says that the CPU was handed what node @p id makes — a script, through kengine_node_resource — and may
         *        write or read it. A Buffer given no `type` is made where only passes reach it (eDeviceLocal) until then,
         *        and made again from then on as memory the CPU writes into (eDynamic). Kept while the runtime is.
         */
        void ReachedFromTheCpu(std::string_view id);
        /**
         * @brief The usage an Image or a Buffer node's resource was made with, by name: what its `usage` gives, what
         *        uses it uses it as, and eTransferSrc (to be looked at). Empty for any other node, or one not built.
         */
        [[nodiscard]] std::vector<std::string> UsageOf(std::string_view id) const;
        /** @brief The entity that is picked: what a push constant named `selected` is given (its number; 0 for none). */
        void SetSelected(kecs::Entity entity);

        // ---- what an editor's scene view looks at the scene by --------------------------------------------------

        /**
         * @brief Whether this is an editor's scene view of the scene rather than the game's: a pass whose `views` is
         *        "game" is left out of it, and one whose `views` is "editor" is run only in it. False, the default:
         *        what is run, and an editor's Game panel.
         */
        void SetEditorView(bool editor);
        [[nodiscard]] bool EditorView() const;
        /** @brief One of the graph's DebugView nodes: its id, and what it is listed as. */
        struct DebugViewOf {
            std::string id;
            std::string name;
        };
        /** @brief Every DebugView of the graph as it runs, in the graph's order. */
        [[nodiscard]] std::vector<DebugViewOf> DebugViews() const;
        /**
         * @brief Shows the DebugView @p id: its `shown` is true, so the passes wired from it run, and its `out` is the
         *        picture to show (ImageOf). Empty: none — what the graph shows. Taken up at the start of the next frame.
         */
        void ShowDebugView(std::string id);
        [[nodiscard]] const std::string& ShownDebugView() const;
        /**
         * @brief What every pipeline whose shaders declare the specialization constant `koralDebugView`
         *        (ShaderDebugConstant) is built with, unless its node gives it: which channel of its material it shows in
         *        place of its lit colour (ShaderDebugChannel). 0, the default, shows what it shows. Taken up at the start
         *        of the next frame, rebuilding only those pipelines.
         */
        void SetShaderDebugView(int channel);
        [[nodiscard]] int ShaderDebugView() const;
        /** @brief The graphics pipelines, by id, that draw colour and whose shaders do not declare `koralDebugView`: drawn as they are whatever it is. */
        [[nodiscard]] std::vector<std::string> WithoutShaderDebugView() const;
        /**
         * @brief A fixed step has been taken, of @p seconds: called by whoever runs the scene, once for each (a scene's
         *        FixedUpdate, while it plays). A frame with one or more in it runs the passes of the fixed pages; one with
         *        none rests them. @see Graph::fixed
         */
        void FixedStep(float seconds);
        /** @brief What a pass node costs, averaged over recent frames: its GPU time (a few frames late), and the CPU's. */
        struct Timing {
            double gpuMs = 0.0, recordMs = 0.0, prepareMs = 0.0;
            bool gpuMeasured = false;
            std::uint32_t runs = 0;     ///< How many passes of the frame graph it is: one, or one for each fixed step a frame can run.
        };
        /** @brief Each pass node's Timing, by its id: a fixed page's every step's added together. */
        [[nodiscard]] std::map<std::string, Timing, std::less<>> Timings() const;

        /** @brief The frame as it runs: this graph's passes in the order the frame graph schedules them, and what is left out. */
        struct FrameReport {
            struct Pass {
                std::string name;           ///< The frame graph's: the node's id, or `<id>\nstep 2` for a further fixed step's.
                std::string node;           ///< The pass node it is, by id as it runs: `<call>/<node>`, `<node> [n]`.
                std::uint32_t level = 0;    ///< How deep in what it depends on: passes of one level depend on none of each other.
                bool async = false;         ///< On the async compute queue.
                double gpuMs = 0.0, recordMs = 0.0;
                bool gpuMeasured = false;
                /// The newest frame the GPU has measured, as it was: a timeline of one frame. Each start is in ms after the
                /// frame started on the CPU, -1 when it is not known: when it was prepared on the main thread, recorded
                /// (recordThread: 0 the main thread, then one number for each other), and ran on the GPU — on the CPU's
                /// clock when gpuOnCpuClock, else from that frame's first pass on the GPU. kor::FrameGraph::PassTiming
                double cpuPrepareStartMs = -1.0, cpuPrepareMs = 0.0, cpuRecordStartMs = -1.0, cpuRecordMs = 0.0;
                int recordThread = -1;
                double gpuStartMs = -1.0, gpuLastMs = 0.0;
                bool gpuOnCpuClock = false;
                std::uint8_t work = 0;      ///< What it did on the GPU, as kor::TimerWork bits: drew, dispatched, traced rays, copied.
                /// What it uses, by the node that made each (or the entities' set it is): 'r' read, 'w' written, 'b' both.
                std::vector<std::pair<std::string, char>> uses;
            };
            std::vector<Pass> passes;
            /// Passes of this graph's that did not run, by node: skipped (what they need a pass left out makes), culled (nothing needs what
            /// they make), or resting (nothing that runs this frame reads what they make: they stay in the frame, and record nothing).
            std::vector<std::pair<std::string, std::string>> leftOut;
            std::uint64_t bytes = 0;            ///< What the frame graph allocated for what it makes itself.
            std::uint64_t unsharedBytes = 0;    ///< What that would be with nothing sharing memory.
        };
        [[nodiscard]] FrameReport Frame() const;
        /** @brief How long the last frames took, oldest first, in ms: the CPU's frame time and the GPU's time for the passes. */
        struct FrameTimes { std::vector<float> frameMs, gpuMs; };
        [[nodiscard]] FrameTimes Times() const;
        /** @brief How far the frame being made is from the last fixed step to the next, 0 to 1: a FixedStep's `alpha`. */
        void SetFixedFraction(float fraction);
        /** @brief How many fixed steps the frame being made has in it: what its fixed pages are run for. */
        [[nodiscard]] std::uint32_t FixedSteps() const;
        /**
         * @brief Runs the setup pages again on the next frame, as if built again: what a scene that starts or stops
         *        playing asks, so that what setup made — a simulation's starting state — starts from the world as it is.
         */
        void RunSetupAgain();
        /** @brief How big the scene's window is, each frame: what a Screen node gives as its extent. Sized by it, an image follows the window. */
        void SetScreenExtent(kor::UVec2 extent);
        /** @brief What the scene's window presents in (`eBGRA8_UNORM`...) and how many of its pixels a point is: a Screen node's `format` and `scale`. */
        void SetScreenLook(std::string format, float scale);
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
        /** @brief What is at a pixel: the entity drawn there, and — the Pick wired a `depth` — the depth round it. */
        struct PickHit {
            kecs::Entity entity = kecs::NullEntity;
            bool deep = false;              ///< Whether `depth` was read: the Pick has a depth image.
            /// The depth at the pixel and round it, rows top to bottom, as the depth image holds it: [4] is the pixel's.
            /// Where the picture is at an edge, the nearest pixel inside it stands for those past it.
            float depth[9] {};
            kor::IVec2 pixel { 0, 0 };      ///< Which pixel: across and down.
            kor::UVec2 extent { 0, 0 };     ///< Of the picture it is in.
            kor::Mat4 viewProjection { 1.f };   ///< As the frame that drew it saw the scene.
        };
        /** @brief Pick, with the depth round the pixel when the Pick node has it: what a click on a surface is to place, or measure. */
        bool PickAt(float u, float v, std::function<void(const PickHit&)> then);

        /** @brief How node @p id fared, or null when there is none. */
        [[nodiscard]] const NodeStatus* Status(std::string_view id) const;
        /** @brief Whether every node is ok. */
        [[nodiscard]] bool Ok() const;
        /** @brief Every node that is not ok, with why: "id: error". */
        [[nodiscard]] std::vector<std::string> Problems() const;

        /**
         * @brief What node @p id 's @p output is, when it is a resource; null otherwise. A default mesh is found
         *        the same way, with no node for it: `$cube`, `$sphere?rings=24` (kgraph::DefaultMeshId), made when first asked for.
         *        So are three pictures of one texel — `$white`, `$black`, `$normal` — and what is in a file: `@wall.png`
         *        is that picture, and `@model.gltf#3` the fourth mesh of that model. @see assets.h
         *        No @p output is the one the node is looked at by: ShownOutput.
         */
        [[nodiscard]] std::shared_ptr<Resource> ResourceOf(std::string_view id, std::string_view output = {}) const;
        /** @brief The same, as the image it is; empty when it is not one. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> ImageOf(std::string_view id, std::string_view output = {}) const;
        /**
         * @brief Which of node @p id 's outputs it is looked at by — watched, shown on it, made a thumbnail of: the one
         *        WatchOutput says, when it has it; otherwise `out`; and for a node that has none, a Script node, its first
         *        image, or its first resource. A Function call's the same: WatchOutput's, else the first of its outputs
         *        that holds an image, else the first that holds anything.
         */
        [[nodiscard]] std::string ShownOutput(std::string_view id) const;
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
         * @brief What a Bundle holds, by its members' names, in order: of a Bundle node's `out`, a ForEach's `element`, an
         *        Unbundle's `of`. Known for those that something takes, once the graph is set; empty otherwise.
         */
        [[nodiscard]] std::vector<std::string> BundleOf(std::string_view id, std::string_view output) const;
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
        /**
         * @brief The material kinds pipeline node @p id's shaders take fields of (`[PbrMaterial("roughness")]`), in the
         *        order they are first named: what an entity must have every one of to be drawn by it. None for another node.
         */
        [[nodiscard]] std::vector<std::string> MaterialsOf(std::string_view id) const;
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
        /** @brief Whether a Draw or a Dispatch runs its elements as one, could, or what it would take. */
        struct Batching {
            bool batched = false;           ///< One draw, or one dispatch, of every element.
            std::size_t elements = 0;       ///< How many elements it has: draws or dispatches a frame, when it is not batched.
            bool offered = false;           ///< A Draw that `batched`, and nothing more, would make one draw of them all.
            std::string why;                ///< When it is not batched and not offered: what it would take, or why it cannot be.
        };
        /**
         * @brief How a Draw or a Dispatch of many elements is batched: null for another kind of node, and for one of a
         *        single element, which there is nothing to batch of.
         */
        [[nodiscard]] std::optional<Batching> BatchingOf(std::string_view id) const;
        /**
         * @brief The pass nodes that use what node @p id made — an image or a buffer, or a view of one, which is a use
         *        of what it views — in the order of their pages, and in a page the order of the graph: so what wrote it
         *        before a page is what comes before it here. Uses through what entities hold are not among them.
         */
        [[nodiscard]] std::vector<PassUse> UsesOf(std::string_view id, std::string_view output = "out") const;
        /**
         * @brief Makes @p script one a Script node can name, or puts it in the place of the one so named: what a script's
         *        source declares, then what runs it, then the same again when it is edited. Whatever it filled is made and
         *        filled again, the next frame.
         */
        void SetResourceScript(ResourceScript script);
        /** @brief Forgets a resource script: a Script node that names it says there is none. */
        void RemoveResourceScript(std::string_view name);
        /** @brief The resource scripts a Script node can name, by name: of the runtime it follows, for one that follows another. */
        [[nodiscard]] std::vector<ResourceScript> ResourceScripts() const;

        // ---- materials ------------------------------------------------------------------------------------------------

        /**
         * @brief Makes @p kind a component of the world — or gives the one so named its new shape, entities keeping
         *        the fields whose name and type are the same — each new one's pictures starting as the kind says, and
         *        writes its Slang module (materials.h) where SetGeneratedShaderDirectory says, with the other kinds'
         *        declared before the next frame or graph: nothing is built of a kind before then. NoComponent, with
         *        @p error saying why, when the name is a component's that is no material kind's, or there is no world.
         */
        kecs::ComponentId DeclareMaterialKind(MaterialKind kind, std::string* error = nullptr);
        /** @brief The material kinds there are, by name. */
        [[nodiscard]] const std::vector<MaterialKind>& MaterialKinds() const;
        /** @brief The kind so named; null for none. */
        [[nodiscard]] const MaterialKind* MaterialKindOf(std::string_view name) const;
        /**
         * @brief Where the material kinds' Slang modules are written, and shaders find them (`import PbrMaterial;`,
         *        `import koral_materials;`): a project's generated shaders. Until it is set, a folder of the user's
         *        temporary files. Every kind's is written there at once. With @p own false the folder is another runtime's,
         *        which has other kinds: only this one's are written there, and nothing of it is taken away.
         */
        void SetGeneratedShaderDirectory(std::filesystem::path directory, bool own = true);
        [[nodiscard]] const std::filesystem::path& GeneratedShaderDirectory() const;
        /** @brief What writing the kinds' modules last said: fields of no GPU form, a file that could not be written. */
        [[nodiscard]] const std::vector<std::string>& MaterialNotes() const;
        /**
         * @brief The inputs taken off pipeline nodes because an edited shader no longer has them, or has them as
         *        another kind: "node.input", in the order it happened. Current() no longer has them. Emptied by the call.
         */
        [[nodiscard]] std::vector<std::string> TakeReset();

    private:
        struct Impl;
        void Run();
        void WriteBack();
        std::unique_ptr<Impl> _impl;
        Graph _graph;
        Graph _running;     // the graph with its loops written out: what is evaluated
        std::uint64_t _revision = 0;
    };
}
