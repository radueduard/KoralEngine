//
// kgraph: a graph, running. Nodes are worked out in the order their wires need, each built again only when what
// goes into it changed; the passes among them are put in the scene's frame graph, which orders them by what they
// read and write.
//

#include <kgraph/runtime.h>
#include <kgraph/random.h>

#include <atomic>
#include <thread>
#include <bit>
#include <cctype>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_map>

#include <builderDescriptions.h>
#include <capiInterop.h>
#include <commandBuffer.h>
#include <descriptorSet.h>
#include <frameGraph.h>
#include <framebuffer.h>
#include <graphicsPipeline.h>
#include <computePipeline.h>
#include <imageView.h>
#include <buffer.h>
#include <bufferView.h>
#include <bindless.h>
#include <imageView.h>

#include <kasset/cook.h>
#include <koral_c.h>
#include <log.h>
#include <mesh.h>
#include <pipeline.h>
#include <shader.h>
#include <task.h>
#include <context.h>
#include <parallel.h>
#include <vertexLayout.h>

#include <debugDraw.h>
#include <kmath/quaternion.h>
#include <kmath/transform.h>
#include <deviceFeatures.h>

#include <koralImageImport.h>
#include <kasset/artifact.h>
#include <kasset/beside.h>
#include <kasset/meta.h>
#include <kasset/import.h>

#include <kgraph/entities.h>
#include <kgraph/meshes.h>
#include <kgraph/scene.h>

#include "blocks.h"
#include "expand.h"
#include "names.h"

namespace kgraph
{
    namespace cook = kasset::cook;

    Resource::~Resource()
    {
        if (handle) koral_resource_release(handle);
    }

    namespace {
        std::string FormatName(kor::Image::Format format);     // (further down, with the rest of what looks at an image)
        namespace kb = kor::builders;

        /** What one input of a node comes to: a value, or the resource it is wired to. */
        struct Resolved {
            Value value;
            std::shared_ptr<Resource> resource;
            bool linked = false;
        };
        using Inputs = std::map<std::string, Resolved>;

        struct Output {
            Value value;
            std::shared_ptr<Resource> resource;
        };

        template <typename T> using Expected = std::expected<T, std::string>;
        std::unexpected<std::string> Fail(std::string message) { return std::unexpected(std::move(message)); }

        using Held = std::shared_ptr<Resource>;

        /** An image or a buffer a pass uses: what the frame graph orders passes by. Told apart by which object it is. */
        struct Member {
            const void* id = nullptr;
            kor::ResourceRef<const kor::Image> image;
            kor::ResourceRef<const kor::Buffer> buffer;
        };
        struct Use {
            Member what;
            bool read = true, write = false;
        };

        /** The image a resource is, or is a view of; empty for anything else. */
        kor::ResourceRef<const kor::Image> ImageRefOf(const Resource* resource)
        {
            if (!resource || !resource->handle) return {};
            if (resource->kind == "Image") return kor::capi::ImageOf(resource->handle);
            if (resource->kind == "ImageView") return kor::capi::ImageViewOf(resource->handle)->SourceImage();
            return {};
        }
        kor::ResourceRef<const kor::Buffer> BufferRefOf(const Resource* resource)
        {
            if (!resource || !resource->handle) return {};
            if (resource->kind == "Buffer") return kor::capi::BufferOf(resource->handle);
            if (resource->kind == "BufferView") return kor::capi::BufferViewOf(resource->handle)->SourceBuffer();
            return {};
        }
        Member MemberOf(const Resource* resource)
        {
            Member member;
            if ((member.image = ImageRefOf(resource)).Get()) member.id = member.image.Get();
            else if ((member.buffer = BufferRefOf(resource)).Get()) member.id = member.buffer.Get();
            return member;
        }
        Member MemberOf(kor::ResourceRef<const kor::Image> image)
        {
            Member member;
            member.id = image.Get();
            member.image = std::move(image);
            return member;
        }

        /**
         * Told what a resource is used as, by whatever uses it: an Image or a Buffer node is made with what it is used as
         * among its usage (Impl::UsedAs). Set while a runtime builds or runs what uses resources; null otherwise.
         */
        thread_local std::function<void(const Resource&, std::uint32_t)>* usedAs = nullptr;
        /** Whether an image (or a view of one) was made without what it is used as here. Said either way: it is made so next time. */
        bool MadeWithout(const Resource& resource, const kor::Image::Usage usage)
        {
            if (usedAs) (*usedAs)(resource, static_cast<std::uint32_t>(usage));
            const auto image = ImageRefOf(&resource);
            return !image.Get() || !(image->UsageFlags() & usage);
        }
        bool MadeWithout(const Resource& resource, const kor::Buffer::Usage usage)
        {
            if (usedAs) (*usedAs)(resource, static_cast<std::uint32_t>(usage));
            const auto buffer = BufferRefOf(&resource);
            return !buffer.Get() || !(buffer->UsageFlags() & usage);
        }

        // ---- the component a pipeline's entities have -----------------------------------------------------------

        /** One resource of a generated component: a parameter the node left unwired. */
        /**
         * A buffer its shader marks `[entity("PER_INSTANCE")]`: an element for each entity a draw draws, which the draw
         * makes and fills. A field marked `[entity("MATRIX")]` (or NORMAL_MATRIX, POSITION, ID) is where the entity is,
         * or which; one marked TEXTURE is a picture the entity holds, by its bindless index, and one marked STORAGE an
         * image it holds that the shader writes, by its bindless storage index; one marked with a material kind's
         * attribute — `[PbrMaterial("roughness")]` — is that field of the entity's material of that kind, a picture of it
         * by its bindless index; any other is the entity's own, a field of the pipeline's component.
         */
        struct Instancing {
            struct Field {
                enum class From { eComponent, eMatrix, eNormalMatrix, ePosition, eId, eTexture, eStorage, eMaterial, eMaterialTexture };
                /** Whether it names an image the entity holds, by an index: TEXTURE, STORAGE, or a material's picture. */
                [[nodiscard]] bool Pictured() const { return from == From::eTexture || from == From::eStorage || from == From::eMaterialTexture; }
                /** Whether it is of a material of the entity's: a field of it, or a picture. */
                [[nodiscard]] bool Material() const { return from == From::eMaterial || from == From::eMaterialTexture; }
                std::string name {};
                std::uint32_t offset = 0, size = 0;     // in the element
                From from = From::eComponent;
                std::string type {};                       // eComponent, eMaterial: the member's type, as a component field's is named
                std::size_t at = 0;                     // eComponent, eTexture: where it is in the pipeline's component; a material's, in it
                std::string kind {};                    // a material's: its kind
                kecs::FieldDescription source {};       // and its field
                kecs::ComponentId component = kecs::NoComponent;    // the kind's component, in the world
            };
            std::uint32_t stride = 0;
            std::vector<Field> fields;
        };

        struct Slot {
            std::string field {};              // the component's field
            std::string parameter {};          // the shader's parameter it fills
            kor::DescriptorType type {};
            bool sampler = false;           // the sampler of the sampled image in the slot before
            bool read = true, write = false;
            std::shared_ptr<Instancing> instancing {};     // a buffer of each instance's: null for any other
        };

        /**
         * How a pipeline's component is laid out: a shared reference for each of its resources, then its push
         * constants, each where the shader has it in the range. The component type's context.
         */
        struct EntityLayout {
            std::vector<Slot> slots;
            kor::u32 pushFirst = 0;                 // where in the shader's range the component's push constants start
            std::size_t pushAt = 0;                 // and where that is, in the component
            std::size_t size = 0;
            std::vector<kecs::FieldDescription> fields;
            std::vector<std::byte> initial;         // a new component's bytes: its push constants as their shader declares them
        };

        /** One push constant a draw gives: where it is in the pipeline's range, and what it is. */
        struct PushMember {
            std::string name;
            kor::u32 offset = 0, size = 0;          // from the start of the range
            kor::u32 scalar = 0, rows = 1, columns = 1;
            Value initial;                          // what it is when given nothing: its default in the shader
        };

        std::size_t SlotCount(const kecs::ComponentDescription& type) { return static_cast<const EntityLayout*>(type.context.get())->slots.size(); }

        void ConstructEntity(void* at, const kecs::ComponentDescription& type)
        {
            std::memset(at, 0, type.size);
            const auto& initial = static_cast<const EntityLayout*>(type.context.get())->initial;
            if (!initial.empty()) std::memcpy(at, initial.data(), std::min(initial.size(), type.size));     // no Held has been made in it yet
            for (std::size_t i = 0; i < SlotCount(type); ++i) ::new (static_cast<Held*>(at) + i) Held();
        }
        void CopyEntity(void* to, const void* from, const kecs::ComponentDescription& type)
        {
            std::memcpy(to, from, type.size);
            for (std::size_t i = 0; i < SlotCount(type); ++i) ::new (static_cast<Held*>(to) + i) Held(static_cast<const Held*>(from)[i]);
        }
        void RelocateEntity(void* to, void* from, const kecs::ComponentDescription& type)
        {
            std::memcpy(to, from, type.size);
            for (std::size_t i = 0; i < SlotCount(type); ++i) {
                Held& source = static_cast<Held*>(from)[i];
                ::new (static_cast<Held*>(to) + i) Held(std::move(source));
                source.~Held();
            }
        }
        void DestroyEntity(void* at, const kecs::ComponentDescription& type)
        {
            for (std::size_t i = 0; i < SlotCount(type); ++i) static_cast<Held*>(at)[i].~Held();
        }
        void AssignHeld(void* to, const void* from, const kecs::FieldDescription&) { *static_cast<Held*>(to) = *static_cast<const Held*>(from); }

        bool IsImage(const kor::DescriptorType type)
        {
            return type == kor::DescriptorType::eSampledImage || type == kor::DescriptorType::eStorageImage || type == kor::DescriptorType::eCombinedImageSampler;
        }
        /** The kind of resource a parameter takes, as a node's output and a component's field are typed. */
        const char* KindOf(const kor::DescriptorType type)
        {
            return IsImage(type) ? "Image"
                : type == kor::DescriptorType::eSampler ? "Sampler"
                : type == kor::DescriptorType::eAccelerationStructure ? "AccelerationStructure"
                : type == kor::DescriptorType::eUniformTexelBuffer || type == kor::DescriptorType::eStorageTexelBuffer ? "BufferView" : "Buffer";
        }
        bool Takes(const kor::DescriptorType type, const std::string& kind)
        {
            return IsImage(type) ? kind == "Image" || kind == "ImageView" : kind == KindOf(type);
        }

        /** What a push constant is called as a component's field, and how many bytes of it are the value. */
        std::pair<std::string, std::size_t> ConstantType(const kor::Pipeline::PushConstantMember& member)
        {
            const bool plain = !member.aggregate && member.count == 1;
            if (plain && member.columns == 1 && member.rows >= 1 && member.rows <= 4 && member.scalar <= 3) {
                static constexpr const char* scalars[] { "f32", "i32", "u32", "bool32" };
                static constexpr const char* vectors[] { "vec", "ivec", "uvec", "bvec" };
                if (member.rows == 1) return { scalars[member.scalar], 4 };
                return { std::format("{}{}", vectors[member.scalar], member.rows), member.rows * std::size_t { 4 } };
            }
            if (plain && member.scalar == 4 && member.rows == 1 && member.columns == 1) return { "f64", 8 };
            if (plain && member.scalar == 0 && member.rows == 4 && member.columns == 4 && member.size == 64) return { "mat4", 64 };
            return { std::format("bytes{}", member.size), member.size };
        }

        // ---- pipelines and passes -------------------------------------------------------------------------------

        /** What a pipeline node worked out besides the pipeline. */
        struct PipelineInfo {
            kor::ResourceRef<const kor::Pipeline> pipeline;
            kor::Resource<kor::DescriptorSet> set;  // set 0: what is wired, bound once
            bool hasSet = false;
            std::vector<Use> uses;                  // what set 0 reads and writes
            std::map<std::string, Held> wired;      // parameter → what is wired to it
            std::vector<std::string> perEntity;     // what is not wired: set 1, each draw's to give
            std::vector<Port> ports;                // its shaders' parameters and constants, as inputs
            // What each draw of it gives (a Draw's or a Dispatch's `param.` and `push.`): set 1, and the push constants.
            std::vector<Slot> slots;
            std::map<std::string, Held> samplers;   // a sampled image's sampler, when the pipeline's node wires it
            kor::u32 pushFirst = 0, pushSize = 0;
            std::vector<PushMember> pushes;
            std::vector<Port> drawPorts;            // those, as a Draw's or a Dispatch's inputs

            std::shared_ptr<const EntityLayout> layout;         // its component's: null when it has none
            kecs::ComponentId component = kecs::NoComponent;    // the component of that layout, in the world
            std::vector<kor::ResourceRef<const kor::Shader>> shaders;   // every shader it is made of
            std::vector<Held> named;                            // those it names itself, kept while it is
            Held resource;                                      // the pipeline itself
            Held framebuffer;                                   // a graphics pipeline's
            kor::ResourceRef<const kor::Shader> vertexShader;
            kor::u32 numThreads[3] { 0, 0, 0 };                 // a compute pipeline's work group; zeros for the rest

            bool meshShaders = false;                           // a graphics pipeline drawn with mesh shaders
            bool vertexInputs = false;                          // its vertex shader reads vertices: it needs a mesh

            /**
             * The same pipeline made for another vertex layout: what a mesh laid out otherwise is drawn with, when it
             * has everything the vertex shader reads. Made as the node was, but for the layout, the first time a
             * mesh of that layout is drawn; kept while the node is.
             */
            struct Variants {
                const kb::Description* description = nullptr;
                std::string node;
                std::vector<kb::Value> constructor;
                std::vector<kb::Call> calls;
                const kb::Setting* layoutSetting = nullptr;     // the setting whose argument is the vertex layout
                std::size_t layoutArgument = 0;
                std::map<std::string, Held, std::less<>> made;          // by layout
                std::map<std::string, std::string, std::less<>> failed; // by layout: why it could not be made
            };
            std::shared_ptr<Variants> variants = std::make_shared<Variants>();
        };


        /** An IndirectDraws node's buffer of commands, and what was last written to it. */
        struct Packed {
            kor::ResourceRef<kor::Buffer> buffer;
            Held held;
            std::vector<std::byte> bytes;
            std::size_t room = 0;           // an IndirectDraws': how many instances its `visible` has room for
            bool everyFrame = false;        // written every frame, whether or not it changed (eGPU: from zero)
        };

        /**
         * A RayScene's: a bottom-level structure for each mesh of the pack, and the top-level one placing an instance
         * of one where each of the Filter's entities is — made again when one moves, comes or goes.
         */
        struct RayState {
            std::string filter;                         // the Filter whose elements the instances are
            kecs::ComponentId masked = kecs::NoComponent;
            kecs::ComponentId unshadowing = kecs::NoComponent;      // its entities cast no shadow: mask 2, not 3
            std::vector<Held> bottoms;                  // by the pack's mesh; null for one with no triangles
            Held top, scene, meshOf;
            kor::ResourceRef<kor::Buffer> sceneWritten, meshOfWritten;     // the two, to write into
            std::size_t meshOfRoom = 0;                 // how many instances meshOf has room for
            std::uint64_t seen = 0;                     // what the instances were when the top was made
            bool made = false;
        };

        /** A MeshPack's: which meshes it packed, in order — built again when they are other ones, or one arrives from its file. */
        struct MeshPacking {
            std::vector<const Resource*> of;
            // What `of` was last found the same from: the list, its version, the changes of the component holding the meshes,
            // and the resources made again in their places.
            struct Seen {
                const void* list = nullptr;
                std::uint64_t listed = 0, changes = 0, replacements = 0;
                bool operator==(const Seen&) const = default;
            };
            std::optional<Seen> seen;
        };

        /** A ForEach node's (or an Entities node's): who has its component now, and what of each of them it gives. */
        struct EntityList {
            kecs::ComponentId component = kecs::NoComponent;
            std::string shape;                                  // the component's fields when it was built: built again when they change
            /** A field it gives: of its component, or of one of `with`'s, by the output it is given as. */
            struct Read {
                std::string output;
                kecs::ComponentId component = kecs::NoComponent;
                kecs::FieldDescription field;
                bool resource = false;      // a shared reference to a resource: a mesh, a picture
            };
            std::vector<Read> fields;
            std::map<kecs::ComponentId, std::vector<std::byte>> nones;     // a component of nothing, each: what its outputs are with no entity
            std::vector<kecs::Entity> entities;                 // as of this frame, parents before their children — or grouped
            std::size_t repeat = 1;                             // how many elements each entity is, one after another
            bool grouped = false;                               // a Filter's, grouped by a component: each entity's group is said
            std::vector<std::uint32_t> groupOf;                 // grouped: each entity's group, from 0
            // What the entities were found from, kept while it is the same: none was made, destroyed, given or rid of a
            // component, switched, renamed or moved in the hierarchy; the list it keeps some of is the one it was; and
            // the component it groups by holds what it did. A frame of thousands that only move finds nothing again.
            struct Made {
                std::uint64_t structure = 0, upstream = 0, grouping = 0;
                std::vector<kecs::ComponentId> with, without;
                kecs::ComponentId component = kecs::NoComponent, groupBy = kecs::NoComponent;
                std::string select, named;
                std::string where;                              // `where`, as it was given
                std::vector<std::uint64_t> whereChanges;        // the changes of the components `where` reads
                std::size_t repeat = 1;
                bool nobody = false;
                double only = -1;                               // a Filter's `entityId`: one entity alone; −1, not given
                std::uint64_t hidden = 0;                       // the runtime's hidden entities, as they were (SetHidden)
                auto operator<=>(const Made&) const = default;
            };
            std::optional<Made> made;
            std::uint64_t version = 0;                          // goes up each time its entities are found again: what a Filter of it keeps

            /** How many elements there are: an entity's after another's. */
            [[nodiscard]] std::size_t Rows() const { return entities.size() * repeat; }
        };

        /**
         * A Readback's: a few buffers it copies into one after another, a frame each, and reads the oldest of — copied
         * frames enough before that the GPU is done with it — and what is in them.
         */
        struct ReadState {
            static constexpr std::size_t Ring = 4;
            std::array<kor::ResourceRef<const kor::Buffer>, Ring> buffers;
            std::array<std::shared_ptr<kor::Resource<kor::Buffer>>, Ring> owners;
            std::atomic<std::uint64_t> copies = 0;
            std::size_t bytes = 0;
            char kind = 'f';                    // of a buffer: 'f' floats, 'i' ints, 'u' uints, each four bytes
            std::size_t count = 1;
            std::optional<TexelLayout> texel;   // of an image: its texel's
        };

        /** A ShaderBuffer node's: how its shader lays an element out, what was last written, and where its elements come from. */
        struct Mapped {
            BlockLayout layout;
            kor::ResourceRef<kor::Buffer> buffer;
            Held held;
            Held shader;                        // the shader that declares it, kept while the buffer is
            std::vector<std::byte> bytes;
            std::size_t capacity = 1;           // how many elements there is room for
            std::size_t count = 0;              // how many it holds
            std::string after;                  // the ShaderBuffer whose elements come first
            std::string source;                 // the ForEach its fields follow; empty for one element
            std::size_t most = 0;               // a BufferBuilder given a count: no more elements than that; 0 for as many as there are
            std::vector<std::string> chain;     // what is worked out between the two, in the order it is
        };

        /** A click asking which entity is where: the pixel is copied out one frame, and read a few frames on. */
        struct PickState {
            std::optional<std::pair<float, float>> asked;   // where, from 0 to 1 each way
            std::function<void(kor::u32, const Runtime::PickHit&)> then;
            std::optional<kor::IVec3> copyAt;               // this frame's: read while the pass records
            int waiting = -1;
            Runtime::PickHit hit;                           // of the copy waited for: where, and as it was seen
        };

        /**
         * Where one of what a draw has of its own comes from — its mesh, a `param.`, a `push.` — each time it draws:
         * nothing; given on the node; wired from what follows no ForEach, the same for every draw; or of the element
         * drawn — a field of its entity's, where it is (a ForEach's other outputs), or worked out from those.
         */
        struct Source {
            enum class Kind { eNone, eGiven, eWired, eField, eWhere, eChain };
            Kind kind = Kind::eNone;
            std::string input;
            Value value;                                        // eGiven
            Held resource;                                      // eGiven and eWired: the resource
            std::string node, output;                           // eWired, eWhere, eChain: what is read
            kecs::ComponentId component = kecs::NoComponent;    // eField: whose field, read straight from the entity
            kecs::FieldDescription field;
            bool held = false;                                  // eField: a resource
            bool raw = false;                                   // eField, for a push constant: its bytes are the push constant's
        };

        /** One draw a pass makes — an element of a ForEach, or the one — as readied this frame. */
        struct Item {
            kecs::Entity entity;
            kor::ResourceRef<const kor::DescriptorSet> set;
            kor::ResourceRef<const kor::Mesh> mesh;
            kor::ResourceRef<const kor::GraphicsPipeline> pipeline;    // what it is drawn with: made for its mesh's layout
            std::size_t push = 0;           // where its push constants are, in EntityRun::pushed
            kor::u32 groups[3] { 1, 1, 1 };
            bool indexed = false;
            // A part of a mesh that holds a whole model: which indices are its, and what they count from.
            bool part = false;
            kor::u32 firstIndex = 0, indexCount = 0;
            kor::i32 vertexOffset = 0;
        };

        enum class DrawWith { eAuto, eDraw, eDrawIndexed };

        /**
         * A buffer of each instance's: an element for each entity of the Filter a Draw's mesh's pack packs, written once a
         * frame. Shared by every Draw that would write the same into it — of the same Filter, an element laid out alike,
         * and the same component for the fields that are the entity's own: a shadow pass's turns, a pass and its prepass.
         */
        struct Instances {
            std::shared_ptr<const Instancing> instancing;
            std::string source;                     // the Filter whose entities they are
            std::vector<std::string> draws;         // the Draws that draw with it: built again when there are more than there is room for
            kecs::ComponentId component = kecs::NoComponent;    // the pipeline's: where each entity's own fields are
            // The material kinds its fields are of, each once: what an entity must have all of to be drawn.
            std::vector<kecs::ComponentId> materials;
            std::size_t skipped = 0;                // entities laid out as no one's for want of one of them, the last time
            kor::ResourceRef<kor::Buffer> buffer;
            Held held;                              // the buffer, as what a Draw's slot holds
            std::size_t capacity = 0;               // how many elements there is room for
            std::shared_ptr<const std::uint64_t> clock;     // the frames of the runtime that made it: another that follows it shares it
            std::uint64_t filled = 0;               // the frame it was last written in, by that clock: once a frame, whoever asks first
            std::uint64_t written = 0;              // what was written, made one number: its version
            bool full = false;                      // outgrown: the next Draw built makes another
            std::vector<Member> pictures;           // what its TEXTURE fields name, once each: what the Draws read by index
            std::vector<Member> storages;           // and its STORAGE fields: what they write by index
            std::uint64_t named = 0;                // goes up each time pictures or storages change
            std::vector<std::byte> staged;          // every element, laid out: what changed is written in as the frame begins
            // What each element was last laid out from, and when, by version: what is looked at to write only what changed.
            std::uint64_t version = 0;              // goes up each time an element is laid out again; 0, never laid out
            std::vector<std::uint64_t> rowKey;      // the entity in each element (KeyOf), 0 for none
            // Which of what it reads that entity had: the pipeline's component (bit 0), and each material kind it names (from bit 1).
            std::vector<std::uint32_t> rowOwned;
            std::vector<std::uint64_t> rowChanged;  // the version each element was last laid out at
            std::vector<const void*> rowPictures;   // what each element's TEXTURE fields name, a Member's id: so many an element
            // Each image named, and how many times: read (TEXTURE) and written (STORAGE).
            struct Named { Member member; std::size_t reads = 0, writes = 0; };
            std::unordered_map<const void*, Named> pictureCount;
            std::size_t laid = 0;                   // how many elements entities were in
            // What they were laid out from, when they last were.
            const void* seenList = nullptr;
            std::uint64_t seenListed = 0, seenMoves = 0, seenOwns = 0, seenReplacements = 0;
            kecs::Tick seenTick = 0;
            std::vector<std::uint64_t> synced;      // the version each of the buffer's copies holds, a copy a frame in flight
        };

        /**
         * A buffer of instances' elements, written into the copy of it the frame being made reads: as it begins (a pass's
         * Prepare), once the frame before with that copy is done. Laid out between frames, they would be written into the
         * copy of the frame just drawn — and the next frame read another, of a frame before: an index of a picture that
         * is gone since among it. Only what was laid out again since that copy was last written is written into it.
         */
        static void WriteStaged(Instances& instances)
        {
            if (instances.staged.empty() || !instances.buffer.Get()) return;
            const std::size_t copies = std::max<std::size_t>(instances.buffer->CopyCount(), 1);
            if (instances.synced.size() != copies) instances.synced.assign(copies, 0);
            const std::size_t copy = copies > 1 ? kor::Context::Scheduler().CurrentImageIndex() % copies : 0;
            std::uint64_t& synced = instances.synced[copy];
            if (synced == instances.version) return;
            const std::size_t stride = instances.instancing->stride;
            auto mapping = instances.buffer->Map<std::byte>(instances.staged.size(), 0);
            std::byte* to = mapping.AsSpan().data();
            // The changed elements, in runs: written as few copies as there are runs of them, a long run in parts, and
            // the parts on every core when there are many.
            constexpr std::size_t Part = 4096;
            const std::size_t rows = instances.rowChanged.size();
            std::vector<std::pair<std::size_t, std::size_t>> runs;
            for (std::size_t row = 0; row < rows;) {
                if (instances.rowChanged[row] <= synced) { ++row; continue; }
                std::size_t end = row + 1;
                while (end < rows && instances.rowChanged[end] > synced && end - row < Part) ++end;
                runs.emplace_back(row, end);
                row = end;
            }
            const auto write = [&](const std::size_t first, const std::size_t last) {
                for (std::size_t i = first; i < last; ++i)
                    std::memcpy(to + runs[i].first * stride, instances.staged.data() + runs[i].first * stride, (runs[i].second - runs[i].first) * stride);
            };
            if (runs.size() > 4) kor::ParallelForRanges(0, runs.size(), write, 1).Wait();
            else write(0, runs.size());
            synced = instances.version;
        }

        /** Bytes mixed into a number that is, as near as matters, another for any other bytes: what a Draw `whenChanged` compares. */
        static void Mix(std::uint64_t& hash, const void* bytes, const std::size_t size)
        {
            const auto* at = static_cast<const unsigned char*>(bytes);
            std::size_t i = 0;
            for (; i + 8 <= size; i += 8) {
                std::uint64_t word;
                std::memcpy(&word, at + i, 8);
                hash = (hash ^ word) * 1099511628211ull;
                hash ^= hash >> 29;
            }
            for (; i < size; ++i) hash = (hash ^ at[i]) * 1099511628211ull;
        }

        /** What tells one buffer of instances from another: whose entities, and what is written of each, where. */
        static std::string InstancesKey(const std::string& source, const Instancing& instancing, const kecs::ComponentId component)
        {
            std::string key = std::format("{}|{}", source, instancing.stride);
            bool own = false;
            for (const auto& field : instancing.fields) {
                key += std::format("|{}:{}:{}", field.offset, field.size, static_cast<int>(field.from));
                if (field.Material()) { key += std::format("@{}:{}:{}", field.component, field.at, field.type); continue; }
                if (field.from == Instancing::Field::From::eComponent || field.Pictured()) { key += std::format("@{}", field.at); own = true; }
            }
            // Only fields of the entity's own are read from the component: without any, which component it is says nothing.
            if (own) key += std::format("|{}", component);
            return key;
        }

        /** A Draw's or a Dispatch's draws: once, or once an element of a ForEach, and what was made for each. */
        struct EntityRun {
            std::shared_ptr<const PipelineInfo> info;
            bool draws = false;
            bool tasks = false;                     // a Draw of mesh shaders: no mesh
            std::string each;                       // the ForEach it draws an element of; empty, once
            std::vector<std::string> chain;         // what is worked out of the ForEach between, in the order it is
            std::set<std::string, std::less<>> asked;   // what of the ForEach that is asked for
            bool rows = false;                      // whether the ForEach is set to each element: something is worked out of it
            Source mesh;
            std::vector<Source> slots;              // one each of the pipeline's
            std::vector<Source> pushes;             // the same
            std::vector<std::shared_ptr<Instances>> instances;  // the buffers of each instance's it makes
            DrawWith command = DrawWith::eAuto;
            int cover = -1;                         // the slot whose image decides how many groups
            // A Draw's `cull`: the output whose matrix — the `cullIndex`th of them — what is out of sight is told by.
            std::string cullNode, cullOutput;
            int cullIndex = 0;
            // A Draw's `idle`: the output whose number — the `cullIndex`th — says, while not zero, that nothing is drawn.
            std::string idleNode, idleOutput;
            bool idle = false;
            // A Draw's `whenChanged`: idle, too, while what it would draw is what it drew last into what it draws into.
            bool whenChanged = false;
            std::vector<const void*> targets;       // the views it draws into
            kor::u32 copies = 1;                    // how many of them there are of each: a per-frame image's, one a frame in flight
            std::uint64_t drawnAs = 0;              // what it last drew, made one number
            kor::u32 drawnTimes = 0;                // how many times in a row it drew that
            std::uint64_t drawingAs = 0;            // what it would draw this frame, made one number
            bool due = false;                       // whether it is to draw this frame
            kor::u32 groups[3] { 1, 1, 1 };
            kor::u32 groupSize[3] { 1, 1, 1 };

            struct Cached {
                std::vector<const Resource*> key;   // what its descriptor set was made of
                std::vector<Held> held;             // kept, while the set refers to them
                kor::Resource<kor::DescriptorSet> set;
                std::vector<Member> members;        // a slot each
                bool seen = false;
            };
            std::map<std::pair<std::uint64_t, std::size_t>, Cached> cache;     // by entity, and turn
            // What each mesh is drawn with (PipelineFor), from frame to frame: the mesh held, so that another at its
            // address is not taken for it. The pipeline, or why it cannot be drawn.
            struct Fit {
                kor::ResourceRef<const kor::Mesh> mesh;
                kor::ResourceRef<const kor::GraphicsPipeline> pipeline;
                std::string why;
            };
            std::unordered_map<const kor::Mesh*, Fit> fits;

            // As readied this frame: read while the pass records.
            std::vector<Item> items;
            // What the items draw, held until they are readied again: an entity destroyed after they are readied — its mesh with it — is
            // drawn this frame all the same, rather than recorded with a mesh that is gone.
            std::vector<Held> holding;
            std::vector<std::byte> pushed;
            std::vector<std::vector<Member>> members;   // a slot each: every entity's, once each
            std::vector<Member> pictures;               // what its buffers of each instance's name by index: read, though no slot binds them
            std::vector<Member> storages;               // and write by index
            std::string batchOf;                        // a Dispatch's: the Filter it runs once for each of, along z, in one dispatch
            Held drawn;                                 // a Draw `compacted`: the buffer its shader reads, of what is in sight, which a cull writes
            std::vector<std::pair<const void*, std::uint64_t>> namedSeen;  // the buffers of instances its pictures and storages were last gathered from, and when
            bool changed = true;                        // members changed since the frame graph was last told
            PassReport report;
        };

        struct PassContext {
            kor::ResourceRef<const kor::Image> screen;
        };

        /** What a pass node comes to: what it uses, and what it records. */
        struct PassPlan {
            std::vector<Use> uses;
            bool readsScreen = false, writesScreen = false;
            std::shared_ptr<EntityRun> entities;    // when it runs for each entity
            std::function<void(kor::CommandBuffer&, const PassContext&)> record;
            std::function<void()> prepare;      // each frame, on the scene's thread, before it is recorded
            /// Set: the pass records nothing this frame — a watch's copy between the frames it is due on. It stays in the
            /// frame graph, with what it uses, so that leaving it out and putting it back builds nothing again.
            std::shared_ptr<std::atomic<bool>> resting = std::make_shared<std::atomic<bool>>(false);
            /// Set: idle, as its node's `idle` says this frame (a pass's other than a Draw's, whose run says).
            std::shared_ptr<std::atomic<bool>> idle;
            bool async = false;     // run on the async compute queue (its node's `async`)
        };

        /** What a pass declares to the frame graph, under the name the resource — or the set it is in — is imported by. */
        struct Declared {
            std::string name;
            bool read = false, write = false;
            bool operator==(const Declared&) const = default;
        };

        class GraphPass final : public kor::RenderPass {
        public:
            GraphPass(std::string name, std::shared_ptr<const PassPlan> plan, std::vector<Declared> declared)
                : RenderPass(std::move(name)), plan(std::move(plan)), declared(std::move(declared)) {}

            void Setup(kor::PassBuilder& builder) override
            {
                bool writes = plan->writesScreen;
                for (const auto& use : declared) {
                    if (use.write) { builder.Write(use.name); writes = true; }
                    else builder.Read(use.name);
                }
                if (plan->readsScreen) builder.Read(kor::FrameGraph::Screen, kor::Image::Usage::eTransferSrc);
                if (plan->writesScreen) builder.Write(kor::FrameGraph::Screen, kor::Image::Usage::eTransferDst);
                if (!writes) builder.SideEffect();   // it only reads: kept all the same, since it was asked for
                if (plan->async) builder.AsyncCompute();
            }
            void Initialize(const kor::PassResources& resources) override
            {
                if (plan->readsScreen || plan->writesScreen) _context.screen = resources.ImageNamed(kor::FrameGraph::Screen);
            }
            void Prepare() override
            {
                if (plan->entities)
                    for (const auto& instances : plan->entities->instances) WriteStaged(*instances);
            }
            void Record(kor::CommandBuffer& commands) const override
            {
                if (steps && step >= steps->load()) return;     // a further fixed step this frame does not have
                if (plan->record && !plan->resting->load()) plan->record(commands, _context);
            }

            std::shared_ptr<const PassPlan> plan;
            std::vector<Declared> declared;
            /// A pass of the fixed step again, for the frame's step after the first: which (1 the second), and how many
            /// the frame being made has. Null for any other.
            std::uint32_t step = 0;
            std::shared_ptr<std::atomic<std::uint32_t>> steps;

        private:
            PassContext _context;
        };

        /** Puts what a parameter is given in a descriptor set being built. */
        Expected<void> WriteParameter(kor::DescriptorSet::Builder& set, const std::string& parameter, const kor::DescriptorType type,
                                      const Resource& given, const Resource* sampler, const std::string& what)
        {
            if (given.screen) return Fail(std::format("'{}': the screen is written by a pass, and cannot be a parameter", what));
            const std::string& kind = given.kind;
            // What it is used as, it has to have been made for: bound without, it is the driver's to fall over.
            const auto lacksImage = [&](const kor::Image::Usage usage, const char* name, const char* as) -> Expected<void> {
                if (ImageRefOf(&given).Get() && MadeWithout(given, usage))
                    return Fail(std::format("'{}' is {} by the shader: the image needs {} among its usage", what, as, name));
                return {};
            };
            const auto lacksBuffer = [&](const kor::Buffer::Usage usage, const char* name, const char* as) -> Expected<void> {
                if (BufferRefOf(&given).Get() && MadeWithout(given, usage))
                    return Fail(std::format("'{}' is {} by the shader: the buffer needs {} among its usage", what, as, name));
                return {};
            };
            Expected<void> fits {};
            switch (type) {
            case kor::DescriptorType::eSampledImage:
            case kor::DescriptorType::eCombinedImageSampler: fits = lacksImage(kor::Image::Usage::eSampled, "eSampled", "sampled"); break;
            case kor::DescriptorType::eStorageImage: fits = lacksImage(kor::Image::Usage::eStorage, "eStorage", "read and written as a storage image"); break;
            case kor::DescriptorType::eUniformBuffer: fits = lacksBuffer(kor::Buffer::Usage::eUniform, "eUniform", "read as a uniform buffer"); break;
            case kor::DescriptorType::eStorageBuffer: fits = lacksBuffer(kor::Buffer::Usage::eStorage, "eStorage", "read as a storage buffer"); break;
            case kor::DescriptorType::eUniformTexelBuffer:
            case kor::DescriptorType::eStorageTexelBuffer: fits = lacksBuffer(kor::Buffer::Usage::eTexel, "eTexel", "read as a texel buffer"); break;
            default: break;
            }
            if (!fits) return Fail(fits.error());
            const auto wrong = [&](const char* takes) { return Fail(std::format("'{}' takes {}, and was given a {}", what, takes, kind)); };
            switch (type) {
            case kor::DescriptorType::eSampler:
                if (kind != "Sampler") return wrong("a Sampler");
                set.Write(parameter, kor::capi::SamplerOf(given.handle));
                break;
            case kor::DescriptorType::eSampledImage:
            case kor::DescriptorType::eStorageImage:
                if (kind == "Image") set.Write(parameter, kor::capi::ImageOf(given.handle));
                else if (kind == "ImageView") set.Write(parameter, kor::capi::ImageViewOf(given.handle));
                else return wrong("an Image or an ImageView");
                break;
            case kor::DescriptorType::eCombinedImageSampler:
                if (!sampler || !sampler->handle || sampler->kind != "Sampler") return Fail(std::format("'{}' is sampled: '{}.sampler' needs a Sampler", what, what));
                if (kind == "Image") set.Write(parameter, kor::capi::ImageOf(given.handle), kor::capi::SamplerOf(sampler->handle));
                else if (kind == "ImageView") set.Write(parameter, kor::capi::ImageViewOf(given.handle), kor::capi::SamplerOf(sampler->handle));
                else return wrong("an Image or an ImageView");
                break;
            case kor::DescriptorType::eUniformBuffer:
            case kor::DescriptorType::eStorageBuffer:
                if (kind != "Buffer") return wrong("a Buffer");
                set.Write(parameter, kor::capi::BufferOf(given.handle));
                break;
            case kor::DescriptorType::eUniformTexelBuffer:
            case kor::DescriptorType::eStorageTexelBuffer:
                if (kind != "BufferView") return wrong("a BufferView");
                set.Write(parameter, kor::capi::BufferViewOf(given.handle));
                break;
            case kor::DescriptorType::eAccelerationStructure:
                if (kind != "AccelerationStructure") return wrong("an AccelerationStructure");
                set.Write(parameter, kor::capi::AccelerationStructureOf(given.handle));
                break;
            }
            return {};
        }

        /** What a pipeline's shaders ask for: every parameter and constant, once each. */
        struct Parameter {
            std::string name {};
            kor::DescriptorType type {};
            kor::Shader::AccessKind access {};
            std::shared_ptr<Instancing> instancing {};     // marked `[entity("PER_INSTANCE")]`
            std::string problem {};                        // what is wrong with how it is marked
            [[nodiscard]] bool Writes() const
            {
                return access != kor::Shader::AccessKind::eRead
                    && (type == kor::DescriptorType::eStorageImage || type == kor::DescriptorType::eStorageBuffer || type == kor::DescriptorType::eStorageTexelBuffer);
            }
            [[nodiscard]] bool Reads() const { return !Writes() || access != kor::Shader::AccessKind::eWrite; }
        };
        struct Interface {
            std::vector<Parameter> parameters;
            std::vector<kor::Shader::SpecializationConstant> constants;
            std::vector<Port> ports;    // `param.<name>` and `constant.<name>`

            [[nodiscard]] const Port* Find(const std::string_view name) const
            {
                const auto it = std::ranges::find(ports, name, &Port::name);
                return it == ports.end() ? nullptr : &*it;
            }
        };
        /** What a shader's value is, by its reflected kind (float, int, uint, bool, double) and how many: none for another kind. */
        Shape ShapeOfScalar(const unsigned scalar, const unsigned components)
        {
            static constexpr Scalar kinds[] { Scalar::eFloat, Scalar::eInt, Scalar::eUInt, Scalar::eBool, Scalar::eDouble };
            if (scalar >= std::size(kinds) || components < 1 || components > 4) return {};
            return { kinds[scalar], static_cast<std::uint8_t>(components) };
        }

        /**
         * A push constant's input as its shader declares it: starting as its default, and kept and drawn as its attributes say —
         * `[Range(min, max)]`, `[Min(n)]`, `[Max(n)]`, `[Logarithmic]`, `[Color]`, and on a struct's field `[FieldRange]` and the rest
         * (packages/koral/shaders/koral_inspector.slang).
         */
        void Annotate(Port& port, const kor::Shader::Annotation& annotation)
        {
            if (!annotation.initial.empty()) {
                Value::List numbers(annotation.initial.begin(), annotation.initial.end());
                port.initial = Conform(numbers.size() == 1 ? numbers.front() : Value(std::move(numbers)), port.shape);
            } else if (annotation.declaresInitial) {
                port.doc += " Its default in the shader is worked out, not written as numbers: zero here, until it is given.";
            }
            for (const auto& attribute : annotation.attributes) {
                const auto& n = attribute.numbers;
                // A field's are the same, named apart: Slang lets an attribute be a parameter's or a field's, not both.
                const std::string_view name = std::string_view(attribute.name).starts_with("Field") ? std::string_view(attribute.name).substr(5) : std::string_view(attribute.name);
                if (name == "Range" && n.size() >= 2) { port.min = static_cast<float>(n[0]); port.max = static_cast<float>(n[1]); }
                else if (name == "Min" && !n.empty()) port.min = static_cast<float>(n[0]);
                else if (name == "Max" && !n.empty()) port.max = static_cast<float>(n[0]);
                else if (name == "Logarithmic") port.logarithmic = true;
                else if (name == "Color" && port.shape.scalar == Scalar::eFloat && port.shape.components >= 3) port.editor = "color";
            }
        }

        bool NamesColor(std::string name)
        {
            std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name.find("color") != std::string::npos || name.find("colour") != std::string::npos || name.find("tint") != std::string::npos;
        }

        /** A parameter its shader marks for the engine: a buffer of each instance's, as its element's fields are marked. */
        static void Instanced(const kor::Shader::Descriptor& descriptor, Parameter& parameter, const std::vector<MaterialKind>& kinds, const kecs::World* world)
        {
            const std::string& name = parameter.name;
            if (descriptor.semanticNamespace != "entity") return;   // another module's to fill
            if (descriptor.semantic != "PER_INSTANCE") { parameter.problem = std::format("'{}' is marked entity(\"{}\"): a buffer is entity(\"PER_INSTANCE\")", name, descriptor.semantic); return; }
            if (descriptor.type != kor::DescriptorType::eStorageBuffer || descriptor.elementMembers.empty() || descriptor.elementStride == 0) {
                parameter.problem = std::format("'{}' is each instance's, and so a StructuredBuffer of a struct: an element for each", name);
                return;
            }
            auto instancing = std::make_shared<Instancing>();
            instancing->stride = descriptor.elementStride;
            for (const auto& member : descriptor.elementMembers) {
                Instancing::Field field { member.name, member.offset, member.size };
                const bool matrix = member.scalar == 0 && member.rows == 4 && member.columns == 4 && member.size == 64;
                const auto wrong = [&](const char* what) { parameter.problem = std::format("'{}.{}' is marked entity(\"{}\"), which is {}", name, member.name, member.semantic, what); };
                if (member.semanticNamespace.empty()) {
                    kor::Pipeline::PushConstantMember as;
                    as.scalar = member.scalar;
                    as.rows = member.rows;
                    as.columns = member.columns;
                    as.size = member.size;
                    as.count = 1;
                    const auto [type, size] = ConstantType(as);
                    if (type.starts_with("bytes") || size != member.size) {
                        parameter.problem = std::format("'{}.{}' is the entity's own, and so a number, a vector or a float4x4: not {} bytes of something else", name, member.name, member.size);
                        return;
                    }
                    field.type = type;
                } else if (const auto kind = std::ranges::find(kinds, member.semanticNamespace, &MaterialKind::name); kind != kinds.end()) {
                    // A field of the entity's material of that kind: the one it names, or — named nothing — the member's own name.
                    const std::string wanted = member.semantic.empty() ? member.name : member.semantic;
                    const auto source = std::ranges::find(kind->fields, wanted, &kecs::FieldDescription::name);
                    if (source == kind->fields.end()) {
                        std::string known;
                        for (const auto& f : kind->fields) known += std::format("{}{}", known.empty() ? "" : ", ", f.name);
                        parameter.problem = std::format("'{}.{}' is marked {}(\"{}\"), which the kind has not: it has {}", name, member.name, kind->name, wanted, known);
                        return;
                    }
                    kor::Pipeline::PushConstantMember as;
                    as.scalar = member.scalar;
                    as.rows = member.rows;
                    as.columns = member.columns;
                    as.size = member.size;
                    as.count = 1;
                    const std::string type = ConstantType(as).first;
                    if (const std::string misfit = MaterialFieldMisfit(*source, type); !misfit.empty()) {
                        parameter.problem = std::format("'{}.{}' is marked {}(\"{}\"), which is {}", name, member.name, kind->name, wanted, misfit);
                        return;
                    }
                    field.from = source->type == "Image" ? Instancing::Field::From::eMaterialTexture : Instancing::Field::From::eMaterial;
                    field.type = type;
                    field.at = source->offset;
                    field.kind = kind->name;
                    field.source = *source;
                    field.component = world ? world->Find(kind->name) : kecs::NoComponent;
                } else if (member.semanticNamespace != "entity") {
                    parameter.problem = std::format("'{}.{}' asks the {} module for {}: an instance's field is the entity's — entity(\"MATRIX\"), NORMAL_MATRIX, POSITION, ID — a material kind's, or its own", name, member.name, member.semanticNamespace, member.semantic);
                    return;
                } else if (member.semantic == "MATRIX" || member.semantic == "NORMAL_MATRIX") {
                    if (!matrix) return wrong("a float4x4");
                    field.from = member.semantic == "MATRIX" ? Instancing::Field::From::eMatrix : Instancing::Field::From::eNormalMatrix;
                } else if (member.semantic == "POSITION") {
                    if (member.scalar != 0 || member.columns != 1 || member.rows < 3) return wrong("a float3");
                    field.from = Instancing::Field::From::ePosition;
                } else if (member.semantic == "ID") {
                    if ((member.scalar != 1 && member.scalar != 2) || member.rows != 1 || member.columns != 1) return wrong("a uint: 0 for none, and one more than its index for an entity");
                    field.from = Instancing::Field::From::eId;
                } else if (member.semantic == "TEXTURE") {
                    if ((member.scalar != 1 && member.scalar != 2) || member.rows != 1 || member.columns != 1)
                        return wrong("a uint: the bindless index of the picture the entity holds there, for kor::texture2D — ~0 for none");
                    field.from = Instancing::Field::From::eTexture;
                } else if (member.semantic == "STORAGE") {
                    if ((member.scalar != 1 && member.scalar != 2) || member.rows != 1 || member.columns != 1)
                        return wrong("a uint: the bindless storage index of the image the entity holds there, for kor_RWTexture2D — ~0 for none");
                    field.from = Instancing::Field::From::eStorage;
                } else {
                    parameter.problem = std::format("'{}.{}' is marked entity(\"{}\"): an instance's field is MATRIX, NORMAL_MATRIX, POSITION, ID, TEXTURE or STORAGE", name, member.name, member.semantic);
                    return;
                }
                instancing->fields.push_back(std::move(field));
            }
            parameter.instancing = std::move(instancing);
        }

        Interface Reflect(const std::vector<kor::ResourceRef<const kor::Shader>>& shaders, const std::vector<MaterialKind>& kinds = {}, const kecs::World* world = nullptr)
        {
            Interface out;
            for (const auto& shader : shaders) {
                for (const auto& [number, set] : shader->BlockLayout().descriptorSets) {
                    if (number == kor::BindlessSet) continue;   // the bindless tables (koral_bindless): the framework binds them
                    for (const auto& descriptor : set.descriptors | std::views::values) {
                        const std::string& name = descriptor.name.empty() ? descriptor.blockName : descriptor.name;
                        const auto known = std::ranges::find(out.parameters, name, &Parameter::name);
                        if (known == out.parameters.end()) {
                            out.parameters.push_back({ name, descriptor.type, descriptor.access });
                            if (!descriptor.semanticNamespace.empty()) Instanced(descriptor, out.parameters.back(), kinds, world);
                        }
                        else if (known->access != descriptor.access) known->access = kor::Shader::AccessKind::eReadWrite;
                    }
                }
                for (const auto& constant : shader->BlockLayout().specializationConstants | std::views::values)
                    if (!constant.name.empty() && std::ranges::find(out.constants, constant.name, &kor::Shader::SpecializationConstant::name) == out.constants.end())
                        out.constants.push_back(constant);
            }
            for (const auto& parameter : out.parameters) {
                out.ports.push_back({ "param." + parameter.name, PortKind::eResource, KindOf(parameter.type), false, {}, nullptr });
                if (parameter.type == kor::DescriptorType::eCombinedImageSampler)
                    out.ports.push_back({ "param." + parameter.name + ".sampler", PortKind::eResource, "Sampler", false, {}, nullptr });
            }
            for (const auto& constant : out.constants) {
                Port port { "constant." + constant.name, PortKind::eValue, constant.scalar == 3 ? "bool" : "number", false, {}, nullptr };
                port.shape = ShapeOfScalar(constant.scalar, 1);
                port.integer = port.shape.Whole();
                if (port.shape.scalar == Scalar::eUInt) port.min = 0.f;
                // What the shader gives it, its bits in the low bytes, as what it is.
                const auto bits = constant.defaultValue;
                const auto as = [&]<typename T>(T) { T v {}; std::memcpy(&v, &bits, sizeof v); return static_cast<double>(v); };
                switch (port.shape.scalar) {
                    case Scalar::eFloat: port.initial = Value(as(float {})); break;
                    case Scalar::eDouble: port.initial = Value(as(double {})); break;
                    case Scalar::eInt: port.initial = Value(as(std::int32_t {})); break;
                    case Scalar::eUInt: port.initial = Value(as(std::uint32_t {})); break;
                    case Scalar::eBool: port.initial = Value((bits & 0xffffffffu) != 0); break;
                    case Scalar::eNone: break;
                }
                out.ports.push_back(std::move(port));
            }
            return out;
        }

        std::uint64_t KeyOf(const kecs::Entity entity) { return (std::uint64_t { entity.index } << 32) | entity.generation; }

        // ---- values -------------------------------------------------------------------------------------------

        /** Numbers, or vectors a component at a time: a number goes with every component of a vector. */
        Expected<Value> Each(const std::vector<const Value*>& values, const std::function<double(const double*)>& apply)
        {
            std::size_t count = 0;
            for (const Value* value : values) {
                if (value->Number() || value->Bool()) continue;
                const auto* items = value->Items();
                if (!items || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; })) return Fail("its inputs are numbers, or vectors of them");
                if (count != 0 && items->size() != count) return Fail(std::format("a vector of {} and one of {} do not go together", count, items->size()));
                count = items->size();
            }
            const auto at = [&](const Value* value, const std::size_t i) {
                if (const double* number = value->Number()) return *number;
                if (const bool* flag = value->Bool()) return *flag ? 1.0 : 0.0;
                return *(*value->Items())[i].Number();
            };
            std::vector<double> arguments(values.size());
            if (count == 0) {
                for (std::size_t v = 0; v < values.size(); ++v) arguments[v] = at(values[v], 0);
                return Value(apply(arguments.data()));
            }
            Value::List out;
            for (std::size_t i = 0; i < count; ++i) {
                for (std::size_t v = 0; v < values.size(); ++v) arguments[v] = at(values[v], i);
                out.emplace_back(apply(arguments.data()));
            }
            return Value(std::move(out));
        }

        using Operation = double (*)(const double*);
        /** What is worked out of one, two or three numbers, by the node type's name. */
        const std::map<std::string, std::pair<int, Operation>, std::less<>>& Operations()
        {
            static const std::map<std::string, std::pair<int, Operation>, std::less<>> operations {
                { "Negate", { 1, [](const double* v) { return -v[0]; } } },
                { "Abs", { 1, [](const double* v) { return std::abs(v[0]); } } },
                { "Floor", { 1, [](const double* v) { return std::floor(v[0]); } } },
                { "Ceil", { 1, [](const double* v) { return std::ceil(v[0]); } } },
                { "Round", { 1, [](const double* v) { return std::round(v[0]); } } },
                { "Fract", { 1, [](const double* v) { return v[0] - std::floor(v[0]); } } },
                { "Sign", { 1, [](const double* v) { return v[0] > 0 ? 1.0 : v[0] < 0 ? -1.0 : 0.0; } } },
                { "Saturate", { 1, [](const double* v) { return std::clamp(v[0], 0.0, 1.0); } } },
                { "Sqrt", { 1, [](const double* v) { return std::sqrt(v[0]); } } },
                { "Exp", { 1, [](const double* v) { return std::exp(v[0]); } } },
                { "Log", { 1, [](const double* v) { return std::log(v[0]); } } },
                { "Sin", { 1, [](const double* v) { return std::sin(v[0]); } } },
                { "Cos", { 1, [](const double* v) { return std::cos(v[0]); } } },
                { "Tan", { 1, [](const double* v) { return std::tan(v[0]); } } },
                { "Radians", { 1, [](const double* v) { return v[0] * 3.14159265358979323846 / 180.0; } } },
                { "Degrees", { 1, [](const double* v) { return v[0] * 180.0 / 3.14159265358979323846; } } },
                { "Asin", { 1, [](const double* v) { return std::asin(v[0]); } } },
                { "Acos", { 1, [](const double* v) { return std::acos(v[0]); } } },
                { "Atan", { 1, [](const double* v) { return std::atan(v[0]); } } },
                { "Sinh", { 1, [](const double* v) { return std::sinh(v[0]); } } },
                { "Cosh", { 1, [](const double* v) { return std::cosh(v[0]); } } },
                { "Tanh", { 1, [](const double* v) { return std::tanh(v[0]); } } },
                { "Exp2", { 1, [](const double* v) { return std::exp2(v[0]); } } },
                { "Log2", { 1, [](const double* v) { return std::log2(v[0]); } } },
                { "Log10", { 1, [](const double* v) { return std::log10(v[0]); } } },
                { "InverseSqrt", { 1, [](const double* v) { return 1.0 / std::sqrt(v[0]); } } },
                { "Trunc", { 1, [](const double* v) { return std::trunc(v[0]); } } },
                { "Reciprocal", { 1, [](const double* v) { return 1.0 / v[0]; } } },
                { "OneMinus", { 1, [](const double* v) { return 1.0 - v[0]; } } },
                { "Add", { 2, [](const double* v) { return v[0] + v[1]; } } },
                { "Subtract", { 2, [](const double* v) { return v[0] - v[1]; } } },
                { "Multiply", { 2, [](const double* v) { return v[0] * v[1]; } } },
                { "Divide", { 2, [](const double* v) { return v[0] / v[1]; } } },
                { "Min", { 2, [](const double* v) { return std::min(v[0], v[1]); } } },
                { "Max", { 2, [](const double* v) { return std::max(v[0], v[1]); } } },
                { "Pow", { 2, [](const double* v) { return std::pow(v[0], v[1]); } } },
                { "Mod", { 2, [](const double* v) { return v[0] - v[1] * std::floor(v[0] / v[1]); } } },
                { "Step", { 2, [](const double* v) { return v[1] < v[0] ? 0.0 : 1.0; } } },
                { "Atan2", { 2, [](const double* v) { return std::atan2(v[0], v[1]); } } },
                { "Clamp", { 3, [](const double* v) { return std::clamp(v[0], std::min(v[1], v[2]), std::max(v[1], v[2])); } } },
                { "Lerp", { 3, [](const double* v) { return v[0] + (v[1] - v[0]) * v[2]; } } },
                { "InverseLerp", { 3, [](const double* v) { return v[1] == v[0] ? 0.0 : (v[2] - v[0]) / (v[1] - v[0]); } } },
                { "MultiplyAdd", { 3, [](const double* v) { return v[0] * v[1] + v[2]; } } },
                { "Remap", { 5, [](const double* v) { return v[2] == v[1] ? v[3] : v[3] + (v[0] - v[1]) / (v[2] - v[1]) * (v[4] - v[3]); } } },
                { "Smoothstep", { 3, [](const double* v) {
                    const double t = std::clamp((v[2] - v[0]) / (v[1] - v[0]), 0.0, 1.0);
                    return t * t * (3.0 - 2.0 * t);
                } } },
            };
            return operations;
        }

        /** The numbers of a vector, or of a number as a vector of one. */
        Expected<std::vector<double>> NumbersOf(const Value& value, const char* name)
        {
            if (const double* number = value.Number()) return std::vector<double> { *number };
            const auto* items = value.Items();
            if (!items || items->empty() || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; }))
                return Fail(std::format("'{}' is a vector", name));
            std::vector<double> out;
            for (const auto& item : *items) out.push_back(*item.Number());
            return out;
        }

        // ---- a node's values, as a builder takes them -------------------------------------------------------------

        Expected<std::uint64_t> Enumerator(const kb::Type& type, const Value& value)
        {
            if (const double* number = value.Number()) return static_cast<std::uint64_t>(*number);
            const std::string* name = value.String();
            if (!name || !type.enumeration) return Fail(std::format("one of {}'s values, by name", type.name));
            for (const auto& entry : type.enumeration->values)
                if (entry.name == *name) return entry.value;
            std::string known;
            for (const auto& entry : type.enumeration->values) known += std::format("{}{}", known.empty() ? "" : ", ", entry.name);
            return Fail(std::format("'{}' is none of {}'s values: {}", *name, type.name, known));
        }

        Expected<kb::Value> Convert(const kb::Type& type, const Resolved& input);

        Expected<kb::Value> ConvertValue(const kb::Type& type, const Value& value)
        {
            return Convert(type, Resolved { value, nullptr, false });
        }

        Expected<kb::Value> Convert(const kb::Type& type, const Resolved& input)
        {
            using kb::Kind;
            const Value& value = input.value;
            if (type.kind == Kind::eResource) {
                if (!input.resource) {
                    if (!input.linked && value.IsNull()) return kb::Value {};   // not given: null, where a builder takes that
                    return Fail(std::format("a {} is wired in, from the node that makes it", type.name));
                }
                if (input.resource->screen) return Fail("the screen is written by a pass, and cannot be given here");
                if (type.name != "Resource" && input.resource->kind != type.name)
                    return Fail(std::format("takes a {}, and was given a {}", type.name, input.resource->kind));
                return kb::Value::Of(input.resource->handle);
            }
            if (input.resource) return Fail("takes a value, and was wired to a resource");
            if (value.IsNull()) return kb::Value {};
            switch (type.kind) {
            case Kind::eBool:
                if (const bool* b = value.Bool()) return kb::Value::Of(*b);
                return Fail("true or false");
            case Kind::eInt:
                if (const double* n = value.Number()) return kb::Value::Of(static_cast<std::int64_t>(std::llround(*n)));
                return Fail("a number");
            case Kind::eUInt:
                if (const double* n = value.Number(); n && *n >= 0) return kb::Value::Of(static_cast<std::uint64_t>(std::llround(*n)));
                return Fail("a number, not below zero");
            case Kind::eFloat:
                if (const double* n = value.Number()) return kb::Value::Of(*n);
                return Fail("a number");
            case Kind::eString:
                if (const std::string* s = value.String()) return kb::Value::Of(*s);
                return Fail("text");
            case Kind::eEnum:
            case Kind::eChoice: {
                const auto entry = Enumerator(type, value);
                if (!entry) return Fail(entry.error());
                return kb::Value::Of(*entry);
            }
            case Kind::eFlags: {
                // Any of its values: one name, or a list of them.
                std::uint64_t bits = 0;
                if (const auto* names = value.Items()) {
                    for (const auto& name : *names) {
                        const auto entry = Enumerator(type, name);
                        if (!entry) return Fail(entry.error());
                        bits |= *entry;
                    }
                } else {
                    const auto entry = Enumerator(type, value);
                    if (!entry) return Fail(entry.error());
                    bits = *entry;
                }
                return kb::Value::Of(bits);
            }
            case Kind::eArray:
            case Kind::eList: {
                const auto* items = value.Items();
                if (!items) return Fail(type.kind == Kind::eArray ? std::format("a list of {}", type.length) : "a list");
                if (type.kind == Kind::eArray && items->size() != type.length) return Fail(std::format("a list of {}, and was given {}", type.length, items->size()));
                std::vector<kb::Value> out;
                for (const auto& item : *items) {
                    auto converted = ConvertValue(*type.element, item);
                    if (!converted) return converted;
                    out.push_back(std::move(*converted));
                }
                return kb::Value::List(std::move(out));
            }
            case Kind::eStruct: {
                // Its fields, by name; one not named is zero.
                const auto* members = value.Named();
                if (!members) return Fail(std::format("the fields of a {}, by name", type.name));
                for (const auto& [name, member] : *members)
                    if (std::ranges::find(type.fields, name, &kb::Field::name) == type.fields.end())
                        return Fail(std::format("a {} has no field '{}'", type.name, name));
                std::vector<kb::Value> out;
                for (const auto& field : type.fields) {
                    const Value* member = value.Find(field.name);
                    if (!member) { out.emplace_back(); continue; }
                    auto converted = ConvertValue(*field.type, *member);
                    if (!converted) return Fail(std::format("{}: {}", field.name, converted.error()));
                    out.push_back(std::move(*converted));
                }
                return kb::Value::List(std::move(out));
            }
            case Kind::eUnion: {
                // One of its fields: the member given says which.
                const auto* members = value.Named();
                if (!members || members->size() != 1) return Fail("one of its alternatives, by name");
                for (std::size_t i = 0; i < type.fields.size(); ++i) {
                    if (type.fields[i].name != (*members)[0].first) continue;
                    auto converted = ConvertValue(*type.fields[i].type, (*members)[0].second);
                    if (!converted) return converted;
                    return kb::Value::List({ kb::Value::Of(static_cast<std::uint64_t>(i)), std::move(*converted) });
                }
                return Fail(std::format("it has no alternative '{}'", (*members)[0].first));
            }
            default:
                return Fail("cannot be given in a graph");
            }
        }

        /** Whether a pipeline can draw a mesh: empty when it can, and why not otherwise. */
        std::string Fits(const kor::Mesh& mesh, const kor::GraphicsPipeline& pipeline, const kor::ResourceRef<const kor::Shader>& vertexShader)
        {
            const auto& wanted = pipeline.VertexAttributeDescriptions();
            if (!wanted || wanted->empty() || !vertexShader.Get()) return {};
            const auto has = mesh.Layout().Resolve(*vertexShader);
            if (!has) return std::format("its mesh does not fit the pipeline: {}", has.error().message);
            for (const auto& attribute : *wanted) {
                const auto it = std::ranges::find(*has, attribute.location, &kor::VertexInputAttributeDescription::location);
                if (it == has->end() || it->binding != attribute.binding || it->offset != attribute.offset || it->channelCount != attribute.channelCount
                    || it->channelType != attribute.channelType)
                    return std::format("its mesh lays out the vertex input at location {} differently from the pipeline", attribute.location);
            }
            if (const auto& bindings = pipeline.VertexBindingDescriptions())
                for (const auto& binding : *bindings) {
                    const auto& meshBindings = mesh.Layout().bindings;
                    const auto it = std::ranges::find(meshBindings, binding.binding, &kor::VertexInputBindingDescription::binding);
                    if (it == meshBindings.end() || it->stride != binding.stride || it->inputRate != binding.inputRate)
                        return std::format("its mesh's vertex buffer {} is not spaced as the pipeline's", binding.binding);
                }
            return {};
        }

        /** A pipeline's vertex layout input, as @p layout is: exactly, with whatever is between its attributes. */
        static Value LayoutValueOf(const kor::VertexLayout& layout)
        {
            static constexpr const char* types[] { "eFloat", "eInt", "eUInt", "eShort", "eUShort", "eByte", "eUByte", "eDouble" };
            Value::List bindings, attributes;
            for (const auto& binding : layout.bindings)
                bindings.emplace_back(Value::Members { { "binding", binding.binding }, { "stride", binding.stride },
                                                       { "inputRate", binding.inputRate == kor::VertexInputRate::eInstance ? "eInstance" : "eVertex" } });
            double position = -1;
            for (const auto& attribute : layout.attributes) {
                if (attribute.semantic == "POSITION" && position < 0) position = static_cast<double>(attributes.size());
                attributes.emplace_back(Value::Members {
                    { "semantic", attribute.semantic }, { "semanticNamespace", attribute.semanticNamespace }, { "binding", attribute.binding },
                    { "offset", attribute.offset }, { "channelType", types[static_cast<int>(attribute.channelType) & 7] }, { "channelCount", attribute.channelCount },
                    { "location", attribute.location ? static_cast<double>(*attribute.location) : -1.0 }, { "locations", attribute.locations },
                    { "locationStride", attribute.locationStride } });
            }
            return Value(Value::Members { { "bindings", std::move(bindings) }, { "attributes", std::move(attributes) }, { "positionAttribute", position } });
        }

        /** What tells one vertex layout from another: where each attribute is, and how far apart vertices are. */
        static std::string LayoutSignature(const kor::VertexLayout& layout)
        {
            std::string key;
            for (const auto& binding : layout.bindings) key += std::format("b{}:{}:{};", binding.binding, binding.stride, static_cast<int>(binding.inputRate));
            for (const auto& attribute : layout.attributes)
                key += std::format("{}:{}:{}:{}:{}:{};", attribute.semantic, attribute.binding, attribute.offset, static_cast<int>(attribute.channelType), attribute.channelCount,
                                   attribute.location ? static_cast<int>(*attribute.location) : -1);
            return key;
        }

        /**
         * A vertex layout, from a list of what a vertex has: `{ "semantic": "POSITION", "type": "eFloat", "count": 3 }`.
         * Attributes are packed in the order given, a vertex buffer (`binding`) at a time; the result is what a
         * pipeline's `vertexShader.layout` and a Mesh's `vertexLayout` take.
         */
        Expected<Value> LayoutValue(const Value& attributes)
        {
            const auto* items = attributes.Items();
            if (!items || items->empty()) return Fail("'attributes' is a list of what a vertex has: { \"semantic\": \"POSITION\", \"type\": \"eFloat\", \"count\": 3 }");
            static const std::map<std::string, std::size_t, std::less<>> sizes {
                { "eFloat", 4 }, { "eInt", 4 }, { "eUInt", 4 }, { "eShort", 2 }, { "eUShort", 2 }, { "eByte", 1 }, { "eUByte", 1 }, { "eDouble", 8 } };
            std::map<std::uint32_t, std::pair<std::size_t, bool>> bindings;   // binding → bytes so far, an instance at a time
            Value::List out;
            double position = -1;
            for (const auto& item : *items) {
                if (!item.Named()) return Fail("each attribute is its fields, by name");
                for (const auto& name : *item.Named() | std::views::keys)
                    if (name != "semantic" && name != "namespace" && name != "type" && name != "count" && name != "binding" && name != "instanced")
                        return Fail(std::format("an attribute has no field '{}'. It has: semantic, namespace, type, count, binding, instanced", name));
                const Value* semantic = item.Find("semantic");
                const Value* type = item.Find("type");
                const Value* count = item.Find("count");
                const Value* binding = item.Find("binding");
                const Value* instanced = item.Find("instanced");
                const Value* space = item.Find("namespace");
                if (!semantic || !semantic->String()) return Fail("an attribute needs its 'semantic': what it is, as the shader names it");
                const std::string typeName = type && type->String() ? *type->String() : "eFloat";
                const auto size = sizes.find(typeName);
                if (size == sizes.end()) return Fail(std::format("'{}' is no channel type: eFloat, eInt, eUInt, eShort, eUShort, eByte, eUByte, eDouble", typeName));
                if (!count || !count->Number() || *count->Number() < 1 || *count->Number() > 4) return Fail(std::format("'{}' needs its 'count', 1 to 4", *semantic->String()));
                const auto channels = static_cast<std::size_t>(*count->Number());
                const auto index = static_cast<std::uint32_t>(binding && binding->Number() ? *binding->Number() : 0);
                auto& [bytes, perInstance] = bindings[index];
                if (instanced && instanced->Bool() && *instanced->Bool()) perInstance = true;
                if (*semantic->String() == "POSITION" && position < 0) position = static_cast<double>(out.size());
                out.emplace_back(Value::Members {
                    { "semantic", *semantic->String() }, { "semanticNamespace", space && space->String() ? *space->String() : std::string() },
                    { "binding", index }, { "offset", bytes }, { "channelType", typeName }, { "channelCount", channels },
                    { "location", -1 }, { "locations", 1 }, { "locationStride", 0 } });
                bytes += size->second * channels;
            }
            Value::List buffers;
            for (const auto& [index, binding] : bindings)
                buffers.emplace_back(Value::Members { { "binding", index }, { "stride", binding.first }, { "inputRate", binding.second ? "eInstance" : "eVertex" } });
            return Value(Value::Members { { "bindings", std::move(buffers) }, { "attributes", std::move(out) }, { "positionAttribute", position } });
        }

        /**
         * The name a file in the assets is found by, `@` and all: its path from @p directory, or its whole path for one
         * outside them. Worked out by the names alone, as a name is joined to the directory: a folder linked into the
         * assets is in them, wherever the link leads.
         */
        std::string AssetName(const std::filesystem::path& file, const std::filesystem::path& directory, const int mesh)
        {
            std::filesystem::path name = file;
            std::error_code ec;
            if (!directory.empty() && file.is_absolute())
                if (const auto whole = std::filesystem::absolute(directory, ec); !ec)
                    if (const auto relative = file.lexically_normal().lexically_relative(whole.lexically_normal()); !relative.empty() && *relative.begin() != "..") name = relative;
            return mesh < 0 ? std::format("@{}", name.generic_string()) : std::format("@{}#{}", name.generic_string(), mesh);
        }
    }

    // A graph's pipeline may have tessellation and geometry shaders: the device has them enabled where the GPU has them
    // (Context::Supports), as mesh shaders and ray tracing are already.
    KORAL_REQUEST_FEATURES(kor::Feature::eTessellationShader | kor::Feature::eGeometryShader);

    // ---- the runtime ---------------------------------------------------------------------------------------------

    struct Runtime::Impl {
        struct Built {
            std::string signature {};
            NodeStatus status {};
            std::map<std::string, Output, std::less<>> outputs {};
            std::uint64_t stamp = 0;                    // changes whenever it is built again: what those wired to it watch
            std::shared_ptr<PipelineInfo> pipeline {};
            std::string automaticType {};                // a Buffer given no `type`: the one it was made with, chosen for it
            // An Image's or a Buffer's: the usage it was given, and what it was made with — that, and what uses it as what (UsedAs).
            std::uint32_t givenUsage = 0, madeFor = 0;
            std::shared_ptr<PassPlan> pass {};
            // A setup pass's: the build it last recorded for, and the frames it has yet to record on.
            std::uint64_t ranFor = 0;
            kor::u32 runsLeft = 0;
            bool setup = false;
            std::optional<kecs::Migration> migration {};   // a pipeline's: what its component last became
            // The shaders it is made of — a shader node's own, a pipeline's named on it — each told of when it is edited.
            std::vector<std::pair<kor::ResourceRef<const kor::Shader>, kor::u64>> watches {};
            std::shared_ptr<Packed> packed {};             // an IndirectDraws': what it writes each frame
            std::shared_ptr<EntityList> listed {};         // a ForEach's: who it is of
            std::string madeWith {};                       // a resource's: the values wired into it as it was made — a size
            std::shared_ptr<Mapped> mapped {};             // a ShaderBuffer's or a BufferBuilder's
            std::shared_ptr<MeshPacking> packing {};       // a MeshPack's
            std::shared_ptr<RayState> rays {};             // a RayScene's
            std::shared_ptr<ReadState> readback {};        // a Readback's
            std::shared_ptr<std::atomic<std::uint64_t>> recorded {};   // a Keep's: how many times it has copied, since it was made
            // What it has besides what its type lists: a ShaderBuffer's fields as inputs, an Entities node's as outputs.
            std::vector<Port> ports {}, gives {};
        };

        kor::FrameGraph& frameGraph;
        kecs::World* world;
        Impl* source = nullptr;                 // whose files, default meshes and made meshes these are, when it follows another
        std::set<std::string, std::less<>> shared;  // Buffer nodes whose buffer is the source's own, not one of this one's
        // What whoever runs the graph says of the frame: seen from where, into how big a window, with what picked.
        std::optional<View> viewOverride;
        std::set<kecs::Entity> hidden;          // left out of every list of entities with a Mesh (SetHidden)
        std::uint64_t hiddenVersion = 0;
        kor::UVec2 screenExtent { 0, 0 };
        std::string screenFormat = "eBGRA8_UNORM";
        float screenScale = 1.f;
        // Which frame it is, from 0: one more each frame (EachFrame). What a Time's `frame` is, and what the frame before is told apart by.
        std::uint64_t timeFrame = 0;
        bool timeBegun = false;
        // Each entity's matrix as it was asked for this frame and the one before: a ForEach's `previousMatrix`.
        struct Moved {
            kecs::Entity entity;
            std::uint64_t frame = 0;
            kor::Mat4 now, before;
        };
        mutable std::unordered_map<std::uint32_t, Moved> moved;
        // A LastFrame's: what it was given this frame and the frame before.
        struct LastSeen {
            std::uint64_t frame = 0;
            Value now, before;
            bool valid = false;
        };
        std::map<std::string, LastSeen, std::less<>> lastSeen;
        std::shared_ptr<std::atomic<kor::u32>> selectedId = std::make_shared<std::atomic<kor::u32>>(0);
        // Whose view of the scene it is, and what that view looks at it by: its DebugView shown, and the channel of a
        // material its shaders show through `koralDebugView`. Each as asked, and as built: what is asked is built at the
        // start of a frame, never while one is recorded.
        bool editorView = false, editorViewAsked = false;
        std::string debugView, debugViewAsked;
        int shaderDebug = 0, shaderDebugAsked = 0;
        // The fixed steps taken since the last frame was made, and how many the frame being made has: its fixed pages run
        // when it has any. The length of the last, and how many there have been.
        std::uint32_t fixedPending = 0, fixedSteps = 0, fixedLost = 0;
        std::shared_ptr<std::atomic<std::uint32_t>> fixedNow = std::make_shared<std::atomic<std::uint32_t>>(0);   // fixedSteps, as the passes read it
        double fixedSeconds = 1.0 / 60.0, fixedFraction = 0.0;
        std::uint64_t fixedCount = 0;
        // How long the graph has run, frame by frame, and how long the last frame took: a Time node's.
        double timeSeconds = 0.0, timeDelta = 0.0;
        std::optional<std::chrono::steady_clock::time_point> timeLast;
        bool setupAgain = false;      // setup is to be run again, on the next frame: the scene started or stopped playing
        kor::DebugDraw* debug = nullptr;
        kor::Mat4 viewProjection { 1.f };       // the main lens's projection times the main View's matrix
        kor::Mat4 mainProjection { 1.f };
        View currentView;
        float currentAspect = 1.f;
        std::shared_ptr<PickState> pick = std::make_shared<PickState>();
        const Graph* current = nullptr;         // the graph as last set: what the nodes that follow the world are found in
        // Its nodes by id, and what each pipeline's draws take from elsewhere: worked out once for each graph evaluated,
        // not once for each node that asks.
        std::unordered_map<std::string_view, const Node*> currentById;
        mutable std::optional<std::map<std::string, std::set<std::string>, std::less<>>> takenByPipeline;
        std::map<std::string, std::weak_ptr<Instances>> sharedInstances;    // buffers of instances, by what they hold (InstancesKey)
        std::shared_ptr<std::uint64_t> frames = std::make_shared<std::uint64_t>(0);     // goes up a frame: what a buffer of instances is filled by
        std::vector<std::string> live;          // those nodes, in the order they depend on one another
        std::vector<std::string> sized;         // resources wired from what follows the world: made again when what they are given changes
        /// What follows the world for setup alone: a buffer on a setup page laid out from it, and what is worked out only
        /// for such buffers. Read again only when setup records, or when entities or components came or went.
        std::set<std::string, std::less<>> setupOnly;
        std::uint64_t structureSeen = ~0ull;
        bool stale = false;                     // one of them has to be built again: the graph is, next frame
        std::map<std::string, Built, std::less<>> built;
        std::map<std::string, Held, std::less<>> defaultMeshes;    // the default meshes asked for so far, by name
        Tangents defaultMeshTangents = Graph().defaultMeshTangents;  // what they have of their surface's directions, as the graph says
        std::map<std::string, GraphPass*, std::less<>> passes;
        std::vector<std::string> wanted;        // the passes that built, in the graph's order
        std::vector<std::string> passOrder;     // as they were added to the frame graph
        struct Set {
            std::shared_ptr<kor::ResourceSet> set;
            std::vector<const void*> members;
        };
        std::map<std::string, Set> sets;        // the frame graph's sets of entities' resources, by the name they are imported by
        std::vector<std::string> reset;
        // The graph as written has loops; what runs has them written out. What could not be, and why; and what each
        // node of a loop is on each of its turns.
        std::map<std::string, NodeStatus, std::less<>> unwritten;
        std::map<std::string, std::vector<std::string>, std::less<>> turns;
        // The calls, as they were written out — their ports, what each output is, the nodes they are — and the edges of
        // a function opened in place, which stand for what they are given. How each call fared, worked out when asked.
        std::map<std::string, Expansion::Call, std::less<>> calls;
        std::map<std::string, Input, std::less<>> aliases;
        std::map<std::string, NodeStatus, std::less<>> callStatus;
        // The Bundles, as they were written out: what each holds, the inputs they filled, and what a node says of its.
        std::map<std::string, std::vector<std::pair<std::string, Input>>, std::less<>> bundles;
        std::map<std::string, std::set<std::string>, std::less<>> fromBundle;
        std::map<std::string, std::string, std::less<>> bundleNotes;
        // The Draws `batched` written out as one indirect draw, and those that could not be, with why.
        // The nodes whose resource the CPU was handed (a script, through kengine_node_resource): a Buffer of them given no
        // `type` is memory the CPU writes into.
        std::set<std::string, std::less<>> reachedFromTheCpu;
        // What each Image and Buffer node's resource is used as (MadeWithout), by what uses it: a node by its id, or what entities
        // supply (EntitiesUse). It is made with all of it among its usage, besides what it is given.
        std::map<std::string, std::map<std::string, std::uint32_t, std::less<>>, std::less<>> usedAsBy;
        static constexpr std::string_view EntitiesUse = "#entities";
        std::string user = std::string(EntitiesUse);      // what is using resources now, as UsedAs is told
        int remadeRounds = 0;                               // how deep Evaluate is in building again what was made for less
        bool evaluating = false;
        std::function<void(const Resource&, std::uint32_t)> usedAsHook = [this](const Resource& resource, const std::uint32_t usage) { UsedAs(resource, usage); };
        // What each pass reads and writes, by the names the frame graph knows them by, as Resolve last told it; those of the
        // names that are memory the CPU can read; and the passes resting this frame because nothing that runs reads what
        // they make (RestUnread).
        std::map<std::string, std::vector<Declared>, std::less<>> usesOf;
        std::set<std::string, std::less<>> seenByTheCpu;
        std::set<std::string, std::less<>> restingUnread;
        std::set<std::string, std::less<>> batched;
        std::map<std::string, std::string, std::less<>> unbatched;

        /** Whether a Bundle gave node @p id its input @p name, rather than a wire or a value of its own. */
        [[nodiscard]] bool FromBundle(const std::string_view id, const std::string& name) const
        {
            const auto it = fromBundle.find(id);
            return it != fromBundle.end() && it->second.contains(name);
        }
        std::uint64_t counter = 0;
        // What is looked at: each node, and how what copies it out was set up — its size and kind, as text — when
        // the graph was last written out. When that is no longer what the node is, the graph is written out again.
        struct Watching {
            std::string setUp;
            std::uint32_t mip = 0, layer = 0;   // of an image: which level and layer is copied
            std::string output;                 // which of the node's, when it is not the one it is shown by
            std::uint32_t every = 1;            // copied once in so many frames
            std::uint64_t since = 0;            // the frame it was first copied on: its first copies are made at once
        };
        // The scripts that fill resources as they are made, by name, each with how many times it has been set: what a
        // Script node naming it is built again by. A runtime that follows another has the other's.
        struct Scripted {
            ResourceScript script;
            std::uint64_t generation = 0;
        };
        std::map<std::string, Scripted, std::less<>> scripts;
        std::uint64_t scriptsVersion = 0, scriptsSeen = 0;
        [[nodiscard]] const Scripted* ScriptNamed(const std::string_view name) const
        {
            if (source) return source->ScriptNamed(name);
            const auto it = scripts.find(name);
            return it == scripts.end() ? nullptr : &it->second;
        }
        [[nodiscard]] std::uint64_t ScriptsVersion() const { return source ? source->ScriptsVersion() : scriptsVersion; }

        // The material kinds of the world, by name, and where their Slang modules are written.
        std::vector<MaterialKind> materialKinds;
        /** The kinds a shader's attributes name: this runtime's, or — one that follows another, with none of its own — the other's. */
        [[nodiscard]] const std::vector<MaterialKind>& Kinds() const { return materialKinds.empty() && source ? source->Kinds() : materialKinds; }
        std::filesystem::path generatedShaders;
        std::vector<std::string> materialNotes;
        void WriteMaterials()
        {
            if (generatedShaders.empty()) {
                generatedShaders = std::filesystem::temp_directory_path() / "koral" / "materials";
                kor::Shader::AddSearchPath(generatedShaders);
            }
            materialNotes = WriteMaterialModules(generatedShaders, materialKinds);
        }

        std::uint64_t watchFrame = 0;
        std::map<std::string, Watching, std::less<>> watched;
        /// What each WriteBack last did: the bytes it wrote from, what it put where (to tell what something else moved),
        /// and how many read-backs it lets pass, from before setup was made again.
        struct WritingBack {
            std::vector<std::byte> bytes;
            std::map<std::uint64_t, std::pair<kecs::Vec3, kecs::Quat>> put;
            int skip = 0;
        };
        std::map<std::string, WritingBack, std::less<>> writingBack;
        /// The copies of watched References: each copies what it stands for as it is when the Reference's page begins.
        std::set<std::string, std::less<>> before;
        bool setupRecords = false;              // a setup pass records this frame
        mutable bool rewatch = false;

        /** What node @p id built as: itself, or — for a node of a loop — what it is on its first turn. */
        [[nodiscard]] const Built* Find(const std::string_view id) const
        {
            if (const auto it = built.find(id); it != built.end()) return &it->second;
            if (const auto of = turns.find(id); of != turns.end() && !of->second.empty())
                if (const auto it = built.find(of->second.front()); it != built.end()) return &it->second;
            return nullptr;
        }
        std::atomic<bool> reloaded = false;
        // A shader could not be made — its file is not there yet, or does not compile and nothing is watching it:
        // the nodes that failed are tried again every so often, for the file to turn up.
        bool shaderFailed = false;
        std::chrono::steady_clock::time_point lastRetry = std::chrono::steady_clock::now();

        Impl(kor::FrameGraph& graph, kecs::World* entities) : frameGraph(graph), world(entities)
        {
            if (world) world->Id<Mesh>();      // what a ForEach of meshes is of, before anything has one
        }

        void Unwatch(Built& node)
        {
            for (auto& [shader, watch] : node.watches)
                if (shader.Alive()) const_cast<kor::Shader&>(*shader).UnregisterReloadCallback(watch);
            node.watches.clear();
        }

        // What an edit replaced: kept until the frame that may be drawing with it is done. An edit made from an
        // interface comes in the middle of a frame, after the scene has recorded commands with these and before the
        // frame is finished with them. Let go of at the second Update after: by then that frame is long handed over.
        std::vector<Built> retired, retiring;
        std::vector<Held> retiredResources, retiringResources;
        std::vector<EntityRun::Cached> retiredSets, retiringSets;   // an entity's descriptor set, and what it was made of
        void Retire(Built&& old) { retired.push_back(std::move(old)); }
        void LetGo()
        {
            retiring = std::exchange(retired, {});
            retiringResources = std::exchange(retiredResources, {});
            retiringSets = std::exchange(retiredSets, {});
        }

        void Watch(Built& node, kor::ResourceRef<const kor::Shader> shader)
        {
            const kor::u64 watch = const_cast<kor::Shader&>(*shader).RegisterReloadCallback([this] { reloaded = true; });
            node.watches.emplace_back(std::move(shader), watch);
        }

        ~Impl()
        {
            // What is still being read is told there is nobody to hand it to, and waited for: it comes back to this
            // thread to end, and what it holds is the device's, which is still here now and may not be later.
            *alive = false;
            while (!std::ranges::all_of(reading, [](const kor::Task<void>& task) { return task.Done(); })) {
                kor::Context::DrainMainThread();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            reading.clear();
            for (auto& node : built | std::views::values) Unwatch(node);
            for (auto* pass : passes | std::views::values) frameGraph.Remove(*pass);
            for (auto& set : sets | std::views::values) set.set->Clear();
        }

        // ---- one node ---------------------------------------------------------------------------------------

        /** The settings a builder node was given, as calls on its builder. */
        Expected<std::vector<kb::Call>> Calls(const kb::Description& description, const Inputs& inputs, std::vector<kb::Value>& constructor,
                                              const std::function<bool(std::string_view)>& handledElsewhere)
        {
            std::set<std::string> used;
            for (const auto& field : description.constructor) {
                const auto it = inputs.find(std::string(field.name));
                auto value = Convert(*field.type, it == inputs.end() ? Resolved {} : it->second);
                if (!value) return Fail(std::format("'{}' {}", field.name, value.error()));
                if (it != inputs.end()) used.insert(it->first);
                constructor.push_back(std::move(*value));
            }
            std::vector<kb::Call> calls;
            // What an argument not given is: what its input starts as, when its type says (a depth attachment clears to
            // 1, a third dimension is 1), or nothing — the builder's own zero.
            const NodeType* type = FindType(description.name);
            const auto initial = [&](const std::string& name) -> const Value* {
                if (!type) return nullptr;
                std::string listed = name;
                if (const auto open = listed.find('['); open != std::string::npos)
                    if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
                const auto port = std::ranges::find(type->inputs, listed, &Port::name);
                return port == type->inputs.end() || port->initial.IsNull() || port->kind != PortKind::eValue ? nullptr : &port->initial;
            };
            for (const auto& setting : description.settings) {
                // How many times it is given: once, or — for one given more than once — up to the highest index named.
                int count = setting.repeatable ? 0 : 1;
                if (setting.repeatable)
                    for (const auto& name : inputs | std::views::keys)
                        if (name.starts_with(std::string(setting.name) + "["))
                            count = std::max(count, std::atoi(name.c_str() + setting.name.size() + 1) + 1);
                for (int index = 0; index < count; ++index) {
                    kb::Call call { &setting, {} };
                    bool given = false;
                    std::vector<std::size_t> open;     // the arguments not given, filled from what they start as once the call is made
                    for (const auto& argument : setting.arguments) {
                        const std::string name = InputName(setting, argument, setting.repeatable ? index : -1);
                        const auto it = inputs.find(name);
                        if (it == inputs.end()) { open.push_back(call.arguments.size()); call.arguments.emplace_back(); continue; }
                        auto value = Convert(*argument.type, it->second);
                        if (!value) return Fail(std::format("'{}' {}", name, value.error()));
                        used.insert(name);
                        given = true;
                        call.arguments.push_back(std::move(*value));
                    }
                    if (!given) continue;
                    for (const std::size_t at : open) {
                        const auto& argument = setting.arguments[at];
                        const std::string name = InputName(setting, argument, setting.repeatable ? index : -1);
                        if (const Value* start = initial(name))
                            if (auto value = ConvertValue(*argument.type, *start)) call.arguments[at] = std::move(*value);
                    }
                    calls.push_back(std::move(call));
                }
            }
            for (const auto& name : inputs | std::views::keys)
                if (!used.contains(name) && !handledElsewhere(name)) return Fail(std::format("a {} has no input '{}'", description.name, name));
            return calls;
        }

        Expected<std::shared_ptr<Resource>> BuildWith(const kb::Description& description, const std::string& nodeId, const std::vector<kb::Value>& constructor,
                                                      const std::vector<kb::Call>& calls)
        {
            const auto made = kb::Build(description, constructor, calls);
            if (!made) return Fail(made.error().message);
            auto resource = std::make_shared<Resource>();
            resource->handle = *made;
            resource->kind = std::string(description.result);
            resource->node = nodeId;
            // The framework keeps a shader by its module and entry point, one that failed included: asked for again
            // it hands back the same failure, though the file may be there now, or mended. It is tried afresh.
            if (!koral_resource_valid(*made) && description.name == "Shader") koral_resource_retry(*made);
            if (!koral_resource_valid(*made)) {
                if (description.name == "Shader") shaderFailed = true;
                const char* why = koral_resource_error_message(*made);
                return Fail(why && *why ? std::string(why) : std::format("the {} could not be built", description.name));
            }
            return resource;
        }

        /**
         * What a mesh is drawn with by a pipeline node: the node's own pipeline when the mesh is laid out as it is, and
         * otherwise the same pipeline made for the mesh's layout — when the mesh has everything the vertex shader reads,
         * found by semantic as the shader names it. Why not, when it has not.
         */
        Expected<kor::ResourceRef<const kor::GraphicsPipeline>> PipelineFor(const PipelineInfo& info, const kor::Mesh& mesh)
        {
            const auto own = kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf(info.resource->handle));
            const std::string fits = Fits(mesh, *own, info.vertexShader);
            if (fits.empty()) return own;
            if (!info.vertexShader.Get()) return Fail(fits);
            const auto has = mesh.Layout().Resolve(*info.vertexShader);
            if (!has) return Fail(std::format("its mesh has not what the vertex shader reads: {}", has.error().message));
            auto& variants = *info.variants;
            if (!variants.description || !variants.layoutSetting) return Fail(fits);
            const std::string key = LayoutSignature(mesh.Layout());
            if (const auto made = variants.made.find(key); made != variants.made.end())
                return kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf(made->second->handle));
            if (const auto failed = variants.failed.find(key); failed != variants.failed.end()) return Fail(failed->second);
            // The node's calls, with its mesh's layout where the node's was.
            const auto converted = ConvertValue(*variants.layoutSetting->arguments[variants.layoutArgument].type, LayoutValueOf(mesh.Layout()));
            Expected<Held> made = converted ? Expected<Held>(Fail("")) : Expected<Held>(Fail(converted.error()));
            if (converted) {
                auto calls = variants.calls;
                const auto call = std::ranges::find(calls, variants.layoutSetting, &kb::Call::setting);
                if (call == calls.end()) made = Fail("the pipeline names no vertex shader to make it again with");
                else {
                    if (call->arguments.size() <= variants.layoutArgument) call->arguments.resize(variants.layoutArgument + 1);
                    call->arguments[variants.layoutArgument] = *converted;
                    made = BuildWith(*variants.description, variants.node, variants.constructor, calls);
                }
            }
            if (!made) {
                const std::string why = std::format("its mesh's vertex layout needs the pipeline made for it, and it could not be: {}", made.error());
                variants.failed[key] = why;
                return Fail(why);
            }
            const auto pipeline = kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf((*made)->handle));
            variants.made[key] = std::move(*made);
            return pipeline;
        }

        /** A field's value as its bytes, as a script lays it out. Why not, when the value is not one the field can be. */
        static std::string FieldBytes(const kecs::FieldDescription& field, const Value& value, std::byte* at)
        {
            std::vector<double> numbers;
            if (const double* number = value.Number()) numbers.push_back(*number);
            else if (const bool* flag = value.Bool()) numbers.push_back(*flag ? 1.0 : 0.0);
            else if (const auto* items = value.Items())
                for (const auto& item : *items) {
                    if (const double* number = item.Number()) numbers.push_back(*number);
                    else return "a list of numbers is what it takes";
                }
            else return "a number, true or false, or a vector of numbers is what it takes";
            const std::string& type = field.type;
            char scalar = 'f';
            std::size_t width = 4, count = 1;
            if (type == "f64") { scalar = 'f'; width = 8; }
            else if (type == "bool") { scalar = 'b'; width = 1; }
            else if (type == "i8" || type == "i16" || type == "i32" || type == "i64") { scalar = 'i'; width = static_cast<std::size_t>(std::stoi(type.substr(1))) / 8; }
            else if (type == "u8" || type == "u16" || type == "u32" || type == "u64" || type == "bool32") { scalar = 'u'; width = type == "bool32" ? 4 : static_cast<std::size_t>(std::stoi(type.substr(1))) / 8; }
            else if (type == "mat4") count = 16;
            else if (type == "quat") count = 4;
            else if (type.size() >= 4 && type.back() >= '2' && type.back() <= '4' && (type.starts_with("vec") || type.starts_with("ivec") || type.starts_with("uvec"))) {
                count = static_cast<std::size_t>(type.back() - '0');
                scalar = type[0] == 'i' ? 'i' : type[0] == 'u' ? 'u' : 'f';
            } else if (type != "f32") return std::format("a {} is no number or vector, and cannot be given here", type);
            if (numbers.size() > count) return std::format("it is {} {}, and was given {}", count, count == 1 ? "number" : "numbers", numbers.size());
            if (numbers.size() == 1 && count > 1) numbers.assign(count, numbers.front());     // one for every component
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                std::byte* to = at + i * width;
                const double number = numbers[i];
                if (scalar == 'b') *to = std::byte { number != 0.0 ? std::uint8_t { 1 } : std::uint8_t { 0 } };
                else if (scalar == 'f' && width == 8) std::memcpy(to, &number, 8);
                else if (scalar == 'f') { const auto v = static_cast<float>(number); std::memcpy(to, &v, 4); }
                else { const auto whole = static_cast<long long>(std::llround(number)); std::memcpy(to, &whole, width); }   // little-endian: the low bytes
            }
            return {};
        }

        /**
         * A Script node: the script it names, its settings — what it is given for each field, and what the script's source
         * says for the rest — as the script lays them out, and the resources it makes, each an output by its name. Each is
         * made as a node of its kind would be, of what the source gives it and what the node is given for it
         * (`<name>.<input>`). Those that are of no other of the script's resources — its images and buffers — are made
         * first, filled by the script, and made again with what it wrote; then those made of them, a view of an image.
         */
        Expected<void> BuildScript(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto named = inputs.find("script");
            const std::string* name = named == inputs.end() ? nullptr : named->second.value.String();
            if (!name || name->empty()) return Fail("'script' is the class of one of the project's ResourceScripts");
            const Scripted* found = ScriptNamed(*name);
            if (!found) {
                std::string there;
                for (const auto& each : source ? source->scripts : scripts) there += std::format("{}{}", there.empty() ? "" : ", ", each.first);
                return Fail(std::format("there is no ResourceScript '{}'{}", *name, there.empty() ? std::string(": the project has none") : std::format(": there are {}", there)));
            }
            const ResourceScript& script = found->script;
            std::vector<std::byte> settings(script.size, std::byte { 0 });
            if (script.initial.size() == settings.size()) settings = script.initial;
            std::string has;
            for (const auto& field : script.fields) {
                const bool matrix = field.type == "mat4";
                const bool single = !matrix && field.type != "quat" && !field.type.starts_with("vec") && !field.type.starts_with("ivec") && !field.type.starts_with("uvec");
                Port port { "field." + field.name, PortKind::eValue, field.type == "bool" ? "bool" : single ? "number" : matrix ? "matrix" : "vector", false,
                            std::format("{}.{}: a {}.", script.name, field.name, field.type), nullptr };
                port.optional = true;
                port.place = matrix ? Placement::eWire : Placement::eNode;
                port.min = field.min;       // as the script says: its [Range], its [Min], its [Logarithmic]
                port.max = field.max;
                port.logarithmic = field.logarithmic;
                // A whole number's dragged a unit at a time; an unsigned one's none or more.
                port.integer = field.type.starts_with("i") || field.type.starts_with("u");
                if (field.type.starts_with("u")) port.min = std::max(port.min, 0.f);
                out.ports.push_back(std::move(port));
                has += std::format("{}{}", has.empty() ? "" : ", ", field.name);
            }
            // Each resource's inputs, as its kind has them, starting as the source says; and it, an output.
            std::string makes;
            for (const auto& resource : script.resources) {
                const NodeType* kind = FindType(resource.kind);
                if (!kind || !kind->builder) return Fail(std::format("{}.{} is a {}, which is no resource a script can make", script.name, resource.name, resource.kind));
                const auto declared = [&](const std::string& input) -> const Value* {
                    for (const auto& [setting, value] : resource.settings) if (setting == input) return &value;
                    return nullptr;
                };
                for (const auto& each : kind->inputs) {
                    Port port = each;
                    port.name = resource.name + "." + each.name;
                    port.optional = true;
                    port.companion.clear();
                    port.group = resource.name;     // each of what it makes, a part of its own
                    port.when = {};
                    if (const Value* given = declared(each.name)) port.initial = *given;
                    else if (const auto dot = each.name.rfind('.'); dot != std::string::npos && each.name.size() == dot + 2)
                        if (const Value* whole = declared(each.name.substr(0, dot)); whole && whole->Items())
                            if (const std::size_t at = std::string_view("xyzw").find(each.name.back()); at < whole->Items()->size()) port.initial = (*whole->Items())[at];
                    if (each.kind == PortKind::eResource)
                        port.doc = std::format("{} — wired, or another of {}'s resources, as its source names it.", each.doc, script.name);
                    out.ports.push_back(std::move(port));
                }
                Port output { resource.name, PortKind::eResource, std::string(kind->builder->result), false,
                              std::format("{}.{}: a {}, as the script makes it.", script.name, resource.name, resource.kind), nullptr };
                out.gives.push_back(std::move(output));
                makes += std::format("{}{}", makes.empty() ? "" : ", ", resource.name);
            }
            for (const auto& [input, given] : inputs) {
                if (input == "script") continue;
                if (input.starts_with("field.")) {
                    const kecs::FieldDescription* field = nullptr;
                    for (const auto& each : script.fields) if (each.name == std::string_view(input).substr(6)) field = &each;
                    if (!field) return Fail(std::format("{} has no field '{}': it has {}", script.name, input.substr(6), has.empty() ? std::string("none") : has));
                    if (given.value.IsNull()) continue;
                    if (field->offset + field->size > settings.size()) continue;
                    if (const std::string why = FieldBytes(*field, given.value, settings.data() + field->offset); !why.empty())
                        return Fail(std::format("'{}': {}", input, why));
                    continue;
                }
                const std::string_view resource = std::string_view(input).substr(0, input.find('.'));
                if (input.find('.') == std::string::npos || !std::ranges::contains(script.resources, resource, &ScriptResource::name))
                    return Fail(std::format("{} makes no '{}': it makes {}", script.name, resource, makes.empty() ? std::string("nothing") : makes));
            }

            // What each is made of: what its source says, and what its kind starts with — then what the node says.
            std::map<std::string, Held, std::less<>> made;
            const auto of = [&](const ScriptResource& resource) -> Expected<Inputs> {
                const NodeType* kind = FindType(resource.kind);
                Node as;
                as.id = node.id;
                as.type = resource.kind;
                Inputs base, given;
                for (const auto& port : kind->inputs)
                    if (!port.optional && port.kind == PortKind::eValue && !port.initial.IsNull()) base[port.name].value = port.initial;
                for (const auto& [setting, value] : resource.settings) {
                    const auto port = std::ranges::find(kind->inputs, Listed(setting), &Port::name);
                    Resolved resolved;
                    if (port != kind->inputs.end() && port->kind == PortKind::eResource) {
                        // Another of the script's, by name.
                        const std::string* other = value.String();
                        if (!other || !made.contains(*other))
                            return Fail(std::format("{}.{}'s '{}' names {}, which is none of the script's resources made before it", script.name, resource.name, setting,
                                                    other ? std::format("'{}'", *other) : value.ToJson()));
                        resolved.resource = made.find(*other)->second;
                        resolved.linked = true;
                    } else {
                        resolved.value = value;
                    }
                    // What the source gives whole is the parts the kind starts with given otherwise.
                    for (const auto& part : PartsOf(*kind, setting)) base.erase(part);
                    base[setting] = std::move(resolved);
                }
                for (const auto& [input, value] : inputs)
                    if (input.size() > resource.name.size() + 1 && input.starts_with(resource.name) && input[resource.name.size()] == '.')
                        given[input.substr(resource.name.size() + 1)] = value;
                if (const auto parts = InParts(as, base); !parts) return Fail(std::format("{}.{}: {}", script.name, resource.name, parts.error()));
                if (const auto parts = InParts(as, given); !parts) return Fail(std::format("{}.{}: {}", script.name, resource.name, parts.error()));
                for (auto& [input, value] : given) base[input] = std::move(value);
                // Left open for a wire, and none in it yet: not given.
                std::erase_if(base, [](const auto& each) { return each.second.value.IsNull() && !each.second.resource; });
                FillDefaults(as, base);
                return base;
            };
            const auto build = [&](const ScriptResource& resource, const Inputs& with, const std::vector<std::byte>* filled) -> Expected<Held> {
                const kb::Description& description = *FindType(resource.kind)->builder;
                std::vector<kb::Value> constructor;
                auto calls = Calls(description, with, constructor, [](std::string_view) { return false; });
                if (!calls) return Fail(std::format("{}.{}: {}", script.name, resource.name, calls.error()));
                if (filled) {
                    const auto data = std::ranges::find_if(description.settings, [](const kb::Setting& setting) { return setting.name == "data"; });
                    if (data == description.settings.end()) return Fail(std::format("a {} cannot be given what it holds", description.name));
                    calls->push_back({ &*data, { kb::Value(*filled) } });
                }
                auto built = BuildWith(description, node.id, constructor, *calls);
                if (!built) return Fail(std::format("{}.{}: {}", script.name, resource.name, built.error()));
                (*built)->label = resource.name;
                // A view is of something: kept, for a pass that uses the view to say it uses that.
                if (description.name == "ImageView") if (const auto it = with.find("image"); it != with.end()) (*built)->source = it->second.resource;
                if (description.name == "BufferView") if (const auto it = with.find("buffer"); it != with.end()) (*built)->source = it->second.resource;
                return built;
            };
            const auto sibling = [&](const ScriptResource& resource) {
                const NodeType* kind = FindType(resource.kind);
                return std::ranges::any_of(resource.settings, [&](const auto& setting) {
                    const auto port = std::ranges::find(kind->inputs, Listed(setting.first), &Port::name);
                    return port != kind->inputs.end() && port->kind == PortKind::eResource;
                });
            };

            // Those of nothing else of the script's, first: what it fills.
            struct First { const ScriptResource* resource; Inputs with; };
            std::vector<First> first;
            for (const auto& resource : script.resources) {
                if (sibling(resource)) continue;
                auto with = of(resource);
                if (!with) return Fail(with.error());
                auto built = build(resource, *with, nullptr);
                if (!built) return Fail(built.error());
                made[resource.name] = std::move(*built);
                first.push_back({ &resource, std::move(*with) });
            }
            if (!script.fill) {
                if (!script.resources.empty())
                    out.status.note = std::format("{} fills what it makes when the project runs with its scripts: until then it is made empty", script.name);
            } else {
                std::vector<Contents> contents;
                std::vector<std::vector<std::byte>> bytes;
                std::vector<const First*> filled;
                contents.reserve(first.size());
                bytes.reserve(first.size());
                for (const auto& each : first) {
                    const std::string& kind = each.resource->kind;
                    if (kind != "Image" && kind != "Buffer") continue;
                    Contents one;
                    one.name = each.resource->name;
                    one.node = node.id;
                    one.kind = kind;
                    std::size_t size = 0;
                    const Held& held = made[one.name];
                    if (kind == "Image") {
                        const auto image = ImageRefOf(held.get());
                        if (image->IsPerFrame()) return Fail(std::format("{}.{} is filled once, and a per-frame image is a copy for each frame in flight: it cannot be per frame", script.name, one.name));
                        one.format = FormatName(image->PixelFormat());
                        const TexelLayout texel = TexelLayoutOf(one.format);
                        if (texel.channels == 0) return Fail(std::format("{}.{} is of {}, which a script cannot fill: a texel of it is not plain numbers", script.name, one.name, one.format));
                        one.extent = image->Extent();
                        one.layers = std::max<std::uint32_t>(image->ArrayLayers(), 1);
                        one.texelBytes = static_cast<std::uint32_t>(texel.Bytes());
                        size = static_cast<std::size_t>(one.extent.x) * one.extent.y * one.extent.z * one.layers * one.texelBytes;
                    } else {
                        const auto buffer = BufferRefOf(held.get());
                        if (buffer->IsPerFrame()) return Fail(std::format("{}.{} is filled once, and a per-frame buffer is a copy for each frame in flight: it cannot be per frame", script.name, one.name));
                        size = static_cast<std::size_t>(buffer->size());
                    }
                    bytes.emplace_back(size, std::byte { 0 });
                    one.bytes = bytes.back();
                    contents.push_back(std::move(one));
                    filled.push_back(&each);
                }
                if (const std::string why = script.fill(contents, settings); !why.empty()) return Fail(std::format("{}: {}", script.name, why));
                // Made again, the same, with what was written.
                for (std::size_t i = 0; i < filled.size(); ++i) {
                    auto rebuilt = build(*filled[i]->resource, filled[i]->with, &bytes[i]);
                    if (!rebuilt) return Fail(rebuilt.error());
                    made[filled[i]->resource->name] = std::move(*rebuilt);
                }
            }
            // Then what is made of them, each after what it names.
            std::vector<const ScriptResource*> rest;
            for (const auto& resource : script.resources) if (sibling(resource)) rest.push_back(&resource);
            while (!rest.empty()) {
                const auto ready = std::ranges::find_if(rest, [&](const ScriptResource* resource) {
                    return std::ranges::all_of(resource->settings, [&](const auto& setting) {
                        const auto port = std::ranges::find(FindType(resource->kind)->inputs, Listed(setting.first), &Port::name);
                        const std::string* other = setting.second.String();
                        return port == FindType(resource->kind)->inputs.end() || port->kind != PortKind::eResource || !other
                            || made.contains(*other) || !std::ranges::contains(rest, *other, &ScriptResource::name);
                    });
                });
                if (ready == rest.end()) return Fail(std::format("{}'s resources are made of each other, round: {} cannot be made", script.name, rest.front()->name));
                auto with = of(**ready);
                if (!with) return Fail(with.error());
                auto built = build(**ready, *with, nullptr);
                if (!built) return Fail(built.error());
                made[(*ready)->name] = std::move(*built);
                rest.erase(ready);
            }
            for (auto& [resource, held] : made) out.outputs[resource].resource = std::move(held);
            return {};
        }

        /** Told by MadeWithout that a resource is used as `usage`, by what is using resources now: the node that made it is made so. */
        void UsedAs(const Resource& resource, const std::uint32_t usage)
        {
            const Resource* made = &resource;
            while (made->source && (made->kind == "ImageView" || made->kind == "BufferView")) made = made->source.get();
            const auto node = currentById.find(made->node);
            if (node == currentById.end() || !MadeForItsUse(node->second->type) || shared.contains(made->node)) return;
            usedAsBy[made->node][user] |= usage;
            // Made without it: made again with it, and what uses it — at once, while the graph is being built (Evaluate
            // goes round again), and next frame when it is found so as a frame is readied (an entity's).
            if (const auto it = built.find(made->node); !evaluating && it != built.end() && (it->second.madeFor & usage) != usage) stale = true;
        }
        static bool MadeForItsUse(const std::string_view type) { return type == "Image" || type == "Buffer"; }
        /** An Image's or a Buffer's usage flags, by name, as its builder has them. */
        static const kb::Enumeration* UsagesOf(const std::string_view type)
        {
            const kb::Description* description = kb::Find(type);
            const kb::Setting* setting = description ? description->FindSetting("usage") : nullptr;
            return setting && !setting->arguments.empty() ? setting->arguments.front().type->enumeration : nullptr;
        }
        /** The usage an Image or a Buffer node is given: what it has besides what it is used as. */
        static std::uint32_t GivenUsage(const std::string_view type, const Inputs& inputs)
        {
            const kb::Enumeration* usages = UsagesOf(type);
            const auto given = inputs.find("usage");
            if (!usages || given == inputs.end() || !given->second.value.Items()) return 0;
            std::uint32_t usage = 0;
            for (const auto& entry : usages->values)
                if (std::ranges::contains(*given->second.value.Items(), Value(std::string(entry.name)))) usage |= static_cast<std::uint32_t>(entry.value);
            return usage;
        }
        /** What an Image or a Buffer node's resource is made for: what uses it, and to be copied from, which a watch does. */
        std::uint32_t UsageFor(const std::string_view id) const
        {
            std::uint32_t usage = static_cast<std::uint32_t>(kor::Image::Usage::eTransferSrc);
            static_assert(static_cast<std::uint32_t>(kor::Image::Usage::eTransferSrc) == static_cast<std::uint32_t>(kor::Buffer::Usage::eTransferSrc));
            if (const auto it = usedAsBy.find(id); it != usedAsBy.end())
                for (const std::uint32_t by : it->second | std::views::values) usage |= by;
            return usage;
        }
        /** Sets what is using resources, as UsedAs is told, for as long as it lasts; and a runtime's MadeWithout reaching it. */
        struct Using {
            Using(Impl& impl, std::string user) : impl(impl), user(std::exchange(impl.user, std::move(user))), hook(std::exchange(usedAs, &impl.usedAsHook)) {}
            ~Using() { impl.user = std::move(user); usedAs = hook; }
            Impl& impl;
            std::string user;
            std::function<void(const Resource&, std::uint32_t)>* hook;
        };

        Expected<void> BuildBuilder(const Node& node, const kb::Description& description, const Inputs& given, Built& out)
        {
            // A Buffer given no `type` is where it is best kept: on the device, which only passes reach — unless the CPU
            // reaches it too (a script was handed it: ReachedFromTheCpu), when it is memory the CPU writes into.
            Inputs automatic;
            const bool chosen = description.name == "Buffer" && (!given.contains("type") || given.at("type").value.IsNull());
            if (chosen) {
                automatic = given;
                automatic["type"].value = Value(reachedFromTheCpu.contains(node.id) ? "eDynamic" : "eDeviceLocal");
            }
            // An Image or a Buffer is made for what it is used as (UsedAs), and for what its `usage` adds.
            const kb::Enumeration* usages = MadeForItsUse(description.name) ? UsagesOf(description.name) : nullptr;
            const auto givenUsage = given.find("usage");
            const bool listed = givenUsage == given.end() || givenUsage->second.value.IsNull() || givenUsage->second.value.Items();
            const bool madeFor = usages && listed;
            if (madeFor) {
                if (!chosen) automatic = given;
                Value::List names;
                if (givenUsage != given.end() && givenUsage->second.value.Items()) names = *givenUsage->second.value.Items();
                out.givenUsage = GivenUsage(description.name, given);
                out.madeFor = out.givenUsage | UsageFor(node.id);
                for (const auto& entry : usages->values)
                    if (entry.value != 0 && (out.madeFor & entry.value) == entry.value && !(out.givenUsage & entry.value)) names.emplace_back(std::string(entry.name));
                automatic["usage"].value = Value(std::move(names));
            }
            const Inputs& inputs = chosen || madeFor ? automatic : given;
            if (chosen) out.automaticType = automatic.at("type").value.String() ? *automatic.at("type").value.String() : std::string();
            std::vector<kb::Value> constructor;
            const auto calls = Calls(description, inputs, constructor, [](std::string_view) { return false; });
            if (!calls) return Fail(calls.error());
            // A framebuffer's images have to have been made to be drawn into.
            if (description.name == "Framebuffer")
                for (const auto& [name, input] : inputs) {
                    const auto image = ImageRefOf(input.resource.get());
                    if (!image.Get()) continue;
                    const bool depth = name.starts_with("depthStencil.");
                    const auto needed = depth ? kor::Image::Usage::eDepthStencilAttachment : kor::Image::Usage::eColorAttachment;
                    if (MadeWithout(*input.resource, needed))
                        return Fail(std::format("'{}' is drawn into: its image needs {} among its usage", name, depth ? "eDepthStencilAttachment" : "eColorAttachment"));
                }
            auto resource = BuildWith(description, node.id, constructor, *calls);
            if (!resource) return Fail(resource.error());
            // A view is of something: kept, for a pass that uses the view to say it uses that.
            if (description.name == "ImageView") if (const auto it = inputs.find("image"); it != inputs.end()) (*resource)->source = it->second.resource;
            if (description.name == "BufferView") if (const auto it = inputs.find("buffer"); it != inputs.end()) (*resource)->source = it->second.resource;
            if (description.name == "Shader") {
                // Told when the file is edited: what was built from it is built again (Update).
                Watch(out, kor::capi::ShaderOf((*resource)->handle));
            }
            out.outputs["out"].resource = std::move(*resource);
            return {};
        }

        /** The shaders wired to a pipeline node, in the order of its inputs. */
        static std::vector<kor::ResourceRef<const kor::Shader>> ShadersOf(const Inputs& inputs, kor::ResourceRef<const kor::Shader>* vertex = nullptr)
        {
            std::vector<kor::ResourceRef<const kor::Shader>> shaders;
            for (const auto& [name, input] : inputs) {
                if (!input.resource || input.resource->kind != "Shader") continue;
                shaders.push_back(kor::capi::ShaderOf(input.resource->handle));
                if (vertex && name == "vertexShader.shader") *vertex = shaders.back();
            }
            return shaders;
        }

        /**
         * A pipeline: its shaders' parameters are split by what is wired. What is wired goes in set 0, which the
         * pipeline keeps a descriptor set for; what is not goes in set 1, for each entity to supply in the component
         * made for it here, along with the shaders' push constants.
         */
        /**
         * A pipeline node's inputs, as its builder takes them. The node names each shader by the stage it is — a
         * module and an entry point — and what goes with one by the stage too: here each named shader is made, and
         * everything is put under the name the builder's setting has.
         */
        Expected<Inputs> AsTheBuilderTakesThem(const Node& node, const kb::Description& description, const Inputs& given, PipelineInfo& info, Built& out)
        {
            Inputs inputs = given;
            const std::string stages = description.name == "GraphicsPipeline" ? StagesOf(node) : std::string();
            if (const auto it = inputs.find("stages"); it != inputs.end()) {
                const std::string* chosen = it->second.value.String();
                if (description.name != "GraphicsPipeline") return Fail(std::format("a {} has no input 'stages'", description.name));
                if (!it->second.value.IsNull() && (!chosen || (*chosen != "eVertex" && *chosen != "eMesh"))) return Fail("'stages' is eVertex or eMesh");
                inputs.erase(it);
            }
            const kb::Description* shaderType = kb::Find("Shader");
            const kb::Setting* entryPoint = shaderType ? shaderType->FindSetting("entryPoint") : nullptr;
            for (const auto& setting : description.settings) {
                if (HiddenPipelineSetting(setting.name)) continue;
                const std::string_view mode = StageMode(setting.name);
                // How many times it is given: once, or up to the highest index any of its inputs names.
                int count = 1;
                if (setting.repeatable) {
                    count = 0;
                    for (const auto& argument : setting.arguments)
                        for (const std::string& shown : { PipelineInputName(setting, argument), InputName(setting, argument) }) {
                            const std::string prefix = shown.substr(0, shown.find("[]") + 1);
                            for (const auto& name : given | std::views::keys)
                                if (name.starts_with(prefix)) count = std::max(count, std::atoi(name.c_str() + prefix.size()) + 1);
                        }
                }
                for (int index = 0; index < count; ++index) {
                    for (const auto& argument : setting.arguments) {
                        const std::string shown = PipelineInputName(setting, argument, setting.repeatable ? index : -1);
                        const std::string taken = InputName(setting, argument, setting.repeatable ? index : -1);
                        // One that belongs to the other way of drawing is refused before anything is made of it.
                        if (!mode.empty() && mode != stages)
                            for (const std::string& name : { shown + ".module", shown + ".entry", shown, taken })
                                if (given.contains(name) && (IsShader(argument) || name == shown || name == taken))
                                    return Fail(std::format("'{}' is part of a pipeline whose 'stages' is {}, and this one's is {}", name, mode, stages));
                        if (IsShader(argument)) {
                            const auto module = inputs.find(shown + ".module");
                            const auto entry = inputs.find(shown + ".entry");
                            const std::string* file = module == inputs.end() ? nullptr : module->second.value.String();
                            const std::string* function = entry == inputs.end() ? nullptr : entry->second.value.String();
                            if (module != inputs.end() && module->second.linked) return Fail(std::format("'{}.module' is the module's name, and cannot be wired", shown));
                            if (file && !file->empty()) {
                                if (inputs.contains(taken)) return Fail(std::format("'{}' is named and wired both: one or the other", shown));
                                if (!entryPoint) return Fail("the framework describes no Shader builder");
                                // The entry point, when it is not said: the stage's name and Main, as shaders here are written — of
                                // a hit group's, the stage's own: `hitGroup[0].closestHit`'s is closestHitMain, `miss[1]`'s missMain.
                                std::string stage = shown.substr(shown.rfind('.') == std::string::npos ? 0 : shown.rfind('.') + 1);
                                stage = stage.substr(0, stage.find('['));
                                std::string main = function && !function->empty() ? *function : stage + "Main";
                                auto shader = BuildWith(*shaderType, node.id, {}, { { entryPoint, { kb::Value::Of(*file), kb::Value::Of(main) } } });
                                if (!shader) {
                                    // The commonest of them, said as what to do about it.
                                    if (shader.error().find("cannot open file") != std::string::npos)
                                        return Fail(std::format("'{}': there is no {}.slang among the shaders. Put it in the project's shaders folder (the project has to be "
                                                                "saved for it to have one); it is picked up when it is there.", shown, *file));
                                    return Fail(std::format("'{}': {}", shown, shader.error()));
                                }
                                (*shader)->kind = "Shader";
                                Watch(out, kor::capi::ShaderOf((*shader)->handle));
                                info.named.push_back(*shader);
                                Resolved resolved;
                                resolved.resource = std::move(*shader);
                                resolved.linked = true;
                                inputs[taken] = std::move(resolved);
                            } else if (function && !function->empty()) {
                                return Fail(std::format("'{}.entry' is an entry point of a module: '{}.module' says which", shown, shown));
                            }
                            inputs.erase(shown + ".module");
                            inputs.erase(shown + ".entry");
                        } else if (shown != taken) {
                            if (const auto it = inputs.find(shown); it != inputs.end()) {
                                inputs[taken] = it->second;
                                inputs.erase(shown);
                            }
                        }
                    }
                }
            }
            return inputs;
        }

        Expected<void> BuildPipeline(const Node& node, const kb::Description& description, const Inputs& given, Built& out)
        {
            auto info = std::make_shared<PipelineInfo>();
            const auto translated = AsTheBuilderTakesThem(node, description, given, *info, out);
            if (!translated) return Fail(translated.error());
            const Inputs& inputs = *translated;
            info->shaders = ShadersOf(inputs, &info->vertexShader);
            const Interface interface = Reflect(info->shaders, Kinds(), world);
            info->ports = interface.ports;
            info->meshShaders = node.type == "GraphicsPipeline" && StagesOf(node) == "eMesh";
            info->vertexInputs = info->vertexShader.Get() && !info->vertexShader->BlockLayout().inputs.empty();
            out.pipeline = info;    // its inputs are known from here, built or not

            for (const auto& parameter : interface.parameters)
                if (!parameter.problem.empty()) return Fail(parameter.problem);
            const auto ours = [&](const std::string_view name) { return interface.Find(name) != nullptr; };
            for (const auto& name : inputs | std::views::keys)
                if (name.starts_with("push."))
                    return Fail(std::format("'{}' is a draw's now: give it on the Draw or the Dispatch that runs the pipeline, where what each draw has of its own is", name));
            for (const auto& name : inputs | std::views::keys)
                if ((name.starts_with("param.") || name.starts_with("constant.")) && !ours(name)) {
                    std::string known;
                    for (const auto& port : info->ports) known += std::format("{}{}", known.empty() ? "" : ", ", port.name);
                    return Fail(std::format("its shaders have no '{}'. They have: {}", name, known.empty() ? "nothing" : known));
                }

            std::vector<kb::Value> constructor;
            auto calls = Calls(description, inputs, constructor, ours);
            if (!calls) return Fail(calls.error());

            // The split, as bindings.
            const kb::Setting* binding = description.FindSetting("binding");
            const kb::Setting* constantSetting = description.FindSetting("specializationConstantNamed");
            std::uint64_t wired = 0, unwired = 0;
            std::vector<const Parameter*> bound, loose;
            for (const auto& parameter : interface.parameters) {
                const auto it = inputs.find("param." + parameter.name);
                const bool given = it != inputs.end() && it->second.resource;
                calls->push_back({ binding, { kb::Value::Of(parameter.name), kb::Value::Of(given ? std::uint64_t { 0 } : std::uint64_t { 1 }),
                                              kb::Value::Of(given ? wired++ : unwired++) } });
                if (given) { bound.push_back(&parameter); info->wired[parameter.name] = it->second.resource; }
                else { loose.push_back(&parameter); info->perEntity.push_back(parameter.name); }
            }
            for (const auto& constant : interface.constants) {
                const auto it = inputs.find("constant." + constant.name);
                // What a material shows of itself, as the view asks — unless the graph says otherwise.
                const bool shown = constant.name == ShaderDebugConstant && shaderDebug != 0 && (it == inputs.end() || it->second.value.IsNull());
                if (!shown && (it == inputs.end() || it->second.value.IsNull())) continue;
                const Value value = shown ? Value(shaderDebug) : it->second.value;
                std::vector<std::byte> bytes(constant.size);
                const double* number = value.Number();
                const bool* flag = value.Bool();
                if (!number && !flag) return Fail(std::format("'constant.{}' is a number, or true or false", constant.name));
                const double n = number ? *number : (*flag ? 1.0 : 0.0);
                if (constant.scalar == 0 && constant.size == 4) { const auto v = static_cast<float>(n); std::memcpy(bytes.data(), &v, 4); }
                else if (constant.scalar == 4 || (constant.scalar == 0 && constant.size == 8)) { std::memcpy(bytes.data(), &n, 8); }
                else if (constant.size == 8) { const auto v = static_cast<std::int64_t>(std::llround(n)); std::memcpy(bytes.data(), &v, 8); }
                else { const auto v = static_cast<std::int32_t>(std::llround(n)); std::memcpy(bytes.data(), &v, 4); }
                calls->push_back({ constantSetting, { kb::Value::Of(constant.name), kb::Value::Of(std::move(bytes)) } });
            }

            auto resource = BuildWith(description, node.id, constructor, *calls);
            if (!resource) return Fail(resource.error());
            KoralResource* handle = (*resource)->handle;
            info->pipeline =
                description.name == "ComputePipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::ComputePipelineOf(handle))
                : description.name == "GraphicsPipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::GraphicsPipelineOf(handle))
                : kor::ResourceRef<const kor::Pipeline>(kor::capi::RayTracingPipelineOf(handle));
            if (const auto it = inputs.find("framebuffer"); it != inputs.end()) info->framebuffer = it->second.resource;

            const auto samplerOf = [&](const Parameter& parameter) -> Held {
                const auto it = inputs.find("param." + parameter.name + ".sampler");
                return it == inputs.end() ? nullptr : it->second.resource;
            };

            // What is wired, bound once.
            if (!bound.empty()) {
                kor::DescriptorSet::Builder set(info->pipeline, 0);
                for (const Parameter* parameter : bound) {
                    const std::string port = "param." + parameter->name;
                    const Held& given = inputs.at(port).resource;
                    const auto written = WriteParameter(set, parameter->name, parameter->type, *given, samplerOf(*parameter).get(), port);
                    if (!written) return Fail(written.error());
                    // What a pass running the pipeline reads and writes, for the frame graph to order it by.
                    if (Member member = MemberOf(given.get()); member.id) info->uses.push_back({ std::move(member), parameter->Reads(), parameter->Writes() });
                }
                info->set = set.Build();
                if (!info->set.Valid())
                    return Fail(info->set.Failure() ? info->set.Failure()->message : std::string("its descriptor set could not be built"));
                info->hasSet = true;
            }

            // What is not: each draw's to give — a Draw's or a Dispatch's `param.<name>` (set 1) and `push.<name>`.
            for (const Parameter* parameter : loose) {
                info->slots.push_back({ parameter->name, parameter->name, parameter->type, false, parameter->Reads(), parameter->Writes(),
                                        parameter->instancing ? std::make_shared<Instancing>(*parameter->instancing) : nullptr });
                if (parameter->type != kor::DescriptorType::eCombinedImageSampler) continue;
                // Its sampler: the one the pipeline's node wires, for every draw; or each draw's own.
                if (Held sampler = samplerOf(*parameter)) info->samplers[parameter->name] = std::move(sampler);
                else info->slots.push_back({ parameter->name + ".sampler", parameter->name, kor::DescriptorType::eSampler, true, true, false });
            }
            const auto& constants = info->pipeline->PushConstants();
            if (!constants.empty()) {
                kor::u32 first = ~kor::u32 { 0 }, end = 0;
                for (const auto& member : constants | std::views::values) {
                    first = std::min(first, member.offset);
                    end = std::max(end, member.offset + member.size);
                }
                info->pushFirst = first;
                info->pushSize = end - first;
                for (const auto& [name, member] : constants) {
                    if (member.aggregate || member.count != 1) continue;    // its fields and elements are listed on their own
                    info->pushes.push_back({ name, member.offset - first, member.size, member.scalar, member.rows, member.columns, {} });
                }
                std::ranges::sort(info->pushes, {}, &PushMember::offset);
            }
            for (const Slot& slot : info->slots)
                info->drawPorts.push_back({ "param." + slot.field, PortKind::eResource, KindOf(slot.type), false,
                                            slot.sampler ? "The sampler its picture is read with." : "What the pipeline's node leaves unwired: each draw's own.", nullptr });
            for (PushMember& push : info->pushes) {
                Port port { "push." + push.name, PortKind::eValue, push.rows * push.columns == 16 ? "matrix" : push.rows * push.columns == 1 ? (push.scalar == 3 ? "bool" : "number") : "vector",
                            false, "A push constant: each draw's own. Its default in the shader when not given, or zero.", nullptr };
                port.optional = true;
                if (push.columns == 1) port.shape = ShapeOfScalar(push.scalar, push.rows);
                port.integer = port.shape.Whole();
                if (port.shape.scalar == Scalar::eUInt) port.min = 0.f;
                if (push.rows >= 3 && push.columns == 1 && push.scalar == 0 && NamesColor(push.name)) port.editor = "color";
                if (const auto* member = info->pipeline->FindPushConstant(push.name)) Annotate(port, member->annotation);
                push.initial = port.initial;
                info->drawPorts.push_back(std::move(port));
            }

            // What its draws take from nobody else is a component of its own, for an entity to hold: a ForEach of it gives
            // each as `field.<name>`, to be wired back to the draw.
            const std::set<std::string> taken = TakenElsewhere(node.id);
            auto layout = std::make_shared<EntityLayout>();
            // A buffer of each instance's is the draw's to make: what is in the component is each instance's own fields.
            for (const Slot& slot : info->slots)
                if (!taken.contains("param." + slot.field) && !slot.instancing) layout->slots.push_back(slot);
            // A picture an instance reads by its bindless index is held by the entity, as a parameter's picture would be; so
            // is an image it writes.
            for (const Slot& slot : info->slots) {
                if (!slot.instancing || taken.contains("param." + slot.field)) continue;
                for (auto& field : slot.instancing->fields) {
                    if (!field.Pictured() || field.Material()) continue;    // a material's is the material's to hold
                    if (std::ranges::find(layout->slots, field.name, &Slot::field) != layout->slots.end())
                        return Fail(std::format("'{}' is a picture of '{}' and of something else its draws take: the entity's own can be only one of them", field.name, slot.parameter));
                    field.at = layout->slots.size() * sizeof(Held);
                    const bool written = field.from == Instancing::Field::From::eStorage;
                    layout->slots.push_back({ field.name, field.name, written ? kor::DescriptorType::eStorageImage : kor::DescriptorType::eSampledImage, false, true, written });
                }
            }
            for (std::size_t i = 0; i < layout->slots.size(); ++i)
                layout->fields.push_back({ layout->slots[i].field, KindOf(layout->slots[i].type), i * sizeof(Held), sizeof(Held), &AssignHeld });
            const auto aligned = [](const std::size_t bytes) { return (bytes + 15) / 16 * 16; };
            layout->pushFirst = info->pushFirst;
            layout->pushAt = aligned(layout->slots.size() * sizeof(Held));
            std::size_t pushEnd = 0;
            for (const auto& [name, member] : constants) {
                if (member.aggregate || member.count != 1 || taken.contains("push." + name)) continue;
                const auto [type, size] = ConstantType(member);
                layout->fields.push_back({ name, type, layout->pushAt + member.offset - info->pushFirst, size, nullptr });
                pushEnd = std::max<std::size_t>(pushEnd, member.offset - info->pushFirst + size);
            }
            std::size_t end = layout->pushAt + pushEnd;
            for (const Slot& slot : info->slots) {
                if (!slot.instancing || taken.contains("param." + slot.field)) continue;
                for (auto& field : slot.instancing->fields) {
                    if (field.from != Instancing::Field::From::eComponent) continue;
                    if (std::ranges::find(layout->fields, field.name, &kecs::FieldDescription::name) != layout->fields.end())
                        return Fail(std::format("'{}' is a field of '{}' and of something else its draws take: the entity's own can be only one of them", field.name, slot.parameter));
                    field.at = end;
                    layout->fields.push_back({ field.name, field.type, end, field.size, nullptr });
                    end += field.size;
                }
            }
            layout->size = std::max<std::size_t>(aligned(end), 16);
            // A new entity's push constants start as their shader declares them.
            for (const PushMember& push : info->pushes) {
                if (push.initial.IsNull() || taken.contains("push." + push.name)) continue;
                if (layout->initial.empty()) layout->initial.assign(layout->size, std::byte { 0 });
                PackPush(push, push.initial, layout->initial.data() + layout->pushAt + push.offset);
            }
            if (world && !layout->fields.empty()) {
                info->layout = layout;
                const auto registered = Register(node.id, *info, out);
                if (!registered) return Fail(registered.error());
            }
            info->resource = *resource;
            // What it is made with, to be made again for a mesh of another vertex layout.
            if (description.name == "GraphicsPipeline") {
                auto& variants = *info->variants;
                variants.description = &description;
                variants.node = node.id;
                variants.constructor = constructor;
                variants.calls = *calls;
                for (const auto& setting : description.settings)
                    for (std::size_t a = 0; a < setting.arguments.size(); ++a)
                        if (setting.name == "vertexShader" && setting.arguments[a].name == "layout") { variants.layoutSetting = &setting; variants.layoutArgument = a; }
            }
            out.outputs["out"].resource = std::move(*resource);
            if (node.type == "ComputePipeline" && !info->shaders.empty()) {
                const auto& size = info->shaders.front()->BlockLayout().localSize;
                std::ranges::copy(size, info->numThreads);
                out.outputs["numThreads"].value = Value(Value::List { size[0], size[1], size[2] });
            }
            return {};
        }

        /** What a link comes from, past the References between: the node, by id. */
        [[nodiscard]] const Node* Origin(const std::string& id) const
        {
            const auto find = [&](const std::string_view which) -> const Node* {
                if (!current) return nullptr;
                if (currentById.size() == current->nodes.size()) {
                    const auto it = currentById.find(which);
                    return it == currentById.end() ? nullptr : it->second;
                }
                return current->Find(which);
            };
            const Node* from = find(id);
            for (int hops = 0; from && from->type == "Reference" && hops < 16; ++hops) {
                const auto target = from->inputs.find("node");
                from = target != from->inputs.end() && target->second.value.String() ? find(*target->second.value.String()) : nullptr;
            }
            return from;
        }

        /**
         * What the draws of pipeline @p id — the Draws and Dispatches it is wired to — take from elsewhere than the
         * pipeline's own component: each `param.` and `push.` given on the node, or wired from anything but a ForEach of
         * that component's field of the same name. What is left is the component.
         */
        [[nodiscard]] std::set<std::string> TakenElsewhere(const std::string& id) const
        {
            if (!current) return {};
            // Every pipeline's at once, the first time one is asked of this graph.
            if (!takenByPipeline) {
                takenByPipeline.emplace();
                for (const Node& node : current->nodes) {
                    if (node.type != "Draw" && node.type != "Dispatch" && node.type != "TraceRays") continue;
                    const auto pipeline = node.inputs.find("pipeline");
                    if (pipeline == node.inputs.end() || !pipeline->second.link) continue;
                    if (const Node* of = Origin(pipeline->second.link->node)) TakenBy(node, of->id, (*takenByPipeline)[of->id]);
                }
            }
            const auto it = takenByPipeline->find(id);
            return it == takenByPipeline->end() ? std::set<std::string>() : it->second;
        }

        /** What a draw of pipeline @p id takes from elsewhere than its component, added to @p taken. */
        void TakenBy(const Node& node, const std::string& id, std::set<std::string>& taken) const
        {
            {
                for (const auto& [name, input] : node.inputs) {
                    if (!name.starts_with("param.") && !name.starts_with("push.")) continue;
                    const std::string field = name.substr(name.find('.') + 1);
                    if (!input.link) { if (!input.value.IsNull()) taken.insert(name); continue; }
                    const Node* from = Origin(input.link->node);
                    const auto component = from ? from->inputs.find("component") : node.inputs.end();
                    const bool own = from && IsEach(from->type) && component != from->inputs.end() && component->second.value.String()
                                     && *component->second.value.String() == id && input.link->output == "field." + field;
                    const bool beside = from && IsEach(from->type) && input.link->output == id + "." + field;
                    if (!own && !beside) taken.insert(name);
                }
            }
        }

        /** Makes the world's component of a pipeline's layout, or gives the one it has its new shape. */
        Expected<void> Register(const std::string& name, PipelineInfo& info, Built& out)
        {
            const EntityLayout& layout = *info.layout;
            kecs::ComponentDescription description;
            description.name = name;
            description.size = layout.size;
            description.alignment = 16;
            description.construct = &ConstructEntity;
            description.copy = &CopyEntity;
            description.relocate = &RelocateEntity;
            description.destroy = &DestroyEntity;
            description.fields = layout.fields;
            description.context = info.layout;

            const kecs::ComponentId existing = world->Find(name);
            if (existing == kecs::NoComponent) {
                info.component = world->Register(std::move(description));
                return {};
            }
            const kecs::ComponentDescription& now = world->Describe(existing);
            if (now.construct != &ConstructEntity)
                return Fail(std::format("its component would be named '{}' after it, and there is one so named already: the node needs another id", name));
            const bool same = now.size == layout.size && std::ranges::equal(now.fields, layout.fields, [](const kecs::FieldDescription& a, const kecs::FieldDescription& b) {
                return a.SameAs(b) && a.offset == b.offset;
            });
            if (!same) {
                try { out.migration = world->Reshape(existing, std::move(description)); }
                catch (const std::exception& e) { return Fail(e.what()); }
            }
            info.component = existing;
            return {};
        }

        Expected<void> BuildPass(const Node& node, const Inputs& inputs, Built& out)
        {
            auto plan = std::make_shared<PassPlan>();
            const auto image = [&](const char* name) -> Expected<Held> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || !it->second.resource) return Fail(std::format("'{}' needs an image wired in", name));
                if (it->second.resource->screen) return it->second.resource;
                if (!ImageRefOf(it->second.resource.get()).Get()) return Fail(std::format("'{}' takes an Image, and was given a {}", name, it->second.resource->kind));
                return it->second.resource;
            };
            const auto lacks = [](const Resource& resource, const kor::Image::Usage usage) { return MadeWithout(resource, usage); };
            const auto value = [&](const char* name) -> const Value& {
                static const Value none;
                const auto it = inputs.find(name);
                return it == inputs.end() ? none : it->second.value;
            };
            const auto counts = [&](const char* prefix, kor::u32 (&into)[3], const char* what) -> Expected<void> {
                const char* axes[3] { ".x", ".y", ".z" };
                for (int axis = 0; axis < 3; ++axis) {
                    const std::string name = std::string(prefix) + axes[axis];
                    const Value& given = value(name.c_str());
                    if (given.IsNull()) continue;
                    const double* count = given.Number();
                    // Rounded up: what an image's extent divided by a group's size comes to is how many cover it.
                    if (!count || !(*count > 0) || *count > 65535) return Fail(std::format("'{}' is {}, more than none", name, what));
                    into[axis] = static_cast<kor::u32>(std::ceil(*count - 1e-9));
                }
                return {};
            };
            // What a pass is set up with, read one at a time: the first that is wrong is why the node fails.
            std::string wrong;
            const auto mistaken = [&](std::string why) { if (wrong.empty()) wrong = std::move(why); };
            const auto has = [&](const char* name) { return !value(name).IsNull(); };
            const auto num = [&](const char* name, const double otherwise, const double least = 0, const double most = 4294967295.0) -> double {
                const Value& given = value(name);
                if (given.IsNull()) return otherwise;
                const double* number = given.Number();
                if (!number || *number < least || *number > most) { mistaken(std::format("'{}' is a number from {} to {}", name, least, most)); return otherwise; }
                return *number;
            };
            const auto flag = [&](const char* name) -> std::optional<bool> {
                const Value& given = value(name);
                if (given.IsNull()) return std::nullopt;
                if (!given.Bool()) { mistaken(std::format("'{}' is true or false", name)); return std::nullopt; }
                return *given.Bool();
            };
            const auto pick = [&](const char* name, const std::vector<std::string>& of) -> int {   // which of them; -1 when not given
                const Value& given = value(name);
                if (given.IsNull()) return -1;
                const std::string* chosen = given.String();
                const auto at = chosen ? std::ranges::find(of, *chosen) : of.end();
                if (at == of.end()) {
                    std::string all;
                    for (const auto& each : of) all += std::format("{}{}", all.empty() ? "" : ", ", each);
                    mistaken(std::format("'{}' is one of {}", name, all));
                    return -1;
                }
                return static_cast<int>(at - of.begin());
            };
            const auto several = [&](const char* name, const std::size_t count, const char* what) -> std::optional<std::vector<float>> {
                const Value& given = value(name);
                if (given.IsNull()) return std::nullopt;
                const auto* items = given.Items();
                if (!items || items->size() != count || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; })) {
                    mistaken(std::format("'{}' is {} numbers: {}", name, count, what));
                    return std::nullopt;
                }
                std::vector<float> out;
                for (const auto& item : *items) out.push_back(static_cast<float>(*item.Number()));
                return out;
            };
            const auto each3 = [&](const std::string& prefix, const int otherwise) {
                kor::IVec3 out(otherwise);
                const char* axes[3] { ".x", ".y", ".z" };
                for (int axis = 0; axis < 3; ++axis) out[axis] = static_cast<int>(std::llround(num((prefix + axes[axis]).c_str(), otherwise, 0, 65536)));
                return out;
            };
            const auto whole = [&](const char* name, const double otherwise = 0) { return static_cast<kor::u32>(std::llround(num(name, otherwise))); };
            // A buffer wired in; empty when nothing is, and wrong when it has to be there or lacks what it is used as.
            const auto bufferIn = [&](const char* name, const bool needed, const kor::Buffer::Usage usage, const char* usageName, const char* as) -> kor::ResourceRef<const kor::Buffer> {
                const auto it = inputs.find(name);
                const auto ref = it == inputs.end() ? kor::ResourceRef<const kor::Buffer>() : BufferRefOf(it->second.resource.get());
                if (!ref.Get()) {
                    if (needed || (it != inputs.end() && it->second.resource)) mistaken(std::format("'{}' needs a buffer wired in", name));
                    return {};
                }
                if (MadeWithout(*it->second.resource, usage)) mistaken(std::format("the buffer wired to '{}' needs {} among its usage, {}", name, usageName, as));
                return ref;
            };
            const auto bufferUse = [](const kor::ResourceRef<const kor::Buffer>& buffer) {
                Member member;
                member.id = buffer.Get();
                member.buffer = buffer;
                return member;
            };
            // The pipeline a pass runs, and what its node worked out.
            const auto pipelineOf = [&](const char* kind) -> Expected<std::shared_ptr<const PipelineInfo>> {
                const auto it = inputs.find("pipeline");
                if (it == inputs.end() || !it->second.resource || it->second.resource->kind != kind) return Fail(std::format("'pipeline' needs a {} wired in", kind));
                const auto source = built.find(it->second.resource->node);
                if (source == built.end() || !source->second.pipeline) return Fail("'pipeline' is not a pipeline node's output");
                return std::shared_ptr<const PipelineInfo>(source->second.pipeline);
            };
            // What a draw has of its own, and where each comes from: what its pipeline leaves unwired, and its mesh. Wired
            // from a ForEach, it is a draw an element; otherwise, one.
            const auto drawsOf = [&](const std::shared_ptr<const PipelineInfo>& info, const bool draws) -> Expected<std::shared_ptr<EntityRun>> {
                out.ports = info->drawPorts;     // shown, built or not
                if (has("with") || has("without"))
                    return Fail("'with' and 'without' are a ForEach's now: draw from a ForEach of Mesh, `with` what else an entity has to have");
                for (const auto& name : inputs | std::views::keys)
                    if ((name.starts_with("param.") || name.starts_with("push.")) && std::ranges::find(info->drawPorts, name, &Port::name) == info->drawPorts.end()
                        && !FromBundle(node.id, name)) {     // given by a Bundle, by name: let pass, and said on the node
                        std::string known;
                        for (const auto& port : info->drawPorts) known += std::format("{}{}", known.empty() ? "" : ", ", port.name);
                        return Fail(std::format("its pipeline leaves it no '{}': a parameter its node wires is the same for every draw. It leaves: {}", name, known.empty() ? "nothing" : known));
                    }
                auto run = std::make_shared<EntityRun>();
                run->info = info;
                run->draws = draws;
                std::vector<std::string> names { "mesh" };
                for (const Slot& slot : info->slots) names.push_back("param." + slot.field);
                for (const PushMember& push : info->pushes) names.push_back("push." + push.name);
                // The ForEach they follow, when any does: one, as a buffer's fields do.
                std::set<std::string> sources;
                std::map<std::string, bool> seen;
                for (const auto& name : names)
                    if (const auto wired = node.inputs.find(name); wired != node.inputs.end() && wired->second.link)
                        Follows(wired->second.link->node, sources, run->chain, seen);
                if (sources.size() > 1)
                    return Fail(std::format("what it draws with follows more than one ForEach, '{}' and '{}': a draw is one element of one", *sources.begin(), *std::next(sources.begin())));
                const Built* each = nullptr;
                if (!sources.empty()) {
                    run->each = *sources.begin();
                    const auto found = built.find(run->each);
                    if (found == built.end() || !found->second.status.ok || !found->second.listed) return Fail(std::format("'{}' is not built", run->each));
                    each = &found->second;
                }
                const auto classify = [&](const std::string& name) -> Source {
                    Source source;
                    source.input = name;
                    const auto wired = node.inputs.find(name);
                    if (wired == node.inputs.end()) return source;
                    if (!wired->second.link) {
                        if (!wired->second.value.IsNull()) { source.kind = Source::Kind::eGiven; source.value = wired->second.value; }
                        return source;
                    }
                    const auto& link = *wired->second.link;
                    source.node = link.node;
                    source.output = link.output;
                    std::set<std::string> theirs;
                    std::vector<std::string> between;
                    std::map<std::string, bool> looked;
                    if (!each || !Follows(link.node, theirs, between, looked)) {
                        source.kind = Source::Kind::eWired;
                        if (const auto resolved = inputs.find(name); resolved != inputs.end()) source.resource = resolved->second.resource;
                        return source;
                    }
                    if (link.node != run->each) { source.kind = Source::Kind::eChain; run->rows = true; return source; }
                    for (const auto& read : each->listed->fields)
                        if (read.output == link.output) {
                            source.kind = Source::Kind::eField;
                            source.component = read.component;
                            source.field = read.field;
                            source.held = read.resource;
                            return source;
                        }
                    source.kind = Source::Kind::eWhere;
                    // Where it is, and which it is, are read straight from the world; anything else the ForEach says, through it.
                    if (link.output != "matrix" && link.output != "normalMatrix" && link.output != "entityId") { run->asked.insert(link.output); run->rows = true; }
                    return source;
                };
                run->mesh = classify("mesh");
                for (const Slot& slot : info->slots) {
                    run->slots.push_back(classify("param." + slot.field));
                    if (run->slots.back().kind == Source::Kind::eNone && slot.instancing) {
                        auto made = InstancesFor(node, *info, slot, *run);
                        if (!made) return Fail(made.error());
                        run->slots.back() = std::move(*made);
                    }
                    if (run->slots.back().kind == Source::Kind::eNone)
                        return Fail(slot.sampler ? std::format("'param.{}' needs a sampler: here, or wired on the pipeline for every draw", slot.field)
                                                 : std::format("'param.{}' is given nothing: wire it here — from a ForEach, for each element's own — or on the pipeline, for every draw", slot.field));
                    if (run->slots.back().kind == Source::Kind::eGiven) return Fail(std::format("'param.{}' takes a resource, wired: not a value", slot.field));
                }
                for (const PushMember& push : info->pushes) {
                    Source source = classify("push." + push.name);
                    // Given nothing: its default in the shader.
                    if (source.kind == Source::Kind::eNone && !push.initial.IsNull()) { source.kind = Source::Kind::eGiven; source.value = push.initial; }
                    if (source.kind == Source::Kind::eField && !source.held) {
                        kor::Pipeline::PushConstantMember member;
                        member.scalar = static_cast<kor::u8>(push.scalar);
                        member.rows = static_cast<kor::u8>(push.rows);
                        member.columns = static_cast<kor::u8>(push.columns);
                        member.size = push.size;
                        member.count = 1;
                        const auto [type, size] = ConstantType(member);
                        source.raw = source.field.type == type && source.field.size >= size;
                    }
                    if (source.kind == Source::Kind::eField && source.held) return Fail(std::format("'push.{}' is a value, and was wired to a resource", push.name));
                    run->pushes.push_back(std::move(source));
                }
                // What is worked out between asks the ForEach for what it is worked out of.
                for (const auto& id : run->chain)
                    if (const Node* between = current ? current->Find(id) : nullptr)
                        for (const auto& input : between->inputs | std::views::values)
                            if (input.link && input.link->node == run->each) run->asked.insert(input.link->output);
                return run;
            };

            if (node.type == "Clear") {
                const auto target = image("image");
                if (!target) return Fail(target.error());
                kor::Vec4 color(0.f, 0.f, 0.f, 1.f);
                if (const Value& given = value("color"); !given.IsNull()) {
                    const auto* items = given.Items();
                    if (!items || items->size() != 4 || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; }))
                        return Fail("'color' is four numbers: red, green, blue, alpha");
                    for (int i = 0; i < 4; ++i) color[i] = static_cast<float>(*(*items)[static_cast<std::size_t>(i)].Number());
                }
                if ((*target)->screen) {
                    plan->writesScreen = true;
                    plan->record = [color](kor::CommandBuffer& cb, const PassContext& context) { cb.ClearColorImage(context.screen, color); };
                } else {
                    if (lacks(**target, kor::Image::Usage::eTransferDst)) return Fail("the image needs eTransferDst among its usage, to be cleared");
                    const auto ref = ImageRefOf(target->get());
                    // What is cleared to is the bits of a clear value: a depth image's depth and stencil, and an image of whole
                    // numbers' numbers, are those bits, not the floats a colour is.
                    const std::string format = FormatName(ref->PixelFormat());
                    const TexelLayout texel = TexelLayoutOf(format);
                    const auto bits = [](const std::uint32_t whole) { return std::bit_cast<float>(whole); };
                    if (format.starts_with("eD") || format.starts_with("eS8")) {
                        const double depth = num("depth", 1.0, 0.0, 1.0);
                        const double stencil = num("stencil", 0.0, 0.0, 255.0);
                        if (!wrong.empty()) return Fail(wrong);
                        color = kor::Vec4(static_cast<float>(depth), bits(static_cast<std::uint32_t>(stencil)), 0.f, 0.f);
                    } else if (texel.kind == 'U' || texel.kind == 'I') {
                        for (int i = 0; i < 4; ++i) {
                            const double n = std::trunc(static_cast<double>(color[i]));
                            color[i] = texel.kind == 'U' ? bits(static_cast<std::uint32_t>(std::clamp(n, 0.0, 4294967295.0)))
                                                         : bits(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(std::clamp(n, -2147483648.0, 2147483647.0))));
                        }
                    }
                    plan->uses.push_back({ MemberOf(ref), false, true });
                    plan->record = [color, ref](kor::CommandBuffer& cb, const PassContext&) { cb.ClearColorImage(ref, color); };
                }
            } else if (node.type == "Blit") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen) return Fail("'source' cannot be the screen");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be copied from");
                const auto from = ImageRefOf(source->get());
                kor::Blit how;
                if (pick("filter", { "eNearest", "eLinear" }) == 1) how.filtering = kor::Filter::eLinear;
                how.srcOffset = each3("sourceOffset", 0);
                how.srcExtent = each3("sourceExtent", -1);
                how.dstOffset = each3("destinationOffset", 0);
                how.dstExtent = each3("destinationExtent", -1);
                how.srcMipLevel = whole("sourceMip");
                how.dstMipLevel = whole("destinationMip");
                how.srcBaseArrayLayer = whole("sourceLayer");
                how.dstBaseArrayLayer = whole("destinationLayer");
                how.layerCount = whole("layers", 1);
                if (!wrong.empty()) return Fail(wrong);
                if (how.srcMipLevel >= from->MipLevels()) return Fail(std::format("'sourceMip' is {}, and the source has {} mip levels", how.srcMipLevel, from->MipLevels()));
                plan->uses.push_back({ MemberOf(from), true, false });
                if ((*destination)->screen) {
                    plan->writesScreen = true;
                    plan->record = [from, how](kor::CommandBuffer& cb, const PassContext& context) { cb.Blit(from, context.screen, how); };
                } else {
                    if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be copied to");
                    const auto to = ImageRefOf(destination->get());
                    if (how.dstMipLevel >= to->MipLevels()) return Fail(std::format("'destinationMip' is {}, and the destination has {} mip levels", how.dstMipLevel, to->MipLevels()));
                    // A part of it written leaves the rest as it was: it is read as well.
                    const bool part = has("destinationOffset.x") || has("destinationOffset.y") || has("destinationExtent.x") || has("destinationExtent.y") || how.dstMipLevel > 0;
                    plan->uses.push_back({ MemberOf(to), part, true });
                    plan->record = [from, to, how](kor::CommandBuffer& cb, const PassContext&) { cb.Blit(from, to, how); };
                }
            } else if (node.type == "Resolve") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen) return Fail("'source' cannot be the screen");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be resolved from");
                const auto from = ImageRefOf(source->get());
                if (from->Samples() == kor::SampleCount::e1)
                    return Fail("the source has one sample a pixel, and there is nothing to resolve: give it more (sampleCount), or use a Blit");
                kor::Resolve how;
                how.srcOffset = each3("sourceOffset", 0);
                how.srcExtent = each3("sourceExtent", -1);
                how.dstOffset = each3("destinationOffset", 0);
                how.dstExtent = each3("destinationExtent", -1);
                how.srcMipLevel = whole("sourceMip");
                how.dstMipLevel = whole("destinationMip");
                how.srcBaseArrayLayer = whole("sourceLayer");
                how.dstBaseArrayLayer = whole("destinationLayer");
                how.layerCount = whole("layers", 1);
                if (!wrong.empty()) return Fail(wrong);
                plan->uses.push_back({ MemberOf(from), true, false });
                if ((*destination)->screen) {
                    plan->writesScreen = true;
                    plan->record = [from, how](kor::CommandBuffer& cb, const PassContext& context) { cb.Resolve(from, context.screen, how); };
                } else {
                    if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be resolved to");
                    const auto to = ImageRefOf(destination->get());
                    if (to->Samples() != kor::SampleCount::e1) return Fail("the destination is multisampled too: a resolve is onto an image of one sample a pixel");
                    plan->uses.push_back({ MemberOf(to), false, true });
                    plan->record = [from, to, how](kor::CommandBuffer& cb, const PassContext&) { cb.Resolve(from, to, how); };
                }
            } else if (node.type == "Copy") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen || (*destination)->screen) return Fail("the screen is shown with a Blit: a Copy is between two images of one size");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be copied from");
                if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be copied to");
                const auto from = ImageRefOf(source->get());
                const auto to = ImageRefOf(destination->get());
                if (from->Extent() != to->Extent())
                    return Fail(std::format("a Copy is between images of one size, and these are {}x{} and {}x{}: a Blit scales", from->Extent().x, from->Extent().y, to->Extent().x, to->Extent().y));
                plan->uses.push_back({ MemberOf(from), true, false });
                plan->uses.push_back({ MemberOf(to), false, true });
                plan->record = [from, to](kor::CommandBuffer& cb, const PassContext&) { cb.CopyImage(from, to); };
            } else if (node.type == "GenerateMipmaps") {
                const auto target = image("image");
                if (!target) return Fail(target.error());
                if ((*target)->screen) return Fail("the screen has no mip levels");
                if (lacks(**target, kor::Image::Usage::eTransferSrc) || lacks(**target, kor::Image::Usage::eTransferDst))
                    return Fail("the image needs eTransferSrc and eTransferDst among its usage: each level is made by copying the one above");
                const auto ref = ImageRefOf(target->get());
                if (ref->MipLevels() < 2) return Fail("the image has one mip level: give it more (mipLevels) for there to be any to fill");
                plan->uses.push_back({ MemberOf(ref), true, true });
                plan->record = [ref](kor::CommandBuffer& cb, const PassContext&) { cb.GenerateMipmaps(ref); };
            } else if (node.type == "CopyBuffer") {
                const auto from = bufferIn("source", true, kor::Buffer::Usage::eTransferSrc, "eTransferSrc", "to be copied from");
                const auto to = bufferIn("destination", true, kor::Buffer::Usage::eTransferDst, "eTransferDst", "to be copied to");
                const auto sourceOffset = static_cast<kor::u64>(num("sourceOffset", 0, 0, 1e15));
                const auto destinationOffset = static_cast<kor::u64>(num("destinationOffset", 0, 0, 1e15));
                if (!wrong.empty()) return Fail(wrong);
                if (sourceOffset >= from->size() || destinationOffset >= to->size()) return Fail("an offset is past the end of its buffer");
                const kor::u64 most = std::min(from->size() - sourceOffset, to->size() - destinationOffset);
                const auto bytes = static_cast<kor::u64>(num("bytes", static_cast<double>(most), 1, static_cast<double>(most)));
                if (!wrong.empty()) return Fail(wrong);
                plan->uses.push_back({ bufferUse(from), true, false });
                plan->uses.push_back({ bufferUse(to), bytes < to->size(), true });
                plan->record = [from, to, bytes, sourceOffset, destinationOffset](kor::CommandBuffer& cb, const PassContext&) {
                    cb.CopyBuffer(from, to, bytes, sourceOffset, destinationOffset);
                };
            } else if (node.type == "Keep") {
                // A copy of its own of what it is given, made like it — and to be read, sampled or as storage — and copied into where this pass runs.
                const auto given = inputs.find("of");
                if (given == inputs.end() || !given->second.resource) return Fail("'of' needs a buffer or an image wired in: what is kept a copy of");
                const Held& of = given->second.resource;
                if (of->screen) return Fail("the screen is not kept: draw it into an image, and keep that");
                Held kept;
                if (const auto buffer = BufferRefOf(of.get()); buffer.Get()) {
                    if (MadeWithout(*of, kor::Buffer::Usage::eTransferSrc)) return Fail("the buffer needs eTransferSrc among its usage, to be kept a copy of");
                    auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(buffer->size()))
                        .SetUsage(buffer->UsageFlags() | kor::Buffer::Usage::eTransferDst | kor::Buffer::Usage::eStorage).SetType(kor::Buffer::Type::eDeviceLocal).Build();
                    if (!made.Valid()) return Fail("its copy could not be made");
                    kept = MakeResource(std::move(made));
                    const auto to = BufferRefOf(kept.get());
                    // Its source as read and written, though only read: so that it takes its place among what writes it, where
                    // it is among the passes — not after all of them, as what only reads does — and copies it as it is there.
                    plan->uses.push_back({ bufferUse(buffer), true, true });
                    plan->uses.push_back({ bufferUse(to), false, true });
                    plan->record = [buffer, to](kor::CommandBuffer& cb, const PassContext&) { cb.CopyBuffer(buffer, to, buffer->size(), 0, 0); };
                } else if (const auto picture = ImageRefOf(of.get()); picture.Get()) {
                    if (MadeWithout(*of, kor::Image::Usage::eTransferSrc)) return Fail("the image needs eTransferSrc among its usage, to be kept a copy of");
                    if (picture->Samples() != kor::SampleCount::e1) return Fail("a multisampled image is not kept: resolve it into one of a sample a pixel, and keep that");
                    auto made = kor::Image::Builder().SetType(picture->ImageType()).SetFormat(picture->PixelFormat()).SetExtent(picture->Extent())
                        .SetMipLevels(picture->MipLevels()).SetArrayLayers(picture->ArrayLayers())
                        .SetUsage(picture->UsageFlags() | kor::Image::Usage::eTransferDst | kor::Image::Usage::eSampled).Build();
                    if (!made.Valid()) return Fail("its copy could not be made");
                    kept = MakeResource(std::move(made));
                    const auto to = ImageRefOf(kept.get());
                    plan->uses.push_back({ MemberOf(picture), true, true });     // (read and written: as a buffer's, above)
                    plan->uses.push_back({ MemberOf(to), false, true });
                    plan->record = [picture, to](kor::CommandBuffer& cb, const PassContext&) { cb.CopyImage(picture, to); };
                } else {
                    return Fail(std::format("'of' is a {}: a buffer or an image is kept", of->kind));
                }
                kept->node = node.id;
                out.outputs["out"].resource = std::move(kept);
                // Valid once it has been copied into: on the frames after the first that records it.
                out.recorded = std::make_shared<std::atomic<std::uint64_t>>(0);
                out.outputs["valid"].value = false;
                plan->record = [copy = std::move(plan->record), recorded = out.recorded](kor::CommandBuffer& cb, const PassContext& context) {
                    copy(cb, context);
                    ++*recorded;
                };
            } else if (node.type == "ClearBuffer") {
                const auto buffer = bufferIn("buffer", true, kor::Buffer::Usage::eTransferDst, "eTransferDst", "to be cleared");
                const auto offset = static_cast<kor::u64>(num("offset", 0, 0, 1e15));
                if (!wrong.empty()) return Fail(wrong);
                if (offset >= buffer->size()) return Fail("'offset' is past the end of the buffer");
                const auto bytes = static_cast<kor::u64>(num("bytes", static_cast<double>(buffer->size() - offset), 1, static_cast<double>(buffer->size() - offset)));
                if (!wrong.empty()) return Fail(wrong);
                plan->uses.push_back({ bufferUse(buffer), bytes < buffer->size(), true });
                plan->record = [buffer, offset, bytes](kor::CommandBuffer& cb, const PassContext&) { cb.ClearBuffer(buffer, offset, bytes); };
            } else if (node.type == "CopyBufferToImage" || node.type == "CopyImageToBuffer") {
                const bool toImage = node.type == "CopyBufferToImage";
                const auto picture = image("image");
                if (!picture) return Fail(picture.error());
                if ((*picture)->screen) return Fail("the screen is not copied to or from a buffer: an image is, and a Blit shows it");
                const auto buffer = toImage ? bufferIn("buffer", true, kor::Buffer::Usage::eTransferSrc, "eTransferSrc", "to be copied from")
                                            : bufferIn("buffer", true, kor::Buffer::Usage::eTransferDst, "eTransferDst", "to be copied to");
                if (lacks(**picture, toImage ? kor::Image::Usage::eTransferDst : kor::Image::Usage::eTransferSrc))
                    return Fail(toImage ? "the image needs eTransferDst among its usage, to be copied to" : "the image needs eTransferSrc among its usage, to be copied from");
                const auto ref = ImageRefOf(picture->get());
                kor::Copy how;
                how.bufferOffset = static_cast<kor::u64>(num("bufferOffset", 0, 0, 1e15));
                how.bufferRowLength = whole("bufferRowLength");
                how.bufferImageHeight = whole("bufferImageHeight");
                how.imageOffset = each3("imageOffset", 0);
                how.imageExtent = each3("imageExtent", -1);
                how.imageMipLevel = whole("mip");
                how.imageBaseArrayLayer = whole("layer");
                how.imageLayerCount = whole("layers", 1);
                if (!wrong.empty()) return Fail(wrong);
                if (how.imageMipLevel >= ref->MipLevels()) return Fail(std::format("'mip' is {}, and the image has {} mip levels", how.imageMipLevel, ref->MipLevels()));
                if (how.bufferOffset >= buffer->size()) return Fail("'bufferOffset' is past the end of the buffer");
                const bool part = has("imageOffset.x") || has("imageOffset.y") || has("imageExtent.x") || has("imageExtent.y") || how.imageMipLevel > 0;
                plan->uses.push_back({ bufferUse(buffer), toImage, !toImage });
                plan->uses.push_back({ MemberOf(ref), !toImage || part, toImage });
                if (toImage) plan->record = [buffer, ref, how](kor::CommandBuffer& cb, const PassContext&) { cb.CopyBufferToImage(buffer, ref, how); };
                else plan->record = [buffer, ref, how](kor::CommandBuffer& cb, const PassContext&) {
                    // An image made anew at another size, a frame before what reads it back is made to fit: left out
                    // for that frame, rather than copied past the end of the buffer.
                    if (const std::size_t texel = TexelLayoutOf(FormatName(ref->PixelFormat())).Bytes(); texel > 0) {
                        const auto extent = ref->Extent();
                        const std::size_t wide = how.imageExtent.x > 0 ? static_cast<std::size_t>(how.imageExtent.x) : std::max<std::size_t>(extent.x >> how.imageMipLevel, 1);
                        const std::size_t tall = how.imageExtent.y > 0 ? static_cast<std::size_t>(how.imageExtent.y) : std::max<std::size_t>(extent.y >> how.imageMipLevel, 1);
                        if (how.bufferOffset + wide * tall * texel * std::max<std::size_t>(how.imageLayerCount, 1) > buffer->size()) return;
                    }
                    cb.CopyImageToBuffer(ref, buffer, how);
                };
            } else if (node.type == "Readback") {
                // A few bytes of a buffer, or one texel of an image, copied into the next of its buffers each frame.
                auto state = std::make_shared<ReadState>();
                kor::ResourceRef<const kor::Buffer> from;
                kor::ResourceRef<const kor::Image> picture;
                kor::Copy one;
                kor::u64 offset = 0;
                const auto buffer = inputs.find("buffer");
                const auto pictureIn = inputs.find("image");
                if (buffer != inputs.end() && buffer->second.resource) {
                    from = bufferIn("buffer", true, kor::Buffer::Usage::eTransferSrc, "eTransferSrc", "to be read back");
                    static const std::map<std::string, std::pair<char, std::size_t>, std::less<>> types {
                        { "eFloat", { 'f', 1 } }, { "eInt", { 'i', 1 } }, { "eUInt", { 'u', 1 } }, { "eVec2", { 'f', 2 } }, { "eVec3", { 'f', 3 } }, { "eVec4", { 'f', 4 } },
                        { "eIVec2", { 'i', 2 } }, { "eIVec4", { 'i', 4 } }, { "eUVec2", { 'u', 2 } }, { "eUVec4", { 'u', 4 } } };
                    const Value& typed = value("type");
                    const auto found = types.find(typed.String() ? *typed.String() : std::string("eFloat"));
                    if (found == types.end()) return Fail("'type' is eFloat, eInt, eUInt, eVec2, eVec3, eVec4, eIVec2, eIVec4, eUVec2 or eUVec4");
                    state->kind = found->second.first;
                    state->count = found->second.second;
                    state->bytes = 4 * state->count;
                    offset = static_cast<kor::u64>(num("offset", 0, 0, 1e15));
                    if (!wrong.empty()) return Fail(wrong);
                    if (offset + state->bytes > from->size()) return Fail(std::format("{} bytes from {} is past the end of the buffer, of {}", state->bytes, offset, from->size()));
                    plan->uses.push_back({ bufferUse(from), true, false });
                } else if (pictureIn != inputs.end() && pictureIn->second.resource) {
                    const auto given = image("image");
                    if (!given) return Fail(given.error());
                    if ((*given)->screen) return Fail("the screen is not read back: draw it into an image, and read that");
                    if (lacks(**given, kor::Image::Usage::eTransferSrc)) return Fail("the image needs eTransferSrc among its usage, to be read back");
                    picture = ImageRefOf(given->get());
                    const TexelLayout texel = TexelLayoutOf(FormatName(picture->PixelFormat()));
                    if (texel.channels == 0) return Fail(std::format("a texel of {} is not read back: a format of channels of one size is", FormatName(picture->PixelFormat())));
                    state->texel = texel;
                    state->bytes = texel.Bytes();
                    const Value& at = value("texel");
                    std::vector<double> where;
                    if (!at.IsNull()) {
                        auto read = NumbersOf(at, "texel");
                        if (!read || read->size() < 2) return Fail("'texel' is x and y, and z or a layer");
                        where = std::move(*read);
                    }
                    one.imageMipLevel = whole("mip");
                    if (!wrong.empty()) return Fail(wrong);
                    if (one.imageMipLevel >= picture->MipLevels()) return Fail(std::format("'mip' is {}, and the image has {} mip levels", one.imageMipLevel, picture->MipLevels()));
                    const auto extent = picture->Extent();
                    const int wide = std::max(static_cast<int>(extent.x >> one.imageMipLevel), 1), tall = std::max(static_cast<int>(extent.y >> one.imageMipLevel), 1);
                    const int x = where.empty() ? 0 : static_cast<int>(where[0]), y = where.empty() ? 0 : static_cast<int>(where[1]);
                    if (x < 0 || y < 0 || x >= wide || y >= tall) return Fail(std::format("texel ({}, {}) is outside the image, of {} by {}", x, y, wide, tall));
                    const int z = where.size() > 2 ? static_cast<int>(where[2]) : 0;
                    const bool layered = picture->ArrayLayers() > 1;
                    one.imageOffset = kor::IVec3(x, y, layered ? 0 : z);
                    one.imageBaseArrayLayer = layered ? static_cast<kor::u32>(std::max(z, 0)) : 0u;
                    one.imageExtent = { 1, 1, 1 };
                    plan->uses.push_back({ MemberOf(picture), true, false });
                } else {
                    return Fail("'buffer' or 'image' needs wiring: what is read back");
                }
                for (std::size_t i = 0; i < ReadState::Ring; ++i) {
                    auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(state->bytes, 4))).SetUsage(kor::Buffer::Usage::eTransferDst)
                        .SetType(kor::Buffer::Type::eReadback).Build();
                    if (!made.Valid()) return Fail("a buffer it is read back into could not be made");
                    state->buffers[i] = kor::ResourceRef<const kor::Buffer>(made);
                    state->owners[i] = std::make_shared<kor::Resource<kor::Buffer>>(std::move(made));
                }
                out.readback = state;
                out.outputs["ready"].value = false;
                plan->record = [state, from, picture, one, offset](kor::CommandBuffer& cb, const PassContext&) {
                    const auto into = state->buffers[state->copies % ReadState::Ring];
                    if (from.Get()) cb.CopyBuffer(from, into, state->bytes, offset, 0);
                    else cb.CopyImageToBuffer(picture, into, one);
                    ++state->copies;
                };
            } else if (node.type == "Pick") {
                // Which entity is at a pixel: the image of ids a pipeline draws `entityId` into, read back when asked.
                const auto picture = image("ids");
                if (!picture) return Fail(picture.error());
                if ((*picture)->screen) return Fail("'ids' is an image of which entity is where, not the screen");
                if (lacks(**picture, kor::Image::Usage::eTransferSrc)) return Fail("the image needs eTransferSrc among its usage, to be read back");
                const auto ref = ImageRefOf(picture->get());
                // The depth round the pixel too, where it is wired: three rows of three, read into a buffer of their own.
                kor::ResourceRef<const kor::Image> depth;
                if (const auto it = inputs.find("depth"); it != inputs.end() && it->second.resource) {
                    depth = ImageRefOf(it->second.resource.get());
                    if (!depth.Get() || MadeWithout(*it->second.resource, kor::Image::Usage::eTransferSrc)) return Fail("'depth' is a depth image with eTransferSrc among its usage, to be read back");
                    if (depth->Extent().x != ref->Extent().x || depth->Extent().y != ref->Extent().y) return Fail("'depth' is as big as 'ids': the depth they were drawn with");
                }
                auto readback = kor::Buffer::RawBuilder{}.SetRawSize(4).SetUsage(kor::Buffer::Usage::eTransferDst).SetType(kor::Buffer::Type::eReadback).Build();
                // (Room past the nine depths: the copy reckons a depth texel's size on the safe side.)
                auto depths = depth.Get() ? kor::Buffer::RawBuilder{}.SetRawSize(9 * 8).SetUsage(kor::Buffer::Usage::eTransferDst).SetType(kor::Buffer::Type::eReadback).Build()
                                          : kor::Resource<kor::Buffer>();
                if (!readback.Valid() || (depth.Get() && !depths.Valid())) return Fail("the buffer a pixel is read back into could not be made");
                const kor::ResourceRef<const kor::Buffer> into(readback), depthsInto(depths);
                const auto owner = std::make_shared<std::pair<kor::Resource<kor::Buffer>, kor::Resource<kor::Buffer>>>(std::move(readback), std::move(depths));
                plan->uses.push_back({ MemberOf(ref), true, false });
                if (depth.Get()) plan->uses.push_back({ MemberOf(depth), true, false });
                plan->prepare = [this, state = this->pick, owner, ref, deep = depth.Get() != nullptr] {
                    // More frames than are in flight: by then the frame that copied it has finished.
                    if (state->waiting > 0 && --state->waiting == 0) {
                        const auto id = owner->first->Read<kor::u32>(1);
                        if (deep) {
                            const auto read = owner->second->Read<float>(9);
                            state->hit.deep = read.size() == 9;
                            for (std::size_t i = 0; i < read.size() && i < 9; ++i) state->hit.depth[i] = read[i];
                        }
                        if (auto then = std::exchange(state->then, {}); then && !id.empty()) then(id[0], state->hit);
                    }
                    state->copyAt.reset();
                    if (const auto asked = std::exchange(state->asked, std::nullopt)) {
                        const auto extent = ref->Extent();
                        state->copyAt = kor::IVec3(std::clamp(static_cast<int>(asked->first * static_cast<float>(extent.x)), 0, static_cast<int>(extent.x) - 1),
                                                   std::clamp(static_cast<int>(asked->second * static_cast<float>(extent.y)), 0, static_cast<int>(extent.y) - 1), 0);
                        state->hit = {};
                        state->hit.pixel = { state->copyAt->x, state->copyAt->y };
                        state->hit.extent = { extent.x, extent.y };
                        state->hit.viewProjection = viewProjection;
                        state->waiting = 4;
                    }
                };
                plan->record = [state = this->pick, into, ref, depthsInto, depth](kor::CommandBuffer& cb, const PassContext&) {
                    if (!state->copyAt) return;
                    kor::Copy one;
                    one.imageOffset = *state->copyAt;
                    one.imageExtent = { 1, 1, 1 };
                    cb.CopyImageToBuffer(ref, into, one);
                    if (!depth.Get()) return;
                    // Each of the nine on its own, the picture's edge standing in for what is past it.
                    const auto extent = depth->Extent();
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column) {
                            kor::Copy texel;
                            texel.imageOffset = { std::clamp(state->copyAt->x + column - 1, 0, static_cast<int>(extent.x) - 1),
                                                  std::clamp(state->copyAt->y + row - 1, 0, static_cast<int>(extent.y) - 1), 0 };
                            texel.imageExtent = { 1, 1, 1 };
                            texel.bufferOffset = static_cast<kor::u64>(row * 3 + column) * 4;
                            cb.CopyImageToBuffer(depth, depthsInto, texel);
                        }
                };
            } else if (node.type == "DebugDraw") {
                // The scene's debug shapes — a gizmo, a light's reach — over a picture, seen as the scene is.
                const auto picture = image("image");
                if (!picture) return Fail(picture.error());
                if ((*picture)->screen) return Fail("the debug shapes are drawn into an image, which is then shown: not straight to the screen");
                if (lacks(**picture, kor::Image::Usage::eColorAttachment)) return Fail("the image needs eColorAttachment among its usage, to be drawn into");
                const auto color = ImageRefOf(picture->get());
                auto builder = kor::Framebuffer::Builder().AddColor({ .view = color });
                kor::ResourceRef<const kor::Image> depth;
                if (const auto it = inputs.find("depth"); it != inputs.end() && it->second.resource) {
                    depth = ImageRefOf(it->second.resource.get());
                    if (!depth.Get() || MadeWithout(*it->second.resource, kor::Image::Usage::eDepthStencilAttachment)) return Fail("'depth' is a depth image, with eDepthStencilAttachment among its usage");
                    builder.SetDepth({ .view = depth });
                }
                auto framebuffer = builder.Build();
                if (!framebuffer.Valid()) return Fail(framebuffer.Failure() ? framebuffer.Failure()->message : std::string("its framebuffer could not be made"));
                const kor::ResourceRef<const kor::Framebuffer> target(framebuffer);
                const auto owner = std::make_shared<kor::Resource<kor::Framebuffer>>(std::move(framebuffer));
                plan->uses.push_back({ MemberOf(color), true, true });
                if (depth.Get()) plan->uses.push_back({ MemberOf(depth), true, true });
                auto matrix = std::make_shared<kor::Mat4>(1.f);
                plan->prepare = [this, owner, target, matrix] {
                    *matrix = viewProjection;
                    if (debug) debug->Prepare(target, *matrix);
                };
                plan->record = [this, target, matrix](kor::CommandBuffer& cb, const PassContext&) {
                    if (debug) debug->Record(cb, *matrix, target);
                };
            } else if (node.type == "Dispatch" || node.type == "TraceRays") {
                const bool rays = node.type == "TraceRays";
                const auto info = pipelineOf(rays ? "RayTracingPipeline" : "ComputePipeline");
                if (!info) return Fail(info.error());
                // A group works on what the shader says it does, unless the node says otherwise.
                kor::u32 groups[3] { 1, 1, 1 }, groupSize[3] { 1, 1, 1 };
                for (int axis = 0; axis < 3; ++axis) groupSize[axis] = std::max<kor::u32>((*info)->numThreads[axis], 1);
                if (rays) std::ranges::fill(groupSize, 1u);   // a ray a pixel
                if (const auto read = counts(rays ? "size" : "groups", groups, rays ? "a number of rays" : "a number of groups"); !read) return Fail(read.error());
                if (const auto read = counts("groupSize", groupSize, "how many a group works on"); !rays && !read) return Fail(read.error());
                // How many groups, read from a buffer a pass before this one wrote.
                const auto indirectBuffer = rays ? kor::ResourceRef<const kor::Buffer>() : bufferIn("indirect", false, kor::Buffer::Usage::eIndirect, "eIndirect", "to be dispatched from");
                const auto indirectOffset = rays ? 0 : static_cast<kor::u64>(num("indirectOffset", 0, 0, 1e15));
                if (!wrong.empty()) return Fail(wrong);
                if (indirectBuffer.Get() && has("cover")) return Fail("'indirect' and 'cover' both say how many groups: one or the other");

                auto made = drawsOf(*info, false);
                if (!made) return Fail(made.error());
                const std::shared_ptr<EntityRun> run = std::move(*made);
                // A TraceRays' buffer of each instance's, as a Draw's: a pass may read its material too.
                if (!run->instances.empty()) {
                    out.outputs["instances"].resource = run->instances.front()->held;
                    out.outputs["instanceStride"].value = Value(static_cast<double>(run->instances.front()->instancing->stride));     // the bytes of an element of it
                }
                // A Dispatch of a Filter's entities: its groups once for each of them, one after another along z, in one dispatch.
                if (!rays)
                    if (const auto entities = node.inputs.find("entities"); entities != node.inputs.end() && entities->second.link) {
                        const Node* filter = Origin(entities->second.link->node);
                        const auto listed = filter ? built.find(filter->id) : built.end();
                        if (listed == built.end() || !listed->second.listed) return Fail("'entities' is a Filter's entities, or an Entities node's: what it runs once for each of");
                        if (!run->each.empty())
                            return Fail(std::format("it runs once for each of '{}', and is wired a value of each element of '{}' too: what each is to its shader is its "
                                                    "[entity(\"PER_INSTANCE\")] buffer's", filter->id, run->each));
                        if (indirectBuffer.Get()) return Fail("'indirect' says how many groups, and 'entities' that they are run once for each: one or the other");
                        run->batchOf = filter->id;
                    }
                // An image to cover: as many groups as it takes, each element's own image or one the pipeline wires.
                if (const Value& cover = value("cover"); !cover.IsNull()) {
                    const std::string* name = cover.String();
                    if (!name) return Fail("'cover' is the name of one of the pipeline's image parameters");
                    bool found = false;
                    for (std::size_t i = 0; i < (*info)->slots.size() && !found; ++i) {
                        const Slot& slot = (*info)->slots[i];
                        if (slot.field == *name && !slot.sampler && IsImage(slot.type)) { run->cover = static_cast<int>(i); found = true; }
                    }
                    if (const auto wired = (*info)->wired.find(*name); !found && wired != (*info)->wired.end()) {
                        if (const auto covered = ImageRefOf(wired->second.get()); covered.Get()) {
                            // A view of one of an image's smaller levels is as big as that level is.
                            const kor::u32 level = wired->second->kind == "ImageView" ? kor::capi::ImageViewOf(wired->second->handle)->BaseMipLevel() : 0;
                            const auto extent = covered->Extent();
                            for (int axis = 0; axis < 3; ++axis)
                                groups[axis] = std::max<kor::u32>((std::max<kor::u32>(static_cast<kor::u32>(extent[axis]) >> level, 1) + groupSize[axis] - 1) / groupSize[axis], 1);
                            found = true;
                        }
                    }
                    if (!found) return Fail(std::format("'cover' names one of the pipeline's image parameters, and '{}' is not one", *name));
                }
                std::ranges::copy(groups, run->groups);
                std::ranges::copy(groupSize, run->groupSize);
                plan->uses = (*info)->uses;
                if (indirectBuffer.Get()) plan->uses.push_back({ bufferUse(indirectBuffer), true, false });
                plan->entities = run;
                KoralResource* const handle = inputs.at("pipeline").resource->handle;
                const auto compute = rays ? kor::ResourceRef<const kor::ComputePipeline>() : kor::ResourceRef<const kor::ComputePipeline>(kor::capi::ComputePipelineOf(handle));
                const auto tracing = rays ? kor::ResourceRef<const kor::RayTracingPipeline>(kor::capi::RayTracingPipelineOf(handle)) : kor::ResourceRef<const kor::RayTracingPipeline>();
                plan->record = [compute, tracing, rays, indirectBuffer, indirectOffset, info = *info, run](kor::CommandBuffer& cb, const PassContext&) {
                    if (run->items.empty()) return;
                    if (rays) cb.BindRayTracingPipeline(tracing);
                    else cb.BindComputePipeline(compute);
                    if (info->hasSet) cb.BindDescriptorSet(0, info->set);
                    for (const Item& item : run->items) {
                        if (item.set.Get()) cb.BindDescriptorSet(1, item.set);
                        if (info->pushSize) cb.PushConstantBytes({ run->pushed.data() + item.push, info->pushSize }, info->pushFirst);
                        if (rays) cb.TraceRays(item.groups[0], item.groups[1], item.groups[2]);
                        else if (indirectBuffer.Get()) cb.DispatchIndirect(indirectBuffer, indirectOffset);
                        else cb.Dispatch(item.groups[0], item.groups[1], item.groups[2]);
                    }
                };
            } else {   // Draw
                const auto info = pipelineOf("GraphicsPipeline");
                if (!info) return Fail(info.error());
                Held target = (*info)->framebuffer;
                if (const auto it = inputs.find("framebuffer"); it != inputs.end() && it->second.resource) target = it->second.resource;
                if (!target || !target->handle || target->kind != "Framebuffer")
                    return Fail("it draws into a framebuffer: the pipeline's, or one wired to 'framebuffer'. To show it, Blit one of the framebuffer's images to the screen");
                const auto framebuffer = kor::capi::FramebufferOf(target->handle);

                // Which draw.
                enum : int { eAuto, ePlain, eIndexed, eTasks, eIndirect, eIndexedIndirect, eIndirectCount, eIndexedIndirectCount, eTasksIndirect };
                int command = std::max(pick("command", { "eAuto", "eDraw", "eDrawIndexed", "eDrawMeshTasks", "eDrawIndirect", "eDrawIndexedIndirect", "eDrawIndirectCount",
                                                         "eDrawIndexedIndirectCount", "eDrawMeshTasksIndirect" }), 0);
                if (!wrong.empty()) return Fail(wrong);
                if (command == eAuto && (*info)->meshShaders) command = eTasks;
                const bool tasks = command == eTasks || command == eTasksIndirect;
                if (tasks && !(*info)->meshShaders) return Fail("it draws mesh tasks, and the pipeline has no mesh shader: its 'stages' is eVertex");
                if (!tasks && (*info)->meshShaders) return Fail("the pipeline is drawn with mesh shaders: 'command' is eDrawMeshTasks or eDrawMeshTasksIndirect, or left as eAuto");
                const bool indirect = command >= eIndirect;
                const bool counted = command == eIndirectCount || command == eIndexedIndirectCount;
                const bool mustIndex = command == eIndexed || command == eIndexedIndirect || command == eIndexedIndirectCount;

                // What is drawn, and with what: once, or once for each element of the ForEach it is wired from.
                auto made = drawsOf(*info, true);
                if (!made) return Fail(made.error());
                const std::shared_ptr<EntityRun> run = std::move(*made);
                // Its buffer of each instance's, when it made or shares one: to be looked at like any buffer.
                if (!run->instances.empty()) {
                    out.outputs["instances"].resource = run->instances.front()->held;
                    out.outputs["instanceStride"].value = Value(static_cast<double>(run->instances.front()->instancing->stride));     // the bytes of an element of it
                    for (const auto& field : run->instances.front()->instancing->fields)     // where in an element its MATRIX is
                        if (field.from == Instancing::Field::From::eMatrix) { out.outputs["instanceMatrixOffset"].value = Value(static_cast<double>(field.offset)); break; }
                    if (run->drawn) out.outputs["drawnInstances"].resource = run->drawn;     // compacted: what its shader reads, which a cull writes
                }
                run->tasks = tasks;
                if (tasks && run->mesh.kind != Source::Kind::eNone) return Fail("mesh shaders make their own vertices: there is no 'mesh' to give them");
                if (!tasks && run->mesh.kind == Source::Kind::eNone && (mustIndex || (*info)->vertexInputs))
                    return Fail("its pipeline's vertex shader reads vertices: wire it a 'mesh' — a ForEach of Mesh's `field.mesh`, a mesh for each element, or a Primitive's");
                if (run->mesh.kind == Source::Kind::eGiven) return Fail("'mesh' takes a mesh, wired: not a value");
                const std::optional<kor::u64> vertexCount = has("vertexCount") ? std::optional<kor::u64>(whole("vertexCount", 3)) : std::nullopt;
                const kor::u32 firstVertex = whole("firstVertex");
                const auto vertexOffset = static_cast<kor::i32>(std::llround(num("vertexOffset", 0, -2147483648.0, 2147483647.0)));
                const kor::u32 instances = static_cast<kor::u32>(std::llround(num("instances", 1, 1)));
                const kor::u32 firstInstance = whole("firstInstance");
                kor::u32 taskGroups[3] { 1, 1, 1 };
                if (const auto read = counts("tasks", taskGroups, "a number of groups"); !read) return Fail(read.error());

                const auto indirectBuffer = bufferIn("indirect", indirect, kor::Buffer::Usage::eIndirect, "eIndirect", "to be drawn from");
                const auto countBuffer = bufferIn("count", counted, kor::Buffer::Usage::eIndirect, "eIndirect", "to be counted from");
                const auto indirectOffset = static_cast<kor::u64>(num("indirectOffset", 0, 0, 1e15));
                const auto countOffset = static_cast<kor::u64>(num("countOffset", 0, 0, 1e15));
                // None: an IndirectDraws of nothing yet — the framebuffer is cleared, and nothing drawn.
                const kor::u32 drawCount = static_cast<kor::u32>(std::llround(num("drawCount", 1, 0)));
                const kor::u32 stride = whole("stride");
                if (!wrong.empty()) return Fail(wrong);
                if (!indirect && indirectBuffer.Get()) return Fail("'indirect' is read by an Indirect command: choose one for 'command'");
                if (!counted && countBuffer.Get()) return Fail("'count' is read by an IndirectCount command: choose one for 'command'");

                // What the framebuffer starts as, and what is kept of it.
                kor::RenderInfo render(framebuffer);
                const std::optional<bool> loadAll = flag("load");
                const std::vector<std::string> loads { "eClear", "eLoad", "eDontCare" }, stores { "eStore", "eDontCare" };
                const auto loadOf = [&](const char* name) {
                    const int chosen = pick(name, loads);
                    if (chosen < 0) return loadAll.value_or(false) ? kor::LoadOperation::eLoad : kor::LoadOperation::eClear;
                    return chosen == 0 ? kor::LoadOperation::eClear : chosen == 1 ? kor::LoadOperation::eLoad : kor::LoadOperation::eDontCare;
                };
                const auto storeOf = [&](const char* name) { return pick(name, stores) == 1 ? kor::StoreOperation::eDontCare : kor::StoreOperation::eStore; };
                render.SetColorLoadOperation(loadOf("colorLoad")).SetDepthLoadOperation(loadOf("depthLoad")).SetStencilLoadOperation(loadOf("stencilLoad"));
                render.SetColorStoreOperation(storeOf("colorStore")).SetDepthStoreOperation(storeOf("depthStore")).SetStencilStoreOperation(storeOf("stencilStore"));
                if (const auto color = several("clearColor", 4, "red, green, blue, alpha"))
                    for (kor::u32 i = 0; i < framebuffer->ColorAttachmentCount(); ++i) render.SetClearColor(i, kor::Vec4((*color)[0], (*color)[1], (*color)[2], (*color)[3]));
                if (has("clearDepth")) render.SetClearDepth(static_cast<float>(num("clearDepth", 1, 0, 1)));
                if (has("clearStencil")) render.SetClearStencil(static_cast<kor::i32>(whole("clearStencil")));

                // What the pipeline leaves to the draw.
                struct State {
                    std::optional<std::vector<float>> viewport, scissor, depthBias, blendConstants;
                    int cullMode = -1, frontFace = -1, depthCompare = -1;
                    std::optional<bool> depthTest, depthWrite, stencilTest, rasterizerDiscard, primitiveRestart;
                    std::optional<float> lineWidth;
                    std::optional<kor::u32> stencilReference, stencilCompareMask, stencilWriteMask;
                } state;
                state.viewport = several("viewport", 4, "x, y, width, height");
                state.scissor = several("scissor", 4, "x, y, width, height");
                state.depthBias = several("depthBias", 3, "a constant, a clamp, and a factor of the slope");
                state.blendConstants = several("blendConstants", 4, "red, green, blue, alpha");
                state.cullMode = pick("cullMode", { "eNone", "eFront", "eBack", "eFrontAndBack" });
                state.frontFace = pick("frontFace", { "eCounterClockwise", "eClockwise" });
                state.depthCompare = pick("depthCompare", { "eNever", "eLess", "eEqual", "eLessOrEqual", "eGreater", "eNotEqual", "eGreaterOrEqual", "eAlways" });
                state.depthTest = flag("depthTest");
                state.depthWrite = flag("depthWrite");
                state.stencilTest = flag("stencilTest");
                state.rasterizerDiscard = flag("rasterizerDiscard");
                state.primitiveRestart = flag("primitiveRestart");
                if (has("lineWidth")) state.lineWidth = static_cast<float>(num("lineWidth", 1, 0, 64));
                if (has("stencilReference")) state.stencilReference = whole("stencilReference");
                if (has("stencilCompareMask")) state.stencilCompareMask = whole("stencilCompareMask");
                if (has("stencilWriteMask")) state.stencilWriteMask = whole("stencilWriteMask");
                for (const auto* box : { &state.viewport, &state.scissor })
                    if (*box && (std::ranges::any_of(**box, [](const float v) { return v < 0.f; }) || (**box)[2] < 1.f || (**box)[3] < 1.f))
                        mistaken("'viewport' and 'scissor' are x, y, width, height, in pixels: none below zero, and a width and a height of 1 or more");
                if (!wrong.empty()) return Fail(wrong);

                if (const auto cull = node.inputs.find("cull"); cull != node.inputs.end() && cull->second.link) {
                    run->cullNode = cull->second.link->node;
                    run->cullOutput = cull->second.link->output;
                    if (const auto index = inputs.find("cullIndex"); index != inputs.end() && index->second.value.Number()) run->cullIndex = static_cast<int>(*index->second.value.Number());
                }
                if (const auto idle = node.inputs.find("idle"); idle != node.inputs.end() && idle->second.link) {
                    run->idleNode = idle->second.link->node;
                    run->idleOutput = idle->second.link->output;
                    if (const auto index = inputs.find("cullIndex"); index != inputs.end() && index->second.value.Number()) run->cullIndex = static_cast<int>(*index->second.value.Number());
                }
                run->command = mustIndex ? DrawWith::eDrawIndexed : command == eAuto ? DrawWith::eAuto : DrawWith::eDraw;
                run->whenChanged = flag("whenChanged").value_or(false);
                const auto drawsInto = [&](const kor::ResourceRef<const kor::ImageView>& view) {
                    if (!view.Get()) return;
                    run->targets.push_back(view.Get());
                    if (view->SourceImage().Get() && view->SourceImage()->IsPerFrame())
                        run->copies = std::max(run->copies, kor::Context::HasScheduler() ? kor::Context::Scheduler().ImageCount() : 1u);
                };
                for (kor::u32 i = 0; i < framebuffer->ColorAttachmentCount(); ++i) drawsInto(framebuffer->ColorAttachment(i));
                if (framebuffer->HasDepthAttachment()) drawsInto(framebuffer->DepthAttachment());

                const bool colorLoaded = render.ColorLoadOperation() == kor::LoadOperation::eLoad;
                for (kor::u32 i = 0; i < framebuffer->ColorAttachmentCount(); ++i)
                    if (const auto view = framebuffer->ColorAttachment(i); view.Get()) plan->uses.push_back({ MemberOf(view->SourceImage()), colorLoaded, true });
                if (framebuffer->HasDepthAttachment())
                    if (const auto view = framebuffer->DepthAttachment(); view.Get()) plan->uses.push_back({ MemberOf(view->SourceImage()), true, true });
                plan->uses.insert(plan->uses.end(), (*info)->uses.begin(), (*info)->uses.end());
                if (indirectBuffer.Get()) plan->uses.push_back({ bufferUse(indirectBuffer), true, false });
                if (countBuffer.Get()) plan->uses.push_back({ bufferUse(countBuffer), true, false });
                plan->entities = run;
                plan->record = [=, info = *info, t0 = taskGroups[0], t1 = taskGroups[1], t2 = taskGroups[2]](kor::CommandBuffer& cb, const PassContext&) {
                    const auto extent = framebuffer->Extent();
                    if (run->idle) return;   // left as it was drawn last: not cleared, and nothing drawn
                    cb.BeginRendering(render);   // with nothing to draw, it still clears
                    // Bound with what goes with it: a pipeline made for another mesh's layout is bound as the node's is.
                    const auto ready = [&](const kor::ResourceRef<const kor::GraphicsPipeline>& bind) {
                        cb.BindGraphicsPipeline(bind);
                        const auto box = [&](const std::optional<std::vector<float>>& given, const int at, const kor::u32 otherwise) {
                            return given ? static_cast<kor::u32>((*given)[static_cast<std::size_t>(at)]) : otherwise;
                        };
                        cb.SetViewport(box(state.viewport, 0, 0), box(state.viewport, 1, 0), box(state.viewport, 2, extent.x), box(state.viewport, 3, extent.y));
                        cb.SetScissor(box(state.scissor, 0, 0), box(state.scissor, 1, 0), box(state.scissor, 2, extent.x), box(state.scissor, 3, extent.y));
                        if (state.cullMode >= 0) {
                            kor::Flags<kor::CullMode> cull {};
                            if (state.cullMode == 1 || state.cullMode == 3) cull |= kor::CullMode::eFront;
                            if (state.cullMode == 2 || state.cullMode == 3) cull |= kor::CullMode::eBack;
                            cb.SetCullMode(cull);
                        }
                        if (state.frontFace >= 0) cb.SetFrontFace(state.frontFace == 0 ? kor::FrontFace::eCounterClockwise : kor::FrontFace::eClockwise);
                        if (state.depthTest) cb.SetDepthTestEnable(*state.depthTest);
                        if (state.depthWrite) cb.SetDepthWriteEnable(*state.depthWrite);
                        if (state.depthCompare >= 0) cb.SetDepthCompareOp(static_cast<kor::CompareOp>(state.depthCompare));
                        if (state.depthBias) cb.SetDepthBiasEnable(true).SetDepthBias((*state.depthBias)[0], (*state.depthBias)[1], (*state.depthBias)[2]);
                        if (state.lineWidth) cb.SetLineWidth(*state.lineWidth);
                        if (state.blendConstants) cb.SetBlendConstants({ (*state.blendConstants)[0], (*state.blendConstants)[1], (*state.blendConstants)[2], (*state.blendConstants)[3] });
                        if (state.stencilTest) cb.SetStencilTestEnable(*state.stencilTest);
                        if (state.stencilReference) cb.SetStencilReference(kor::StencilFace::eFrontAndBack, *state.stencilReference);
                        if (state.stencilCompareMask) cb.SetStencilCompareMask(kor::StencilFace::eFrontAndBack, *state.stencilCompareMask);
                        if (state.stencilWriteMask) cb.SetStencilWriteMask(kor::StencilFace::eFrontAndBack, *state.stencilWriteMask);
                        if (state.rasterizerDiscard) cb.SetRasterizerDiscardEnable(*state.rasterizerDiscard);
                        if (state.primitiveRestart) cb.SetPrimitiveRestartEnable(*state.primitiveRestart);
                        if (info->hasSet) cb.BindDescriptorSet(0, info->set);
                    };
                    const kor::GraphicsPipeline* lastPipeline = nullptr;
                    const kor::Mesh* lastMesh = nullptr;
                    const kor::DescriptorSet* lastSet = nullptr;
                    const auto draw = [&](const Item& item) {
                        if (item.pipeline.Get() != lastPipeline) {
                            ready(item.pipeline);
                            lastPipeline = item.pipeline.Get();
                            lastMesh = nullptr;
                            lastSet = nullptr;
                        }
                        if (item.set.Get() && item.set.Get() != lastSet) { cb.BindDescriptorSet(1, item.set); lastSet = item.set.Get(); }
                        if (info->pushSize) cb.PushConstantBytes({ run->pushed.data() + item.push, info->pushSize }, info->pushFirst);
                        const kor::ResourceRef<const kor::Mesh>& bound = item.mesh;
                        if (bound.Get() && bound.Get() != lastMesh) { cb.BindMesh(bound); lastMesh = bound.Get(); }
                        if (indirect && drawCount == 0) return;
                        switch (command) {
                        case eTasks: cb.DrawMeshTasks(t0, t1, t2); break;
                        case eIndirect: cb.DrawIndirect(indirectBuffer, indirectOffset, drawCount, stride); break;
                        case eIndexedIndirect: cb.DrawIndexedIndirect(indirectBuffer, indirectOffset, drawCount, stride); break;
                        case eIndirectCount: cb.DrawIndirectCount(indirectBuffer, indirectOffset, countBuffer, countOffset, drawCount, stride); break;
                        case eIndexedIndirectCount: cb.DrawIndexedIndirectCount(indirectBuffer, indirectOffset, countBuffer, countOffset, drawCount, stride); break;
                        case eTasksIndirect: cb.DrawMeshTasksIndirect(indirectBuffer, indirectOffset, drawCount, stride); break;
                        default:
                            if (bound.Get() && item.part) cb.DrawIndexed(item.indexCount, instances, item.firstIndex, item.vertexOffset, firstInstance);
                            else if (item.indexed) cb.DrawIndexed(vertexCount.value_or(kor::WholeSize), instances, firstVertex, vertexOffset, firstInstance);
                            // With no mesh there is nothing to say how many: a triangle, unless the node says.
                            else cb.Draw(vertexCount.value_or(bound.Get() ? kor::WholeSize : 3), instances, firstVertex, firstInstance);
                        }
                    };
                    for (const Item& item : run->items) draw(item);
                    cb.EndRendering();
                };
            }
            // Its `idle`: while what it is given is not zero, or true, it records nothing — a value worked out each frame,
            // read as the frame is readied. (A Draw's is its own: a number for each turn.)
            if (node.type != "Draw")
                if (const auto idle = node.inputs.find("idle"); idle != node.inputs.end() && plan->record) {
                    const auto said = [](const Value& value) { return value.Bool() ? *value.Bool() : value.Number() && *value.Number() != 0.0; };
                    // Resting from the first frame as it is built to, before a frame has been readied to say so.
                    const auto now = inputs.find("idle");
                    auto resting = std::make_shared<std::atomic<bool>>(now != inputs.end() && said(now->second.value));
                    if (idle->second.link) {
                        plan->prepare = [this, before = std::move(plan->prepare), resting, said, from = *idle->second.link] {
                            if (before) before();
                            const auto source = built.find(from.node);
                            const auto output = source == built.end() ? nullptr : &source->second.outputs;
                            const auto it = output ? output->find(from.output) : decltype(output->find(from.output)) {};
                            resting->store(output && it != output->end() && said(it->second.value));
                        };
                    } else {
                        resting->store(said(idle->second.value));
                    }
                    plan->record = [record = std::move(plan->record), resting](kor::CommandBuffer& cb, const PassContext& context) {
                        if (!resting->load()) record(cb, context);
                    };
                    plan->idle = resting;
                }
            // On a page of the fixed step, what it writes is to be there for the next step: a copy for each frame in
            // flight is not — each step would go on from another copy.
            if (current && current->IsFixed(node.pass))
                for (const auto& use : plan->uses)
                    if (use.write && ((use.what.image.Get() && use.what.image->IsPerFrame()) || (use.what.buffer.Get() && use.what.buffer->IsPerFrame()))) {
                        out.status.note = "it writes a per-frame resource on the fixed step: a copy for each frame in flight, so a step does not go on "
                                          "from what the step before wrote. A state kept from step to step is one copy (isPerFrame false).";
                        break;
                    }
            out.pass = std::move(plan);
            return {};
        }

        /** A matrix of four by four from its sixteen numbers, column after column. */
        static Expected<kor::Mat4> MatrixOf(const Value& value, const char* name)
        {
            const auto numbers = NumbersOf(value, name);
            if (!numbers || numbers->size() != 16) return Fail(std::format("'{}' is a matrix: sixteen numbers, column after column", name));
            kor::Mat4 m(1.f);
            for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = static_cast<float>((*numbers)[static_cast<std::size_t>(i)]);
            return m;
        }

        /** What the editor's gizmos and the debug shapes go by: the main View's matrix and the main lens's. */
        void SeenThrough() { viewProjection = mainProjection * currentView.view; }

        /**
         * A view, as glm's lookAt makes one: from a position, towards a target, up as up. The main one is the scene's:
         * in an editor, the editor's own in its place.
         */
        Expected<void> BuildView(const Inputs& inputs, Built& out)
        {
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            const auto vector = [&](const char* name, const kor::Vec3 otherwise) -> Expected<kor::Vec3> {
                const Value* v = given(name);
                if (!v) return otherwise;
                const auto numbers = NumbersOf(*v, name);
                if (!numbers || numbers->size() < 3) return Fail(std::format("'{}' is a vector of three", name));
                return kor::Vec3(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]));
            };
            // What a View was before: a camera's or a light's, by its struct. It is numbers now, wired.
            for (const char* gone : { "of", "kind", "face", "fieldOfView", "size", "extent" })
                if (inputs.contains(gone))
                    return Fail(std::format("'{}' is no View's now: a View is a position, a target and up — a ForEach's `worldPosition`, that and its "
                                            "`forward` added, and its `up` — and a Perspective wired from it is the lens", gone));
            const auto position = vector("position", kor::Vec3(0.f));
            if (!position) return Fail(position.error());
            const auto target = vector("target", *position + kor::Vec3(0.f, 0.f, -1.f));
            if (!target) return Fail(target.error());
            const auto up = vector("up", kor::Vec3(0.f, 1.f, 0.f));
            if (!up) return Fail(up.error());
            const bool main = given("main") && given("main")->Bool() && *given("main")->Bool();

            kor::Mat4 view(1.f);
            if (main && viewOverride) {
                view = viewOverride->view;      // where whoever runs the graph says the scene is seen from: an editor's own
            } else {
                // Looking at where it is, or with up the way it looks, is no way to look: a step along -z, and another up.
                kor::Vec3 towards = *target - *position;
                towards = kor::Length(towards) > 1e-6f ? kor::Normalize(towards) : kor::Vec3(0.f, 0.f, -1.f);
                kor::Vec3 upward = kor::Length(*up) > 1e-6f ? kor::Normalize(*up) : kor::Vec3(0.f, 1.f, 0.f);
                if (kor::Length(kor::Cross(towards, upward)) < 1e-4f) upward = std::abs(towards.y) > 0.99f ? kor::Vec3(0.f, 0.f, 1.f) : kor::Vec3(0.f, 1.f, 0.f);
                view = kor::LookAt(*position, *position + towards, upward);
            }
            const kor::Mat4 placed = kor::Inverse(view);
            if (main) { currentView.view = view; SeenThrough(); }
            const auto set = [&](const char* name, Value value) { out.outputs[name].value = std::move(value); };
            set("out", Matrix(view));
            set("view", Matrix(view));
            set("inverseView", Matrix(placed));
            set("position", Three({ placed[3][0], placed[3][1], placed[3][2] }));
            set("forward", Three(kor::Normalize(kor::Vec3(-placed[2][0], -placed[2][1], -placed[2][2]))));
            set("up", Three(kor::Normalize(kor::Vec3(placed[1][0], placed[1][1], placed[1][2]))));
            set("right", Three(kor::Normalize(kor::Vec3(placed[0][0], placed[0][1], placed[0][2]))));
            return {};
        }

        /**
         * A projection: a Perspective, an Orthographic, a Frustum, or an Oblique's near plane moved. Each has depth 0 at
         * its near plane and 1 at its far one, and Y pointing down as the device has it unless told otherwise. It knows
         * nothing of where it is seen from. The main one is the scene's lens: a Perspective's, in an editor, the editor's.
         */
        Expected<void> BuildLens(const Node& node, const Inputs& inputs, Built& out)
        {
            static constexpr float Radians = 3.14159265358979323846f / 180.f;
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            std::string failed;
            const auto number = [&](const char* name, const float otherwise) {
                const Value* v = given(name);
                if (!v) return otherwise;
                if (!v->Number()) { if (failed.empty()) failed = std::format("'{}' is a number", name); return otherwise; }
                return static_cast<float>(*v->Number());
            };
            for (const char* gone : { "view", "extent" })
                if (inputs.contains(gone))
                    return Fail(std::format("'{}' is no lens's now: a lens is of nothing but its numbers — a MatrixMultiply of it and a View is the two as one, "
                                            "and a Screen's `aspect` is its aspect", gone));
            float aspect = 1.f;
            if (const Value* own = given("aspect")) {
                if (!own->Number() || !(*own->Number() > 0.0)) return Fail("'aspect' is a number more than 0: the picture's width over its height");
                aspect = static_cast<float>(*own->Number());
            }
            const bool main = given("main") && given("main")->Bool() && *given("main")->Bool();
            const auto set = [&](const char* name, Value value) { out.outputs[name].value = std::move(value); };
            const std::string& type = node.type;
            kor::Mat4 projection(1.f);
            if (type == "Perspective") {
                float fieldOfView = number("fieldOfView", 60.f), near = number("near", 0.1f), far = number("far", 100.f);
                if (main && viewOverride) { fieldOfView = viewOverride->fieldOfView / Radians; near = viewOverride->near; far = viewOverride->far; }
                fieldOfView = std::clamp(fieldOfView, 1.f, 179.f);
                near = std::max(near, 1e-4f);
                const bool endless = !(far > 0.f) || !std::isfinite(far);
                if (!endless && far <= near) return Fail(std::format("'far' ({}) is to be farther than 'near' ({}), or 0 for no far plane", far, near));
                const bool reversed = given("reversedZ") && given("reversedZ")->Bool() && *given("reversedZ")->Bool();
                if (main && viewOverride && viewOverride->orthographic > 0.f) {
                    // The editor's view seen straight on: a box as tall as it says, depth the same way round as the lens's.
                    const float high = viewOverride->orthographic * 0.5f, wide = high * aspect;
                    const float from = viewOverride->near, to = viewOverride->far;
                    projection = reversed ? kor::Orthographic(-wide, wide, -high, high, to, from) : kor::Orthographic(-wide, wide, -high, high, from, to);
                } else if (reversed) projection = kor::PerspectiveReversedZ(fieldOfView * Radians, aspect, near, endless ? std::numeric_limits<float>::infinity() : far);
                else if (endless) projection = kor::InfinitePerspective(fieldOfView * Radians, aspect, near);
                else projection = kor::Perspective(fieldOfView * Radians, aspect, near, far);
                set("fieldOfView", fieldOfView);
                set("aspect", aspect);
                set("near", near);
                set("far", endless ? 0.f : far);
                if (main) { currentView.fieldOfView = fieldOfView * Radians; currentView.near = near; currentView.far = endless ? 1e6f : far; }
            } else if (type == "Orthographic" || type == "Frustum") {
                const bool box = type == "Orthographic";
                const float high = number("size", 10.f) * 0.5f, wide = high * aspect;
                const float left = number("left", box ? -wide : -0.1f), right = number("right", box ? wide : 0.1f);
                const float bottom = number("bottom", box ? -high : -0.1f), top = number("top", box ? high : 0.1f);
                const float near = number("near", box ? 0.f : 0.1f), far = number("far", 100.f);
                if (left == right || bottom == top) return Fail("'left' and 'right', and 'bottom' and 'top', are to be apart: it would see nothing");
                if (far <= near) return Fail(std::format("'far' ({}) is to be farther than 'near' ({})", far, near));
                if (!box && near <= 0.f) return Fail("'near' is to be more than 0: a perspective starts in front of where it is seen from");
                projection = box ? kor::Orthographic(left, right, bottom, top, near, far) : kor::FrustumProjection(left, right, bottom, top, near, far);
                set("near", near);
                set("far", far);
            } else {   // Oblique
                const Value* own = given("projection");
                if (!own) return Fail("'projection' is the projection whose near plane is moved: a Perspective's");
                const auto lens = MatrixOf(*own, "projection");
                if (!lens) return Fail(lens.error());
                projection = *lens;
                const Value* plane = given("plane");
                const auto numbers = plane ? NumbersOf(*plane, "plane") : Expected<std::vector<double>>(Fail(""));
                if (!numbers || numbers->size() != 4) return Fail("'plane' is four numbers: its normal, and its distance");
                const kor::Vec4 clip(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]), static_cast<float>((*numbers)[3]));
                // Lengyel's: the far corner of the picture on the plane's side, and the depth row made the plane, scaled so
                // that corner is still at depth 1.
                const auto sign = [](const float v) { return v > 0.f ? 1.f : v < 0.f ? -1.f : 0.f; };
                const kor::Vec4 corner = kor::Inverse(projection) * kor::Vec4(sign(clip.x * projection[0][0]), sign(clip.y * projection[1][1]), 1.f, 1.f);
                const float along = kor::Dot(clip, corner);
                if (std::abs(along) < 1e-12f) return Fail("'plane' goes through where it is seen from: a near plane is in front of it");
                const kor::Vec4 row = clip * (1.f / along);
                for (int column = 0; column < 4; ++column) projection[column][2] = row[column];
            }
            if (type != "Oblique" && !(given("flipY") && given("flipY")->Bool() && !*given("flipY")->Bool()))
                for (int column = 0; column < 4; ++column) projection[column][1] = -projection[column][1];
            if (!failed.empty()) return Fail(failed);
            if (main) { mainProjection = projection; currentAspect = aspect; SeenThrough(); }
            set("out", Matrix(projection));
            set("projection", Matrix(projection));
            set("inverseProjection", Matrix(kor::Inverse(projection)));
            return {};
        }

        /** Half of all round a point, for a shader to bend into a picture: what it takes, and the turn that faces a view that way. */
        static Expected<void> BuildParaboloid(const Inputs& inputs, Built& out)
        {
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            const float near = given("near") && given("near")->Number() ? static_cast<float>(*given("near")->Number()) : 0.1f;
            const float far = given("far") && given("far")->Number() ? static_cast<float>(*given("far")->Number()) : 100.f;
            if (far <= near) return Fail(std::format("'far' ({}) is to be farther than 'near' ({})", far, near));
            std::string hemisphere = "eFront";
            if (const Value* v = given("hemisphere")) {
                if (!v->String() || (*v->String() != "eFront" && *v->String() != "eBack")) return Fail("'hemisphere' is eFront or eBack");
                hemisphere = *v->String();
            }
            if (given("view")) return Fail("'view' is no Paraboloid's now: a MatrixMultiply of its `turn` and a View's matrix is the view it bends from");
            const bool back = hemisphere == "eBack";
            kor::Mat4 turn(1.f);
            if (back) turn[0][0] = turn[2][2] = -1.f;      // half a turn round its up: its back is its front
            const Value params(Value::List { near, far, 1.f / (far - near), back ? -1.f : 1.f });
            out.outputs["out"].value = params;
            out.outputs["params"].value = params;
            out.outputs["turn"].value = Matrix(turn);
            out.outputs["near"].value = near;
            out.outputs["far"].value = far;
            return {};
        }

        /** One face of a cube round a point, as a cube map's layers are: towards +x, -x, +y, -y, +z, -z. */
        static Expected<void> BuildCubeFace(const Inputs& inputs, Built& out)
        {
            static const kor::Vec3 towards[6] { { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
            static const kor::Vec3 ups[6] { { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f } };
            const auto it = inputs.find("face");
            const double* face = it == inputs.end() ? nullptr : it->second.value.Number();
            const int which = face ? static_cast<int>(std::lround(*face)) : 0;
            if (which < 0 || which > 5) return Fail(std::format("'face' is from 0 to 5, not {}", which));
            out.outputs["direction"].value = Three(towards[which]);
            out.outputs["up"].value = Three(ups[which]);
            return {};
        }

        /**
         * A light's shadow maps: four cascades, each fitted to a slice of what a camera sees — nearer slices smaller, so
         * that a shadow is sharp where it is looked at closely.
         */
        static Expected<void> BuildCascades(const Inputs& inputs, Built& out)
        {
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            const auto number = [&](const char* name, const float otherwise) {
                const Value* v = given(name);
                return v && v->Number() ? static_cast<float>(*v->Number()) : otherwise;
            };
            const auto vector = [&](const Value* v, const kor::Vec3 otherwise) {
                if (!v) return otherwise;
                const auto numbers = NumbersOf(*v, "vector");
                return numbers && numbers->size() >= 3 ? kor::Vec3(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2])) : otherwise;
            };
            if (inputs.contains("camera")) return Fail("'camera' is no ShadowCascades' now: wire the camera's View to 'view' and its Perspective to 'projection'");
            if (!given("view") || !given("projection")) return Fail("'view' and 'projection' are the camera's the cascades are fitted to: a View's and a Perspective's");
            const auto view = MatrixOf(*given("view"), "view");
            if (!view) return Fail(view.error());
            const auto lens = MatrixOf(*given("projection"), "projection");
            if (!lens) return Fail(lens.error());
            const kor::Mat4 placed = kor::Inverse(*view), unseen = kor::Inverse(*lens);
            // Where the camera's near and far planes are, out of its projection: each corner of the picture at depth 0 and
            // at depth 1, as it is seen. A far plane at no distance — an endless perspective's — is none.
            const auto back = [&](const float x, const float y, const float z) -> std::optional<kor::Vec3> {
                const kor::Vec4 p = unseen * kor::Vec4(x, y, z, 1.f);
                if (std::abs(p.w) < 1e-9f) return std::nullopt;
                const kor::Vec3 at(p.x / p.w, p.y / p.w, p.z / p.w);
                if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || std::abs(at.z) > 1e7f) return std::nullopt;
                return at;
            };
            auto nearest = back(0.f, 0.f, 0.f), farthest = back(0.f, 0.f, 1.f);
            const bool reversed = nearest && (!farthest || -nearest->z > -farthest->z);
            const float nearZ = reversed ? 1.f : 0.f, farZ = reversed ? 0.f : 1.f;
            if (reversed) std::swap(nearest, farthest);
            if (!nearest) return Fail("'projection' has no near plane: a Perspective's, an Orthographic's or a Frustum's");
            const float cameraNear = std::max(-nearest->z, 0.001f), cameraFar = farthest ? -farthest->z : std::numeric_limits<float>::infinity();
            // A corner of what it sees, as far from it as `depth`: along the edge from the near plane to the far one — or,
            // with no far plane, along the ray from where it is seen from.
            const auto cornerAt = [&](const float x, const float y, const float depth) {
                const kor::Vec3 start = back(x, y, nearZ).value_or(kor::Vec3(0.f, 0.f, -cameraNear));
                if (const auto end = back(x, y, farZ); end && -end->z - -start.z > 1e-6f)
                    return start + (*end - start) * ((depth - -start.z) / (-end->z - -start.z));
                return start * (depth / std::max(-start.z, 1e-6f));
            };

            const float resolution = std::max(number("resolution", 2048.f), 1.f), lambda = std::clamp(number("lambda", 0.85f), 0.f, 1.f);
            const kor::Vec3 lower = vector(given("boundsMin"), { -20.f, 0.f, -20.f }), upper = vector(given("boundsMax"), { 20.f, 20.f, 20.f });
            kor::Vec3 direction = vector(given("direction"), kor::Vec3(-0.45f, -1.f, -0.35f));
            direction = kor::Length(direction) > 0.f ? kor::Normalize(direction) : kor::Vec3(0.f, -1.f, 0.f);

            const kor::Vec3 up = std::abs(direction.y) > 0.99f ? kor::Vec3(0.f, 0.f, 1.f) : kor::Vec3(0.f, 1.f, 0.f);
            const kor::Mat4 turned = kor::LookAt(kor::Vec3(0.f), direction, up);     // the light's turn, and no place
            const kor::Mat4 undo = kor::Inverse(turned);
            const float near = cameraNear, far = std::max(std::min(cameraFar, number("distance", 200.f)), near * 1.01f);
            Value::List matrices, splits, texels;
            float nearer = near;
            for (int cascade = 0; cascade < 4; ++cascade) {
                // Where the slice ends: between even steps and steps that grow with distance, as lambda says.
                const float part = static_cast<float>(cascade + 1) / 4.f;
                const float farther = (near + (far - near) * part) * (1.f - lambda) + near * std::pow(far / near, part) * lambda;
                // A sphere round the slice: as big whichever way the camera turns, so the shadow does not crawl.
                kor::Vec3 corners[8], centre(0.f);
                int at = 0;
                for (const float depth : { nearer, farther })
                    for (const float y : { -1.f, 1.f })
                        for (const float x : { -1.f, 1.f }) {
                            const kor::Vec3 seen = cornerAt(x, y, depth);
                            const kor::Vec4 corner = placed * kor::Vec4(seen.x, seen.y, seen.z, 1.f);
                            centre += corners[at++] = kor::Vec3(corner.x, corner.y, corner.z);
                        }
                centre /= 8.f;
                float radius = 0.f;
                for (const auto& corner : corners) radius = std::max(radius, kor::Length(corner - centre));
                radius = std::ceil(radius * 16.f) / 16.f;
                // Its centre moves a texel of the map at a time, for the same reason.
                const float texel = 2.f * radius / resolution;
                kor::Vec4 lit = turned * kor::Vec4(centre.x, centre.y, centre.z, 1.f);
                lit.x = std::floor(lit.x / texel) * texel;
                lit.y = std::floor(lit.y / texel) * texel;
                const kor::Vec4 snapped = undo * lit;
                const kor::Vec3 middle(snapped.x, snapped.y, snapped.z);
                // Back towards the light as far as the scene goes: what is above the slice casts into it.
                float reach = 0.f;
                for (int i = 0; i < 8; ++i)
                    reach = std::max(reach, kor::Dot(middle - kor::Vec3(i & 1 ? upper.x : lower.x, i & 2 ? upper.y : lower.y, i & 4 ? upper.z : lower.z), direction));
                const kor::Mat4 seen = kor::LookAt(middle - direction * (reach + radius), middle, up);
                const kor::Mat4 both = kor::Orthographic(-radius, radius, -radius, radius, 0.f, reach + 2.f * radius) * seen;
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) matrices.emplace_back(both[column][row]);
                splits.emplace_back(farther);
                texels.emplace_back(texel);
                nearer = farther;
            }
            out.outputs["matrices"].value = Value(std::move(matrices));
            out.outputs["out"].value = out.outputs["matrices"].value;
            out.outputs["splits"].value = Value(std::move(splits));
            out.outputs["worldTexel"].value = Value(std::move(texels));
            out.outputs["tileTexel"].value = 1.f / resolution;
            out.outputs["direction"].value = Three(direction);
            return {};
        }

        /**
         * The point lights' shadow maps: a cube round each, a face a quarter turn wide towards each way along each axis,
         * from just off the light to as far as it reaches. Which of them are drawn again this frame: `each` of them, a
         * light after another — and whichever is not where, or as far-reaching, as it was when it was drawn last.
         */
        Expected<void> BuildPointShadows(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto number = [&](const char* name, const float otherwise) {
                const auto it = inputs.find(name);
                const double* given = it == inputs.end() ? nullptr : it->second.value.Number();
                return given ? static_cast<float>(*given) : otherwise;
            };
            std::vector<double> lights;
            if (const auto it = inputs.find("lights"); it != inputs.end() && !it->second.value.IsNull()
                                                       && !(it->second.value.Items() && it->second.value.Items()->empty())) {    // none yet: no lights
                auto numbers = NumbersOf(it->second.value, "lights");
                if (!numbers) return Fail("'lights' is four numbers a light: where it is, and how far it reaches");
                lights = std::move(*numbers);
            }
            const int most = std::clamp(static_cast<int>(number("most", 8.f)), 1, 64);
            const float near = std::max(number("near", 0.05f), 0.001f);
            static const kor::Vec3 towards[6] { { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
            static const kor::Vec3 ups[6] { { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f } };
            Value::List matrices;
            matrices.reserve(static_cast<std::size_t>(most) * 96);
            const int each = static_cast<int>(number("each", 0.f));
            auto& drawn = pointShadowsDrawn[node.id];
            std::vector<bool> due(static_cast<std::size_t>(most), each <= 0);
            const int count = std::min(most, static_cast<int>(lights.size() / 4));
            for (int light = 0; light < count; ++light) {
                const auto at = static_cast<std::size_t>(light) * 4;
                const kor::Vec3 from(static_cast<float>(lights[at]), static_cast<float>(lights[at + 1]), static_cast<float>(lights[at + 2]));
                const float range = static_cast<float>(lights[at + 3]);
                const kor::Mat4 lens = kor::Perspective(1.5707963f, 1.f, near, std::max(range, near * 2.f));
                const std::array<float, 4> now { from.x, from.y, from.z, range };
                if (static_cast<std::size_t>(light) >= drawn.as.size()) drawn.as.resize(static_cast<std::size_t>(light) + 1, { 0.f, 0.f, 0.f, -1.f });
                if (drawn.as[static_cast<std::size_t>(light)] != now) { drawn.as[static_cast<std::size_t>(light)] = now; due[static_cast<std::size_t>(light)] = true; }
                for (int face = 0; face < 6; ++face) {
                    const kor::Mat4 both = lens * kor::LookAt(from, from + towards[face], ups[face]);
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row) matrices.emplace_back(both[column][row]);
                }
            }
            // A light there is none of: everything is to one side of what its faces see, and is drawn into none.
            for (int face = count * 6; face < most * 6; ++face)
                for (int i = 0; i < 16; ++i) matrices.emplace_back(i == 12 ? 2.0 : i == 15 ? 1.0 : 0.0);
            out.outputs["matrices"].value = Value(std::move(matrices));
            out.outputs["out"].value = out.outputs["matrices"].value;
            if (each > 0 && count > 0)
                for (int i = 0; i < each; ++i) due[static_cast<std::size_t>((drawn.next + i) % count)] = true;
            if (count > 0) drawn.next = (drawn.next + std::max(each, 0)) % count;
            Value::List idle;
            for (int face = 0; face < most * 6; ++face) idle.emplace_back(face / 6 < count && due[static_cast<std::size_t>(face / 6)] ? 0 : 1);
            out.outputs["idle"].value = Value(std::move(idle));
            out.outputs["count"].value = count;
            out.outputs["faces"].value = most * 6;
            return {};
        }

        /**
         * The shape of the input @p name of @p consumer, when it is said: of its type's, or a draw's `push.` and `param.`
         * as its pipeline has them, or a pipeline's `constant.` as its shaders had them when it was last built.
         */
        [[nodiscard]] Shape InputShape(const Node& consumer, const std::string& name) const
        {
            const auto in = [&](const std::vector<Port>& ports) -> Shape {
                const auto port = std::ranges::find(ports, name, &Port::name);
                return port == ports.end() ? Shape {} : port->shape;
            };
            if (const NodeType* type = FindType(consumer.type))
                if (const Shape shape = in(PortsFor(*type, consumer)); shape.Known()) return shape;
            if (name.starts_with("push.") || name.starts_with("param.")) {
                const auto pipeline = consumer.inputs.find("pipeline");
                if (pipeline == consumer.inputs.end() || !pipeline->second.link) return {};
                const Node* of = Origin(pipeline->second.link->node);
                const auto made = of ? built.find(of->id) : built.end();
                return made != built.end() && made->second.pipeline ? in(made->second.pipeline->drawPorts) : Shape {};
            }
            if (name.starts_with("constant."))
                if (const auto made = built.find(consumer.id); made != built.end() && made->second.pipeline) return in(made->second.pipeline->ports);
            return {};
        }

        /**
         * What the inputs @p id 's output is wired to take, the first that says: through a node that takes any kind and gives
         * what it is given (a Math node's Add, Lerp), what that one's output is wired to. None when nothing says.
         */
        [[nodiscard]] Shape WiredShape(const std::string& id, const int depth = 0) const
        {
            if (!current || depth > 8) return {};
            for (const Node& consumer : current->nodes)
                for (const auto& [name, input] : consumer.inputs) {
                    if (!input.link || input.link->node != id) continue;
                    if (const Shape shape = InputShape(consumer, name); shape.Known()) return shape;
                    const NodeType* type = FindType(consumer.type);
                    const auto out = type ? std::ranges::find(type->outputs, std::string_view("out"), &Port::name) : std::vector<Port>::const_iterator {};
                    const bool passes = type && out != type->outputs.end() && out->type == "value"
                                        && std::ranges::any_of(type->inputs, [&](const Port& port) { return port.name == name && port.type == "value"; });
                    if (passes)
                        if (const Shape shape = WiredShape(consumer.id, depth + 1); shape.Known()) return shape;
                }
            return {};
        }

        /** What a Random gives: what its `type` says, or for Auto what it is wired to takes, and a Float when that says nothing. */
        [[nodiscard]] Shape RandomShape(const Node& node) const
        {
            if (const auto kind = node.inputs.find("type"); kind != node.inputs.end() && !kind->second.link && kind->second.value.String())
                if (const Shape named = ShapeNamed(*kind->second.value.String()); named.Known()) return named;
            const Shape wired = WiredShape(node.id);
            return wired.Known() && wired.scalar != Scalar::eBool ? wired : Shape { Scalar::eFloat, 1 };
        }

        /**
         * A Random's number, or vector of them, of its node's distribution and of RandomShape's: each component drawn in turn
         * from one generator seeded with its seed, by the distribution's inputs — each a number for every component, or a
         * vector, one for each. One not given is what the node type says it starts as.
         */
        Expected<void> BuildRandom(const Node& node, const random::Distribution distribution, const Inputs& inputs, Value& result)
        {
            const NodeType* type = FindType(node.type);
            const auto input = [&](const std::string_view name) -> const Value& {
                static const Value none;
                if (const auto it = inputs.find(std::string(name)); it != inputs.end() && !it->second.value.IsNull()) return it->second.value;
                if (type)
                    if (const auto port = std::ranges::find(type->inputs, name, &Port::name); port != type->inputs.end()) return port->initial;
                return none;
            };
            if (const Value& kind = input("type"); !kind.IsNull() && (!kind.String() || (*kind.String() != "Auto" && !ShapeNamed(*kind.String()).Known())))
                return Fail("'type' is Auto, or a scalar or a vector of one: Float to UVec4");
            const auto shaped = randomShapes.find(node.id);
            const Shape shape = shaped != randomShapes.end() ? shaped->second : RandomShape(node);
            const std::size_t size = shape.components;
            const double* seed = input("seed").Number();
            if (!seed) return Fail("'seed' is a number: a whole one, 0 or more");
            std::mt19937_64 engine(static_cast<std::uint64_t>(std::clamp(std::trunc(*seed), 0.0, 18446744073709549568.0)));

            Value::List components;
            for (std::size_t component = 0; component < size; ++component) {
                random::Parameters with;
                for (const auto& each : random::InputsOf(distribution)) {
                    const Value& given = input(each.name);
                    double* to = random::ParameterNamed(with, each.name);
                    if (const double* n = given.Number()) { *to = *n; continue; }
                    const auto* items = given.Items();
                    if (!items || items->size() != size)
                        return Fail(std::format("'{}' is a number, or a vector of {} numbers, one for each component of the {} it gives", each.name, size, ShapeName(shape)));
                    const double* n = (*items)[component].Number();
                    if (!n) return Fail(std::format("'{}' is a vector of numbers", each.name));
                    *to = *n;
                }
                if (const auto wrong = random::Check(distribution, with, shape.Whole()))
                    return Fail(size > 1 ? std::format("{}, of component {}", *wrong, "xyzw"[component]) : *wrong);
                components.emplace_back(random::Draw(distribution, with, shape.Whole(), engine));
            }
            result = Conform(size == 1 ? components.front() : Value(std::move(components)), shape);
            return {};
        }

        /** A colour said one way, said another: picked (sRGB), linear, by the power 2.2, as hue, saturation and value, or a temperature. */
        Expected<void> BuildColorConvert(const Inputs& inputs, Built& out)
        {
            const auto text = [&](const char* name, const char* otherwise) {
                const auto it = inputs.find(name);
                return it == inputs.end() || !it->second.value.String() ? std::string(otherwise) : *it->second.value.String();
            };
            const std::string from = text("from", "eSrgb"), to = text("to", "eLinear");
            const auto color = inputs.find("color");
            if (color == inputs.end() || color->second.value.IsNull()) return Fail("'color' is not given");
            const auto toLinear = [](const double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
            const auto fromLinear = [](const double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; };
            std::vector<double> rgb;
            bool alone = false;     // one number, not a colour: worked on as one channel
            if (from == "eKelvin") {
                const double* kelvin = color->second.value.Number();
                if (!kelvin || *kelvin <= 0) return Fail("'color' is a temperature, in kelvin: 6500 is daylight");
                // Tanner Helland's fit of a black body's colour, as a display shows it; then made linear.
                const double t = std::clamp(*kelvin, 1000.0, 40000.0) / 100.0;
                const double r = t <= 66 ? 255 : 329.698727446 * std::pow(t - 60, -0.1332047592);
                const double g = t <= 66 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60, -0.0755148492);
                const double b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
                for (const double c : { r, g, b }) rgb.push_back(toLinear(std::clamp(c, 0.0, 255.0) / 255.0));
            } else {
                if (const double* n = color->second.value.Number()) { rgb = { *n }; alone = true; }
                else {
                    auto numbers = NumbersOf(color->second.value, "color");
                    if (!numbers) return Fail(numbers.error());
                    if (numbers->size() < 3) return Fail("'color' is red, green, blue and, if given, alpha");
                    rgb = std::move(*numbers);
                }
                const std::size_t channels = alone ? 1 : 3;
                if (from == "eHsv") {
                    if (alone) return Fail("of eHsv, 'color' is hue, saturation and value");
                    const double h = (rgb[0] - std::floor(rgb[0])) * 6.0, sat = rgb[1], v = rgb[2];
                    const double c = v * sat, x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0)), m = v - c;
                    const int sector = static_cast<int>(h) % 6;
                    const double table[6][3] { { c, x, 0 }, { x, c, 0 }, { 0, c, x }, { 0, x, c }, { x, 0, c }, { c, 0, x } };
                    for (int i = 0; i < 3; ++i) rgb[static_cast<std::size_t>(i)] = toLinear(table[sector][i] + m);
                } else {
                    for (std::size_t i = 0; i < channels; ++i) {
                        if (from == "eSrgb") rgb[i] = toLinear(std::max(rgb[i], 0.0));
                        else if (from == "eGamma22") rgb[i] = std::pow(std::max(rgb[i], 0.0), 2.2);
                        else if (from != "eLinear") return Fail(std::format("'from' is eSrgb, eLinear, eGamma22, eHsv or eKelvin, not '{}'", from));
                    }
                }
            }
            // Linear now: how bright it is, and then what it is to be.
            out.outputs["luminance"].value = alone ? rgb[0] : 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
            const std::size_t channels = alone ? 1 : 3;
            if (to == "eHsv") {
                if (alone) return Fail("of eHsv, 'color' is red, green and blue");
                const double r = fromLinear(rgb[0]), g = fromLinear(rgb[1]), b = fromLinear(rgb[2]);
                const double most = std::max({ r, g, b }), least = std::min({ r, g, b }), d = most - least;
                double h = 0;
                if (d > 0) h = most == r ? std::fmod((g - b) / d, 6.0) : most == g ? (b - r) / d + 2 : (r - g) / d + 4;
                h /= 6.0;
                if (h < 0) h += 1;
                rgb[0] = h;
                rgb[1] = most > 0 ? d / most : 0;
                rgb[2] = most;
            } else {
                for (std::size_t i = 0; i < channels; ++i) {
                    if (to == "eSrgb") rgb[i] = fromLinear(std::max(rgb[i], 0.0));
                    else if (to == "eGamma22") rgb[i] = std::pow(std::max(rgb[i], 0.0), 1.0 / 2.2);
                    else if (to != "eLinear") return Fail(std::format("'to' is eLinear, eSrgb, eGamma22 or eHsv, not '{}'", to));
                }
            }
            out.outputs["out"].value = alone ? Value(rgb[0]) : Value(Value::List(rgb.begin(), rgb.end()));
            return {};
        }

        /** Turns as quaternions — x, y, z, w — made, put together, and turned into other things. */
        Expected<void> BuildQuaternion(const std::string& type, const Inputs& inputs, Built& out)
        {
            const auto numbers = [&](const char* name, const std::size_t size) -> Expected<std::vector<double>> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || it->second.value.IsNull()) return Fail(std::format("'{}' is not given", name));
                auto read = NumbersOf(it->second.value, name);
                if (!read) return Fail(read.error());
                if (read->size() != size) return Fail(std::format("'{}' is {} numbers", name, size));
                return read;
            };
            const auto quat = [&](const char* name) -> Expected<kecs::Quat> {
                auto q = numbers(name, 4);
                if (!q) return Fail(q.error());
                return kecs::Quat { static_cast<float>((*q)[0]), static_cast<float>((*q)[1]), static_cast<float>((*q)[2]), static_cast<float>((*q)[3]) };
            };
            const auto three = [&](const char* name) -> Expected<kecs::Vec3> {
                auto v = numbers(name, 3);
                if (!v) return Fail(v.error());
                return kecs::Vec3 { static_cast<float>((*v)[0]), static_cast<float>((*v)[1]), static_cast<float>((*v)[2]) };
            };
            const auto give = [&](const kecs::Quat& q) { out.outputs["out"].value = Value(Value::List { q.x, q.y, q.z, q.w }); };
            const auto times = [](const kecs::Quat& a, const kecs::Quat& b) {
                return kecs::Quat { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
            };
            if (type == "QuatFromEuler") {
                const auto rotation = three("rotation");
                if (!rotation) return Fail(rotation.error());
                give(kecs::ToQuat(*rotation));
            } else if (type == "QuatFromAxisAngle") {
                const auto axis = three("axis");
                if (!axis) return Fail(axis.error());
                const auto angle = inputs.find("angle");
                const double* degrees = angle == inputs.end() ? nullptr : angle->second.value.Number();
                if (!degrees) return Fail("'angle' is how far it turns, in degrees");
                const float length = std::sqrt(axis->x * axis->x + axis->y * axis->y + axis->z * axis->z);
                if (length <= 0.f) return Fail("'axis' is a direction: not of length 0");
                const float half = static_cast<float>(*degrees * std::numbers::pi / 360.0), sine = std::sin(half) / length;
                give({ axis->x * sine, axis->y * sine, axis->z * sine, std::cos(half) });
            } else if (type == "QuatMultiply") {
                const auto a = quat("a");
                if (!a) return Fail(a.error());
                const auto b = quat("b");
                if (!b) return Fail(b.error());
                give(times(*a, *b));
            } else {
                const auto q = quat(type == "QuatSlerp" ? "a" : "q");
                if (!q) return Fail(q.error());
                if (type == "QuatToEuler") {
                    const kecs::Vec3 angles = kecs::ToEuler(kecs::Normalize(*q));
                    out.outputs["out"].value = Value(Value::List { angles.x, angles.y, angles.z });
                } else if (type == "QuatToMatrix") {
                    out.outputs["out"].value = Matrix(ToKor(kecs::ComposeQuat({}, *q, { 1.f, 1.f, 1.f })));
                } else if (type == "QuatInverse") {
                    const float square = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
                    if (square <= 0.f) return Fail("'q' is of length 0: no turn to turn back");
                    give({ -q->x / square, -q->y / square, -q->z / square, q->w / square });
                } else if (type == "QuatRotate") {
                    const auto v = three("vector");
                    if (!v) return Fail(v.error());
                    const kecs::Quat unit = kecs::Normalize(*q);
                    const kecs::Quat turned = times(times(unit, { v->x, v->y, v->z, 0.f }), { -unit.x, -unit.y, -unit.z, unit.w });
                    out.outputs["out"].value = Value(Value::List { turned.x, turned.y, turned.z });
                } else if (type == "QuatSlerp") {
                    const auto b = quat("b");
                    if (!b) return Fail(b.error());
                    const auto it = inputs.find("t");
                    const double* t = it == inputs.end() ? nullptr : it->second.value.Number();
                    if (!t) return Fail("'t' is a number: 0 a, 1 b");
                    const kecs::Quat from = kecs::Normalize(*q);
                    kecs::Quat towards = kecs::Normalize(*b);
                    double cosine = from.x * towards.x + from.y * towards.y + from.z * towards.z + from.w * towards.w;
                    // The shorter way round: a quaternion and its negation turn alike.
                    if (cosine < 0) { towards = { -towards.x, -towards.y, -towards.z, -towards.w }; cosine = -cosine; }
                    double wa = 1.0 - *t, wb = *t;
                    if (cosine < 0.9995) {
                        const double angle = std::acos(std::min(cosine, 1.0)), sine = std::sin(angle);
                        wa = std::sin((1.0 - *t) * angle) / sine;
                        wb = std::sin(*t * angle) / sine;
                    }
                    give(kecs::Normalize({ static_cast<float>(wa * from.x + wb * towards.x), static_cast<float>(wa * from.y + wb * towards.y),
                                           static_cast<float>(wa * from.z + wb * towards.z), static_cast<float>(wa * from.w + wb * towards.w) }));
                } else {
                    return Fail(std::format("there is no node type '{}'", type));
                }
            }
            return {};
        }

        /** A place within the pixel for a frame, of a Halton (2, 3) sequence, and a projection moved by it. */
        static Expected<void> BuildJitter(const Inputs& inputs, Built& out)
        {
            const auto number = [&](const char* name) -> const double* {
                const auto it = inputs.find(name);
                return it == inputs.end() ? nullptr : it->second.value.Number();
            };
            const double* frame = number("frame");
            if (!frame) return Fail("'frame' is which frame it is: a Time's `frame`");
            const double* given = number("phases");
            const auto phases = static_cast<std::uint64_t>(given ? std::max(1.0, std::floor(*given)) : 8.0);
            const std::uint64_t index = static_cast<std::uint64_t>(std::max(0.0, std::floor(*frame))) % phases;
            const auto halton = [](std::uint64_t i, const std::uint64_t base) {
                double f = 1.0, r = 0.0;
                while (i > 0) { f /= static_cast<double>(base); r += f * static_cast<double>(i % base); i /= base; }
                return r;
            };
            const double x = halton(index + 1, 2) - 0.5, y = halton(index + 1, 3) - 0.5;
            double width = 1.0, height = 1.0;
            if (const auto size = inputs.find("size"); size != inputs.end() && !size->second.value.IsNull()) {
                const auto numbers = NumbersOf(size->second.value, "size");
                if (!numbers || numbers->size() < 2) return Fail("'size' is the picture's width and height, in pixels");
                width = std::max((*numbers)[0], 1.0);
                height = std::max((*numbers)[1], 1.0);
            }
            const double cx = 2.0 * x / width, cy = 2.0 * y / height;
            out.outputs["pixels"].value = Value(Value::List { x, y });
            out.outputs["clip"].value = Value(Value::List { cx, cy });
            out.outputs["index"].value = static_cast<double>(index);
            // Moved in clip space by as much at every depth: each column's x and y gain its w times the offset.
            if (const auto projection = inputs.find("projection"); projection != inputs.end() && !projection->second.value.IsNull()) {
                auto m = NumbersOf(projection->second.value, "projection");
                if (!m || m->size() != 16) return Fail("'projection' is a matrix: sixteen numbers");
                for (std::size_t column = 0; column < 4; ++column) {
                    (*m)[column * 4 + 0] += cx * (*m)[column * 4 + 3];
                    (*m)[column * 4 + 1] += cy * (*m)[column * 4 + 3];
                }
                out.outputs["jittered"].value = Value(Value::List(m->begin(), m->end()));
            }
            return {};
        }

        Expected<void> BuildValue(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto value = [&](const char* name) -> const Value& {
                static const Value none;
                const auto it = inputs.find(name);
                return it == inputs.end() ? none : it->second.value;
            };
            const auto given = [&](const char* name) -> Expected<const Value*> {
                const Value& v = value(name);
                if (v.IsNull()) return Fail(std::format("'{}' is not given", name));
                return &v;
            };
            Value& result = out.outputs["out"].value;
            const std::string& type = node.type;

            // A whole number of a kind: rounded toward zero, kept within what it holds.
            const auto whole = [](const double n, const bool unsigned_) {
                const double low = unsigned_ ? 0.0 : -2147483648.0, high = unsigned_ ? 4294967295.0 : 2147483647.0;
                return std::clamp(std::trunc(n), low, high);
            };
            if (const auto distribution = random::DistributionOfNode(type)) return BuildRandom(node, *distribution, inputs, result);
            if (type == "RandomDevice") {
                auto [kept, fresh] = deviceSeeds.try_emplace(node.id);
                if (fresh || kept->second.draw != value("draw")) kept->second = { value("draw"), std::random_device {}() };
                result = static_cast<double>(kept->second.seed);
                return {};
            }
            if (type == "Double") {
                if (!value("value").Number()) return Fail("'value' is a number");
                result = value("value");
            } else if (type == "Float") {
                if (!value("value").Number()) return Fail("'value' is a number");
                result = static_cast<double>(static_cast<float>(*value("value").Number()));
            } else if (type == "Int" || type == "UInt") {
                if (!value("value").Number()) return Fail("'value' is a number");
                result = whole(*value("value").Number(), type == "UInt");
            } else if ((type.starts_with("Vec") || type.starts_with("IVec") || type.starts_with("UVec")) && type.size() >= 4 && std::isdigit(static_cast<unsigned char>(type.back()))) {
                // As many components as it is of, each of its kind; one not given, 0.
                const int size = type.back() - '0';
                Value::List components;
                for (int i = 0; i < size; ++i) {
                    const char name[2] { "xyzw"[i], 0 };
                    const Value& given = value(name);
                    if (!given.IsNull() && !given.Number()) return Fail(std::format("'{}' is a number", name));
                    const double n = given.Number() ? *given.Number() : 0.0;
                    components.emplace_back(type.starts_with("Vec") ? static_cast<double>(static_cast<float>(n)) : whole(n, type.starts_with("U")));
                }
                result = Value(std::move(components));
            } else if (type == "Bool") {
                if (!value("value").Bool()) return Fail("'value' is true or false");
                result = value("value");
            } else if (type == "String") {
                if (!value("value").String()) return Fail("'value' is text");
                result = value("value");
            } else if (type == "Color") {
                const auto* items = value("color").Items();
                if (!items || items->size() < 3) return Fail("'color' is red, green, blue and alpha");
                result = value("color");
            } else if (type == "Split") {
                // Each component by its letter; one the vector does not have is 0.
                const auto numbers = NumbersOf(value("vector"), "vector");
                if (!numbers) return Fail(numbers.error());
                const char* letters[] { "x", "y", "z", "w" };
                for (std::size_t i = 0; i < 4; ++i) out.outputs[letters[i]].value = Value(i < numbers->size() ? (*numbers)[i] : 0.0);
                result = value("vector");
            } else if (type == "Append") {
                Value::List joined;
                for (const char* name : { "a", "b" }) {
                    const auto numbers = NumbersOf(value(name), name);
                    if (!numbers) return Fail(numbers.error());
                    for (const double number : *numbers) joined.emplace_back(number);
                }
                result = Value(std::move(joined));
            } else if (type == "Slice") {
                const auto numbers = NumbersOf(value("of"), "of");
                if (!numbers) return Fail(numbers.error());
                const double* first = value("first").Number();
                const double* count = value("count").Number();
                if (!first || !count || *first < 0 || *count < 0) return Fail("'first' and 'count' are numbers, from 0");
                const auto from = static_cast<std::size_t>(*first), many = static_cast<std::size_t>(*count);
                if (from + many > numbers->size())
                    return Fail(std::format("{} from the {}th is past the {} numbers of 'of'", many, from, numbers->size()));
                Value::List part;
                for (std::size_t i = from; i < from + many; ++i) part.emplace_back((*numbers)[i]);
                result = Value(std::move(part));
            } else if (type == "MatrixMultiply" || type == "MatrixInverse" || type == "MatrixTranspose" || type == "MatrixDeterminant") {
                const auto a = MatrixOf(value("a"), "a");
                if (!a) return Fail(a.error());
                if (type == "MatrixInverse") result = Matrix(kor::Inverse(*a));
                else if (type == "MatrixTranspose") result = Matrix(kor::Transpose(*a));
                else if (type == "MatrixDeterminant") result = Value(static_cast<double>(kor::Determinant(*a)));
                else {
                    const auto b = MatrixOf(value("b"), "b");
                    if (!b) return Fail(b.error());
                    result = Matrix(*a * *b);
                }
            } else if (type == "MatrixTranslate" || type == "MatrixRotate" || type == "MatrixAxisAngle" || type == "MatrixScale" || type == "MatrixCompose") {
                // A vector of three by name; a number, for a scale, the same every way.
                const auto three = [&](const char* name, const kor::Vec3 otherwise, const bool even = false) -> Expected<kor::Vec3> {
                    const Value& v = value(name);
                    if (v.IsNull()) return otherwise;
                    if (even && v.Number()) return kor::Vec3(static_cast<float>(*v.Number()));
                    const auto numbers = NumbersOf(v, name);
                    if (!numbers || numbers->size() < 3) return Fail(std::format("'{}' is a vector of three", name));
                    return kor::Vec3(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]));
                };
                const auto position = three(type == "MatrixTranslate" ? "offset" : "position", kor::Vec3(0.f));
                const auto rotation = three(type == "MatrixAxisAngle" ? "axis" : "rotation", kor::Vec3(0.f));
                const auto scale = three("scale", kor::Vec3(1.f), true);
                for (const auto* failed : { &position, &rotation, &scale })
                    if (!*failed) return Fail(failed->error());
                if (type == "MatrixAxisAngle") {
                    const double* angle = value("angle").Number();
                    if (!angle) return Fail("'angle' is a number of degrees");
                    if (kor::Length(*rotation) <= 0.f) return Fail("'axis' is a vector that is not all zero");
                    const kor::Quat turn = kor::Quat::AngleAxis(static_cast<float>(*angle) * 3.14159265358979323846f / 180.f, kor::Normalize(*rotation));
                    result = Matrix(kor::Compose(kor::Vec3(0.f), turn, kor::Vec3(1.f)));
                } else {
                    // Turned as an entity is, by the world's own arithmetic: x, y and z in degrees.
                    const bool moves = type == "MatrixTranslate" || type == "MatrixCompose", turns = type == "MatrixRotate" || type == "MatrixCompose";
                    const bool scales = type == "MatrixScale" || type == "MatrixCompose";
                    result = Matrix(ToKor(kecs::Compose(moves ? kecs::Vec3 { position->x, position->y, position->z } : kecs::Vec3 {},
                                                        turns ? kecs::Vec3 { rotation->x, rotation->y, rotation->z } : kecs::Vec3 {},
                                                        scales ? kecs::Vec3 { scale->x, scale->y, scale->z } : kecs::Vec3 { 1.f, 1.f, 1.f })));
                }
            } else if (type == "TransformPlane") {
                const auto m = MatrixOf(value("matrix"), "matrix");
                if (!m) return Fail(m.error());
                const auto numbers = NumbersOf(value("plane"), "plane");
                if (!numbers || numbers->size() != 4) return Fail("'plane' is four numbers: its normal, and its distance");
                const kor::Vec4 plane(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]), static_cast<float>((*numbers)[3]));
                // A plane moves as the inverse of a point does, transposed.
                const kor::Vec4 moved = kor::Transpose(kor::Inverse(*m)) * plane;
                result = Value(Value::List { moved.x, moved.y, moved.z, moved.w });
            } else if (type == "TransformPoint" || type == "TransformDirection") {
                const auto m = MatrixOf(value("matrix"), "matrix");
                if (!m) return Fail(m.error());
                const bool point = type == "TransformPoint";
                const char* name = point ? "point" : "direction";
                const auto numbers = NumbersOf(value(name), name);
                if (!numbers || numbers->size() < 3) return Fail(std::format("'{}' is a vector of three", name));
                const kor::Vec4 moved = *m * kor::Vec4(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]), point ? 1.f : 0.f);
                const float w = point && std::abs(moved.w) > 1e-12f ? moved.w : 1.f;
                result = Three({ moved.x / w, moved.y / w, moved.z / w });
            } else if (type == "Constant") {
                static const std::map<std::string, double, std::less<>> constants {
                    { "ePi", 3.14159265358979323846 }, { "eTau", 6.28318530717958647692 }, { "eE", 2.71828182845904523536 },
                    { "eSqrt2", 1.41421356237309504880 }, { "ePhi", 1.61803398874989484820 } };
                const std::string* name = value("value").String();
                const auto it = name ? constants.find(*name) : constants.end();
                if (it == constants.end()) return Fail("'value' is ePi, eTau, eE, eSqrt2 or ePhi");
                result = Value(it->second);
            } else if (type == "Reflect" || type == "Refract" || type == "Project") {
                const char* first = type == "Project" ? "a" : "incident";
                const char* second = type == "Project" ? "onto" : "normal";
                const auto a = NumbersOf(value(first), first);
                if (!a) return Fail(a.error());
                const auto b = NumbersOf(value(second), second);
                if (!b) return Fail(b.error());
                if (a->size() != b->size()) return Fail(std::format("a vector of {} and one of {} do not go together", a->size(), b->size()));
                double dot = 0.0, square = 0.0;
                for (std::size_t i = 0; i < a->size(); ++i) { dot += (*a)[i] * (*b)[i]; square += (*b)[i] * (*b)[i]; }
                Value::List made;
                if (type == "Reflect") {
                    for (std::size_t i = 0; i < a->size(); ++i) made.emplace_back((*a)[i] - 2.0 * dot * (*b)[i]);
                } else if (type == "Project") {
                    for (std::size_t i = 0; i < a->size(); ++i) made.emplace_back(square > 0.0 ? (*b)[i] * dot / square : 0.0);
                } else {
                    const double* eta = value("eta").Number();
                    if (!eta) return Fail("'eta' is a number: the ratio of the indices of refraction");
                    const double k = 1.0 - *eta * *eta * (1.0 - dot * dot);
                    for (std::size_t i = 0; i < a->size(); ++i) made.emplace_back(k < 0.0 ? 0.0 : *eta * (*a)[i] - (*eta * dot + std::sqrt(k)) * (*b)[i]);
                }
                result = Value(std::move(made));
            } else if (type == "View") {
                return BuildView(inputs, out);
            } else if (type == "Perspective" || type == "Orthographic" || type == "Frustum" || type == "Oblique") {
                return BuildLens(node, inputs, out);
            } else if (type == "Paraboloid") {
                return BuildParaboloid(inputs, out);
            } else if (type == "CubeFace") {
                return BuildCubeFace(inputs, out);
            } else if (type == "ShadowCascades") {
                return BuildCascades(inputs, out);
            } else if (type == "PointShadows") {
                return BuildPointShadows(node, inputs, out);
            } else if (type == "ImageExtent") {
                const auto it = inputs.find("image");
                const auto image = it == inputs.end() ? kor::ResourceRef<const kor::Image>() : ImageRefOf(it->second.resource.get());
                if (!image.Get()) return Fail("'image' needs an image wired in");
                const auto extent = image->Extent();
                result = Value(Value::List { extent.x, extent.y, extent.z });
            } else if (type == "VertexLayout") {
                auto layout = LayoutValue(value("attributes"));
                if (!layout) return Fail(layout.error());
                result = std::move(*layout);
            } else if (type == "Length" || type == "Normalize") {
                const auto a = NumbersOf(value("a"), "a");
                if (!a) return Fail(a.error());
                double squared = 0.0;
                for (const double n : *a) squared += n * n;
                const double length = std::sqrt(squared);
                if (type == "Length") { result = Value(length); return {}; }
                Value::List unit;
                for (const double n : *a) unit.emplace_back(length > 0.0 ? n / length : 0.0);
                result = Value(std::move(unit));
            } else if (type == "Dot" || type == "Distance" || type == "Cross") {
                const auto a = NumbersOf(value("a"), "a");
                if (!a) return Fail(a.error());
                const auto b = NumbersOf(value("b"), "b");
                if (!b) return Fail(b.error());
                if (a->size() != b->size()) return Fail(std::format("a vector of {} and one of {} do not go together", a->size(), b->size()));
                if (type == "Cross") {
                    if (a->size() != 3) return Fail("a cross product is of two vectors of 3");
                    const auto& u = *a;
                    const auto& v = *b;
                    result = Value(Value::List { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] });
                    return {};
                }
                double sum = 0.0;
                for (std::size_t i = 0; i < a->size(); ++i) sum += type == "Dot" ? (*a)[i] * (*b)[i] : ((*a)[i] - (*b)[i]) * ((*a)[i] - (*b)[i]);
                result = Value(type == "Dot" ? sum : std::sqrt(sum));
            } else if (type == "Equal" || type == "NotEqual") {
                const auto a = given("a");
                if (!a) return Fail(a.error());
                const auto b = given("b");
                if (!b) return Fail(b.error());
                result = Value((**a == **b) == (type == "Equal"));
            } else if (type == "Less" || type == "LessOrEqual" || type == "Greater" || type == "GreaterOrEqual") {
                const auto compare = [&](const double a, const double b) {
                    return type == "Less" ? a < b : type == "LessOrEqual" ? a <= b : type == "Greater" ? a > b : a >= b;
                };
                const double* a = value("a").Number();
                const double* b = value("b").Number();
                if (a && b) {
                    result = Value(compare(*a, *b));
                } else {
                    // Of vectors, a component at a time; a number goes with every component of a vector.
                    const auto as = NumbersOf(value("a"), "a");
                    if (!as) return Fail(as.error());
                    const auto bs = NumbersOf(value("b"), "b");
                    if (!bs) return Fail(bs.error());
                    if (as->size() != bs->size() && as->size() != 1 && bs->size() != 1)
                        return Fail(std::format("'a' has {} components and 'b' {}: they are compared a component at a time", as->size(), bs->size()));
                    Value::List each;
                    for (std::size_t i = 0; i < std::max(as->size(), bs->size()); ++i)
                        each.emplace_back(compare((*as)[as->size() == 1 ? 0 : i], (*bs)[bs->size() == 1 ? 0 : i]));
                    result = Value(std::move(each));
                }
            } else if (type == "Any" || type == "All") {
                // A condition for each component, as a comparison of vectors gives it: true or false, or numbers not 0.
                const Value& a = value("a");
                std::vector<bool> each;
                if (const bool* one = a.Bool()) each = { *one };
                else if (const double* n = a.Number()) each = { *n != 0.0 };
                else if (const auto* items = a.Items()) {
                    for (const Value& item : *items) {
                        if (const bool* flag = item.Bool()) each.push_back(*flag);
                        else if (const double* n = item.Number()) each.push_back(*n != 0.0);
                        else return Fail("'a' is true or false, or a list of them");
                    }
                } else {
                    return Fail("'a' is true or false, or a list of them");
                }
                result = Value(type == "Any" ? std::ranges::any_of(each, std::identity {}) : std::ranges::all_of(each, std::identity {}));
            } else if (type == "Not") {
                const bool* a = value("a").Bool();
                if (!a) return Fail("'a' is true or false");
                result = Value(!*a);
            } else if (type == "And" || type == "Or" || type == "Xor") {
                const bool* a = value("a").Bool();
                const bool* b = value("b").Bool();
                if (!a || !b) return Fail("'a' and 'b' are each true or false");
                result = Value(type == "And" ? *a && *b : type == "Or" ? *a || *b : *a != *b);
            } else if (type == "Select") {
                if (const auto* conditions = value("condition").Items()) {
                    // A condition for each component: each component of a or of b, as its own says.
                    const auto as = NumbersOf(value("a"), "a");
                    if (!as) return Fail(as.error());
                    const auto bs = NumbersOf(value("b"), "b");
                    if (!bs) return Fail(bs.error());
                    Value::List chosen;
                    for (std::size_t i = 0; i < conditions->size(); ++i) {
                        const Value& c = (*conditions)[i];
                        const bool* flag = c.Bool();
                        const double* n = c.Number();
                        if (!flag && !n) return Fail("'condition' is true or false, or a list of them");
                        const bool pick = flag ? *flag : *n != 0.0;
                        const std::vector<double>& from = pick ? *as : *bs;
                        if (from.size() != 1 && from.size() <= i) return Fail(std::format("'condition' has {} components, and '{}' fewer", conditions->size(), pick ? "a" : "b"));
                        chosen.emplace_back(from[from.size() == 1 ? 0 : i]);
                    }
                    result = Value(std::move(chosen));
                    return {};
                }
                const bool* condition = value("condition").Bool();
                const double* number = value("condition").Number();
                if (!condition && !number) return Fail("'condition' is true or false");
                const auto chosen = given((condition ? *condition : *number != 0.0) ? "a" : "b");
                if (!chosen) return Fail(chosen.error());
                result = **chosen;
            } else if (type == "ColorConvert") {
                return BuildColorConvert(inputs, out);
            } else if (type.starts_with("Quat")) {
                return BuildQuaternion(type, inputs, out);
            } else if (type == "Jitter") {
                return BuildJitter(inputs, out);
            } else if (type == "DeviceSupports") {
                const std::string* feature = value("feature").String();
                if (!feature) return Fail("'feature' is what is asked of the device: eMeshShader, eRayTracing...");
                static const std::map<std::string, kor::Feature, std::less<>> features {
                    { "eMeshShader", kor::Feature::eMeshShader }, { "eRayTracing", kor::Feature::eRayTracing }, { "eShaderFloat64", kor::Feature::eShaderFloat64 },
                    { "eShaderInt64", kor::Feature::eShaderInt64 }, { "eShaderInt16", kor::Feature::eShaderInt16 }, { "eShaderFloat16", kor::Feature::eShaderFloat16 },
                    { "eInt64Atomics", kor::Feature::eInt64Atomics }, { "eAtomicFloat32", kor::Feature::eAtomicFloat32 }, { "eAtomicFloat64", kor::Feature::eAtomicFloat64 },
                    { "eMultiDrawIndirect", kor::Feature::eMultiDrawIndirect }, { "eDrawIndirectCount", kor::Feature::eDrawIndirectCount },
                    { "eGeometryShader", kor::Feature::eGeometryShader }, { "eTessellationShader", kor::Feature::eTessellationShader },
                    { "eFillModeNonSolid", kor::Feature::eFillModeNonSolid }, { "eWideLines", kor::Feature::eWideLines }, { "eSamplerAnisotropy", kor::Feature::eSamplerAnisotropy } };
                if (*feature == "eAsyncCompute") { result = Value(kor::Context::SupportsAsyncCompute()); return {}; }
                const auto it = features.find(*feature);
                if (it == features.end()) return Fail(std::format("there is no feature '{}'", *feature));
                result = Value(kor::Context::Supports(it->second));
            } else if (const auto operation = Operations().find(type); operation != Operations().end()) {
                // Each is named for what it is: a clamp's are x, min and max; a smoothstep's two edges and x.
                static const std::map<std::string, std::array<const char*, 5>, std::less<>> named {
                    { "Clamp", { "x", "min", "max" } }, { "Smoothstep", { "edge0", "edge1", "x" } }, { "Step", { "edge", "x" } },
                    { "Atan2", { "y", "x" } }, { "Lerp", { "a", "b", "t" } }, { "InverseLerp", { "a", "b", "value" } },
                    { "Remap", { "value", "fromMin", "fromMax", "toMin", "toMax" } } };
                static const std::array<const char*, 5> plain { "a", "b", "c" };
                const auto own = named.find(type);
                const int count = operation->second.first;
                std::vector<const Value*> arguments;
                for (int i = 0; i < count; ++i) {
                    const char* name = (own == named.end() ? plain : own->second)[static_cast<std::size_t>(i)];
                    const auto argument = given(name);
                    if (!argument) return Fail(argument.error());
                    arguments.push_back(*argument);
                }
                auto worked = Each(arguments, operation->second.second);
                if (!worked) return Fail(worked.error());
                result = std::move(*worked);
            } else {
                return Fail(std::format("there is no way to work out a {}", type));
            }
            return {};
        }

        /** What the default meshes have of their surface's directions: the source's, for a runtime that follows another. */
        [[nodiscard]] Tangents DefaultTangents() const { return source ? source->DefaultTangents() : defaultMeshTangents; }

        /**
         * How the default meshes' vertices are laid out: POSITION, NORMAL, the tangents the graph asks for and UV, in one
         * buffer — as a mesh of their attributes is (MeshOf).
         */
        static kor::VertexLayout DefaultLayout(const Tangents tangents)
        {
            std::vector<std::pair<const char*, kor::u32>> attributes { { "POSITION", 3 }, { "NORMAL", 3 } };
            if (tangents == Tangents::eFour) attributes.emplace_back("TANGENT", 4);
            if (tangents == Tangents::eWithBitangent) attributes.insert(attributes.end(), { { "TANGENT", 3 }, { "BITANGENT", 3 } });
            attributes.emplace_back("UV", 2);
            kor::VertexLayout layout;
            kor::u32 offset = 0;
            for (const auto& [semantic, count] : attributes) {
                layout.attributes.emplace_back(semantic, "", 0u, offset * 4u, kor::ChannelType::eFloat, count);
                offset += count;
            }
            layout.bindings.emplace_back(0u, offset * 4u);
            return layout;
        }

        /** Which attributes, of what, in which buffers: meshes alike in this are drawn by one pipeline, and can be one mesh. */
        static std::string LayoutKey(const VertexData& data)
        {
            std::string key;
            for (const auto& attribute : data.attributes) key += std::format("{}:{}:{};", attribute.semantic, static_cast<int>(attribute.type), attribute.count);
            for (const auto& buffer : data.buffers) {
                key += "|";
                for (const auto& semantic : buffer) key += semantic + ",";
            }
            return key;
        }

        /** The kor type of an attribute's numbers. */
        static kor::ChannelType ChannelOf(const AttributeType type)
        {
            return type == AttributeType::eInt ? kor::ChannelType::eInt : type == AttributeType::eUInt ? kor::ChannelType::eUInt : kor::ChannelType::eFloat;
        }

        /**
         * A mesh of any attributes on the device: a vertex buffer for each of its buffers, the attributes it names side
         * by side in each vertex, and one more for what none names. What it has not is not anywhere.
         */
        static Expected<Held> MeshOf(const VertexData& data)
        {
            if (const std::string why = data.Problem(); !why.empty()) return Fail(why);
            std::vector<std::vector<const VertexAttribute*>> groups;
            std::set<std::string_view> placed;
            for (const auto& buffer : data.buffers) {
                std::vector<const VertexAttribute*> group;
                for (const auto& semantic : buffer)
                    if (const VertexAttribute* attribute = data.Find(semantic); attribute && placed.insert(attribute->semantic).second) group.push_back(attribute);
                if (!group.empty()) groups.push_back(std::move(group));
            }
            std::vector<const VertexAttribute*> rest;
            for (const auto& attribute : data.attributes) if (!placed.contains(attribute.semantic)) rest.push_back(&attribute);
            if (!rest.empty()) groups.push_back(std::move(rest));

            kor::Mesh::Builder builder;
            kor::VertexLayout layout;
            std::vector<Held> buffers;
            for (std::size_t b = 0; b < groups.size(); ++b) {
                std::uint32_t stride = 0;
                for (const auto* attribute : groups[b]) stride += attribute->count;
                std::vector<std::uint32_t> words(static_cast<std::size_t>(stride) * data.vertexCount);
                std::uint32_t offset = 0;
                for (const auto* attribute : groups[b]) {
                    for (std::uint32_t v = 0; v < data.vertexCount; ++v)
                        std::copy_n(attribute->words.begin() + static_cast<std::ptrdiff_t>(v) * attribute->count, attribute->count,
                                    words.begin() + static_cast<std::ptrdiff_t>(v) * stride + offset);
                    layout.attributes.emplace_back(attribute->semantic, "", static_cast<kor::u32>(b), offset * 4u, ChannelOf(attribute->type), attribute->count);
                    offset += attribute->count;
                }
                layout.bindings.emplace_back(static_cast<kor::u32>(b), stride * 4u);
                // Each buffer a resource of its own, the mesh referring to it: what a compute shader reads of the mesh.
                auto buffer = kor::Mesh::MakeBuffer(words, kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eVertex) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc);
                builder.SetVertexBuffer(static_cast<kor::u32>(b), kor::ResourceRef<kor::Buffer>(buffer));
                buffers.push_back(MakeResource(std::move(buffer)));
            }
            if (!data.indices.empty()) {
                auto buffer = kor::Mesh::MakeBuffer(data.indices, kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eIndex) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc);
                builder.SetIndexBuffer(kor::ResourceRef<kor::Buffer>(buffer));
                buffers.push_back(MakeResource(std::move(buffer)));
            }
            auto mesh = builder.SetVertexLayout(std::move(layout)).Build();
            if (!mesh.Valid()) return Fail(mesh.Failure() ? mesh.Failure()->message : std::string("the mesh could not be built"));
            Held held = MakeResource(std::move(mesh));
            held->buffers = std::move(buffers);
            Bound(*held, data, 0, data.vertexCount);
            return held;
        }

        /** The box vertices @p first to @p first + @p count of @p data are in, as @p held 's. */
        static void Bound(Resource& held, const VertexData& data, const std::uint32_t first, const std::uint32_t count)
        {
            const VertexAttribute* position = data.Find("POSITION");
            if (!position || count == 0) return;
            for (int axis = 0; axis < 3; ++axis) { held.bounds[axis] = INFINITY; held.bounds[axis + 3] = -INFINITY; }
            for (std::uint32_t v = first; v < first + count; ++v)
                for (int axis = 0; axis < static_cast<int>(std::min(position->count, 3u)); ++axis) {
                    const float at = std::bit_cast<float>(position->words[static_cast<std::size_t>(v) * position->count + static_cast<std::size_t>(axis)]);
                    held.bounds[axis] = std::min(held.bounds[axis], at);
                    held.bounds[axis + 3] = std::max(held.bounds[axis + 3], at);
                }
            if (position->count < 3) held.bounds[2] = held.bounds[5] = 0.f;
            held.bounded = true;
        }

        /** A default mesh, by its name: made the first time it is asked for, and kept. Null for a name that is none. */
        Held DefaultMesh(const std::string_view name)
        {
            if (source) return source->DefaultMesh(name);
            if (const auto it = defaultMeshes.find(name); it != defaultMeshes.end()) return it->second;
            if (std::ranges::find(DefaultMeshNames(), name) == DefaultMeshNames().end()) return nullptr;
            const auto made = MeshOf(ToVertexData(*ShapeData(name), defaultMeshTangents));
            if (!made) return nullptr;
            (*made)->node = DefaultMeshId(name);   // what it is saved as, and found again by
            return defaultMeshes.emplace(std::string(name), *made).first->second;
        }

        // ---- what is in files ---------------------------------------------------------------------------------------

        std::filesystem::path assetDirectory;
        std::map<std::string, Held, std::less<>> assets;        // what was asked for so far, by the name it is found by
        std::map<std::string, std::shared_ptr<const Model>, std::less<>> models;    // by file

        /** Where files named without their whole path are: the source's, for one that follows another. */
        [[nodiscard]] const std::filesystem::path& Assets() const { return source ? source->Assets() : assetDirectory; }
        /** A file named @p name — without the `@` — where it is: under @p directory, unless the name is a whole path. */
        static std::filesystem::path FileNamed(const std::string_view name, const std::filesystem::path& directory)
        {
            std::filesystem::path path(name);
#ifdef _WIN32
            // A file named as WSL names a Windows drive, /mnt/w/..., is W:/... here: the same project opens on both.
            if (name.size() >= 6 && name.starts_with("/mnt/") && std::isalpha(static_cast<unsigned char>(name[5])) && (name.size() == 6 || name[6] == '/'))
                path = std::format("{}:/{}", static_cast<char>(std::toupper(static_cast<unsigned char>(name[5]))), name.substr(std::min<std::size_t>(name.size(), 7)));
#endif
            return path.is_absolute() || directory.empty() ? path : directory / path;
        }
        [[nodiscard]] std::filesystem::path AssetFile(const std::string_view name) const { return FileNamed(name, Assets()); }
        [[nodiscard]] std::string AssetId(const std::filesystem::path& file, const int mesh) const { return AssetName(file, Assets(), mesh); }

        /** A picture of one texel: `white`, `black`, or `normal` — a surface's own normal, as a normal map has it. */
        Held DefaultImage(const std::string_view name)
        {
            if (source) return source->DefaultImage(name);
            const std::string id = std::format("${}", name);
            if (const auto it = assets.find(id); it != assets.end()) return it->second;
            if (name != "white" && name != "black" && name != "normal") return nullptr;
            const std::array<kor::U8Vec4, 1> texel { name == "white" ? kor::U8Vec4(255, 255, 255, 255) : name == "black" ? kor::U8Vec4(0, 0, 0, 255) : kor::U8Vec4(128, 128, 255, 255) };
            auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(1, 1))
                .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                .SetData(std::span<const kor::U8Vec4>(texel)).Build();
            if (!image.Valid()) return nullptr;
            Held held = MakeResource(std::move(image));
            held->node = id;
            return assets.emplace(id, std::move(held)).first->second;
        }

        // A file is read off the frame. Until it is, what was asked for is a stand-in — a white texel for a picture,
        // nothing yet for a mesh — which whoever holds it is led from to the real thing when it arrives (replacedBy),
        // as from a node's resource to the one the node made in its place.
        struct ReadModel {
            std::shared_ptr<Model> model;
            std::vector<std::pair<std::string, VertexData>> meshes; // each by the name it is found by
            std::map<std::string, std::string> names;               // and what the file calls it
        };
        /**
         * A model's meshes of the same attributes in the same buffers, as one: a part of a model is a run of its whole's
         * indices. Parts that differ in what their vertices have cannot share a mesh, and are in one of their own. Each
         * part is all it is but the device's mesh (source), which the whole is made into on the frame.
         */
        struct Whole { VertexData all; std::vector<Held> parts; };
        struct Arrived { std::vector<Whole> wholes; };     // a model's meshes, read and put together off the frame

        /** A model's meshes put together into wholes, each part bounded: the work of it, done where the file was read. */
        static Arrived Together(std::vector<std::pair<std::string, VertexData>> meshes, const std::map<std::string, std::string>& names)
        {
            std::map<std::string, Whole> wholes;
            for (auto& [id, data] : meshes) {
                Whole& whole = wholes[LayoutKey(data)];
                if (whole.parts.empty()) {
                    whole.all.buffers = data.buffers;
                    for (const auto& attribute : data.attributes) whole.all.attributes.push_back({ attribute.semantic, attribute.type, attribute.count, {} });
                }
                Held part = std::make_shared<Resource>();
                part->kind = "Mesh";
                part->node = id;
                part->part = true;
                if (const auto called = names.find(id); called != names.end()) part->label = called->second;
                part->firstIndex = static_cast<std::uint32_t>(whole.all.indices.size());
                part->indexCount = static_cast<std::uint32_t>(data.indices.size());
                part->vertexOffset = static_cast<std::int32_t>(whole.all.vertexCount);
                part->vertexCount = data.vertexCount;
                Bound(*part, data, 0, data.vertexCount);
                for (std::size_t a = 0; a < data.attributes.size(); ++a) {
                    auto& words = whole.all.attributes[a].words;
                    words.insert(words.end(), data.attributes[a].words.begin(), data.attributes[a].words.end());
                }
                whole.all.indices.insert(whole.all.indices.end(), data.indices.begin(), data.indices.end());
                whole.all.vertexCount += data.vertexCount;
                data = {};
                whole.parts.push_back(std::move(part));
            }
            Arrived arrived;
            for (auto& whole : wholes | std::views::values) arrived.wholes.push_back(std::move(whole));
            return arrived;
        }
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
        std::vector<kor::Task<void>> reading;                   // the files being read
        /// A PointShadows node's lights as their maps were last drawn — where each was, and how far it reached — and
        /// whose turn it is to be drawn again. Forgotten when the graph is built again: the maps are then new.
        struct ShadowsDrawn { std::vector<std::array<float, 4>> as; int next = 0; };
        std::map<std::string, ShadowsDrawn> pointShadowsDrawn;
        /// Each RandomDevice's seed, and the `draw` it was drawn for: kept while the graph runs, so that an edit elsewhere
        /// draws nothing new. A `draw` changed draws again.
        struct DeviceSeed { Value draw; std::uint32_t seed = 0; };
        std::map<std::string, DeviceSeed> deviceSeeds;
        /// Each Random's shape, as it was last built: what it draws, and what is looked at again after each evaluation.
        std::map<std::string, Shape> randomShapes;
        std::set<std::string, std::less<>> readingModels;       // by file
        std::set<std::string, std::less<>> rereadModels;        // being read, and to be read again when that is done
        // What files' meshes have, as probed for an import: by file; nothing while being probed, or for no model.
        std::map<std::string, std::optional<std::vector<Model::Attribute>>, std::less<>> probed;
        std::map<std::string, MeshImport, std::less<>> meshImports;    // how each model file is read, by its name in ids
        std::map<std::string, std::string, std::less<>> unread; // a file that could not be read, and why
        std::deque<Arrived> arrivedMeshes;                      // models read, and not yet the device's: each, the next frame
        std::optional<std::chrono::steady_clock::time_point> modelsWaiting;  // since when a model read waits for the others
        std::vector<Held> arrivedPictures;                      // the stand-ins whose pictures came, for the nodes that hold one

        /** Where what is made of the project's files is kept: `.koral/cache` beside its assets. None without them. */
        static std::optional<kasset::Cache> CacheBeside(const std::filesystem::path& assetDirectory)
        {
            if (assetDirectory.empty()) return std::nullopt;
            return kasset::Cache(kasset::CacheDirectoryOf(assetDirectory.parent_path()));
        }

        /**
         * A model file's numbers, as the importer made them — from the cache, when it was read so before — with what it
         * names named as the runtime names files. Touches nothing of the runtime's, so it is read on any thread.
         */
        static std::optional<ReadModel> Read(const std::filesystem::path& file, const std::filesystem::path& assetDirectory, const MeshImport& import)
        {
            const auto named = [&](const std::filesystem::path& path, const int mesh) { return AssetName(path, assetDirectory, mesh); };
            const auto cache = CacheBeside(assetDirectory);
            auto artifact = kasset::ImportModel(file, import, cache ? &*cache : nullptr);
            if (!artifact) return std::nullopt;
            ReadModel read;
            read.model = std::make_shared<Model>(std::move(artifact->model));
            const std::filesystem::path beside = file.parent_path();
            for (ModelMaterial& material : read.model->materials)
                for (std::string* picture : { &material.albedoMap, &material.normalMap, &material.roughnessMap, &material.emissiveMap, &material.occlusionMap,
                                              &material.specularMap, &material.shininessMap, &material.alphaMap })
                    if (!picture->empty()) {
                        const std::filesystem::path path(*picture);
                        *picture = named(path.is_absolute() ? path : beside / path, -1);
                    }
            for (ModelNode& node : read.model->nodes)
                for (auto& part : node.parts)
                    if (part.mesh.starts_with('#')) part.mesh = named(file, std::stoi(part.mesh.substr(1)));
            for (auto& mesh : artifact->meshes) {
                read.names[named(file, mesh.index)] = mesh.name;
                read.meshes.emplace_back(named(file, mesh.index), std::move(mesh.data));
            }
            return read;
        }

        /** Reads a model file on a background thread, and hands what it holds back on the frame's. */
        static kor::Task<void> ReadModelFile(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string key, std::filesystem::path file,
                                             std::filesystem::path assetDirectory, MeshImport import)
        {
            co_await kor::Context::SwitchToBackgroundThread();
            auto read = Read(file, assetDirectory, import);
            std::optional<Arrived> together;
            if (read) together = Together(std::move(read->meshes), read->names);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            self->readingModels.erase(key);
            // How it is to be read changed while it was being read: what was read is not that, and it is read again.
            if (self->rereadModels.erase(key)) { self->WantModel(file); co_return; }
            if (!read) { self->models[key] = nullptr; self->unread[key] = std::format("'{}' could not be read as a model", key); co_return; }
            self->arrivedMeshes.push_back(std::move(*together));
            self->models[key] = std::move(read->model);
        }

        /** What a model file's meshes have, read off the frame without anything of them made: what an import asks about. */
        static kor::Task<void> ProbeModelFile(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string key, std::filesystem::path file)
        {
            const auto cache = CacheBeside(self->Assets());
            co_await kor::Context::SwitchToBackgroundThread();
            // Read as it is by default — every attribute — which is what it is most likely imported as next.
            auto artifact = kasset::ImportModel(file, {}, cache ? &*cache : nullptr);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            if (!artifact) {
                self->probed[key] = std::nullopt;
                self->unread[key] = artifact.error();
                co_return;
            }
            self->probed[key] = std::move(artifact->model.attributes);
        }

        /**
         * Reads a picture off the frame — its file, or the file cooked of it, kept as @p format — and leads whoever holds
         * @p standIn to it.
         */
        static kor::Task<void> ReadPicture(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string id, std::filesystem::path file, Held standIn,
                                           const TextureFormat format = TextureFormat::eAsRead)
        {
            kor::Resource<kor::Image> image = co_await kimg::LoadImageAsync(file, true);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            if (!image.Valid()) { self->unread[id] = std::format("'{}' could not be read as a picture", file.generic_string()); co_return; }
            self->unread.erase(id);
            self->PictureArrived(id, standIn, Spread(MakeResource(std::move(image)), format));
        }

        // ---- pictures kept block-compressed ----------------------------------------------------------------------

        std::map<std::string, TextureImport, std::less<>> textureImports;     // as the graph has them, by file
        std::map<std::string, TexturePack, std::less<>> texturePacks;         // by pack
        std::map<std::string, std::pair<std::string, int>, std::less<>> packedIn;   // a file a pack is made of: the pack, and the channel it went into
        std::deque<cook::Recipe> toCook;                        // asked to be cooked, and not yet begun
        std::vector<kor::Task<void>> cookingTasks;              // the one cooking them, while it is
        Runtime::CookProgress cooking;

        /** Where pictures are cooked to: the project's `.koral/cache/textures`, beside its `assets`. */
        [[nodiscard]] std::filesystem::path TextureCache() const { return Assets().empty() ? std::filesystem::path() : kasset::CacheDirectoryOf(Assets().parent_path()) / "textures"; }
        /** Where a picture file of an asset id is, worked out by a copy of the directory: for a thread off the frame. */
        [[nodiscard]] cook::FileOf FileOf() const
        {
            return [directory = Assets()](const std::string_view id) { return FileNamed(id.starts_with('@') ? id.substr(1) : id, directory); };
        }
        /** What picture @p id is cooked of, and how, as the graph says: a pack's channels, a file's import, or as it is read. */
        [[nodiscard]] cook::Recipe RecipeOf(const std::string_view id) const
        {
            cook::Recipe recipe { std::string(id) };
            if (const auto pack = texturePacks.find(id); pack != texturePacks.end()) {
                recipe.format = pack->second.format;
                recipe.quality = pack->second.quality;
                recipe.pack = pack->second;
            } else if (const auto import = textureImports.find(id); import != textureImports.end() && TextureCompressible(id)) {
                recipe.format = import->second.format;
                recipe.quality = import->second.quality;
            }
            return recipe;
        }
        /** Whether picture @p id has a file cooked of it as it is now, with the settings as they are now. */
        [[nodiscard]] bool Cooked(const std::string_view id) const
        {
            const cook::Recipe recipe = RecipeOf(id);
            if (recipe.format == TextureFormat::eAsRead) return false;
            const auto cooked = cook::CookedPath(TextureCache(), recipe, FileOf());
            std::error_code ec;
            return cooked && std::filesystem::exists(*cooked, ec);
        }

        /** A view of picture @p image whose channels are read as @p mapping says: it holds the picture, which stays as long. */
        static Held ViewOf(const Held& image, const kor::ImageView::ComponentMapping& mapping)
        {
            if (!image || image->kind != "Image" || !image->handle) return image;
            auto view = kor::ImageView::Builder(kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(image->handle))).SetComponentMapping(mapping).Build();
            if (!view.Valid()) return image;
            Held held = MakeResource(std::move(view));
            held->source = image;
            return held;
        }
        /** A picture kept as @p format, as it is to be read: BC4's one channel in red, green and blue; BC5's blue — a z it has not — 1. */
        static Held Spread(Held image, const TextureFormat format)
        {
            using S = kor::ImageView::Swizzle;
            if (format == TextureFormat::eBC4) return ViewOf(image, kor::ImageView::ComponentMapping { S::eR, S::eR, S::eR, S::eOne });
            if (format == TextureFormat::eBC5) return ViewOf(image, kor::ImageView::ComponentMapping { S::eR, S::eG, S::eOne, S::eOne });
            return image;
        }
        /** A file a pack is made of, as whoever holds it reads it: the pack's channel it went into, in red, green and blue. */
        static Held Channel(const Held& pack, const int channel)
        {
            using S = kor::ImageView::Swizzle;
            // The pack may itself be a view (a BC4 or BC5 one): its picture's channels are what is spread.
            const Held& image = pack && pack->kind == "ImageView" && pack->source ? pack->source : pack;
            const S from = channel == 1 ? S::eG : channel == 2 ? S::eB : channel == 3 ? S::eA : S::eR;
            return ViewOf(image, kor::ImageView::ComponentMapping { from, from, from, S::eOne });
        }

        /** Whoever holds @p from — a stand-in, or picture @p id as it was read before — holds @p to from here on. */
        void Lead(const std::string& id, const Held& from, Held to)
        {
            to->node = id;
            if (from && from != to) { from->replacedBy = to; ++replacements; arrivedPictures.push_back(from); }
            assets[id] = std::move(to);
        }
        /** Picture @p id is here: what held @p standIn holds it, and each file it is a pack of reads its channel of it. */
        void PictureArrived(const std::string& id, const Held& standIn, Held real)
        {
            Lead(id, standIn, real);
            for (const auto& [member, in] : packedIn)
                if (in.first == id)
                    if (const auto held = assets.find(member); held != assets.end() && held->second) Lead(member, held->second, Channel(real, in.second));
        }

        /**
         * Starts reading picture @p id into the place of @p standIn: of a file a pack is made of, nothing — it is the
         * pack's channel, once the pack is there; the file cooked of it, when there is one of it as it is; a pack's
         * files, put together; or its file.
         */
        void StartPicture(const std::string& id, const Held& standIn)
        {
            if (const auto member = packedIn.find(id); member != packedIn.end()) {
                const Held pack = Asset(member->second.first);
                if (pack && !pack->pending) Lead(id, standIn, Channel(pack, member->second.second));
                return;     // the pack, when it arrives, leads it on
            }
            // Watched, whether it is read as it is or from what was cooked of it: written again, it is read again.
            if (!IsTexturePack(id)) Remember(AssetFile(id.substr(1)));
            const cook::Recipe recipe = RecipeOf(id);
            if (recipe.format != TextureFormat::eAsRead)
                if (const auto cooked = cook::CookedPath(TextureCache(), recipe, FileOf()); cooked && std::filesystem::exists(*cooked)) {
                    reading.push_back(ReadPicture(this, alive, id, *cooked, standIn, recipe.format));
                    return;
                }
            if (recipe.pack) { reading.push_back(ReadPack(this, alive, recipe, FileOf(), standIn)); return; }
            reading.push_back(ReadPicture(this, alive, id, AssetFile(id.substr(1)), standIn));
        }

        /** A pack not cooked yet: its files read and put together off the frame, made a picture of four bytes a texel, with its mips. */
        static kor::Task<void> ReadPack(Impl* self, std::shared_ptr<std::atomic<bool>> alive, cook::Recipe recipe, cook::FileOf fileOf, Held standIn)
        {
            co_await kor::Context::SwitchToBackgroundThread();
            auto pixels = cook::Pixels(recipe, fileOf);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            if (!pixels) { self->unread[recipe.id] = std::format("'{}': {}", recipe.id, pixels.error()); co_return; }
            kor::u32 levels = 1;
            for (kor::u32 size = std::max(pixels->extent.x, pixels->extent.y); size > 1; size /= 2) ++levels;
            auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(pixels->extent.x, pixels->extent.y))
                .SetMipLevels(levels).SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled))
                .SetData(pixels->pixels.data(), pixels->pixels.size()).Build();
            if (!image.Valid()) { self->unread[recipe.id] = std::format("'{}' could not be made a picture", recipe.id); co_return; }
            self->unread.erase(recipe.id);
            self->PictureArrived(recipe.id, standIn, MakeResource(std::move(image)));
        }

        /**
         * How picture files are kept, as the graph says: each picture whose settings changed — its import, its pack, or
         * the pack it is in — is read again, as it is now to be.
         */
        void SetTextureImports(const std::map<std::string, TextureImport>& imports, const std::map<std::string, TexturePack>& packs)
        {
            if (source) return;     // the source reads the files, as its own graph says
            std::set<std::string> changed;
            for (const auto& [id, import] : imports) if (const auto it = textureImports.find(id); it == textureImports.end() || it->second != import) changed.insert(id);
            for (const auto& id : textureImports | std::views::keys) if (!imports.contains(id)) changed.insert(id);
            const auto members = [&](const TexturePack& pack) { for (const auto& channel : pack.channels) if (!channel.from.empty()) changed.insert(channel.from); };
            for (const auto& [id, pack] : packs)
                if (const auto it = texturePacks.find(id); it == texturePacks.end() || it->second != pack) { changed.insert(id); members(pack); if (it != texturePacks.end()) members(it->second); }
            for (const auto& [id, pack] : texturePacks) if (!packs.contains(id)) { changed.insert(id); members(pack); }
            if (changed.empty()) return;
            textureImports = std::map<std::string, TextureImport, std::less<>>(imports.begin(), imports.end());
            texturePacks = std::map<std::string, TexturePack, std::less<>>(packs.begin(), packs.end());
            packedIn.clear();
            for (const auto& [id, pack] : texturePacks)
                for (int c = 0; c < 4; ++c)
                    if (const auto& from = pack.channels[static_cast<std::size_t>(c)].from; !from.empty()) packedIn.try_emplace(from, id, c);
            // Packs first: a file of one is its channel, and the pack read again leads it on.
            std::vector<std::string> order(changed.begin(), changed.end());
            std::ranges::stable_partition(order, [&](const std::string& id) { return texturePacks.contains(id); });
            for (const auto& id : order)
                if (const auto it = assets.find(id); it != assets.end() && it->second) StartPicture(id, it->second);
        }

        /** Cooks what is asked, one picture after another, off the frame; each read again from what was cooked of it. */
        static kor::Task<void> CookAll(Impl* self, std::shared_ptr<std::atomic<bool>> alive)
        {
            while (*alive && !self->toCook.empty()) {
                cook::Recipe recipe = std::move(self->toCook.front());
                self->toCook.pop_front();
                self->cooking.current = recipe.id;
                const std::filesystem::path cache = self->TextureCache();
                const cook::FileOf fileOf = self->FileOf();
                co_await kor::Context::SwitchToBackgroundThread();
                auto cooked = cook::Cook(cache, recipe, fileOf);
                co_await kor::Context::SwitchToMainThread();
                if (!*alive) co_return;
                ++self->cooking.done;
                if (!cooked) { self->cooking.failed.push_back(cooked.error()); continue; }
                // Read again, from what was cooked — while it is still to be kept as it was cooked.
                if (const auto it = self->assets.find(recipe.id); it != self->assets.end() && it->second && self->RecipeOf(recipe.id) == recipe)
                    self->StartPicture(recipe.id, it->second);
            }
            self->cooking.current.clear();
        }
        void Cook(const std::vector<std::string>& ids)
        {
            if (source) { source->Cook(ids); return; }
            if (cooking.done == cooking.total) cooking = {};
            for (const auto& id : ids) {
                cook::Recipe recipe = RecipeOf(id);
                if (recipe.format == TextureFormat::eAsRead || std::ranges::find(toCook, recipe) != toCook.end()) continue;
                toCook.push_back(std::move(recipe));
                ++cooking.total;
            }
            std::erase_if(cookingTasks, [](const kor::Task<void>& task) { return task.Done(); });
            if (cookingTasks.empty() && !toCook.empty()) cookingTasks.push_back(CookAll(this, alive));
        }

        /** Starts reading a model file, unless it is read or being read. */
        // ---- files written again while they are had ------------------------------------------------------------

        /// Each file read — a picture, a model, and what a model names beside it — by its path: when it had last been
        /// written, as it was read, and the model it is of (empty for a picture).
        struct Stamp { std::filesystem::file_time_type written; std::string model; };
        std::map<std::string, Stamp, std::less<>> stamps;
        std::chrono::steady_clock::time_point lookedAt {};

        static std::optional<std::filesystem::file_time_type> WrittenAt(const std::filesystem::path& file)
        {
            std::error_code failed;
            const auto written = std::filesystem::last_write_time(file, failed);
            if (failed) return std::nullopt;
            return written;
        }
        /** Remembers when @p file was written, as it is read now: of model @p model, or a picture. */
        void Remember(const std::filesystem::path& file, const std::string& model = {})
        {
            if (const auto written = WrittenAt(file)) stamps[file.lexically_normal().generic_string()] = { *written, model };
        }

        /**
         * A second at most: each file read that was written since — by another program, an export from Blender — is read
         * again, and what held what was of it holds what is of it now: a model's meshes, a picture.
         */
        void Watch()
        {
            if (source || stamps.empty()) return;
            const auto now = std::chrono::steady_clock::now();
            if (now - lookedAt < std::chrono::seconds(1)) return;
            lookedAt = now;
            std::set<std::string> models_, pictures;
            for (auto& [path, stamp] : stamps) {
                const auto written = WrittenAt(path);
                if (!written || *written == stamp.written) continue;
                stamp.written = *written;
                if (stamp.model.empty()) pictures.insert(path);
                else models_.insert(stamp.model);
            }
            for (const auto& key : models_) {
                if (readingModels.contains(key)) { rereadModels.insert(key); continue; }
                if (models.erase(key)) WantModel(key);
            }
            if (pictures.empty()) return;
            std::vector<std::pair<std::string, Held>> again;
            for (const auto& [id, held] : assets)
                if (held && id.starts_with('@') && !id.contains('#') && pictures.contains(AssetFile(id.substr(1)).lexically_normal().generic_string())) again.emplace_back(id, held);
            for (const auto& [id, held] : again) StartPicture(id, held);
        }

        void WantModel(const std::filesystem::path& file)
        {
            if (source) { source->WantModel(file); return; }
            const std::string key = file.lexically_normal().generic_string();
            if (models.contains(key) || !readingModels.insert(key).second) return;
            // Watched, with what its meshes and materials are read from beside it.
            Remember(file, key);
            for (const auto& beside : kasset::NamedBeside(file))
                if (kasset::KindOfFile(beside) != kasset::Kind::eTexture) Remember(file.parent_path() / beside, key);
            const auto import = meshImports.find(AssetId(file, -1).substr(1));
            reading.push_back(ReadModelFile(this, alive, key, file, assetDirectory, import == meshImports.end() ? MeshImport {} : import->second));
        }

        /**
         * What the default meshes are to have of their surface's directions, as the graph says: changed, those made are
         * made again, and what held them holds the new; a Primitive's is built again.
         */
        void SetDefaultMeshTangents(const Tangents wanted)
        {
            if (source || wanted == defaultMeshTangents) return;
            defaultMeshTangents = wanted;
            for (auto& [name, kept] : defaultMeshes) {
                const auto made = MeshOf(ToVertexData(*ShapeData(name), wanted));
                if (!made) continue;
                (*made)->node = kept->node;
                { kept->replacedBy = *made; ++replacements; }
                kept = *made;
            }
            for (auto& node : built | std::views::values)
                if (node.outputs.contains("layout") && node.outputs.contains("out")) { node.signature.clear(); stale = true; }
        }

        /** How model files are to be read, as the graph says: a file read already whose settings changed is read again. */
        void SetMeshImports(const std::map<std::string, MeshImport>& wanted)
        {
            if (source) return;     // the source reads the files, as its own graph says
            std::set<std::string> changed;
            for (const auto& [name, import] : wanted) if (const auto it = meshImports.find(name); it == meshImports.end() || it->second != import) changed.insert(name);
            for (const auto& name : meshImports | std::views::keys) if (!wanted.contains(name)) changed.insert(name);
            meshImports = std::map<std::string, MeshImport, std::less<>>(wanted.begin(), wanted.end());
            for (const auto& name : changed) {
                const std::filesystem::path file = AssetFile(name);
                const std::string key = file.lexically_normal().generic_string();
                if (readingModels.contains(key)) { rereadModels.insert(key); continue; }
                if (models.erase(key) == 0) continue;     // not read yet: it is read as it is now asked to be, when it is
                WantModel(file);
            }
        }

        /** A picture of four bytes a texel, its smaller levels made of it: what a generator or a script makes. */
        static std::expected<Held, std::string> PictureOf(const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba)
        {
            if (width == 0 || height == 0) return std::unexpected("a picture has at least one texel");
            if (rgba.size() != static_cast<std::size_t>(width) * height * 4) return std::unexpected(std::format("{} × {} texels are {} bytes, and {} were given", width, height, static_cast<std::size_t>(width) * height * 4, rgba.size()));
            kor::u32 levels = 1;
            for (kor::u32 size = std::max(width, height); size > 1; size /= 2) ++levels;
            auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(width, height))
                .SetMipLevels(levels).SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled))
                .SetData(reinterpret_cast<const unsigned char*>(rgba.data()), rgba.size()).Build();
            if (!image.Valid()) return std::unexpected("the picture could not be made");
            return MakeResource(std::move(image));
        }
        /** A generator's picture as the cache keeps it: its width and height, then its texels. */
        static std::vector<std::byte> PictureBytes(const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba)
        {
            std::vector<std::byte> bytes(8 + rgba.size());
            std::memcpy(bytes.data(), &width, 4);
            std::memcpy(bytes.data() + 4, &height, 4);
            std::memcpy(bytes.data() + 8, rgba.data(), rgba.size());
            return bytes;
        }

        /** Whether @p id is a mesh a generator made: `$script/gen/<script>/<what it was made of>`. */
        static bool Generated(const std::string_view id) { return id.starts_with("$script/gen/"); }
        /** What a generator's mesh is kept by in the cache: its id, which is what it was made of. */
        static kasset::Key GeneratedKey(const std::string_view id) { return kasset::Key("generated@1").Add(id); }

        /** What is in a file, by the name it is found by: `@wall.png`, a picture; `@model.gltf#3`, a mesh. A stand-in, until it is read. */
        Held Asset(const std::string_view id)
        {
            if (source) return source->Asset(id);
            if (const auto it = assets.find(id); it != assets.end()) return it->second;
            // A generator's that nothing has made this run: as it was made before, from the cache.
            if (Generated(id)) {
                const auto cache = CacheBeside(Assets());
                const auto bytes = cache ? cache->Read("meshes", GeneratedKey(id), ".kmesh") : std::nullopt;
                if (!bytes) {
                    // Or a picture a generator made.
                    const auto picture = cache ? cache->Read("images", GeneratedKey(id), ".kimage") : std::nullopt;
                    if (!picture || picture->size() < 8) return nullptr;
                    std::uint32_t width = 0, height = 0;
                    std::memcpy(&width, picture->data(), 4);
                    std::memcpy(&height, picture->data() + 4, 4);
                    auto made = PictureOf(width, height, std::span(*picture).subspan(8));
                    if (!made) return nullptr;
                    (*made)->node = std::string(id);
                    return assets[std::string(id)] = std::move(*made);
                }
                auto data = kasset::DeserializeMesh(*bytes);
                if (!data) return nullptr;
                auto made = MeshOf(*data);
                if (!made) return nullptr;
                (*made)->node = std::string(id);
                (*made)->kind = "Mesh";
                return assets[std::string(id)] = std::move(*made);
            }
            if (!id.starts_with('@')) return nullptr;
            const std::string_view named = id.substr(1);
            Held standIn;
            if (const auto mesh = named.rfind('#'); mesh != std::string_view::npos) {
                standIn = std::make_shared<Resource>();
                standIn->kind = "Mesh";
                standIn->pending = true;
                WantModel(AssetFile(named.substr(0, mesh)));
            } else {
                const std::array<kor::U8Vec4, 1> white { kor::U8Vec4(255, 255, 255, 255) };
                auto texel = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(1, 1))
                    .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                    .SetData(std::span<const kor::U8Vec4>(white)).Build();
                if (!texel.Valid()) return nullptr;
                standIn = MakeResource(std::move(texel));
                standIn->pending = true;
            }
            standIn->node = std::string(id);
            assets.emplace(std::string(id), standIn);
            if (standIn->kind != "Mesh") StartPicture(std::string(id), standIn);
            return assets.find(id)->second;
        }

        /** Each frame: what was read since is made the device's, and what holds a stand-in is led on. */
        void Arrive()
        {
            std::erase_if(reading, [](const kor::Task<void>& task) { return task.Done(); });
            // A model's meshes are one mesh on the device, each a run of its indices: a pass that draws a hundred of
            // them binds one thing, where a mesh each would be bound a hundred times. Every model read since arrives
            // in the same frame: what packs their meshes (a MeshPack, its Meshlets, a RayScene) is built again once for
            // them all, where a model a frame built it again for each.
            // While other models are still being read, those read wait for them a moment — a model read alone at
            // first, the rest a moment after, was every pack built twice over.
            bool hold = false;
            if (!arrivedMeshes.empty() && !readingModels.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (!modelsWaiting) modelsWaiting = now;
                hold = now - *modelsWaiting < std::chrono::milliseconds(400);
            }
            if (!hold) modelsWaiting.reset();
            while (!hold && !arrivedMeshes.empty()) {
                Arrived arrived = std::move(arrivedMeshes.front());
                arrivedMeshes.pop_front();
                for (auto& whole : arrived.wholes) {
                    const auto made = MeshOf(whole.all);
                    if (!made) {
                        for (const auto& part : whole.parts) unread[part->node] = std::format("'{}': {}", part->node, made.error());
                        continue;
                    }
                    for (auto& part : whole.parts) {
                        part->source = *made;
                        // What held the stand-in, or this mesh as the file was read before, holds this one from here on.
                        Held& kept = assets[part->node];
                        if (kept && kept != part) { kept->replacedBy = part; ++replacements; }
                        kept = std::move(part);
                    }
                }
            }
            // A node that made its output of a stand-in — an ImageFile — is built again, with the picture.
            if (arrivedPictures.empty()) return;
            for (auto& node : built | std::views::values)
                for (const auto& output : node.outputs | std::views::values)
                    if (std::ranges::find(arrivedPictures, output.resource) != arrivedPictures.end()) { node.signature.clear(); stale = true; }
            arrivedPictures.clear();
        }

        // ---- what is read from the world, each frame --------------------------------------------------------

        /** Whether a node is a ForEach: every entity with a component, an element at a time. An Entities node is one, by its old name. */
        static bool IsEach(const std::string_view type) { return type == "ForEach" || type == "Entities" || type == "Filter" || type == "Entity"; }
        /** Whether a node is a buffer of elements laid out field by field: a shader's struct, or one set out on the node. */
        static bool IsMapped(const std::string_view type) { return type == "ShaderBuffer" || type == "BufferBuilder"; }

        /** Whether a node's outputs are read from the world every frame, rather than worked out once. */
        static bool ReadsTheWorld(const std::string_view type)
        {
            return type == "Selected" || type == "Keep" || type == "LastFrame" || type == "SceneBounds" || type == "WriteComponent" || type == "DebugShapes" || type == "Readback" || type == "FixedStep" || type == "Time" || IsEach(type);
        }

        static Value Matrix(const kor::Mat4& m)
        {
            Value::List out;
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row) out.emplace_back(m[column][row]);
            return Value(std::move(out));
        }
        static Value Three(const kor::Vec3 v) { return Value(Value::List { v.x, v.y, v.z }); }
        /** A colour as picked — as the display shows it — in the linear terms light adds up in. */

        /** The entity a node is of: the one so named, or — with no name — the first that has the component. */
        [[nodiscard]] kecs::Entity EntityFor(const Inputs& inputs, const kecs::ComponentId component) const
        {
            if (!world) return kecs::NullEntity;
            if (const auto it = inputs.find("entity"); it != inputs.end() && it->second.value.String() && !it->second.value.String()->empty())
                return world->FindEntity(*it->second.value.String());
            if (component == kecs::NoComponent) return kecs::NullEntity;
            for (const kecs::Entity entity : world->All())
                if (world->Has(entity, component) && world->IsEnabled(entity, component)) return entity;
            return kecs::NullEntity;
        }

        /** A field of a component as a value: a number, or a list of them. Null for one that is neither. */
        /** Whether a component's field is a shared reference to a resource — a mesh, a picture — and no value. */
        static bool IsResourceField(const kecs::FieldDescription& field)
        {
            static const std::set<std::string, std::less<>> kinds { "resource", "Image", "Buffer", "Sampler", "BufferView", "AccelerationStructure", "Mesh" };
            return field.size == sizeof(Held) && kinds.contains(field.type);
        }

        static Value FieldValue(const kecs::FieldDescription& field, const std::byte* at)
        {
            const auto numbers = [&](const std::size_t count, const char kind) {
                Value::List out;
                for (std::size_t i = 0; i < count; ++i) {
                    if (kind == 'f') { float v; std::memcpy(&v, at + i * 4, 4); out.emplace_back(v); }
                    else if (kind == 'i') { std::int32_t v; std::memcpy(&v, at + i * 4, 4); out.emplace_back(v); }
                    else { std::uint32_t v; std::memcpy(&v, at + i * 4, 4); out.emplace_back(v); }
                }
                return count == 1 ? out.front() : Value(std::move(out));
            };
            const std::string& type = field.type;
            if (type == "f32") return numbers(1, 'f');
            if (type == "i32") return numbers(1, 'i');
            if (type == "u32" || type == "bool32") return numbers(1, 'u');
            if (type == "f64") { double v; std::memcpy(&v, at, 8); return Value(v); }
            if (type == "bool") return Value(static_cast<bool>(*at != std::byte { 0 }));
            if (type == "mat4") return numbers(16, 'f');
            if (type == "quat") return numbers(4, 'f');
            // Another entity, as a ForEach's entityId has it: its index and one; 0 for none.
            if (type == "entity") { kecs::Entity e; std::memcpy(&e, at, sizeof e); return Value(e.IsNull() ? 0.0 : static_cast<double>(e.index) + 1.0); }
            for (const auto& [prefix, kind] : { std::pair<std::string_view, char> { "vec", 'f' }, { "ivec", 'i' }, { "uvec", 'u' } })
                if (type.size() == prefix.size() + 1 && type.starts_with(prefix) && type.back() >= '2' && type.back() <= '4') return numbers(static_cast<std::size_t>(type.back() - '0'), kind);
            return {};
        }

        /** What a Readback copied frames enough ago that the GPU is done with it, as a value. */
        static void ReadBack(Built& out)
        {
            const auto state = out.readback;
            if (!state) return;
            const std::uint64_t copies = state->copies.load();
            if (copies < ReadState::Ring) { out.outputs["ready"].value = false; return; }
            // The oldest of them: the one the next copy goes into.
            const auto bytes = (*state->owners[copies % ReadState::Ring])->Read<std::byte>();
            if (bytes.size() < state->bytes) return;
            Value::List numbers;
            if (state->texel) {
                const TexelLayout& texel = *state->texel;
                const std::size_t size = static_cast<std::size_t>(texel.bits / 8);
                for (int channel = 0; channel < texel.channels; ++channel) {
                    const std::byte* at = bytes.data() + static_cast<std::size_t>(channel) * size;
                    std::uint32_t raw = 0;
                    std::memcpy(&raw, at, size);
                    double n = 0;
                    if (texel.kind == 'f') {
                        if (size == 4) { float f; std::memcpy(&f, at, 4); n = f; }
                        else {
                            // A half: its sign, five bits of exponent and ten of fraction.
                            const std::uint32_t sign = raw >> 15, exponent = (raw >> 10) & 31, fraction = raw & 1023;
                            n = exponent == 0 ? std::ldexp(fraction, -24) : exponent == 31 ? (fraction ? std::nan("") : INFINITY) : std::ldexp(fraction + 1024, static_cast<int>(exponent) - 25);
                            if (sign) n = -n;
                        }
                    } else if (texel.kind == 'u' || texel.kind == 's') {
                        const double most = std::ldexp(1.0, texel.bits) - 1.0;
                        if (texel.kind == 'u') n = raw / most;
                        else {
                            const std::int64_t signedRaw = raw >= (1u << (texel.bits - 1)) ? static_cast<std::int64_t>(raw) - (std::int64_t { 1 } << texel.bits) : raw;
                            n = std::max(static_cast<double>(signedRaw) / (most / 2.0 - 0.5), -1.0);
                        }
                    } else if (texel.kind == 'I') {
                        n = raw >= (1u << (texel.bits - 1)) && texel.bits < 32 ? static_cast<double>(static_cast<std::int64_t>(raw) - (std::int64_t { 1 } << texel.bits))
                                                                                 : static_cast<double>(static_cast<std::int32_t>(raw));
                    } else {
                        n = raw;
                    }
                    numbers.emplace_back(n);
                }
            } else {
                for (std::size_t i = 0; i < state->count; ++i) {
                    const std::byte* at = bytes.data() + i * 4;
                    if (state->kind == 'f') { float f; std::memcpy(&f, at, 4); numbers.emplace_back(f); }
                    else if (state->kind == 'i') { std::int32_t v; std::memcpy(&v, at, 4); numbers.emplace_back(v); }
                    else { std::uint32_t v; std::memcpy(&v, at, 4); numbers.emplace_back(v); }
                }
            }
            out.outputs["out"].value = numbers.size() == 1 ? numbers.front() : Value(std::move(numbers));
            out.outputs["ready"].value = true;
        }

        /** The box round the entities wired to a SceneBounds: each one's mesh's box where it is, or where it is when it has none. */
        Expected<void> BuildBounds(const Node& node, Built& out)
        {
            const auto wired = node.inputs.find("entities");
            const Node* origin = wired != node.inputs.end() && wired->second.link ? Origin(wired->second.link->node) : nullptr;
            const auto source = origin ? built.find(origin->id) : built.end();
            if (source == built.end() || !source->second.listed) return Fail("'entities' is what it is the box round: wire an Entities node's, or a Filter's");
            const EntityList& list = *source->second.listed;
            kor::Vec3 least(std::numeric_limits<float>::max()), most(-std::numeric_limits<float>::max());
            const kecs::ComponentId meshes = world ? world->Find("Mesh") : kecs::NoComponent;
            const kecs::FieldDescription* field = meshes != kecs::NoComponent ? world->Describe(meshes).FindField("mesh") : nullptr;
            std::size_t count = 0;
            for (const kecs::Entity entity : list.entities) {
                if (!world) break;
                ++count;
                const kor::Mat4 placed = ToKor(world->WorldMatrix(entity));
                const Resource* mesh = nullptr;
                if (field && IsResourceField(*field))
                    if (const auto* bytes = static_cast<const std::byte*>(world->Get(entity, meshes))) {
                        const Held* held = reinterpret_cast<const Held*>(bytes + field->offset);
                        while (*held && (*held)->replacedBy) held = &(*held)->replacedBy;
                        mesh = held->get();
                    }
                if (!mesh || !mesh->bounded) {
                    const kor::Vec3 at(placed[3][0], placed[3][1], placed[3][2]);
                    least = kor::Min(least, at);
                    most = kor::Max(most, at);
                    continue;
                }
                for (int corner = 0; corner < 8; ++corner) {
                    const kor::Vec4 local((corner & 1) ? mesh->bounds[3] : mesh->bounds[0], (corner & 2) ? mesh->bounds[4] : mesh->bounds[1],
                                          (corner & 4) ? mesh->bounds[5] : mesh->bounds[2], 1.f);
                    const kor::Vec4 at = placed * local;
                    least = kor::Min(least, kor::Vec3(at.x, at.y, at.z));
                    most = kor::Max(most, kor::Vec3(at.x, at.y, at.z));
                }
            }
            if (count == 0) least = most = kor::Vec3(0.f);
            const kor::Vec3 middle = (least + most) * 0.5f;
            out.outputs["min"].value = Three(least);
            out.outputs["max"].value = Three(most);
            out.outputs["center"].value = Three(middle);
            out.outputs["radius"].value = static_cast<double>(kor::Length(most - least) * 0.5f);
            out.outputs["count"].value = static_cast<double>(count);
            return {};
        }

        /** A WriteComponent's value written into its entity's field, when the field does not hold it already. */
        Expected<void> WriteComponent(const Inputs& inputs, Built& out)
        {
            const auto text = [&](const char* name) -> const std::string* {
                const auto it = inputs.find(name);
                return it == inputs.end() ? nullptr : it->second.value.String();
            };
            const std::string* component = text("component");
            const std::string* name = text("field");
            if (!component || component->empty()) return Fail("'component' is the component written");
            if (!name || name->empty()) return Fail("'field' is the field of it written, by name");
            out.outputs["written"].value = false;
            if (!world) return {};
            const kecs::ComponentId id = world->Find(*component);
            if (id == kecs::NoComponent) return Fail(std::format("there is no component '{}'", *component));
            const kecs::FieldDescription* field = world->Describe(id).FindField(*name);
            if (!field) return Fail(std::format("'{}' has no field '{}'", *component, *name));
            if (IsResourceField(*field) || field->type == "string" || field->type == "entity")
                return Fail(std::format("'{}.{}' is a {}: a number, a vector, a matrix or true or false is written", *component, *name, field->type));
            const auto given = inputs.find("value");
            if (given == inputs.end() || given->second.value.IsNull()) return {};
            const kecs::Entity entity = EntityFor(inputs, id);
            const auto* bytes = entity.IsNull() ? nullptr : static_cast<const std::byte*>(world->Get(entity, id));
            if (!bytes) return {};
            out.outputs["written"].value = true;
            // What it would hold, compared with what it holds: written only when they differ, so nothing else is told of a change that is none.
            std::vector<double> numbers;
            if (const bool* flag = given->second.value.Bool()) numbers = { *flag ? 1.0 : 0.0 };
            else if (auto read = NumbersOf(given->second.value, "value")) numbers = std::move(*read);
            else return Fail(read.error());
            std::vector<std::byte> wanted(bytes + field->offset, bytes + field->offset + field->size);
            const std::string& type = field->type;
            const auto put = [&](const std::size_t i, const char kind) {
                if (i >= numbers.size() || (i + 1) * 4 > wanted.size()) return;
                if (kind == 'f') { const auto v = static_cast<float>(numbers[i]); std::memcpy(wanted.data() + i * 4, &v, 4); }
                else if (kind == 'i') { const auto v = static_cast<std::int32_t>(std::llround(numbers[i])); std::memcpy(wanted.data() + i * 4, &v, 4); }
                else { const auto v = static_cast<std::uint32_t>(std::llround(std::max(numbers[i], 0.0))); std::memcpy(wanted.data() + i * 4, &v, 4); }
            };
            if (type == "f64") { if (!numbers.empty()) std::memcpy(wanted.data(), &numbers[0], 8); }
            else if (type == "bool") { if (!numbers.empty()) wanted[0] = std::byte { numbers[0] != 0.0 }; }
            else {
                const char kind = type.starts_with("ivec") || type == "i32" ? 'i' : type.starts_with("uvec") || type == "u32" || type == "bool32" ? 'u' : 'f';
                for (std::size_t i = 0; i < field->size / 4; ++i) put(i, kind);
            }
            if (std::memcmp(wanted.data(), bytes + field->offset, wanted.size()) == 0) return {};
            if (auto* writable = static_cast<std::byte*>(world->Mutable(entity, id))) std::memcpy(writable + field->offset, wanted.data(), wanted.size());
            return {};
        }

        /** A DebugShapes' shapes, drawn this frame into the scene's debug shapes: as many as its values have. */
        Expected<void> DrawShapes(const Inputs& inputs)
        {
            const auto numbers = [&](const char* name) -> std::vector<double> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || it->second.value.IsNull()) return {};
                if (const double* n = it->second.value.Number()) return { *n };
                auto read = NumbersOf(it->second.value, name);
                return read ? *read : std::vector<double> {};
            };
            const auto kindOf = inputs.find("kind");
            const std::string kind = kindOf != inputs.end() && kindOf->second.value.String() ? *kindOf->second.value.String() : "eFrustum";
            kor::DebugStyle style;
            if (const auto color = numbers("color"); color.size() >= 3)
                style.color = kor::Vec4(static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]), color.size() > 3 ? static_cast<float>(color[3]) : 1.f);
            const auto three = [](const std::vector<double>& v, const std::size_t i) {
                return kor::Vec3(static_cast<float>(v[i * 3]), static_cast<float>(v[i * 3 + 1]), static_cast<float>(v[i * 3 + 2]));
            };
            const auto matrix = [](const std::vector<double>& v, const std::size_t i) {
                kor::Mat4 m;
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) m[column][row] = static_cast<float>(v[i * 16 + static_cast<std::size_t>(column * 4 + row)]);
                return m;
            };
            if (!debug) return {};
            if (kind == "eLine" || kind == "eArrow") {
                const auto from = numbers("from"), to = numbers("to");
                if (from.size() % 3 != 0 || to.size() != from.size()) return Fail("'from' and 'to' are three numbers for each line, as many of one as of the other");
                for (std::size_t i = 0; i < from.size() / 3; ++i)
                    if (kind == "eLine") debug->Line(three(from, i), three(to, i), style); else debug->Arrow(three(from, i), three(to, i), style);
            } else if (kind == "eBox") {
                const auto least = numbers("min"), most = numbers("max");
                if (least.size() % 3 != 0 || most.size() != least.size()) return Fail("'min' and 'max' are three numbers for each box");
                for (std::size_t i = 0; i < least.size() / 3; ++i) debug->Box(three(least, i), three(most, i), style);
            } else if (kind == "eSphere") {
                const auto centers = numbers("center"), radii = numbers("radius");
                if (centers.size() % 3 != 0 || radii.empty()) return Fail("'center' is three numbers for each sphere, and 'radius' one for all or one for each");
                for (std::size_t i = 0; i < centers.size() / 3; ++i) debug->Sphere(three(centers, i), static_cast<float>(radii[std::min(i, radii.size() - 1)]), style);
            } else if (kind == "eFrustum" || kind == "eAxes" || kind == "eTransformedBox") {
                const auto matrices = numbers("matrix");
                if (matrices.empty() || matrices.size() % 16 != 0) return Fail("'matrix' is sixteen numbers for each");
                const auto size = numbers("size");
                for (std::size_t i = 0; i < matrices.size() / 16; ++i) {
                    if (kind == "eFrustum") debug->Frustum(matrix(matrices, i), style);
                    else if (kind == "eAxes") debug->Axes(matrix(matrices, i), size.empty() ? 1.f : static_cast<float>(size[0]));
                    else debug->Box(matrix(matrices, i), style);
                }
            } else {
                return Fail(std::format("'kind' is eLine, eArrow, eBox, eSphere, eFrustum, eAxes or eTransformedBox, not '{}'", kind));
            }
            return {};
        }

        /**
         * A node that reads the world: its outputs, as the world is now. Called when the graph is set, and again
         * every frame, so that what is wired from one follows the scene.
         */
        Expected<void> BuildLive(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto set = [&](const char* name, Value value) { out.outputs[name].value = std::move(value); };
            const std::string& type = node.type;

            if (type == "Selected") {
                // Which entity is picked, from 1; 0 for none — as whoever runs the graph says (SetSelected).
                const double picked = static_cast<double>(selectedId->load());
                set("out", picked);
                set("entityId", picked);
                set("present", picked > 0.0 ? 1 : 0);
                // A pick asked of this frame: what draws the ids it is read from has to draw them now.
                set("picking", pick->asked ? 1 : 0);
            } else if (type == "Time") {
                set("seconds", timeSeconds);
                set("dt", timeDelta);
                set("frame", static_cast<double>(timeFrame));
            } else if (type == "FixedStep") {
                set("dt", fixedSeconds);
                set("steps", static_cast<double>(fixedSteps));
                set("count", static_cast<double>(fixedCount));
                set("lost", static_cast<double>(fixedLost));
                set("alpha", fixedFraction);
            } else if (type == "LastFrame") {
                const auto given = inputs.find("value");
                const Value now = given == inputs.end() ? Value {} : given->second.value;
                auto [at, fresh] = lastSeen.try_emplace(node.id);
                LastSeen& seen = at->second;
                if (fresh) seen = { timeFrame, now, now, false };
                else if (seen.frame != timeFrame) {
                    // Another frame: what it was given on the last is what it was before, if the last was the frame before.
                    seen.valid = seen.frame + 1 == timeFrame;
                    seen.before = seen.valid ? seen.now : now;
                    seen.frame = timeFrame;
                }
                seen.now = now;
                set("out", seen.before);
                set("valid", seen.valid);
            } else if (type == "SceneBounds") {
                return BuildBounds(node, out);
            } else if (type == "WriteComponent") {
                return WriteComponent(inputs, out);
            } else if (type == "DebugShapes") {
                return DrawShapes(inputs);
            } else if (type == "Readback") {
                ReadBack(out);
            } else if (type == "Keep") {
                set("valid", out.recorded && out.recorded->load() > 0);
            } else if (IsEach(type)) {
                // A ForEach is of a component; an Entities node of every entity; a Filter of those of the node it is wired.
                const bool all = type == "Entities", filter = type == "Filter";
                const auto component = inputs.find("component");
                const std::string* name = component == inputs.end() ? nullptr : component->second.value.String();
                static const std::string none;
                if (all || filter || (type == "Entity" && (!name || name->empty()))) name = &none;
                else if (!name || name->empty()) return Fail("'component' is the name of the component every one of them has");
                const EntityList* from = nullptr;
                if (filter) {
                    const auto wired = node.inputs.find("entities");
                    // Through the References between: what another pass made is brought in by one.
                    const Node* origin = wired != node.inputs.end() && wired->second.link ? Origin(wired->second.link->node) : nullptr;
                    const auto upstream = origin ? built.find(origin->id) : built.end();
                    if (upstream == built.end() || !upstream->second.listed) return Fail("'entities' is what it keeps some of: wire an Entities node's, or another Filter's");
                    from = upstream->second.listed.get();
                }
                std::size_t repeat = 1;
                if (const auto it = inputs.find("repeat"); it != inputs.end() && !it->second.value.IsNull()) {
                    const double* given = it->second.value.Number();
                    if (!given || *given < 1 || *given > 256 || *given != std::floor(*given)) return Fail("'repeat' is how many elements each entity is: a whole number from 1 to 256");
                    repeat = static_cast<std::size_t>(*given);
                }
                // A ForEach is all of them; an Entity one of them: the first, or the one named.
                for (const char* gone : { "select", "which" })
                    if (inputs.contains(gone))
                        return Fail(std::format("'{}' is no ForEach's now: a ForEach is every one of them, and an Entity node is one — the first, or the one it names", gone));
                const bool one = type == "Entity";
                std::string named;
                if (const auto it = inputs.find("entity"); one && it != inputs.end() && !it->second.value.IsNull()) {
                    if (!it->second.value.String()) return Fail("'entity' is the entity's name");
                    named = *it->second.value.String();
                }
                if (one && name->empty() && named.empty()) return Fail("'component' is the component it has, or 'entity' the entity's name: one of them, at least");
                const std::string select = !one ? "eAll" : named.empty() ? "eFirst" : "eNamed";
                const std::size_t which = 0;
                std::vector<kecs::ComponentId> with, without;
                bool nobody = false;
                for (const std::string_view which : { "with", "without" }) {
                    const auto given = inputs.find(std::string(which));
                    if (given == inputs.end() || given->second.value.IsNull()) continue;
                    const auto* names = given->second.value.Items();
                    if (!names || !std::ranges::all_of(*names, [](const Value& v) { return v.String() != nullptr; })) return Fail(std::format("'{}' is a list of component names", which));
                    for (const auto& each : *names) {
                        const kecs::ComponentId id = world ? world->Find(*each.String()) : kecs::NoComponent;
                        if (id != kecs::NoComponent) (which == "with" ? with : without).push_back(id);
                        else if (which == "with") nobody = true;    // nothing has a component there is none of
                    }
                }
                const kecs::ComponentId id = world && !name->empty() ? world->Find(*name) : kecs::NoComponent;
                // A Filter grouped: by one of `with`'s components, each entity next to those whose component is as its own.
                kecs::ComponentId groupBy = kecs::NoComponent;
                if (const auto it = inputs.find("groupBy"); filter && it != inputs.end() && !it->second.value.IsNull()) {
                    const std::string* by = it->second.value.String();
                    if (!by) return Fail("'groupBy' is one of `with`'s components, by name");
                    if (!by->empty()) {
                        groupBy = world ? world->Find(*by) : kecs::NoComponent;
                        if (!nobody && (groupBy == kecs::NoComponent || std::ranges::find(with, groupBy) == with.end()))
                            return Fail(std::format("'groupBy' is one of `with`'s components, and '{}' is not among them", *by));
                    }
                }
                // A Filter of one entity alone, by its id from 1: what an editor's outline draws, of what is picked.
                double only = -1;
                if (const auto it = inputs.find("entityId"); filter && it != inputs.end() && !it->second.value.IsNull()) {
                    const double* given = it->second.value.Number();
                    if (!given || *given < 0 || *given != std::floor(*given)) return Fail("'entityId' is an entity's id from 1 (0 for none)");
                    only = *given;
                }
                // `where`: fields an entity's must hold — its component's as `field.<name>`, another's as `<Component>.<name>`.
                struct Holds { kecs::ComponentId component; kecs::FieldDescription field; Value value; };
                std::vector<Holds> holds;
                std::string whereGiven;
                std::vector<std::uint64_t> whereChanges;
                if (const auto it = inputs.find("where"); it != inputs.end() && !it->second.value.IsNull()) {
                    const auto* members = it->second.value.Named();
                    if (!members) return Fail("'where' is { \"field.<name>\": what it holds }: the fields an entity's must hold");
                    whereGiven = it->second.value.ToJson();
                    for (const auto& [output, value] : *members) {
                        if (!value.Number() && !value.Bool()) return Fail(std::format("'where' has '{}': what it holds is a number, or true or false", output));
                        const bool own = output.starts_with("field.");
                        const auto dot = output.find('.');
                        if (dot == std::string::npos) return Fail(std::format("'where' has '{}': a field is `field.<name>`, or `<Component>.<name>`", output));
                        const kecs::ComponentId of = own ? (world && !name->empty() ? world->Find(*name) : kecs::NoComponent) : world ? world->Find(output.substr(0, dot)) : kecs::NoComponent;
                        if (of == kecs::NoComponent) { nobody = true; continue; }     // nothing has a field of a component there is none of
                        const kecs::FieldDescription* field = world->Describe(of).FindField(output.substr(dot + 1));
                        if (!field) return Fail(std::format("'where' has '{}': {} has no such field", output, world->Describe(of).name));
                        holds.push_back({ of, *field, value });
                        whereChanges.push_back(world->ChangesOf(of));
                    }
                }
                // What it gives: its component's fields, and those of each of `with`'s, as they are now.
                std::string shape;
                std::vector<kecs::ComponentId> shaped;
                if (id != kecs::NoComponent) shaped.push_back(id);
                if (id != kecs::NoComponent || filter) shaped.insert(shaped.end(), with.begin(), with.end());
                for (const kecs::ComponentId each : shaped) {
                    shape += world->Describe(each).name + "{";
                    for (const auto& field : world->Describe(each).fields) shape += std::format("{}:{}@{};", field.name, field.type, field.offset);
                    shape += "}";
                }
                if (!out.listed) {
                    // Built: a number, a vector or a resource each — of the component as `field.<name>`, of another as `<Component>.<name>`.
                    out.listed = std::make_shared<EntityList>();
                    out.listed->component = id;
                    out.listed->shape = shape;
                    const auto gives = [&](const kecs::ComponentId each, const bool own) {
                        const kecs::ComponentDescription& description = world->Describe(each);
                        // What one is with nobody there: a new one, as its type makes it — plain data, as a script's is — or nothing.
                        std::vector<std::byte>& none = out.listed->nones[each];
                        none.assign(description.size, std::byte { 0 });
                        if (description.construct && !description.destroy && !description.IsTag()) description.construct(none.data(), description);
                        for (const auto& field : description.fields) {
                            if (field.offset + field.size > description.size) continue;
                            const std::string output = own ? "field." + field.name : description.name + "." + field.name;
                            const std::string doc = std::format("Each one's {}.{}.", description.name, field.name);
                            if (IsResourceField(field)) {
                                out.gives.push_back({ output, PortKind::eResource, field.type == "resource" ? std::string("Resource") : field.type, false, doc, nullptr });
                                out.listed->fields.push_back({ output, each, field, true });
                                continue;
                            }
                            const Value value = FieldValue(field, none.data() + field.offset);
                            if (value.IsNull()) continue;   // a string: nothing a buffer holds
                            const auto* items = value.Items();
                            out.gives.push_back({ output, PortKind::eValue, items ? (items->size() == 16 ? "matrix" : "vector") : value.Bool() ? "bool" : "number", false, doc, nullptr });
                            out.listed->fields.push_back({ output, each, field, false });
                        }
                    };
                    if (id != kecs::NoComponent) gives(id, true);
                    if (id != kecs::NoComponent || filter)
                        for (const kecs::ComponentId each : with) if (each != id) gives(each, false);
                }
                EntityList& list = *out.listed;
                list.repeat = repeat;
                // Found again only when what they are found from has changed since: see EntityList::Made.
                // A list of what is drawn leaves out what the runtime hides (an editor's view).
                const kecs::ComponentId meshes = world ? world->Find("Mesh") : kecs::NoComponent;
                const bool drawn = meshes != kecs::NoComponent && (id == meshes || std::ranges::contains(with, meshes));
                const EntityList::Made made { world ? world->StructureVersion() : 0, from ? from->version : 0,
                                              world && groupBy != kecs::NoComponent ? world->ChangesOf(groupBy) : 0,
                                              with, without, id, groupBy, select, named, whereGiven, whereChanges, repeat, nobody, only,
                                              drawn ? hiddenVersion : 0 };
                const bool again = !list.made || *list.made != made;
                // The component was made since, or is made of other fields now: what this gives is no longer what it has.
                if (list.component != id || list.shape != shape) {
                    list.entities.clear();
                    list.made.reset();
                    out.signature.clear();
                    stale = true;
                } else if (!again) {
                    // As they were: the entities, their groups, and what was said of the groups.
                } else if (list.entities.clear(); world && (id != kecs::NoComponent || all || filter || select == "eNamed") && !nobody) {
                    const auto has = [&](const kecs::Entity entity, const kecs::ComponentId each) { return world->Has(entity, each) && world->IsEnabled(entity, each); };
                    for (const kecs::Entity entity : from ? from->entities : world->All())
                        if ((!drawn || !hidden.contains(entity)) && (id == kecs::NoComponent || has(entity, id)) && std::ranges::all_of(with, [&](const auto each) { return has(entity, each); })
                            && std::ranges::none_of(without, [&](const auto each) { return has(entity, each); })
                            && std::ranges::all_of(holds, [&](const Holds& hold) {
                                   const auto* bytes = static_cast<const std::byte*>(world->Get(entity, hold.component));
                                   if (!bytes) return false;
                                   const Value now = FieldValue(hold.field, bytes + hold.field.offset);
                                   if (hold.value.Bool()) return now.Bool() ? *now.Bool() == *hold.value.Bool() : now.Number() && (*now.Number() != 0.0) == *hold.value.Bool();
                                   return now.Number() ? std::abs(*now.Number() - *hold.value.Number()) < 1e-6 : now.Bool() && (*now.Bool() ? 1.0 : 0.0) == *hold.value.Number();
                               }))
                            list.entities.push_back(entity);
                    if (select == "eFirst" && list.entities.size() > 1) list.entities.resize(1);
                    if (select == "eIndex") list.entities = which < list.entities.size() ? std::vector { list.entities[which] } : std::vector<kecs::Entity> {};
                    if (select == "eNamed") std::erase_if(list.entities, [&](const kecs::Entity entity) { return world->NameOf(entity) != named; });
                    if (select == "eNamed" && list.entities.size() > 1) list.entities.resize(1);
                    if (only >= 0) std::erase_if(list.entities, [&](const kecs::Entity entity) { return entity.index + 1.0 != only; });
                }
                // Grouped: the entities of each group one after another, each group where its first one was. Two are of one
                // group when their component holds the same: a resource the same one (the same mesh), anything else the same bytes.
                const bool regroup = again && list.component == id && list.shape == shape;
                if (regroup) {
                    list.made = made;
                    ++list.version;
                }
                list.grouped = groupBy != kecs::NoComponent;
                if (regroup) list.groupOf.clear();
                Value::List groups;
                if (regroup && list.grouped && world) {
                    const kecs::ComponentDescription& description = world->Describe(groupBy);
                    std::map<std::string, std::size_t> index;
                    std::vector<std::vector<kecs::Entity>> buckets;
                    for (const kecs::Entity entity : list.entities) {
                        std::string key;
                        if (const auto* bytes = static_cast<const std::byte*>(world->Get(entity, groupBy)))
                            for (const auto& field : description.fields) {
                                if (field.offset + field.size > description.size) continue;
                                if (IsResourceField(field)) {
                                    const Resource* which = reinterpret_cast<const Held*>(bytes + field.offset)->get();
                                    while (which && which->replacedBy) which = which->replacedBy.get();
                                    key.append(reinterpret_cast<const char*>(&which), sizeof which);
                                } else key.append(reinterpret_cast<const char*>(bytes + field.offset), field.size);
                            }
                        const auto [at, fresh] = index.try_emplace(std::move(key), buckets.size());
                        if (fresh) buckets.emplace_back();
                        buckets[at->second].push_back(entity);
                    }
                    list.entities.clear();
                    for (std::size_t group = 0; group < buckets.size(); ++group) {
                        groups.emplace_back(Value::Members { { "first", static_cast<double>(list.entities.size() * repeat) },
                                                             { "count", static_cast<double>(buckets[group].size() * repeat) } });
                        for (const kecs::Entity entity : buckets[group]) {
                            list.entities.push_back(entity);
                            list.groupOf.push_back(static_cast<std::uint32_t>(group));
                        }
                    }
                }
                if (filter && (regroup || !out.outputs.contains("groups"))) {
                    out.outputs["groupCount"].value = static_cast<double>(groups.size());
                    out.outputs["groups"].value = Value(std::move(groups));
                    if (!list.grouped) out.outputs["group"].value = 0;     // not grouped: every element is of the one
                }
                out.outputs["out"].value = static_cast<double>(list.Rows());     // them, for a Filter to keep some of
                SetRow(out, 0);
            }
            return {};
        }

        /**
         * What a ForEach gives, as of one of its elements: the @p index th, which is entity `index / repeat`'s turn
         * `index % repeat`. With none there, a component of nothing at the origin. A buffer its fields are wired to goes
         * through them all, an element each, and asks for only what it reads (@p only).
         */
        void SetRow(Built& out, const std::size_t index, const std::set<std::string, std::less<>>* only = nullptr) const
        {
            const EntityList& list = *out.listed;
            const auto wanted = [&](const std::string_view name) { return !only || only->contains(name); };
            const auto set = [&](const std::string_view name, Value value) {
                if (!wanted(name)) return;
                const auto it = out.outputs.find(name);
                if (it != out.outputs.end()) it->second.value = std::move(value);
                else out.outputs[std::string(name)].value = std::move(value);
            };
            const std::size_t repeat = std::max<std::size_t>(list.repeat, 1), which = index / repeat;
            const bool present = world && which < list.entities.size();
            const kecs::Entity entity = present ? list.entities[which] : kecs::NullEntity;
            for (const auto& read : list.fields) {
                if (!wanted(read.output)) continue;
                const auto* bytes = present ? static_cast<const std::byte*>(world->Get(entity, read.component)) : nullptr;
                if (!bytes) bytes = list.nones.at(read.component).data();
                if (!read.resource) { set(read.output, FieldValue(read.field, bytes + read.field.offset)); continue; }
                Held held = *reinterpret_cast<const Held*>(bytes + read.field.offset);
                while (held && held->replacedBy) held = held->replacedBy;
                out.outputs[read.output].resource = std::move(held);
            }
            set("count", static_cast<double>(list.Rows()));
            set("index", static_cast<double>(index));
            set("turn", static_cast<double>(index % repeat));
            set("entityIndex", static_cast<double>(which));
            set("entities", static_cast<double>(list.entities.size()));
            set("entityId", present ? static_cast<double>(entity.index) + 1.0 : 0.0);
            set("present", present ? 1.0 : 0.0);
            if (list.grouped) set("group", which < list.groupOf.size() ? static_cast<double>(list.groupOf[which]) : 0.0);
            if (only && std::ranges::none_of(std::array<std::string_view, 11> { "matrix", "normalMatrix", "previousMatrix", "worldPosition", "forward", "up", "right", "position", "rotation", "orientation", "scale" }, wanted)) return;
            kecs::Mat4 matrix;
            kecs::Vec3 position, rotation, scale { 1.f, 1.f, 1.f };
            if (present) {
                matrix = world->WorldMatrix(entity);
                position = world->PositionOf(entity);
                rotation = world->RotationOf(entity);
                scale = world->ScaleOf(entity);
            }
            const kor::Mat4 placed = ToKor(matrix);
            set("matrix", Matrix(placed));
            if (wanted("normalMatrix")) set("normalMatrix", Matrix(kor::Transpose(kor::Inverse(placed))));
            if (wanted("previousMatrix")) set("previousMatrix", Matrix(present ? MatrixBefore(entity, placed) : placed));
            set("worldPosition", Three({ placed[3][0], placed[3][1], placed[3][2] }));
            set("forward", Three(kor::Normalize(kor::Vec3(-placed[2][0], -placed[2][1], -placed[2][2]))));
            set("up", Three(kor::Normalize(kor::Vec3(placed[1][0], placed[1][1], placed[1][2]))));
            set("right", Three(kor::Normalize(kor::Vec3(placed[0][0], placed[0][1], placed[0][2]))));
            set("position", Value(Value::List { position.x, position.y, position.z }));
            set("rotation", Value(Value::List { rotation.x, rotation.y, rotation.z }));
            const kecs::Quat turned = present ? world->OrientationOf(entity) : kecs::Quat {};
            set("orientation", Value(Value::List { turned.x, turned.y, turned.z, turned.w }));
            set("scale", Value(Value::List { scale.x, scale.y, scale.z }));
        }

        /**
         * Where an entity was the frame before, of where it is now (@p now): what it was when it was last asked of on the
         * frame before this one. Asked of on no frame before — or not on the last — where it is now: it did not move.
         */
        kor::Mat4 MatrixBefore(const kecs::Entity entity, const kor::Mat4& now) const
        {
            auto [at, fresh] = moved.try_emplace(entity.index);
            Moved& seen = at->second;
            if (fresh || seen.entity != entity) seen = { entity, timeFrame, now, now };
            else if (seen.frame != timeFrame) {
                seen.before = seen.frame + 1 == timeFrame ? seen.now : now;
                seen.frame = timeFrame;
            }
            seen.now = now;
            return seen.before;
        }

        /** Whether a node's values are worked out of other nodes' values, so that they follow whatever those follow. */
        static bool WorksOut(const Node& node)
        {
            const NodeType* type = FindType(node.type);
            return type && !type->builder && (node.type == "Reference" || (type->category != "Passes" && type->category != "Resources" && type->category != "Scene"));
        }

        /**
         * Whether what node @p id gives is one entity's of many: it is an Entities node (which is added to
         * @p sources), or is worked out of one (and is added to @p chain, after what it is worked out of).
         */
        bool Follows(const std::string& id, std::set<std::string>& sources, std::vector<std::string>& chain, std::map<std::string, bool>& seen) const
        {
            if (const auto known = seen.find(id); known != seen.end()) return known->second;
            seen[id] = false;
            const Node* node = current ? current->Find(id) : nullptr;
            if (!node) return false;
            if (IsEach(node->type)) {
                // One that chooses one of them is no loop: what is wired from it is that one's, like any value.
                if (node->type == "Entity") return false;
                sources.insert(id);
                return seen[id] = true;
            }
            if (!WorksOut(*node) || node->type == "Collect") return false;     // a Collect is of every element at once
            bool follows = false;
            for (const auto& input : node->inputs | std::views::values)
                if (input.link && Follows(input.link->node, sources, chain, seen)) follows = true;
            if (node->type == "Reference")
                if (const auto target = node->inputs.find("node"); target != node->inputs.end() && target->second.value.String())
                    if (Follows(*target->second.value.String(), sources, chain, seen)) follows = true;
            if (follows) chain.push_back(id);
            return seen[id] = follows;
        }

        /**
         * A Collect's list: the value wired to it, as each element of the ForEach it follows has it, one after another —
         * worked out again for each, as a buffer's elements are. Of a value that follows none, that one value.
         */
        void CollectAll(const Node& node, Built& out)
        {
            const auto wired = node.inputs.find("value");
            Value::List all;
            std::size_t count = 0;
            if (wired != node.inputs.end() && wired->second.link) {
                std::set<std::string> sources;
                std::vector<std::string> chain;
                std::map<std::string, bool> seen;
                Follows(wired->second.link->node, sources, chain, seen);
                const auto from = built.find(wired->second.link->node);
                Built* source = nullptr;
                if (sources.size() == 1)
                    if (const auto found = built.find(*sources.begin()); found != built.end() && found->second.status.ok && found->second.listed) source = &found->second;
                const auto take = [&] {
                    if (from == built.end() || !from->second.status.ok) return;
                    const auto output = from->second.outputs.find(wired->second.link->output);
                    if (output == from->second.outputs.end()) return;
                    const Value& value = output->second.value;
                    if (const auto* items = value.Items()) all.insert(all.end(), items->begin(), items->end());
                    else if (const bool* flag = value.Bool()) all.emplace_back(*flag ? 1.0 : 0.0);
                    else if (!value.IsNull() && !value.Named()) all.push_back(value);
                    ++count;
                };
                if (!source) {
                    take();
                } else {
                    std::set<std::string, std::less<>> asked;
                    for (const std::string& id : chain)
                        if (const Node* between = current ? current->Find(id) : nullptr)
                            for (const auto& input : between->inputs | std::views::values)
                                if (input.link && input.link->node == *sources.begin()) asked.insert(input.link->output);
                    if (wired->second.link->node == *sources.begin()) asked.insert(wired->second.link->output);
                    for (std::size_t row = 0; row < source->listed->Rows(); ++row) {
                        SetRow(*source, row, &asked);
                        for (const auto& id : chain) Refresh(id);
                        take();
                    }
                    // Anything else wired to those elements has the first of them, as it does without a Collect.
                    SetRow(*source, 0);
                    for (const auto& id : chain) Refresh(id);
                }
            }
            out.outputs["out"].value = Value(std::move(all));
            out.outputs["count"].value = static_cast<double>(count);
        }

        /** One field of an element, written where the shader reads it: as many numbers as it takes, and nothing for the rest. */
        static void Put(const BlockField& field, const Value& value, std::byte* element)
        {
            std::vector<double> numbers;
            if (const bool* flag = value.Bool()) numbers = { *flag ? 1.0 : 0.0 };
            else if (auto read = NumbersOf(value, "field")) numbers = std::move(*read);
            else return;
            std::size_t next = 0;
            for (std::uint32_t item = 0; item < field.count; ++item)
                for (std::uint32_t line = 0; line < field.lines; ++line)
                    for (std::uint32_t component = 0; component < field.components; ++component, ++next) {
                        const double number = next < numbers.size() ? numbers[next] : 0.0;
                        std::byte* at = element + field.offset + item * field.arrayStride + line * field.lineStride + component * 4;
                        if (field.scalar == 'f') { const auto v = static_cast<float>(number); std::memcpy(at, &v, 4); }
                        else if (field.scalar == 'i') { const auto v = static_cast<std::int32_t>(std::llround(number)); std::memcpy(at, &v, 4); }
                        else if (field.scalar == 'b') { const std::uint32_t v = number != 0.0 ? 1 : 0; std::memcpy(at, &v, 4); }
                        else { const auto v = static_cast<std::uint32_t>(std::llround(std::max(number, 0.0))); std::memcpy(at, &v, 4); }
                    }
        }

        /**
         * A ShaderBuffer's elements as they stand, written when they are not what was last written: those of the
         * buffer it comes after, then one for each entity its fields follow — each field as that entity has it,
         * through whatever is worked out in between — or the one element, when they follow none.
         */
        void Fill(const Node& node, Built& out)
        {
            Mapped& mapped = *out.mapped;
            const BlockLayout& layout = mapped.layout;
            const Mapped* first = nullptr;
            if (!mapped.after.empty()) {
                const auto found = built.find(mapped.after);
                if (found == built.end() || !found->second.status.ok || !found->second.mapped) return;
                first = found->second.mapped.get();
            }
            Built* source = nullptr;
            if (!mapped.source.empty()) {
                const auto found = built.find(mapped.source);
                if (found == built.end() || !found->second.status.ok || !found->second.listed) return;
                source = &found->second;
            }
            const std::size_t before = first ? first->count : 0;
            // A count given is how many elements there are: of a ForEach's, the first that many; without one, each the same.
            std::size_t rows = source ? source->listed->Rows() : mapped.most > 0 ? mapped.most : 1;
            if (mapped.most > 0) rows = std::min(rows, mapped.most);
            // More than there is room for is another buffer: it is built again, with whatever is made of it.
            if (before + rows > mapped.capacity) { out.signature.clear(); stale = true; return; }

            std::vector<std::byte> bytes(mapped.bytes.size());
            if (first) std::memcpy(bytes.data() + layout.first, first->bytes.data() + layout.first, before * layout.stride);
            // Where each field's value is: on the node, or in what another node gives — which changes where it is, a
            // row after another. And which of the entities' values anything asks for: only those are worked out.
            std::vector<std::pair<const BlockField*, const Value*>> fields;
            std::set<std::string, std::less<>> asked;
            const auto asks = [&](const Node& of) {
                for (const auto& input : of.inputs | std::views::values)
                    if (input.link && input.link->node == mapped.source) asked.insert(input.link->output);
            };
            asks(node);
            for (const auto& id : mapped.chain)
                if (const Node* between = current ? current->Find(id) : nullptr) asks(*between);
            for (const auto& [name, input] : node.inputs) {
                if (!name.starts_with("field.")) continue;
                const BlockField* field = layout.Find(std::string_view(name).substr(6));
                if (!field) continue;
                if (!input.link) { if (!input.value.IsNull()) fields.emplace_back(field, &input.value); continue; }
                const auto from = built.find(input.link->node);
                if (from == built.end() || !from->second.status.ok) return;
                const auto output = from->second.outputs.find(input.link->output);
                if (output == from->second.outputs.end()) return;
                fields.emplace_back(field, &output->second.value);
            }
            for (std::size_t row = 0; row < rows; ++row) {
                if (source) {
                    SetRow(*source, row, &asked);
                    for (const auto& id : mapped.chain) Refresh(id);
                }
                std::byte* element = bytes.data() + layout.first + (before + row) * layout.stride;
                for (const auto& [field, value] : fields)
                    if (!value->IsNull()) Put(*field, *value, element);
            }
            // Anything else wired to those entities has the first of them, as it does without a buffer.
            if (source && rows > 1) {
                SetRow(*source, 0);
                for (const auto& id : mapped.chain) Refresh(id);
            }
            mapped.count = before + rows;
            out.outputs["count"].value = static_cast<double>(mapped.count);
            if (bytes == mapped.bytes) return;
            mapped.buffer->Write(bytes);
            mapped.bytes = std::move(bytes);
        }

        /** A field of an element as a shader writes its type: `float3`, `float4x4`, `uint[4]`. */
        static std::string FieldKind(const BlockField& field)
        {
            static const std::map<char, const char*> scalars { { 'f', "float" }, { 'i', "int" }, { 'u', "uint" }, { 'b', "bool" } };
            std::string kind = scalars.at(field.scalar);
            if (field.lines > 1) kind += std::format("{}x{}", field.lines, field.components);
            else if (field.components > 1) kind += std::to_string(field.components);
            if (field.count > 1) kind += std::format("[{}]", field.count);
            return kind;
        }

        /** A buffer's element as inputs to its node: `field.<name>` for each of its fields. What it has, by name, for what is said of one it has not. */
        static std::string FieldPorts(const BlockLayout& layout, Built& out)
        {
            std::string has;
            for (const auto& field : layout.fields) {
                const bool matrix = field.lines > 1 && field.count == 1;
                Port port { "field." + field.name, PortKind::eValue, field.Numbers() == 1 ? (field.scalar == 'b' ? "bool" : "number") : matrix ? "matrix" : "vector", false,
                            std::format("{}.{}: a {}, at byte {} of an element.", layout.type, field.name, FieldKind(field), field.offset), nullptr };
                if (field.scalar == 'f' && field.components >= 3 && field.lines == 1 && field.count == 1 && NamesColor(field.name)) port.editor = "color";
                out.ports.push_back(std::move(port));
                has += std::format("{}{}", has.empty() ? "" : ", ", field.name);
            }
            return has.empty() ? std::string("none") : has;
        }

        /**
         * An element laid out as a shader lays out a struct of these fields, one after another: std430 for a storage
         * buffer, std140 for a uniform block. A number takes four bytes, on four; a vector of two eight, on eight; of
         * three or four, on sixteen; a matrix is its columns, each a vector of four's room. An array's elements are each
         * on their own alignment — on sixteen, in std140 — and so is the struct, whose size is a whole number of it.
         */
        static Expected<BlockLayout> LayOut(const std::vector<std::tuple<std::string, std::string, std::uint32_t>>& fields, const bool storage)
        {
            BlockLayout layout;
            layout.storage = storage;
            std::uint32_t at = 0, aligned = 4;
            std::string text;
            const auto up = [](const std::uint32_t n, const std::uint32_t to) { return (n + to - 1) / to * to; };
            for (const auto& [name, type, count] : fields) {
                BlockField field;
                field.name = name;
                field.count = std::max(count, 1u);
                static const std::map<std::string_view, std::tuple<char, std::uint32_t, std::uint32_t>> types {   // scalar, components, columns
                    { "eFloat", { 'f', 1, 1 } }, { "eInt", { 'i', 1, 1 } }, { "eUInt", { 'u', 1, 1 } }, { "eBool", { 'b', 1, 1 } },
                    { "eVec2", { 'f', 2, 1 } }, { "eVec3", { 'f', 3, 1 } }, { "eVec4", { 'f', 4, 1 } },
                    { "eIVec2", { 'i', 2, 1 } }, { "eIVec3", { 'i', 3, 1 } }, { "eIVec4", { 'i', 4, 1 } },
                    { "eUVec2", { 'u', 2, 1 } }, { "eUVec3", { 'u', 3, 1 } }, { "eUVec4", { 'u', 4, 1 } },
                    { "eMat3", { 'f', 3, 3 } }, { "eMat4", { 'f', 4, 4 } } };
                const auto known = types.find(type);
                if (known == types.end())
                    return Fail(std::format("'{}' is a {}, which is none of eFloat, eInt, eUInt, eBool, eVec2, eVec3, eVec4, eIVec2 to eIVec4, eUVec2 to eUVec4, eMat3, eMat4", name, type));
                if (name.empty()) return Fail("a field of the layout has no name");
                if (layout.Find(name)) return Fail(std::format("the layout has two fields called '{}'", name));
                const auto [scalar, components, columns] = known->second;
                field.scalar = scalar;
                field.components = components;
                field.lines = columns;
                // One of it: how big, and on what.
                std::uint32_t size = components == 3 ? 12 : components * 4, alignment = components == 3 ? 16 : components * 4;
                if (columns > 1) { field.lineStride = 16; size = 16 * columns; alignment = 16; }
                if (field.count > 1) {
                    if (!storage) alignment = up(alignment, 16);
                    field.arrayStride = up(size, alignment);
                    size = field.arrayStride * field.count;
                }
                at = up(at, alignment);
                field.offset = at;
                at += size;
                aligned = std::max(aligned, alignment);
                text += std::format("{}{} {}", text.empty() ? "" : " ", FieldKind(field), name) + ";";
                layout.fields.push_back(std::move(field));
            }
            if (!storage) aligned = up(aligned, 16);
            layout.stride = std::max(up(at, aligned), aligned);
            layout.type = "{ " + text + " }";
            layout.name = layout.type;
            return layout;
        }

        /**
         * A buffer of elements laid out on the node: its `layout` says each field. As many elements as its count says, or
         * as the ForEach its fields follow has, or one.
         */
        Expected<void> BuildBufferBuilder(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto find = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            bool storage = true;
            if (const Value* usage = find("usage")) {
                const std::string* chosen = usage->String();
                if (!chosen || (*chosen != "eStorage" && *chosen != "eUniform")) return Fail("'usage' is eStorage (a StructuredBuffer, std430) or eUniform (a ConstantBuffer, std140)");
                storage = *chosen == "eStorage";
            }
            std::size_t most = 0;
            if (const Value* count = find("count")) {
                const double* given = count->Number();
                if (!given || *given < 1 || *given > 1 << 20 || *given != std::floor(*given)) return Fail("'count' is how many elements: a whole number, 1 or more");
                most = static_cast<std::size_t>(*given);
            }
            std::vector<std::tuple<std::string, std::string, std::uint32_t>> fields;
            if (const Value* layout = find("layout")) {
                const auto* items = layout->Items();
                if (!items) return Fail("'layout' is a list of fields: { \"name\": \"position\", \"type\": \"eVec3\" }");
                for (const Value& item : *items) {
                    const Value* name = item.Find("name");
                    const Value* type = item.Find("type");
                    const Value* count = item.Find("count");
                    if (!name || !name->String()) return Fail("each field of 'layout' has a \"name\"");
                    if (count && (!count->Number() || *count->Number() < 1)) return Fail(std::format("the \"count\" of '{}' is how many of it there are: 1 or more", *name->String()));
                    fields.emplace_back(*name->String(), type && type->String() ? *type->String() : std::string("eFloat"), count ? static_cast<std::uint32_t>(*count->Number()) : 1u);
                }
            }
            if (fields.empty()) return Fail("it has no fields yet: give it a 'layout'");
            auto layout = LayOut(fields, storage);
            if (!layout) return Fail(layout.error());

            auto mapped = std::make_shared<Mapped>();
            mapped->layout = std::move(*layout);
            mapped->layout.array = storage || most > 1;
            mapped->most = most;
            const std::string has = FieldPorts(mapped->layout, out);
            return MakeMapped(node, inputs, out, std::move(mapped), has);
        }

        /**
         * A buffer a shader declares, laid out as its compiled code lays it out: a field of the struct an input,
         * by name. The shader is the node's to name — a module, and an entry point that uses the buffer — since the
         * pipeline that reads the buffer is wired from this node, and cannot also be what it is worked out of.
         */
        Expected<void> BuildShaderBuffer(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto text = [&](const char* name) {
                const auto it = inputs.find(name);
                const std::string* given = it == inputs.end() ? nullptr : it->second.value.String();
                return given ? *given : std::string();
            };
            const std::string module = text("module"), entry = text("entry"), parameter = text("parameter");
            if (module.empty()) return Fail("'module' is the shader module that declares the buffer");
            if (entry.empty()) return Fail("'entry' is an entry point of the module that uses the buffer");
            const kb::Description* shaderType = kb::Find("Shader");
            const kb::Setting* entryPoint = shaderType ? shaderType->FindSetting("entryPoint") : nullptr;
            if (!entryPoint) return Fail("the framework describes no Shader builder");
            auto shader = BuildWith(*shaderType, node.id, {}, { { entryPoint, { kb::Value::Of(module), kb::Value::Of(entry) } } });
            if (!shader) {
                if (shader.error().find("cannot open file") != std::string::npos) return Fail(std::format("there is no {}.slang among the shaders", module));
                return Fail(shader.error());
            }
            const auto compiled = kor::capi::ShaderOf((*shader)->handle);
            Watch(out, compiled);

            const std::vector<BlockLayout> blocks = BlocksOf(compiled->Spirv());
            std::string declared;
            for (const auto& block : blocks) declared += std::format("{}{}", declared.empty() ? "" : ", ", block.name);
            if (declared.empty()) declared = "none";
            if (parameter.empty()) return Fail(std::format("'parameter' is what the shader calls the buffer. '{}' of {} uses: {}", entry, module, declared));
            auto block = std::ranges::find(blocks, parameter, &BlockLayout::name);
            if (block == blocks.end()) block = std::ranges::find(blocks, parameter, &BlockLayout::type);
            if (block == blocks.end()) return Fail(std::format("'{}' of {} uses no buffer '{}'. It uses: {}", entry, module, parameter, declared));

            auto mapped = std::make_shared<Mapped>();
            mapped->layout = *block;
            mapped->shader = std::move(*shader);
            const std::string has = FieldPorts(mapped->layout, out);

            return MakeMapped(node, inputs, out, std::move(mapped), has);
        }

        /**
         * What a buffer of elements is made of, once its element is laid out — by its shader or on the node: where its
         * elements come from (a ForEach its fields follow, or none), what comes before them, and room for
         * them all. @p has is the fields it has, for what is said of one it has not.
         */
        Expected<void> MakeMapped(const Node& node, const Inputs& inputs, Built& out, std::shared_ptr<Mapped> mapped, const std::string& has)
        {
            const BlockLayout& layout = mapped->layout;
            // What its fields follow: an element each, when any is wired from a ForEach or from what is worked out of one.
            std::set<std::string> sources;
            std::map<std::string, bool> seen;
            const auto follows = [&](const std::string& name) {
                if (const auto wired = node.inputs.find(name); wired != node.inputs.end() && wired->second.link) Follows(wired->second.link->node, sources, mapped->chain, seen);
            };
            for (const auto& [name, input] : inputs) {
                if (name == "instance") return Fail("'instance' is no more: wire each of its fields, `field.<name>`, from what holds it");
                if (!name.starts_with("field.")) continue;
                if (!layout.Find(std::string_view(name).substr(6))) {
                    if (FromBundle(node.id, name)) continue;     // given by a Bundle, by name: let pass, and said on the node
                    return Fail(std::format("{} has no field '{}'. It has: {}", layout.type, name.substr(6), has));
                }
                if (input.resource) return Fail(std::format("'{}' takes a value, and was wired to a resource", name));
                if (!input.value.IsNull() && !input.value.Bool() && !NumbersOf(input.value, name.c_str())) return Fail(std::format("'{}' is a number, or a list of them", name));
                follows(name);
            }
            if (sources.size() > 1)
                return Fail(std::format("its fields follow more than one ForEach, '{}' and '{}': an element is one of a ForEach's", *sources.begin(), *std::next(sources.begin())));
            std::size_t count = mapped->most > 0 ? mapped->most : 1;
            if (layout.array && !sources.empty()) {
                mapped->source = *sources.begin();
                const auto source = built.find(mapped->source);
                count = source != built.end() && source->second.listed ? source->second.listed->Rows() : 0;
                if (mapped->most > 0) count = std::min(count, mapped->most);
            } else {
                mapped->chain.clear();      // a block of one is the first element's, which is what is there already
            }
            std::size_t earlier = 0;
            if (const auto after = inputs.find("after"); after != inputs.end() && after->second.resource) {
                if (!layout.array) return Fail(std::format("'after' is for a buffer of many elements, and '{}' is a block of one", layout.name));
                const auto first = built.find(after->second.resource->node);
                const Mapped* theirs = first == built.end() ? nullptr : first->second.mapped.get();
                if (!theirs || theirs->layout.type != layout.type || theirs->layout.stride != layout.stride || theirs->layout.first != layout.first)
                    return Fail(std::format("'after' takes another {} of {}", node.type, layout.type));
                mapped->after = after->second.resource->node;
                earlier = theirs->count;
                count += earlier;
            }

            // Room for the next power of two of them, or — given a count — for as many as it says.
            mapped->capacity = mapped->most > 0 ? earlier + mapped->most : std::bit_ceil(std::max<std::size_t>(count, 1));
            mapped->bytes.assign(layout.first + mapped->capacity * layout.stride, std::byte { 0 });
            auto buffer = kor::Buffer::RawBuilder{}.SetRawSize(static_cast<kor::i64>(mapped->bytes.size()))
                .SetUsage(kor::Flags<kor::Buffer::Usage>(layout.storage ? kor::Buffer::Usage::eStorage : kor::Buffer::Usage::eUniform) | kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eDynamic).SetIsPerFrame(true).Build();
            if (!buffer.Valid()) return Fail(buffer.Failure() ? buffer.Failure()->message : std::string("its buffer could not be made"));
            mapped->buffer = kor::ResourceRef<kor::Buffer>(buffer);
            mapped->held = MakeResource(std::move(buffer));
            mapped->held->node = node.id;
            mapped->buffer->Write(mapped->bytes);
            out.outputs["out"].resource = mapped->held;
            out.outputs["stride"].value = static_cast<double>(layout.stride);
            out.outputs["bytes"].value = static_cast<double>(mapped->bytes.size());
            out.outputs["count"].value = 0.0;
            out.mapped = std::move(mapped);
            Fill(node, out);
            return {};
        }

        /**
         * What output @p output of node @p id holds of each of its elements: a resource for each element of a ForEach (a
         * Filter's `Mesh.mesh`), each led on to what replaced it — or the one it holds, of a node that follows none.
         */
        std::vector<Held> EachResource(const std::string& id, const std::string& output) const
        {
            std::vector<Held> all;
            const auto it = built.find(id);
            if (it == built.end()) return all;
            const Built& source = it->second;
            if (source.listed && world) {
                const EntityList& list = *source.listed;
                const auto read = std::ranges::find(list.fields, output, &EntityList::Read::output);
                if (read != list.fields.end() && read->resource) {
                    for (const kecs::Entity entity : list.entities) {
                        const auto* bytes = static_cast<const std::byte*>(world->Get(entity, read->component));
                        if (!bytes) continue;
                        Held held = *reinterpret_cast<const Held*>(bytes + read->field.offset);
                        while (held && held->replacedBy) held = held->replacedBy;
                        all.push_back(std::move(held));
                    }
                    return all;
                }
            }
            if (const auto out = source.outputs.find(output); out != source.outputs.end() && out->second.resource) all.push_back(out->second.resource);
            return all;
        }

        /** A MeshPack's meshes as they are now: the distinct ones of its elements, in the order they come first. */
        std::vector<Held> PackedMeshes(const Node& node) const
        {
            std::vector<Held> meshes;
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return meshes;
            const Node* origin = Origin(wired->second.link->node);
            std::set<const Resource*> seen;
            for (Held& held : EachResource(origin ? origin->id : wired->second.link->node, wired->second.link->output))
                if (held && seen.insert(held.get()).second) meshes.push_back(std::move(held));
            return meshes;
        }

        /**
         * The same meshes as PackedMeshes, as which they are and nothing more: what a MeshPack asks each frame, to know
         * whether to pack again — without holding each entity's mesh on the way, thousands of them.
         */
        std::vector<const Resource*> PackedNow(const Node& node) const
        {
            std::vector<const Resource*> meshes;
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return meshes;
            const Node* origin = Origin(wired->second.link->node);
            const auto it = built.find(origin ? origin->id : wired->second.link->node);
            if (it == built.end()) return meshes;
            const auto add = [&](const Resource* mesh) {
                while (mesh && mesh->replacedBy) mesh = mesh->replacedBy.get();
                if (mesh && std::ranges::find(meshes, mesh) == meshes.end()) meshes.push_back(mesh);     // a few: one each
            };
            const Built& source = it->second;
            if (source.listed && world) {
                const auto read = std::ranges::find(source.listed->fields, wired->second.link->output, &EntityList::Read::output);
                if (read != source.listed->fields.end() && read->resource) {
                    for (const kecs::Entity entity : source.listed->entities)
                        if (const auto* bytes = static_cast<const std::byte*>(world->Get(entity, read->component)))
                            add(reinterpret_cast<const Held*>(bytes + read->field.offset)->get());
                    return meshes;
                }
            }
            if (const auto out = source.outputs.find(wired->second.link->output); out != source.outputs.end()) add(out->second.resource.get());
            return meshes;
        }

        Expected<void> BuildMeshPack(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto asked = inputs.find("positions");
            const bool positionsOnly = asked != inputs.end() && asked->second.value.Bool() && *asked->second.value.Bool();
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return Fail("'meshes' is each element's mesh: wire a Filter's Mesh.mesh");
            const std::vector<Held> meshes = PackedMeshes(node);
            auto packing = std::make_shared<MeshPacking>();
            for (const Held& mesh : meshes) packing->of.push_back(mesh.get());
            // The wholes they are of — a model's meshes are one on the device — each copied once, in the order they come.
            std::vector<const Resource*> wholes;
            std::map<const Resource*, std::size_t> wholeAt;
            for (const Held& mesh : meshes) {
                if (mesh->kind != "Mesh") return Fail(std::format("'{}' is a {}, and only meshes are packed", mesh->node, mesh->kind));
                if (mesh->pending) continue;    // still being read: packed when it is
                const Resource* whole = mesh->part ? mesh->source.get() : mesh.get();
                if (whole && whole->handle && wholeAt.try_emplace(whole, wholes.size()).second) wholes.push_back(whole);
            }
            out.outputs["count"].value = static_cast<double>(meshes.size());
            out.packing = std::move(packing);
            // Nothing there yet to pack — no meshes, or only ones still being read: an empty mesh laid out as the default
            // meshes are, so that the pipeline and the Draw are built and draw nothing (the Draw still clears) until there
            // is. It is built again when one arrives.
            kor::VertexLayout layout = wholes.empty() ? DefaultLayout(DefaultTangents()) : kor::capi::MeshOf(wholes.front()->handle)->Layout();
            if (positionsOnly) {
                layout = {};
                layout.attributes.emplace_back("POSITION", "", 0u, 0u, kor::ChannelType::eFloat, 3u);
                layout.bindings.emplace_back(0u, 12u);
            }
            // Only these, of every mesh: a buffer each, as the first mesh that has it has it — zeros where a mesh has not —
            // whatever else each has and however it lays them out. Meshes of other files are then one.
            std::vector<std::string> only;
            if (const auto wanted = inputs.find("attributes"); wanted != inputs.end() && !wanted->second.value.IsNull()) {
                const auto* items = wanted->second.value.Items();
                if (!items || items->empty()) return Fail("'attributes' is the semantics to pack, each a buffer: [\"POSITION\", \"NORMAL\", \"UV\"]");
                for (const auto& item : *items) {
                    if (!item.String()) return Fail("'attributes' is the semantics to pack, each a buffer: [\"POSITION\", \"NORMAL\", \"UV\"]");
                    only.push_back(*item.String());
                }
            }
            if (!only.empty() && !positionsOnly) {
                const kor::VertexLayout fallback = DefaultLayout(DefaultTangents());
                kor::VertexLayout chosen;
                for (std::size_t i = 0; i < only.size(); ++i) {
                    std::optional<kor::VertexLayout::Attribute> found;
                    for (const Resource* whole : wholes) {
                        const auto& attributes = kor::capi::MeshOf(whole->handle)->Layout().attributes;
                        if (const auto it = std::ranges::find(attributes, std::string_view(only[i]), [](const auto& a) { return std::string_view(a.semantic); }); it != attributes.end()) { found = *it; break; }
                    }
                    if (!found)
                        if (const auto it = std::ranges::find(fallback.attributes, std::string_view(only[i]), [](const auto& a) { return std::string_view(a.semantic); }); it != fallback.attributes.end()) found = *it;
                    if (!found) return Fail(std::format("no mesh it packs has {}", only[i]));
                    if (found->channelType != kor::ChannelType::eFloat && found->channelType != kor::ChannelType::eUInt && found->channelType != kor::ChannelType::eInt)
                        return Fail(std::format("{} is not of 32-bit numbers: only such attributes are packed", only[i]));
                    chosen.attributes.emplace_back(only[i], "", static_cast<kor::u32>(i), 0u, found->channelType, found->channelCount);
                    chosen.bindings.emplace_back(static_cast<kor::u32>(i), found->channelCount * 4u);
                }
                layout = std::move(chosen);
            }
            const std::string shape = LayoutValueOf(layout).ToJson();
            std::vector<kor::u64> vertexBase, indexBase;
            kor::u64 vertices = 0, indices = 0;
            for (const Resource* whole : wholes) {
                const auto mesh = kor::capi::MeshOf(whole->handle);
                if (!positionsOnly && only.empty() && LayoutValueOf(mesh->Layout()).ToJson() != shape)
                    return Fail(std::format("its meshes' vertices are not laid out alike: '{}' and '{}' are not — a Filter of each kind, and a pack of each", wholes.front()->node, whole->node));
                if (!mesh->IndexBuffer() || !mesh->IndexCount() || mesh->IndexType() != kor::ChannelType::eUInt)
                    return Fail(std::format("'{}' is not drawn by indices of 32 bits, and only such meshes are packed", whole->node));
                vertexBase.push_back(vertices);
                indexBase.push_back(indices);
                vertices += mesh->VertexCount();
                indices += *mesh->IndexCount();
            }
            // One buffer for each of the layout's, and one of indices: each whole's after the last, copied on the device.
            // What rays can be traced through, where the device traces them: a RayScene's structures are made of these.
            const kor::Flags<kor::Buffer::Usage> traced = kor::Context::SupportsRayTracing() ? kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eAccelerationStructureInput)
                                                                                              : kor::Flags<kor::Buffer::Usage>();
            const auto made = [traced](const kor::u64 bytes, const kor::Flags<kor::Buffer::Usage> usage) {
                return kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<kor::u64>(bytes, 4)))
                    .SetUsage(usage | traced | kor::Buffer::Usage::eTransferDst | kor::Buffer::Usage::eTransferSrc | kor::Buffer::Usage::eStorage)
                    .SetType(kor::Buffer::Type::eDeviceLocal).Build();
            };
            std::vector<kor::Resource<kor::Buffer>> buffers;
            for (const auto& binding : layout.bindings) {
                buffers.push_back(made(vertices * binding.stride, kor::Buffer::Usage::eVertex));
                if (!buffers.back().Valid()) return Fail("the packed mesh's vertices could not be made a buffer");
            }
            kor::Resource<kor::Buffer> index = made(indices * 4, kor::Buffer::Usage::eIndex);
            if (!index.Valid()) return Fail("the packed mesh's indices could not be made a buffer");
            if (positionsOnly) {
                // Each whole's positions read back and laid one after another, three floats each, whatever it lays out
                // besides: written as one buffer.
                std::vector<float> positions;
                positions.reserve(vertices * 3);
                for (const Resource* whole : wholes) {
                    const auto mesh = kor::capi::MeshOf(whole->handle);
                    const auto& from = mesh->Layout();
                    const auto position = std::ranges::find(from.attributes, std::string_view("POSITION"), [](const auto& a) { return std::string_view(a.semantic); });
                    const auto buffers = mesh->VertexBuffers();
                    if (position == from.attributes.end() || position->binding >= buffers.size() || position->channelType != kor::ChannelType::eFloat || position->channelCount < 3)
                        return Fail(std::format("'{}' has no POSITION of three floats to pack", whole->node));
                    const auto binding = std::ranges::find(from.bindings, position->binding, &kor::VertexInputBindingDescription::binding);
                    const kor::u64 stride = binding == from.bindings.end() ? 12 : binding->stride;
                    const std::vector<std::byte> bytes = buffers[position->binding]->Read<std::byte>(mesh->VertexCount() * stride, 0);
                    if (bytes.size() < mesh->VertexCount() * stride) return Fail(std::format("'{}'s vertices could not be read back", whole->node));
                    for (kor::u64 v = 0; v < mesh->VertexCount(); ++v) {
                        float p[3];
                        std::memcpy(p, bytes.data() + v * stride + position->offset, sizeof p);
                        positions.insert(positions.end(), p, p + 3);
                    }
                }
                if (!positions.empty()) buffers[0]->Write(std::span<const float>(positions));
            }
            if (!only.empty() && !positionsOnly) {
                // Each attribute of each mesh read back, and laid one mesh after another in its own buffer.
                std::vector<std::vector<std::uint32_t>> packedWords(layout.attributes.size());
                for (std::size_t a = 0; a < layout.attributes.size(); ++a) packedWords[a].assign(vertices * layout.attributes[a].channelCount, 0u);
                for (std::size_t w = 0; w < wholes.size(); ++w) {
                    const auto mesh = kor::capi::MeshOf(wholes[w]->handle);
                    const auto& from = mesh->Layout();
                    const auto sources = mesh->VertexBuffers();
                    std::map<kor::u32, std::vector<std::byte>> read;     // each binding of it, read back once
                    for (std::size_t a = 0; a < layout.attributes.size(); ++a) {
                        const auto& to = layout.attributes[a];
                        const auto it = std::ranges::find(from.attributes, std::string_view(to.semantic), [](const auto& x) { return std::string_view(x.semantic); });
                        if (it == from.attributes.end() || it->binding >= sources.size()) continue;      // it has none: zeros
                        const auto binding = std::ranges::find(from.bindings, it->binding, &kor::VertexInputBindingDescription::binding);
                        const kor::u64 stride = binding == from.bindings.end() ? it->channelCount * 4u : binding->stride;
                        auto& bytes = read[it->binding];
                        if (bytes.empty()) bytes = sources[it->binding]->Read<std::byte>(mesh->VertexCount() * stride, 0);
                        if (bytes.size() < mesh->VertexCount() * stride) return Fail(std::format("'{}'s vertices could not be read back", wholes[w]->node));
                        const kor::u32 words = std::min(to.channelCount, it->channelCount);
                        for (kor::u64 v = 0; v < mesh->VertexCount(); ++v)
                            std::memcpy(packedWords[a].data() + (vertexBase[w] + v) * to.channelCount, bytes.data() + v * stride + it->offset, words * 4u);
                    }
                }
                for (std::size_t a = 0; a < layout.attributes.size(); ++a)
                    if (!packedWords[a].empty()) buffers[a]->Write(std::span<const std::uint32_t>(packedWords[a]));
            }
            if (!wholes.empty()) kor::CommandBuffer::SingleTimeCommand([&](kor::CommandBuffer& cb) {
                for (std::size_t w = 0; w < wholes.size(); ++w) {
                    const auto mesh = kor::capi::MeshOf(wholes[w]->handle);
                    const auto from = mesh->VertexBuffers();
                    for (std::size_t b = 0; b < layout.bindings.size() && !positionsOnly && only.empty(); ++b) {
                        const kor::u32 binding = layout.bindings[b].binding, stride = layout.bindings[b].stride;
                        if (binding >= from.size()) continue;
                        cb.CopyBuffer(from[binding], kor::ResourceRef<const kor::Buffer>(buffers[b]), mesh->VertexCount() * stride, 0, vertexBase[w] * stride);
                    }
                    cb.CopyBuffer(*mesh->IndexBuffer(), kor::ResourceRef<const kor::Buffer>(index), static_cast<kor::u64>(*mesh->IndexCount()) * 4, 0, indexBase[w] * 4);
                }
            }, kor::CommandBuffer::Usage::eTransfer).Wait();
            // Each buffer a resource of its own, the mesh referring to it: what a compute shader reads of it (a MeshBuffers).
            kor::Mesh::Builder builder;
            std::vector<Held> own;
            for (std::size_t b = 0; b < layout.bindings.size(); ++b) {
                builder.SetVertexBuffer(layout.bindings[b].binding, kor::ResourceRef<kor::Buffer>(buffers[b]));
                own.push_back(MakeResource(std::move(buffers[b])));
            }
            builder.SetIndexBuffer(kor::ResourceRef<kor::Buffer>(index), kor::ChannelType::eUInt).SetVertexLayout(layout);
            own.push_back(MakeResource(std::move(index)));
            auto mesh = builder.Build();
            if (!mesh.Valid()) return Fail(mesh.Failure() ? mesh.Failure()->message : std::string("the packed mesh could not be built"));
            Held packed = MakeResource(std::move(mesh));
            packed->buffers = std::move(own);
            packed->kind = "Mesh";
            packed->node = node.id;

            // Each mesh's part of it: what an indirect command draws, and a shader's copy of the same.
            Value::List ranges;
            std::vector<std::byte> bytes(meshes.size() * 48, std::byte { 0 });
            bool anyBounds = false;
            for (std::size_t i = 0; i < meshes.size(); ++i) {
                const Resource& m = *meshes[i];
                const Resource* whole = m.part ? m.source.get() : &m;
                const auto at = whole ? wholeAt.find(whole) : wholeAt.end();
                std::uint32_t firstIndex = 0, indexCount = 0;
                std::int32_t vertexOffset = 0;
                if (!m.pending && at != wholeAt.end()) {
                    const auto source = kor::capi::MeshOf(whole->handle);
                    firstIndex = static_cast<std::uint32_t>(indexBase[at->second]) + (m.part ? m.firstIndex : 0);
                    indexCount = m.part ? m.indexCount : *source->IndexCount();
                    vertexOffset = static_cast<std::int32_t>(vertexBase[at->second]) + (m.part ? m.vertexOffset : 0);
                }
                const float* box = m.bounded ? m.bounds : whole && whole->bounded ? whole->bounds : nullptr;
                const Value::List low { box ? box[0] : 0.0, box ? box[1] : 0.0, box ? box[2] : 0.0 }, high { box ? box[3] : 0.0, box ? box[4] : 0.0, box ? box[5] : 0.0 };
                ranges.emplace_back(Value::Members { { "firstIndex", static_cast<double>(firstIndex) }, { "indexCount", static_cast<double>(indexCount) },
                                                     { "vertexOffset", static_cast<double>(vertexOffset) }, { "boundsMin", low }, { "boundsMax", high } });
                std::byte* to = bytes.data() + i * 48;
                std::memcpy(to, &firstIndex, 4);
                std::memcpy(to + 4, &indexCount, 4);
                std::memcpy(to + 8, &vertexOffset, 4);
                if (box) {
                    for (int axis = 0; axis < 3; ++axis) {
                        std::memcpy(to + 16 + axis * 4, &box[axis], 4);
                        std::memcpy(to + 32 + axis * 4, &box[axis + 3], 4);
                    }
                    if (!anyBounds) { std::copy(box, box + 6, packed->bounds); anyBounds = true; }
                    for (int axis = 0; axis < 3; ++axis) {
                        packed->bounds[axis] = std::min(packed->bounds[axis], box[axis]);
                        packed->bounds[axis + 3] = std::max(packed->bounds[axis + 3], box[axis + 3]);
                    }
                }
            }
            packed->bounded = anyBounds;
            auto table = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 48)))
                .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eDynamic).Build();
            if (!table.Valid()) return Fail("its ranges could not be made a buffer");
            table->Write(std::span<const std::byte>(bytes));
            Held rangeBuffer = MakeResource(std::move(table));
            rangeBuffer->node = node.id;
            out.outputs["mesh"].resource = std::move(packed);
            out.outputs["layout"].value = LayoutValueOf(layout);
            out.outputs["ranges"].value = Value(std::move(ranges));
            out.outputs["rangeBuffer"].resource = std::move(rangeBuffer);
            return {};
        }

        /**
         * A MeshPack's meshes cut into meshlets on the CPU: each mesh's triangles taken in their order, a meshlet ending
         * where one more would take it past so many vertices or triangles. Each has a sphere round its vertices and a
         * cone of the ways its triangles face (Meshlets' doc says how a shader tests them).
         */
        Expected<void> BuildMeshlets(const Inputs& inputs, Built& out)
        {
            const auto given = inputs.find("mesh");
            if (given == inputs.end() || !given->second.resource) return Fail("'mesh' is the meshes as one: a MeshPack's mesh");
            Held held = given->second.resource;
            while (held && held->replacedBy) held = held->replacedBy;
            if (!held || held->kind != "Mesh" || !held->handle) return Fail("'mesh' is a mesh: a MeshPack's");
            const auto rangesGiven = inputs.find("ranges");
            const auto* ranges = rangesGiven == inputs.end() ? nullptr : rangesGiven->second.value.Items();
            if (!ranges) return Fail("'ranges' is each mesh's part of it: the MeshPack's ranges");
            const auto number = [&](const char* name, const double otherwise) {
                const auto it = inputs.find(name);
                return it != inputs.end() && it->second.value.Number() ? *it->second.value.Number() : otherwise;
            };
            const auto mostVertices = static_cast<std::uint32_t>(std::clamp(number("maxVertices", 64), 3.0, 256.0));
            const auto mostTriangles = static_cast<std::uint32_t>(std::clamp(number("maxTriangles", 124), 1.0, 256.0));

            // Where its vertices are, and its indices: read back once.
            const auto mesh = kor::capi::MeshOf(held->handle);
            const auto& layout = mesh->Layout();
            const auto position = std::ranges::find(layout.attributes, std::string_view("POSITION"), [](const auto& a) { return std::string_view(a.semantic); });
            const auto sources = mesh->VertexBuffers();
            if (position == layout.attributes.end() || position->binding >= sources.size() || position->channelType != kor::ChannelType::eFloat || position->channelCount < 3)
                return Fail("its mesh has no POSITION of three floats");
            if (!mesh->IndexBuffer() || mesh->IndexType() != kor::ChannelType::eUInt) return Fail("its mesh is not drawn by indices of 32 bits");
            const auto binding = std::ranges::find(layout.bindings, position->binding, &kor::VertexInputBindingDescription::binding);
            const kor::u64 stride = binding == layout.bindings.end() ? 12 : binding->stride;
            const kor::u64 vertexCount = mesh->VertexCount();
            const std::vector<std::byte> vertexBytes = vertexCount ? sources[position->binding]->Read<std::byte>(vertexCount * stride, 0) : std::vector<std::byte>();
            const kor::u64 indexCount = mesh->IndexCount().value_or(0);
            const std::vector<std::uint32_t> indices = indexCount ? (*mesh->IndexBuffer())->Read<std::uint32_t>(indexCount, 0) : std::vector<std::uint32_t>();
            if (vertexBytes.size() < vertexCount * stride || indices.size() < indexCount) return Fail("its mesh's vertices could not be read back");
            const auto at = [&](const std::uint32_t vertex) {
                float p[3] { 0.f, 0.f, 0.f };
                if (vertex < vertexCount) std::memcpy(p, vertexBytes.data() + vertex * stride + position->offset, sizeof p);
                return kor::Vec3(p[0], p[1], p[2]);
            };

            struct Meshlet { std::uint32_t vertexOffset, vertexCount, triangleOffset, triangleCount; float sphere[4]; float cone[4]; };
            static_assert(sizeof(Meshlet) == 48);
            // A run of a mesh's triangles, cut by itself: its meshlets' offsets count from its own vertices and triangles.
            struct Piece { std::vector<Meshlet> meshlets; std::vector<std::uint32_t> vertices, triangles; };
            struct Task { std::size_t range; std::uint64_t first, count; std::int64_t offset; };
            // Each mesh's triangles in runs of so many, cut at once on as many threads: a run ends a meshlet where the
            // mesh would have gone on with it, and nowhere else is it cut otherwise than one after another.
            constexpr std::uint64_t RunTriangles = 1u << 18;
            std::vector<Task> tasks;
            for (std::size_t r = 0; r < ranges->size(); ++r) {
                const Value& range = (*ranges)[r];
                const auto field = [&](const char* name) { const Value* found = range.Find(name); return found && found->Number() ? *found->Number() : 0.0; };
                const auto first = static_cast<std::uint64_t>(field("firstIndex")), count = static_cast<std::uint64_t>(field("indexCount"));
                const auto offset = static_cast<std::int64_t>(field("vertexOffset"));
                if (count < 3) { tasks.push_back({ r, first, 0, offset }); continue; }
                for (std::uint64_t at = 0; at + 3 <= count; at += RunTriangles * 3)
                    tasks.push_back({ r, first + at, std::min<std::uint64_t>(RunTriangles * 3, count - at), offset });
            }
            std::vector<Piece> pieces(tasks.size());
            const auto cutRun = [&](const Task& task, std::vector<std::uint32_t>& local) {
                Piece& piece = pieces[&task - tasks.data()];
                // Its vertices' places in the meshlet being made, by the vertex less the least of the run's.
                std::uint32_t least = ~0u, greatest = 0;
                for (std::uint64_t i = 0; i < task.count && task.first + i < indices.size(); ++i) {
                    const auto v = static_cast<std::int64_t>(indices[task.first + i]) + task.offset;
                    if (v < 0 || static_cast<kor::u64>(v) >= vertexCount) continue;
                    least = std::min(least, static_cast<std::uint32_t>(v));
                    greatest = std::max(greatest, static_cast<std::uint32_t>(v));
                }
                if (least > greatest) return;
                if (local.size() < greatest - least + 1) local.assign(greatest - least + 1, ~0u);
                std::vector<std::uint32_t> made;                         // the vertices of the meshlet being made
                std::vector<kor::Vec3> faces;                            // its triangles' normals
                const auto finish = [&](Meshlet meshlet) {
                    if (meshlet.triangleCount == 0) return;
                    kor::Vec3 low(1e30f), high(-1e30f);
                    for (const std::uint32_t v : made) { low = kor::Min(low, at(v)); high = kor::Max(high, at(v)); }
                    const kor::Vec3 middle = (low + high) * 0.5f;
                    float reach = 0.f;
                    for (const std::uint32_t v : made) reach = std::max(reach, kor::Length(at(v) - middle));
                    kor::Vec3 axis(0.f);
                    for (const auto& face : faces) axis = axis + face;
                    float cutoff = 1.f;     // no cone: never culled by it
                    if (const float length = kor::Length(axis); length > 1e-6f && !faces.empty()) {
                        axis = axis / length;
                        float lowest = 1.f;
                        for (const auto& face : faces) lowest = std::min(lowest, kor::Dot(axis, face));
                        if (lowest > 0.1f) cutoff = std::sqrt(1.f - lowest * lowest);
                    }
                    meshlet.sphere[0] = middle.x; meshlet.sphere[1] = middle.y; meshlet.sphere[2] = middle.z; meshlet.sphere[3] = reach;
                    meshlet.cone[0] = axis.x; meshlet.cone[1] = axis.y; meshlet.cone[2] = axis.z; meshlet.cone[3] = cutoff;
                    for (const std::uint32_t v : made) local[v - least] = ~0u;
                    made.clear();
                    faces.clear();
                    piece.meshlets.push_back(meshlet);
                };
                Meshlet meshlet { 0, 0, 0, 0, {}, {} };
                for (std::uint64_t t = 0; t + 3 <= task.count && task.first + t + 3 <= indices.size(); t += 3) {
                    std::uint32_t corner[3];
                    for (int c = 0; c < 3; ++c) corner[c] = static_cast<std::uint32_t>(static_cast<std::int64_t>(indices[task.first + t + c]) + task.offset);
                    if (corner[0] >= vertexCount || corner[1] >= vertexCount || corner[2] >= vertexCount) continue;
                    int fresh = 0;
                    for (int c = 0; c < 3; ++c) fresh += local[corner[c] - least] == ~0u && (c == 0 || corner[c] != corner[0]) && (c < 2 || corner[c] != corner[1]) ? 1 : 0;
                    if (meshlet.vertexCount + fresh > mostVertices || meshlet.triangleCount + 1 > mostTriangles) {
                        finish(meshlet);
                        meshlet = { static_cast<std::uint32_t>(piece.vertices.size()), 0, static_cast<std::uint32_t>(piece.triangles.size()), 0, {}, {} };
                    }
                    std::uint32_t packed = 0;
                    for (int c = 0; c < 3; ++c) {
                        std::uint32_t& place = local[corner[c] - least];
                        if (place == ~0u) {
                            place = meshlet.vertexCount++;
                            piece.vertices.push_back(corner[c]);
                            made.push_back(corner[c]);
                        }
                        packed |= place << (8 * c);
                    }
                    piece.triangles.push_back(packed);
                    ++meshlet.triangleCount;
                    const kor::Vec3 a = at(corner[0]), b = at(corner[1]), c = at(corner[2]);
                    const kor::Vec3 n = kor::Cross(b - a, c - a);
                    if (const float length = kor::Length(n); length > 1e-12f) faces.push_back(n / length);
                }
                finish(meshlet);
            };
            {
                std::atomic<std::size_t> next { 0 };
                const auto work = [&] {
                    std::vector<std::uint32_t> local;   // all ~0 between meshlets: finish puts back what it took
                    for (std::size_t i; (i = next++) < tasks.size();) cutRun(tasks[i], local);
                };
                const std::size_t threads = std::min<std::size_t>({ std::max(1u, std::thread::hardware_concurrency()), 16, tasks.size() });
                std::vector<std::jthread> helpers;
                for (std::size_t t = 1; t < threads; ++t) helpers.emplace_back(work);
                work();
            }
            // The runs one after another, as one mesh's meshlets were cut: each mesh's are together, in order.
            std::vector<Meshlet> meshlets;
            std::vector<std::uint32_t> vertices, triangles, meshRanges;
            double most = 0;
            for (std::size_t i = 0, r = 0; r < ranges->size(); ++r) {
                const auto firstMeshlet = static_cast<std::uint32_t>(meshlets.size());
                for (; i < tasks.size() && tasks[i].range == r; ++i) {
                    Piece& piece = pieces[i];
                    const auto vertexBase = static_cast<std::uint32_t>(vertices.size()), triangleBase = static_cast<std::uint32_t>(triangles.size());
                    for (Meshlet meshlet : piece.meshlets) {
                        meshlet.vertexOffset += vertexBase;
                        meshlet.triangleOffset += triangleBase;
                        meshlets.push_back(meshlet);
                    }
                    vertices.insert(vertices.end(), piece.vertices.begin(), piece.vertices.end());
                    triangles.insert(triangles.end(), piece.triangles.begin(), piece.triangles.end());
                    piece = {};
                }
                const auto cut = static_cast<std::uint32_t>(meshlets.size()) - firstMeshlet;
                most = std::max(most, static_cast<double>(cut));
                meshRanges.insert(meshRanges.end(), { firstMeshlet, cut, 0u, 0u });
            }
            const auto buffer = [&](const std::span<const std::byte> bytes) -> Held {
                auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 16)))
                    .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferDst | kor::Buffer::Usage::eTransferSrc)
                    .SetType(kor::Buffer::Type::eDeviceLocal).Build();
                if (!made.Valid()) return nullptr;
                if (!bytes.empty()) made->Write(bytes);
                return MakeResource(std::move(made));
            };
            Held meshletBuffer = buffer(std::as_bytes(std::span(meshlets))), vertexBuffer = buffer(std::as_bytes(std::span(vertices)));
            Held triangleBuffer = buffer(std::as_bytes(std::span(triangles))), rangeBuffer = buffer(std::as_bytes(std::span(meshRanges)));
            if (!meshletBuffer || !vertexBuffer || !triangleBuffer || !rangeBuffer) return Fail("its meshlets could not be made buffers");
            out.outputs["meshlets"].resource = std::move(meshletBuffer);
            out.outputs["vertices"].resource = std::move(vertexBuffer);
            out.outputs["triangles"].resource = std::move(triangleBuffer);
            out.outputs["meshRanges"].resource = std::move(rangeBuffer);
            out.outputs["count"].value = static_cast<double>(meshlets.size());
            out.outputs["most"].value = most;
            return {};
        }

        /** A RayScene: its bottom-level structures, made now; its top-level one, made now and whenever its instances change. */
        Expected<void> BuildRayScene(const Node& node, const Inputs& inputs, Built& out)
        {
            if (!kor::Context::SupportsRayTracing()) return Fail("this device has no ray tracing");
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return Fail("'meshes' is each element's mesh: wire the Filter's Mesh.mesh the MeshPack packs");
            const Node* filter = Origin(wired->second.link->node);
            if (!filter || !IsEach(filter->type)) return Fail("'meshes' comes from a Filter: its Mesh.mesh");
            const auto given = inputs.find("mesh");
            Held pack = given == inputs.end() ? nullptr : given->second.resource;
            while (pack && pack->replacedBy) pack = pack->replacedBy;
            if (!pack || pack->kind != "Mesh" || !pack->handle) return Fail("'mesh' is the meshes as one: the MeshPack's mesh");
            const auto rangesGiven = inputs.find("ranges");
            const auto* ranges = rangesGiven == inputs.end() ? nullptr : rangesGiven->second.value.Items();
            if (!ranges) return Fail("'ranges' is each mesh's part of it: the MeshPack's ranges");

            auto state = std::make_shared<RayState>();
            state->filter = filter->id;
            if (const auto it = inputs.find("masked"); it != inputs.end() && it->second.value.String() && !it->second.value.String()->empty() && world)
                state->masked = world->Find(*it->second.value.String());
            if (const auto it = inputs.find("noShadow"); it != inputs.end() && it->second.value.String() && !it->second.value.String()->empty() && world)
                state->unshadowing = world->Find(*it->second.value.String());
            const auto mesh = kor::capi::MeshOf(pack->handle);
            for (const auto& range : *ranges) {
                const auto field = [&](const char* name) { const Value* found = range.Find(name); return found && found->Number() ? *found->Number() : 0.0; };
                const auto indexCount = static_cast<kor::u64>(field("indexCount"));
                if (indexCount < 3) { state->bottoms.push_back(nullptr); continue; }
                kor::AccelerationStructure::Builder builder;
                builder.AddGeometry({ .mesh = mesh, .firstVertex = static_cast<kor::u64>(std::max(0.0, field("vertexOffset"))), .vertexCount = 0,
                                      .firstIndex = static_cast<kor::u64>(field("firstIndex")), .indexCount = indexCount });
                auto bottom = builder.Build();
                if (!bottom.Valid()) return Fail(bottom.Failure() ? bottom.Failure()->message : std::string("a mesh's structure could not be made"));
                state->bottoms.push_back(MakeResource(std::move(bottom)));
            }
            const auto buffer = [](const kor::u64 bytes, kor::ResourceRef<kor::Buffer>& written) -> Held {
                auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<kor::u64>(bytes, 16)))
                    .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eDynamic).Build();
                if (!made.Valid()) return nullptr;
                written = kor::ResourceRef<kor::Buffer>(made);
                return MakeResource(std::move(made));
            };
            state->scene = buffer(16, state->sceneWritten);
            const EntityList* list = built.contains(filter->id) ? built.at(filter->id).listed.get() : nullptr;
            state->meshOfRoom = std::max<std::size_t>(64, list ? list->entities.size() * 2 : 0);
            state->meshOf = buffer(state->meshOfRoom * 4, state->meshOfWritten);
            if (!state->scene || !state->meshOf) return Fail("its buffers could not be made");
            const std::uint32_t none[4] { ~0u, 0u, 0u, 0u };
            state->sceneWritten->Write(std::span<const std::uint32_t>(none));
            out.rays = state;
            UpdateRays(node.id, out, true);
            out.outputs["scene"].resource = state->scene;
            out.outputs["meshOf"].resource = state->meshOf;
            return {};
        }

        /** A RayScene's top-level structure made again, when its instances are not what they were: where each is, which, how many. */
        void UpdateRays(const std::string& id, Built& node, const bool building = false)
        {
            RayState& state = *node.rays;
            const auto found = built.find(state.filter);
            const EntityList* list = found == built.end() ? nullptr : found->second.listed.get();
            if (!list || !world) return;
            // What the instances are now, as one number.
            std::uint64_t now = 1469598103934665603ull;
            const auto mix = [&](const void* data, const std::size_t size) {
                const auto* bytes = static_cast<const unsigned char*>(data);
                for (std::size_t i = 0; i < size; ++i) now = (now ^ bytes[i]) * 1099511628211ull;
            };
            std::vector<kecs::Mat4> placed;
            placed.reserve(list->entities.size());
            for (std::size_t i = 0; i < list->entities.size(); ++i) {
                const kecs::Entity entity = list->entities[i];
                placed.push_back(world->Alive(entity) ? world->WorldMatrix(entity) : kecs::Mat4 {});
                mix(placed.back().m.data(), sizeof(float) * 16);
                const std::uint32_t group = list->grouped && i < list->groupOf.size() ? list->groupOf[i] : 0u;
                const bool masked = state.masked != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.masked);
                const bool unshadowing = state.unshadowing != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.unshadowing);
                mix(&group, sizeof group);
                mix(&masked, sizeof masked);
                mix(&unshadowing, sizeof unshadowing);
            }
            if (state.made && now == state.seen) return;
            state.seen = now;
            state.made = true;
            kor::AccelerationStructure::Builder builder;
            std::vector<std::uint32_t> meshOf(list->entities.size(), 0u);
            for (std::size_t i = 0; i < list->entities.size(); ++i) {
                const std::uint32_t group = list->grouped && i < list->groupOf.size() ? list->groupOf[i] : 0u;
                meshOf[i] = group;
                if (group >= state.bottoms.size() || !state.bottoms[group]) continue;
                kor::Mat4 transform;
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) transform[column][row] = placed[i].At(column, row);
                const kecs::Entity entity = list->entities[i];
                builder.AddInstance({ .blas = kor::capi::AccelerationStructureOf(state.bottoms[group]->handle), .transform = transform,
                                      .instanceCustomIndex = static_cast<kor::u32>(i), .hitGroupIndex = 0,
                                      .opaque = !(state.masked != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.masked)),
                                      .mask = static_cast<kor::u8>(state.unshadowing != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.unshadowing) ? 0x02 : 0x03) });
            }
            if (builder.instances.empty()) {
                // Nothing left to place: no scene, rather than the one of what was there before.
                node.outputs["count"].value = 0.0;
                if (!state.top) return;
                retiredResources.push_back(std::move(state.top));
                const std::uint32_t none[4] { ~0u, 0u, 0u, 0u };
                state.sceneWritten->Write(std::span<const std::uint32_t>(none));
                return;
            }
            auto top = builder.Build();
            if (!top.Valid()) return;
            if (state.top) retiredResources.push_back(std::move(state.top));
            state.top = MakeResource(std::move(top));
            const auto index = kor::Bindless::AccelerationStructure(kor::capi::AccelerationStructureOf(state.top->handle));
            const std::uint32_t scene[4] { index.Valid() ? index.index : ~0u, static_cast<std::uint32_t>(list->entities.size()), 0u, 0u };
            state.sceneWritten->Write(std::span<const std::uint32_t>(scene));
            // More instances than its row buffer has room for: the node is made again, with more.
            if (meshOf.size() > state.meshOfRoom) { node.signature.clear(); stale = true; }
            else if (!meshOf.empty()) state.meshOfWritten->Write(std::span<const std::uint32_t>(meshOf));
            node.outputs["structure"].resource = state.top;
            node.outputs["count"].value = static_cast<double>(list->entities.size());
            // What is wired to the structure itself is built again with the new one; what reads it by its index is not.
            const bool direct = current && std::ranges::any_of(current->nodes, [&](const Node& other) {
                return std::ranges::any_of(other.inputs, [&](const auto& input) { return input.second.link && input.second.link->node == id && input.second.link->output == "structure"; });
            });
            if (!building && direct) { node.stamp = ++counter; stale = true; }
        }

        /** An IndirectDraws' commands, as their bytes, and how many instances its groups have. */
        static Expected<std::pair<std::vector<std::byte>, std::size_t>> IndirectCommands(const Inputs& inputs, const bool gpu)
        {
            const auto value = [&](const char* name) -> const Value* { const auto it = inputs.find(name); return it == inputs.end() ? nullptr : &it->second.value; };
            const Value* rangesValue = value("ranges");
            const Value* groupsValue = value("groups");
            const auto* ranges = rangesValue ? rangesValue->Items() : nullptr;
            const auto* groups = groupsValue ? groupsValue->Items() : nullptr;
            if (!ranges) return Fail("'ranges' is each mesh's part of the mesh drawn: a MeshPack's ranges");
            if (!groups) return Fail("'groups' is how many of each to draw: a Filter's groups, grouped by Mesh");
            if (ranges->size() != groups->size())
                return Fail(std::format("there are {} meshes and {} groups: group the Filter by Mesh, and pack its Mesh.mesh", ranges->size(), groups->size()));
            const auto number = [](const Value& of, const char* name) { const Value* found = of.Find(name); return found && found->Number() ? *found->Number() : 0.0; };
            std::vector<std::byte> bytes(ranges->size() * 20, std::byte { 0 });
            std::size_t instances = 0;
            for (std::size_t i = 0; i < ranges->size(); ++i) {
                const auto count = static_cast<std::uint32_t>(number((*groups)[i], "count"));
                const std::uint32_t words[5] { static_cast<std::uint32_t>(number((*ranges)[i], "indexCount")), gpu ? 0u : count,
                                               static_cast<std::uint32_t>(number((*ranges)[i], "firstIndex")),
                                               std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(number((*ranges)[i], "vertexOffset"))),
                                               static_cast<std::uint32_t>(number((*groups)[i], "first")) };
                std::memcpy(bytes.data() + i * 20, words, 20);
                instances += count;
            }
            return std::pair { std::move(bytes), instances };
        }

        static bool OnTheGpu(const Inputs& inputs)
        {
            const auto it = inputs.find("mode");
            return it != inputs.end() && it->second.value.String() && *it->second.value.String() == "eGPU";
        }

        Expected<void> BuildIndirectDraws(const Node& node, const Inputs& inputs, Built& out)
        {
            if (const auto it = inputs.find("mode"); it != inputs.end() && !it->second.value.IsNull())
                if (const std::string* mode = it->second.value.String(); !mode || (*mode != "eCPU" && *mode != "eGPU")) return Fail("'mode' is eCPU or eGPU");
            const bool gpu = OnTheGpu(inputs);
            auto commands = IndirectCommands(inputs, gpu);
            if (!commands) return Fail(commands.error());
            auto& [bytes, instances] = *commands;
            // Written by the CPU each frame it changes, and read as a draw's commands, each frame in flight its own. On the
            // GPU's, one on the device: written in by a pass of its own each frame, for a pass after it to count into.
            auto buffer = gpu
                ? kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 20)))
                      .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eIndirect) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc
                                | kor::Buffer::Usage::eTransferDst)
                      .SetType(kor::Buffer::Type::eDeviceLocal).Build()
                : kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 20)))
                      .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eIndirect) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc)
                      .SetType(kor::Buffer::Type::eDynamic).SetIsPerFrame(true).Build();
            if (!buffer.Valid()) return Fail(buffer.Failure() ? buffer.Failure()->message : std::string("its buffer could not be made"));
            auto packed = std::make_shared<Packed>();
            packed->buffer = kor::ResourceRef<kor::Buffer>(buffer);
            packed->held = MakeResource(std::move(buffer));
            packed->held->node = node.id;
            if (!bytes.empty() && !gpu) packed->buffer->Write(std::span<const std::byte>(bytes));
            packed->bytes = std::move(bytes);
            packed->room = instances;
            packed->everyFrame = gpu;
            out.outputs["commands"].resource = packed->held;
            out.outputs["out"].resource = packed->held;     // what it holds, to be looked at: its commands
            out.outputs["drawCount"].value = static_cast<double>(packed->bytes.size() / 20);
            out.outputs["stride"].value = 20;
            if (gpu) {
                auto visible = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(instances, 1) * 4))
                    .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc | kor::Buffer::Usage::eTransferDst)
                    .SetType(kor::Buffer::Type::eDeviceLocal).Build();
                if (!visible.Valid()) return Fail("its visible list could not be made a buffer");
                Held held = MakeResource(std::move(visible));
                held->node = node.id;
                out.outputs["visible"].resource = std::move(held);
                // Its commands start at none every frame on the GPU, written as the frame is recorded — a pass of its
                // own, before anything counts into them. A buffer the CPU writes is copied to the other frames' copies
                // after, what was counted into it with it.
                auto plan = std::make_shared<PassPlan>();
                plan->uses.push_back({ MemberOf(packed->held.get()), false, true });
                auto now = std::make_shared<std::vector<std::byte>>(packed->bytes);
                plan->prepare = [packed, now] { *now = packed->bytes; };
                plan->record = [target = kor::ResourceRef<const kor::Buffer>(packed->buffer), now](kor::CommandBuffer& cb, const PassContext&) {
                    // An update a command buffer carries is of 64 KiB at most: written in as many as it takes.
                    constexpr std::size_t Most = 65536;
                    for (std::size_t at = 0; at < now->size(); at += Most)
                        cb.FillBuffer(target, now->data() + at, at, std::min(Most, now->size() - at));
                };
                out.pass = std::move(plan);
            }
            out.packed = std::move(packed);
            return {};
        }

        Expected<void> BuildPrimitive(const Node& node, const Inputs& inputs, Built& out)
        {
            static const std::string shapes = [] {
                std::string all;
                const auto& names = DefaultMeshNames();
                for (std::size_t i = 0; i < names.size(); ++i)
                    all += std::format("{}e{}{}", i == 0 ? "" : i + 1 == names.size() ? " or " : ", ", static_cast<char>(std::toupper(static_cast<unsigned char>(names[i][0]))), names[i].substr(1));
                return all;
            }();
            std::string shape = "eCube";
            if (const auto it = inputs.find("shape"); it != inputs.end() && !it->second.value.IsNull()) {
                if (!it->second.value.String()) return Fail(std::format("'shape' is {}", shapes));
                shape = *it->second.value.String();
            }
            // eCube is "cube": the name of the shape, as the default meshes have it.
            std::string name = shape.size() > 1 && shape[0] == 'e' ? shape.substr(1) : std::string();
            if (!name.empty()) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
            const auto data = ShapeData(name);
            if (!data) return Fail(std::format("'{}' is no shape: {}", shape, shapes));
            const VertexData vertices = ToVertexData(*data, DefaultTangents());
            auto resource = MeshOf(vertices);
            if (!resource) return Fail(resource.error());
            (*resource)->node = node.id;
            out.outputs["out"].resource = std::move(*resource);
            Value::List attributes;
            for (const auto& attribute : vertices.attributes)
                attributes.emplace_back(Value::Members { { "semantic", attribute.semantic }, { "count", static_cast<double>(attribute.count) } });
            auto layout = LayoutValue(Value(std::move(attributes)));
            if (!layout) return Fail(layout.error());
            out.outputs["layout"].value = std::move(*layout);
            return {};
        }

        /** An input given more than once, as its type lists it: `field[2].value` is `field[].value`. */
        static std::string Listed(std::string name)
        {
            if (const auto open = name.find('['); open != std::string::npos)
                if (const auto close = name.find(']', open); close != std::string::npos) name.erase(open + 1, close - open - 1);
            return name;
        }

        /**
         * What a node a Bundle gave inputs to says of it: the names it had that the node has no input for, and the inputs of
         * its kind the node has that it had not — what the expansion said, for a BufferBuilder, whose fields it knew.
         */
        std::string BundleNote(const Node& node, const Built& made) const
        {
            if (const auto said = bundleNotes.find(node.id); said != bundleNotes.end()) return said->second;
            const auto filled = fromBundle.find(node.id);
            if (filled == fromBundle.end() || filled->second.empty()) return {};
            const std::string prefix = filled->second.begin()->substr(0, filled->second.begin()->find('.') + 1);
            std::vector<Port> ports = made.ports;
            if (made.pipeline) ports.insert(ports.end(), made.pipeline->ports.begin(), made.pipeline->ports.end());
            std::string unmatched, missing;
            for (const auto& name : filled->second)
                if (std::ranges::find(ports, name, &Port::name) == ports.end()) unmatched += std::format("{}'{}'", unmatched.empty() ? "" : ", ", name.substr(prefix.size()));
            for (const auto& port : ports)
                if (port.name.starts_with(prefix) && !node.inputs.contains(port.name)) missing += std::format("{}'{}'", missing.empty() ? "" : ", ", port.name.substr(prefix.size()));
            std::string note;
            if (!unmatched.empty()) note = std::format("the bundle has {}, which it has not", unmatched);
            if (!missing.empty()) note += std::format("{}it has {}, which the bundle has not", note.empty() ? "" : "; ", missing);
            return note;
        }

        Expected<void> Build(const Node& node, const Inputs& inputs, Built& out)
        {
            const NodeType* type = FindType(node.type);
            if (!type) return Fail(std::format("there is no node type '{}'", node.type));
            // A Bundle is wiring, written out before anything runs: what makes one, or takes one apart, builds nothing.
            if (node.type == "Bundle" || node.type == "Unbundle") return {};
            if (type->builder) {
                return IsPipeline(type->name) ? BuildPipeline(node, *type->builder, inputs, out) : BuildBuilder(node, *type->builder, inputs, out);
            }
            // The engine's own: only the inputs its type lists.
            // A ShaderBuffer has its struct's fields besides: which, it says itself, once it has read its shader.
            const bool fields = IsMapped(node.type);
            // A Script node has its script's: its fields, and what each of its resources is made with — its to say.
            const bool script = node.type == "Script";
            // A Draw or a Dispatch has its pipeline's besides, which it says once it is built: and what it took before, to say where that went.
            const bool draws = node.type == "Draw" || node.type == "Dispatch" || node.type == "TraceRays";
            // A lens took a view and an extent before: what became of them is its to say.
            const bool lens = node.type == "Perspective" || node.type == "Orthographic" || node.type == "Frustum" || node.type == "Oblique" || node.type == "Paraboloid";
            const auto own = [&](const std::string& name) {
                return (draws && (name.starts_with("param.") || name.starts_with("push.") || name == "with" || name == "without"))
                    || (lens && (name == "view" || name == "extent")) || (node.type == "ForEach" && (name == "select" || name == "which" || name == "entity"));
            };
            for (const auto& name : inputs | std::views::keys)
                if (!(fields && name.starts_with("field.")) && !(script && name.find('.') != std::string::npos) && !own(name) && std::ranges::find(type->inputs, Listed(name), &Port::name) == type->inputs.end())
                    return Fail(std::format("a {} has no input '{}'", type->name, name));
            if (type->category == "Passes") {
                const auto truthy = [](const Value& value) { return value.Bool() ? *value.Bool() : value.Number() && *value.Number() != 0.0; };
                // Left out: there is the node, and no pass of it.
                const auto enabled = inputs.find("enabled");
                if (enabled != inputs.end() && !truthy(enabled->second.value)) return {};
                // Left out of the views it is not for: the game's, or an editor's scene view.
                if (const auto views = inputs.find("views"); views != inputs.end() && views->second.value.String()) {
                    const std::string& which = *views->second.value.String();
                    if (which != "all" && which != "game" && which != "editor") return Fail(std::format("'views' is all, game or editor, not '{}'", which));
                    if ((which == "game" && editorView) || (which == "editor" && !editorView)) return {};
                }
                const auto async = inputs.find("async");
                const bool onAsync = async != inputs.end() && truthy(async->second.value);
                Inputs rest = inputs;
                rest.erase("enabled");
                rest.erase("async");
                rest.erase("views");
                auto built = BuildPass(node, rest, out);
                if (built && onAsync && out.pass) {
                    out.pass->async = true;
                    if (!kor::Context::SupportsAsyncCompute()) out.status.note = "async: this device has no second queue, so it runs on the graphics queue, in order";
                    else if (out.pass->readsScreen || out.pass->writesScreen) out.status.note = "async: it uses the screen, so it stays on the graphics queue";
                }
                return built;
            }
            if (node.type == "Note") return {};     // words: there is nothing of it to build
            if (node.type == "DebugView") {
                const auto picture = inputs.find("picture");
                if (picture == inputs.end() || !picture->second.resource) return Fail("'picture' is the picture shown in its place: wire an image to it");
                out.outputs["out"].resource = picture->second.resource;
                out.outputs["shown"].value = Value(node.id == debugView);
                return {};
            }
            if (node.type == "Switch") {
                const auto index = inputs.find("index");
                const Value chosen = index == inputs.end() ? Value(0) : index->second.value;
                const int which = chosen.Bool() ? (*chosen.Bool() ? 1 : 0) : chosen.Number() ? static_cast<int>(std::llround(*chosen.Number())) : -1;
                const auto option = inputs.find(std::format("option[{}]", which));
                if (option == inputs.end()) return Fail(std::format("'index' is {}, and there is no 'option[{}]' wired", chosen.ToJson(), which));
                out.outputs["out"].value = option->second.value;
                out.outputs["out"].resource = option->second.resource;
                return {};
            }
            if (node.type == "Screen") {
                auto screen = std::make_shared<Resource>();
                screen->kind = "Image";
                screen->node = node.id;
                screen->screen = true;
                out.outputs["out"].resource = std::move(screen);
                // How big the window is: what an image that is to be shown in it, a pixel for a pixel, is sized by.
                const kor::UVec2 size { std::max(screenExtent.x, 1u), std::max(screenExtent.y, 1u) };
                out.outputs["extent"].value = Value(Value::List { size.x, size.y, 1 });
                out.outputs["size"].value = Value(Value::List { size.x, size.y });
                out.outputs["width"].value = size.x;
                out.outputs["height"].value = size.y;
                out.outputs["aspect"].value = static_cast<double>(size.x) / static_cast<double>(size.y);
                out.outputs["format"].value = screenFormat;
                out.outputs["scale"].value = static_cast<double>(screenScale);
                return {};
            }
            if (ReadsTheWorld(node.type)) return BuildLive(node, inputs, out);
            if (node.type == "WriteBack") {
                // Checked here; what it does, it does each frame (Runtime::WriteBack).
                const auto buffer = inputs.find("buffer");
                if (buffer == inputs.end() || !BufferRefOf(buffer->second.resource.get()).Get()) return Fail("'buffer' needs a buffer wired in: what is read back");
                const auto each = inputs.find("forEach");
                const std::string* id = each == inputs.end() ? nullptr : each->second.value.String();
                const Node* forEach = id && current ? current->Find(*id) : nullptr;
                if (!forEach || !IsEach(forEach->type)) return Fail("'forEach' is the id of the ForEach whose elements the buffer's are");
                const auto stride = inputs.find("stride");
                const double* bytes = stride == inputs.end() ? nullptr : stride->second.value.Number();
                if (!bytes || *bytes < 4) return Fail("'stride' is how many bytes an element is: 4 or more");
                for (const char* name : { "position", "rotation" })
                    if (const auto at = inputs.find(name); at != inputs.end() && !at->second.value.IsNull()) {
                        const double* offset = at->second.value.Number();
                        if (!offset || *offset < 0 || *offset + (name[0] == 'p' ? 12 : 16) > *bytes) return Fail(std::format("'{}' is where in an element it is, in bytes, within its stride", name));
                    }
                if (const auto fields = inputs.find("fields"); fields != inputs.end() && !fields->second.value.IsNull()) {
                    const auto* members = fields->second.value.Named();
                    if (!members) return Fail("'fields' is { \"Component.field\": offset }, each where it is in an element");
                    for (const auto& [name, offset] : *members)
                        if (name.find('.') == std::string::npos || !offset.Number()) return Fail(std::format("'fields' has '{}': each is `Component.field`, and where it is in bytes", name));
                }
                return {};
            }
            if (node.type == "MeshBuffers") {
                // A mesh's buffers, as buffers: where its positions are, and its indices — a part's, of its whole's.
                const auto given = inputs.find("mesh");
                Held mesh = given == inputs.end() ? nullptr : given->second.resource;
                while (mesh && mesh->replacedBy) mesh = mesh->replacedBy;
                if (!mesh || mesh->kind != "Mesh") return Fail("'mesh' needs a mesh wired in: a Primitive, an entity's, a model's");
                if (mesh->pending) return Fail("its mesh is still being read");
                const Resource* whole = mesh->part ? mesh->source.get() : mesh.get();
                if (!whole || !whole->handle || whole->buffers.empty()) return Fail("its mesh has no buffers of its own to read: one made another way");
                const auto device = kor::capi::MeshOf(whole->handle);
                const auto& layout = device->Layout();
                const auto position = std::ranges::find(layout.attributes, std::string_view("POSITION"), [](const auto& a) { return std::string_view(a.semantic); });
                if (position == layout.attributes.end() || position->binding >= whole->buffers.size()) return Fail("its mesh has no POSITION");
                const auto binding = std::ranges::find(layout.bindings, position->binding, &kor::VertexInputBindingDescription::binding);
                const bool indexed = device->IndexBuffer().has_value();
                out.outputs["vertices"].resource = whole->buffers[position->binding];
                if (indexed) out.outputs["indices"].resource = whole->buffers.back();
                out.outputs["stride"].value = static_cast<double>(binding == layout.bindings.end() ? 0 : binding->stride);
                out.outputs["position"].value = static_cast<double>(position->offset);
                out.outputs["vertexCount"].value = static_cast<double>(mesh->part ? mesh->vertexCount : device->VertexCount());
                out.outputs["indexCount"].value = static_cast<double>(mesh->part ? mesh->indexCount : indexed ? device->IndexCount().value_or(0) : 0);
                out.outputs["firstIndex"].value = static_cast<double>(mesh->part ? mesh->firstIndex : 0);
                out.outputs["vertexOffset"].value = static_cast<double>(mesh->part ? mesh->vertexOffset : 0);
                // And where its normals and its first UVs are, when it has them: what a mesh shader or a hit shader reads
                // of a vertex besides where it is.
                for (const auto& [semantic, name] : { std::pair { "NORMAL", "normals" }, std::pair { "UV", "uvs" } }) {
                    const auto it = std::ranges::find(layout.attributes, std::string_view(semantic), [](const auto& a) { return std::string_view(a.semantic); });
                    if (it == layout.attributes.end() || it->binding >= whole->buffers.size()) continue;
                    const auto bound = std::ranges::find(layout.bindings, it->binding, &kor::VertexInputBindingDescription::binding);
                    out.outputs[name].resource = whole->buffers[it->binding];
                    out.outputs[std::format("{}Stride", name)].value = static_cast<double>(bound == layout.bindings.end() ? 0 : bound->stride);
                    out.outputs[std::format("{}Offset", name)].value = static_cast<double>(it->offset);
                }
                return {};
            }
            if (node.type == "MeshPack") return BuildMeshPack(node, inputs, out);
            if (node.type == "Meshlets") return BuildMeshlets(inputs, out);
            if (node.type == "RayScene") return BuildRayScene(node, inputs, out);
            if (node.type == "IndirectDraws") return BuildIndirectDraws(node, inputs, out);
            if (node.type == "ShaderBuffer") return BuildShaderBuffer(node, inputs, out);
            if (node.type == "BufferBuilder") return BuildBufferBuilder(node, inputs, out);
            if (node.type == "Collect") { CollectAll(node, out); return {}; }
            if (node.type == "Primitive") return BuildPrimitive(node, inputs, out);
            if (node.type == "Script") return BuildScript(node, inputs, out);
            if (node.type == "ImageFile") {
                const auto file = inputs.find("file");
                const std::string* name = file == inputs.end() ? nullptr : file->second.value.String();
                if (!name || name->empty()) return Fail("'file' is the picture's file: under the project's assets folder, or by its whole path");
                const Held held = Asset(AssetId(*name, -1));
                if (!held) return Fail(std::format("'{}' could not be read as a picture", AssetFile(*name).generic_string()));
                out.outputs["out"].resource = held;
                return {};
            }
            if (node.type == "Reference") {
                const auto it = inputs.find("node");
                const std::string* target = it == inputs.end() ? nullptr : it->second.value.String();
                if (!target) return Fail("'node' is the id of the node it stands for");
                const auto found = built.find(*target);
                if (found == built.end()) return Fail(std::format("there is no node '{}'", *target));
                if (!found->second.status.ok) return Fail(std::format("'{}', which it stands for, failed", *target));
                out.outputs = found->second.outputs;
                return {};
            }
            return BuildValue(node, inputs, out);
        }

        /**
         * What a node gives besides what it makes: everything about it that another node might want. Each value it
         * was given, under the input's name; the parts of one thing as one (`extent` of `extent.x`, `.y`, `.z`); what
         * an input is when it is not given; and what the object it made says of itself — an image's size, a buffer's.
         */
        static void AddOutputs(const Node& node, const Inputs& inputs, Built& out)
        {
            const auto add = [&](const std::string& name, Value value) {
                if (!out.outputs.contains(name)) out.outputs[name].value = std::move(value);
            };
            // What the object says of itself comes first: it is what is so, whatever was asked for.
            if (const auto made = out.outputs.find("out"); made != out.outputs.end() && made->second.resource && made->second.resource->handle) {
                const Resource& resource = *made->second.resource;
                if (const auto image = ImageRefOf(&resource); image.Get()) {
                    const auto extent = image->Extent();
                    add("extent", Value(Value::List { extent.x, extent.y, extent.z }));
                    add("size", Value(Value::List { extent.x, extent.y }));
                    add("width", extent.x);
                    add("height", extent.y);
                    add("depth", extent.z);
                    add("mipLevels", image->MipLevels());
                    add("arrayLayers", image->ArrayLayers());
                } else if (const auto buffer = BufferRefOf(&resource); buffer.Get()) {
                    add("bytes", static_cast<double>(buffer->size()));
                } else if (resource.kind == "Framebuffer") {
                    const auto extent = kor::capi::FramebufferOf(resource.handle)->Extent();
                    add("size", Value(Value::List { extent.x, extent.y }));
                    add("width", extent.x);
                    add("height", extent.y);
                } else if (resource.kind == "Mesh") {
                    const auto mesh = kor::capi::MeshOf(resource.handle);
                    add("vertexCount", static_cast<double>(mesh->VertexCount()));
                    add("indexCount", static_cast<double>(mesh->IndexCount().value_or(0)));
                }
            }
            const NodeType* type = FindType(node.type);
            const auto ports = type ? PortsFor(*type, node) : std::vector<Port> {};
            for (const auto& [name, input] : inputs)
                if (!input.resource && !input.value.IsNull()) add(name, input.value);
            for (const auto& port : ports)
                if (port.kind == PortKind::eValue && !port.initial.IsNull()) add(port.name, port.initial);
            // Parts as a whole.
            std::map<std::string, Value::List> wholes;
            for (const auto& port : ports) {
                if (port.name.size() < 3 || port.name[port.name.size() - 2] != '.') continue;
                const char part = port.name.back();
                if (part != 'x' && part != 'y' && part != 'z' && part != 'w') continue;
                const auto value = out.outputs.find(port.name);
                if (value == out.outputs.end() || !value->second.value.Number()) continue;
                wholes[port.name.substr(0, port.name.size() - 2)].push_back(value->second.value);
            }
            for (auto& [name, parts] : wholes) add(name, Value(std::move(parts)));
        }

        /**
         * An input given whole is given in its parts: `groups` of [8, 8, 1] is `groups.x`, `.y` and `.z`, which is how
         * everything after this reads it. As many parts as the vector has numbers: an image's size, of two, leaves z
         * as it is when not given.
         */
        static Expected<void> InParts(const Node& node, Inputs& inputs)
        {
            const NodeType* type = FindType(node.type);
            if (!type) return {};
            for (auto it = inputs.begin(); it != inputs.end();) {
                const auto parts = PartsOf(*type, it->first);
                if (parts.empty()) { ++it; continue; }
                for (const auto& part : parts)
                    if (inputs.contains(part)) return Fail(std::format("'{}' is given whole, and '{}' on its own as well: one or the other", it->first, part));
                // Left open for a wire, and none in it yet: not given.
                if (it->second.value.IsNull() && !it->second.resource) { it = inputs.erase(it); continue; }
                const auto numbers = NumbersOf(it->second.value, it->first.c_str());
                if (!numbers || it->second.resource) return Fail(std::format("'{}' is a vector: a number for each of x, y and z", it->first));
                for (std::size_t i = 0; i < parts.size() && i < numbers->size(); ++i) {
                    Resolved part;
                    part.value = (*numbers)[i];
                    part.linked = it->second.linked;
                    inputs.emplace(parts[i], std::move(part));
                }
                it = inputs.erase(it);
            }
            return {};
        }

        /** A struct given some of its fields has the rest as the node type says they are, not as nothing. */
        static void FillDefaults(const Node& node, Inputs& inputs)
        {
            const NodeType* type = FindType(node.type);
            if (!type) return;
            for (auto& [name, input] : inputs) {
                const Value::Members* given = input.value.Named();
                if (!given || input.resource) continue;
                // One given more than once is listed once: `color[1].clear` is `color[].clear`.
                std::string listed = name;
                if (const auto open = listed.find('['); open != std::string::npos)
                    if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
                const auto port = std::ranges::find(type->inputs, listed, &Port::name);
                const Value::Members* defaults = port == type->inputs.end() ? nullptr : port->initial.Named();
                if (!defaults) continue;
                Value::Members merged = *given;
                for (const auto& [field, value] : *defaults)
                    if (std::ranges::find_if(merged, [&](const auto& m) { return m.first == field; }) == merged.end()) merged.emplace_back(field, value);
                // So does each struct of a list in it: a blend attachment given its factors still writes every channel, as
                // the framework's has it, not none.
                static const std::map<std::string, Value::Members, std::less<>> itemDefaults {
                    { "colorBlendState.attachments", { { "blendEnable", false }, { "srcColorBlendFactor", "eOne" }, { "dstColorBlendFactor", "eZero" },
                                                       { "colorBlendOp", "eAdd" }, { "srcAlphaBlendFactor", "eOne" }, { "dstAlphaBlendFactor", "eZero" },
                                                       { "alphaBlendOp", "eAdd" }, { "colorWriteMask", Value::List { "eR", "eG", "eB", "eA" } } } },
                };
                for (auto& [field, value] : merged) {
                    const auto items = itemDefaults.find(listed + "." + field);
                    if (items == itemDefaults.end() || !value.Items()) continue;
                    Value::List completed;
                    for (const Value& item : *value.Items()) {
                        Value::Members each = item.Named() ? *item.Named() : Value::Members {};
                        for (const auto& [name, initial] : items->second)
                            if (std::ranges::find_if(each, [&](const auto& m) { return m.first == name; }) == each.end()) each.emplace_back(name, initial);
                        completed.push_back(item.Named() ? Value(std::move(each)) : item);
                    }
                    value = Value(std::move(completed));
                }
                input.value = Value(std::move(merged));
            }
        }

        // ---- the whole graph ----------------------------------------------------------------------------------

        void Evaluate(const Graph& graph)
        {
            // Nodes that are gone take what they made with them.
            for (auto it = built.begin(); it != built.end();) {
                if (graph.Find(it->first)) { ++it; continue; }
                Unwatch(it->second);
                Retire(std::move(it->second));
                it = built.erase(it);
            }

            current = &graph;
            currentById.clear();
            currentById.reserve(graph.nodes.size());
            for (const Node& each : graph.nodes) currentById.emplace(each.id, &each);
            // What is used as what, by nodes that are gone, is not.
            for (auto it = usedAsBy.begin(); it != usedAsBy.end();) {
                std::erase_if(it->second, [&](const auto& by) { return by.first != EntitiesUse && !currentById.contains(by.first); });
                it = currentById.contains(it->first) ? std::next(it) : usedAsBy.erase(it);
            }
            const Using using_(*this, std::string(EntitiesUse));
            struct Evaluating { bool& now; bool was; ~Evaluating() { now = was; } } const evaluatingNow { evaluating, std::exchange(evaluating, true) };
            takenByPipeline.reset();
            // What each node waits for: what it is wired to, and what it stands for.
            std::map<std::string, std::vector<std::string>> needs;
            for (const auto& node : graph.nodes) {
                auto& list = needs[node.id];
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link) list.push_back(input.link->node);
                if (node.type == "Reference")
                    if (const auto it = node.inputs.find("node"); it != node.inputs.end() && it->second.value.String()) list.push_back(*it->second.value.String());
                // A ForEach of a pipeline's component — of it, or `with` it — is after the pipeline that makes the component.
                if (IsEach(node.type)) {
                    std::vector<std::string> named;
                    if (const auto it = node.inputs.find("component"); it != node.inputs.end() && it->second.value.String()) named.push_back(*it->second.value.String());
                    if (const auto it = node.inputs.find("with"); it != node.inputs.end() && it->second.value.Items())
                        for (const auto& each : *it->second.value.Items()) if (each.String()) named.push_back(*each.String());
                    for (const auto& name : named)
                        if (const Node* other = graph.Find(name); other && other->id != node.id && IsPipeline(other->type)) list.push_back(name);
                }
            }
            std::vector<const Node*> order;
            std::set<std::string> done, visiting, circular;
            const std::function<void(const Node&)> visit = [&](const Node& node) {
                if (done.contains(node.id)) return;
                if (!visiting.insert(node.id).second) { circular.insert(node.id); return; }
                for (const auto& other : needs[node.id])
                    if (const Node* found = graph.Find(other)) visit(*found);
                visiting.erase(node.id);
                done.insert(node.id);
                order.push_back(&node);
            };
            for (const auto& node : graph.nodes) visit(node);

            for (const Node* node : order) {
                Built& out = built[node->id];
                const auto fail = [&](std::string why) {
                    // Failed for another reason than last time, or for the first: what was built is let go of.
                    Unwatch(out);
                    Retire(Built { .outputs = std::move(out.outputs), .pipeline = out.pipeline, .pass = std::move(out.pass) });
                    out.outputs.clear();
                    out.pass.reset();
                    out.status.ok = false;
                    out.status.error = std::move(why);
                    out.signature.clear();
                    out.stamp = ++counter;
                };
                if (circular.contains(node->id)) { fail("it is wired, through other nodes, to itself"); continue; }

                // What goes into it, and a signature of that: its own values, and for each wire which build of what.
                Inputs inputs;
                std::string signature = node->type;
                std::string blocked;
                for (const auto& [name, input] : node->inputs) {
                    Resolved resolved;
                    if (input.link) {
                        resolved.linked = true;
                        const auto source = built.find(input.link->node);
                        if (!graph.Find(input.link->node) || source == built.end()) { blocked = std::format("'{}' is wired to '{}', which is not in the graph", name, input.link->node); break; }
                        if (!source->second.status.ok) { blocked = std::format("'{}' comes from '{}', which failed", name, input.link->node); break; }
                        const auto output = source->second.outputs.find(input.link->output);
                        if (output == source->second.outputs.end()) { blocked = std::format("'{}' has no output '{}'", input.link->node, input.link->output); break; }
                        resolved.value = output->second.value;
                        resolved.resource = output->second.resource;
                        signature += std::format("|{}=@{}.{}#{}", name, input.link->node, input.link->output, source->second.stamp);
                    } else {
                        resolved.value = input.value;
                        signature += std::format("|{}={}", name, input.value.ToJson());
                    }
                    inputs.emplace(name, std::move(resolved));
                }
                if (node->type == "Screen") signature += std::format("|{}x{}", screenExtent.x, screenExtent.y);
                // A Random is of the shape of what it is wired to: drawn again when that is another.
                if (random::DistributionOfNode(node->type)) {
                    const Shape shape = RandomShape(*node);
                    randomShapes[node->id] = shape;
                    signature += "|as " + ShapeName(shape);
                }
                // An Image or a Buffer is made again when it is used as what it was neither given nor made for, or no longer is.
                if (MadeForItsUse(node->type)) signature += std::format("|for {}", UsageFor(node->id) & ~GivenUsage(node->type, inputs));
                // What the view looks at the scene by: built again when that changes.
                if (node->type == "DebugView" && node->id == debugView) signature += "|shown";
                if (node->inputs.contains("views")) signature += editorView ? "|editor view" : "|game view";
                if (shaderDebug != 0 && out.pipeline && std::ranges::find(out.pipeline->ports, std::format("constant.{}", ShaderDebugConstant), &Port::name) != out.pipeline->ports.end())
                    signature += std::format("|{}={}", ShaderDebugConstant, shaderDebug);
                // A Script node is the script as it was set: made again when the script is set again — edited, loaded, run.
                if (node->type == "Script")
                    if (const auto it = node->inputs.find("script"); it != node->inputs.end() && it->second.value.String())
                        if (const Scripted* script = ScriptNamed(*it->second.value.String())) signature += std::format("|script#{}", script->generation);
                // A ForEach gives what its components hold: made again when one of them is another shape — a pipeline's, reshaped just now.
                if (IsEach(node->type) && world) {
                    const auto shape = [&](const std::string& name) {
                        if (const kecs::ComponentId id = world->Find(name); id != kecs::NoComponent)
                            for (const auto& field : world->Describe(id).fields) signature += std::format("|{}.{}:{}@{}", name, field.name, field.type, field.offset);
                    };
                    if (const auto it = node->inputs.find("component"); it != node->inputs.end() && it->second.value.String()) shape(*it->second.value.String());
                    if (const auto it = node->inputs.find("with"); it != node->inputs.end() && it->second.value.Items())
                        for (const auto& each : *it->second.value.Items()) if (each.String()) shape(*each.String());
                }
                // A pipeline's component is what its draws take from nobody else: made again when that changes.
                if (IsPipeline(node->type))
                    for (const auto& name : TakenElsewhere(node->id)) signature += "|taken:" + name;
                if (node->type == "Reference")
                    if (const auto it = node->inputs.find("node"); it != node->inputs.end() && it->second.value.String())
                        if (const auto target = built.find(*it->second.value.String()); target != built.end()) signature += std::format("|#{}", target->second.stamp);
                // A buffer the source's is shared: the one it built, taken again whenever it builds another.
                const Built* theirs = source && node->type == "Buffer" && shared.contains(node->id) ? source->Find(node->id) : nullptr;
                if (theirs && !theirs->status.ok) theirs = nullptr;
                if (theirs) signature += std::format("|shared#{}", theirs->stamp);
                if (!blocked.empty()) { fail(std::move(blocked)); continue; }
                if (out.status.ok && out.signature == signature) continue;   // nothing that goes into it changed
                if (const auto parts = InParts(*node, inputs); !parts) { fail(parts.error()); continue; }
                FillDefaults(*node, inputs);

                Unwatch(out);
                Built fresh;
                fresh.status.builds = out.status.builds + 1;
                if (theirs) fresh.outputs = theirs->outputs;
                // What it uses resources as is told again as it is built: what it no longer uses one as, it is not made for.
                for (auto& by : usedAsBy | std::views::values) by.erase(node->id);
                const auto result = [&] {
                    const Using using_(*this, node->id);
                    return theirs ? Expected<void> {} : Build(*node, inputs, fresh);
                }();
                if (!result) {
                    const auto pipeline = fresh.pipeline;   // its inputs are still worth knowing
                    Unwatch(fresh);
                    fail(result.error());
                    out.status.builds = fresh.status.builds;
                    out.pipeline = pipeline;
                    out.ports = std::move(fresh.ports);
                    out.gives = std::move(fresh.gives);
                    continue;
                }
                AddOutputs(*node, inputs, fresh);
                fresh.status.ok = true;
                if (const auto note = BundleNote(*node, fresh); !note.empty()) fresh.status.note = note;
                if (const auto why = unbatched.find(node->id); why != unbatched.end()) fresh.status.note = why->second;
                if (batched.contains(node->id) && fresh.pass && fresh.pass->entities && fresh.pass->entities->instances.empty())
                    fresh.status.note = "batched, and its shader has no [entity(\"PER_INSTANCE\")] buffer: every element is drawn with the same push constants";
                fresh.signature = std::move(signature);
                if (IsMadeResource(node->type)) fresh.madeWith = WiredValues(*node);
                fresh.stamp = ++counter;
                if (!fresh.migration) fresh.migration = std::move(out.migration);
                // What it made before is not what it makes now: an entity holding the old one is led to the new.
                for (const auto& [name, output] : out.outputs) {
                    if (!output.resource || output.resource->node != node->id) continue;
                    const auto now = fresh.outputs.find(name);
                    if (now != fresh.outputs.end() && now->second.resource && now->second.resource != output.resource) { output.resource->replacedBy = now->second.resource; ++replacements; }
                }
                Retire(std::move(out));
                out = std::move(fresh);
            }
            // An Image or a Buffer found used as what it was not made for, or no longer used as what it was: made again
            // now, and what uses it, so that nothing waits a frame on it. Each time round what it is used as only grows,
            // unless what uses it is built otherwise; a few times round is all it takes.
            const auto remade = [&](const Node* node) {
                const auto it = built.find(node->id);
                return MadeForItsUse(node->type) && !shared.contains(node->id) && it != built.end() && it->second.status.ok && it->second.madeFor != (it->second.givenUsage | UsageFor(node->id));
            };
            // A Random built before what it is wired to said what it takes — a pipeline built after it — is drawn again, as that.
            const auto reshaped = [&](const Node* node) {
                if (!random::DistributionOfNode(node->type)) return false;
                const auto was = randomShapes.find(node->id);
                return was != randomShapes.end() && was->second != RandomShape(*node);
            };
            if (remadeRounds < 4 && (std::ranges::any_of(order, remade) || std::ranges::any_of(order, reshaped))) {
                ++remadeRounds;
                Evaluate(graph);
                --remadeRounds;
                return;
            }

            // What follows the world from here on, each frame: the nodes that read it, and what is worked out of them.
            live.clear();
            sized.clear();
            std::set<std::string> following;
            for (const Node* node : order) {
                const NodeType* type = FindType(node->type);
                // The main view follows whoever runs the graph: an editor's view moves every frame.
                const auto main = node->inputs.find("main");
                const bool lens = node->type == "View" || node->type == "Perspective" || node->type == "Orthographic" || node->type == "Frustum";
                bool follows = ReadsTheWorld(node->type) || (lens && main != node->inputs.end() && main->second.value.Bool() && *main->second.value.Bool());
                const bool carries = type && !type->builder && (IsMapped(node->type) || node->type == "Reference"
                                                                || node->type == "MeshPack" || node->type == "IndirectDraws"
                                                                || (type->category != "Passes" && type->category != "Resources" && type->category != "Scene"));
                const bool given = std::ranges::any_of(needs[node->id], [&](const std::string& other) { return following.contains(other); });
                if (!follows && carries) follows = given;
                // A buffer or an image is made once, of what it is given then: one given what follows the world — a count
                // of entities — is watched, and made again when that changes.
                if (!follows && given && IsMadeResource(node->type)) sized.push_back(node->id);
                if (!follows) continue;
                following.insert(node->id);
                live.push_back(node->id);
            }
            // What follows the world for setup alone, from the buffers on setup pages back: what nothing but those
            // takes. Thousands of entities each worked out every frame, for a buffer setup copies now and then, is
            // most of a frame.
            setupOnly.clear();
            {
                std::map<std::string, std::vector<std::string>, std::less<>> takers;
                for (const auto& [id, list] : needs)
                    for (const auto& other : list) takers[other].push_back(id);
                for (auto it = live.rbegin(); it != live.rend(); ++it) {
                    const Node* node = graph.Find(*it);
                    if (!node) continue;
                    const bool laidOut = IsMapped(node->type);
                    const auto taken = takers.find(*it);
                    const bool forSetup = laidOut ? graph.IsSetup(node->pass)
                        : taken != takers.end() && !taken->second.empty()
                          && std::ranges::all_of(taken->second, [&](const std::string& taker) { return setupOnly.contains(taker); });
                    if (forSetup) setupOnly.insert(*it);
                }
            }
            structureSeen = ~0ull;
            pointShadowsDrawn.clear();
            Install(graph);
        }

        // ---- each frame: the entities ---------------------------------------------------------------------------

        /** A value as a push constant's bytes: as many numbers as it takes, each of the shader's kind, and zero for the rest. */
        static void PackPush(const PushMember& push, const Value& value, std::byte* at)
        {
            std::vector<double> numbers;
            if (const bool* flag = value.Bool()) numbers = { *flag ? 1.0 : 0.0 };
            else if (auto read = NumbersOf(value, "push")) numbers = std::move(*read);
            const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(push.rows) * push.columns, push.size / 4);
            const std::size_t stride = push.columns > 1 && push.size >= push.columns * 16 && push.rows < 4 ? 16 : push.rows * 4;     // a matrix's columns
            for (std::size_t i = 0; i < count; ++i) {
                const double number = i < numbers.size() ? numbers[i] : 0.0;
                std::byte* to = at + (push.columns > 1 ? (i / push.rows) * stride + (i % push.rows) * 4 : i * 4);
                if (push.scalar == 0) { const auto v = static_cast<float>(number); std::memcpy(to, &v, 4); }
                else if (push.scalar == 1) { const auto v = static_cast<std::int32_t>(std::llround(number)); std::memcpy(to, &v, 4); }
                else { const auto v = static_cast<std::uint32_t>(std::llround(std::max(number, 0.0))); std::memcpy(to, &v, 4); }
            }
        }

        /** What is said of a draw given nothing for a resource: of the entity's field, when that is where it comes from. */
        static std::string Lacks(const Source& source)
        {
            return std::format("'{}' has nothing in it", source.kind == Source::Kind::eField ? source.field.name : source.input);
        }

        /**
         * A buffer of each instance's, for a Draw that is given nothing for it: made here, with room for more than there
         * are, and filled each frame from the entities of the Filter whose meshes the Draw's MeshPack packs — in the order
         * the pack's indirect commands draw them. A TraceRays' is of the Filter wired to its `entities`, in its order: the
         * InstanceID() a RayScene of the same Filter gives each.
         */
        Expected<Source> InstancesFor(const Node& draw, const PipelineInfo& info, const Slot& slot, EntityRun& run)
        {
            const std::string port = "param." + slot.field;
            if (!world) return Fail(std::format("'{}' is each instance's, and there is no world for there to be instances of", port));
            if (!run.each.empty()) return Fail(draw.type == "Dispatch" ? std::format("'{}' is each instance's, and this runs once an element of '{}': wire 'entities' a Filter's instead, for one dispatch of them all, or wire it", port, run.each)
                                                                 : std::format("'{}' is each instance's, and this draws one element of '{}' at a time: set 'batched', draw a MeshPack's mesh, or wire it", port, run.each));
            const bool rays = draw.type == "TraceRays" || draw.type == "Dispatch";     // of the Filter wired to `entities`
            const Node* filter = nullptr;
            if (rays) {
                if (const auto entities = draw.inputs.find("entities"); entities != draw.inputs.end() && entities->second.link) filter = Origin(entities->second.link->node);
            } else {
                const auto mesh = draw.inputs.find("mesh");
                const Node* pack = mesh != draw.inputs.end() && mesh->second.link ? Origin(mesh->second.link->node) : nullptr;
                if (pack && pack->type == "MeshPack")
                    if (const auto meshes = pack->inputs.find("meshes"); meshes != pack->inputs.end() && meshes->second.link) filter = Origin(meshes->second.link->node);
            }
            const auto found = filter ? built.find(filter->id) : built.end();
            if (found == built.end() || !found->second.status.ok || !found->second.listed)
                return Fail(rays ? std::format("'{}' is each instance's: they are the entities wired to 'entities', a Filter's — or wire it", port)
                                 : std::format("'{}' is each instance's: they are the entities of the Filter whose Mesh.mesh the 'mesh' MeshPack packs — or wire it", port));

            // Another Draw's, when it would write the same: one buffer, written once a frame, for all of them.
            const std::string key = InstancesKey(filter->id, *slot.instancing, info.component);
            std::erase_if(sharedInstances, [](const auto& entry) { return entry.second.expired(); });
            // A runtime that follows another — an editor's Game panel, the same graph over the same world — has the one
            // that runtime made: the same entities, written once a frame for both.
            std::shared_ptr<Instances> theirs;
            for (const Impl* owner : { source, this })
                if (owner && !theirs)
                    if (const auto shared = owner->sharedInstances.find(key); shared != owner->sharedInstances.end())
                        if (auto found = shared->second.lock(); found && !found->full) theirs = std::move(found);
            // Compacted: what its shader reads is another buffer as big, of what is in sight one after another, which a
            // cull before it writes (CullInSight's `drawn`); the buffer of instances is filled all the same, for the cull.
            const auto compacted = draw.inputs.find("compacted");
            const bool compacts = !rays && compacted != draw.inputs.end() && compacted->second.value.Bool() && *compacted->second.value.Bool();
            const auto bound = [&](const Instances& instances) -> Expected<Source> {
                Source source;
                source.kind = Source::Kind::eWired;
                source.input = port;
                source.resource = instances.held;
                if (compacts) {
                    auto drawn = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(instances.capacity * instances.instancing->stride))
                        .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc | kor::Buffer::Usage::eTransferDst)
                        .SetType(kor::Buffer::Type::eDeviceLocal).Build();
                    if (!drawn.Valid()) return Fail(drawn.Failure() ? drawn.Failure()->message : std::format("'{}' could not be made a buffer of what is in sight", port));
                    run.drawn = MakeResource(std::move(drawn));
                    run.drawn->node = draw.id;
                    source.resource = run.drawn;
                }
                return source;
            };
            if (theirs) {
                if (std::ranges::find(theirs->draws, draw.id) == theirs->draws.end()) theirs->draws.push_back(draw.id);
                run.instances.push_back(theirs);
                return bound(*theirs);
            }

            auto instances = std::make_shared<Instances>();
            instances->instancing = slot.instancing;
            instances->source = filter->id;
            instances->draws = { draw.id };
            instances->component = info.component;
            for (const auto& field : slot.instancing->fields)
                if (field.Material() && field.component != kecs::NoComponent && !std::ranges::contains(instances->materials, field.component))
                    instances->materials.push_back(field.component);
            instances->clock = frames;
            instances->capacity = std::bit_ceil(std::max<std::size_t>(found->second.listed->Rows(), 64));
            auto buffer = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(instances->capacity * slot.instancing->stride))
                .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc)
                .SetType(kor::Buffer::Type::eDynamic).SetIsPerFrame(true).Build();
            if (!buffer.Valid()) return Fail(buffer.Failure() ? buffer.Failure()->message : std::format("'{}' could not be made a buffer", port));
            instances->buffer = kor::ResourceRef<kor::Buffer>(buffer);
            instances->held = MakeResource(std::move(buffer));
            instances->held->node = draw.id;
            FillInstances(*instances, true);
            sharedInstances[key] = instances;
            auto source = bound(*instances);
            run.instances.push_back(std::move(instances));
            return source;
        }

        /**
         * Each instance's element as its entity is now: where it is, which it is, and its own fields. Once a frame, and
         * only what changed: an element is written again when another entity is in its place, when its entity moved
         * (kecs::World::MovedAt) or when its entity's own fields were written (ChangedTickOf) since it was last written.
         * A frame in which nothing of theirs changed looks at none of them.
         */
        void FillInstances(Instances& instances, const bool now = false)
        {
            const std::uint64_t frame = instances.clock ? *instances.clock : placedFrame;
            if (!now && instances.filled == frame) return;    // another Draw that shares it wrote it this frame
            instances.filled = frame;
            const std::size_t stride = instances.instancing->stride;
            const std::size_t capacity = instances.capacity;
            const auto& fields = instances.instancing->fields;
            // Every element there is room for is written, not only the list's: a GPU cull reads the buffer to its end,
            // and an element left as it was holds what the memory held before — of entities gone since, or of another
            // resource altogether, its pictures' indices among it. One no entity is in is nothing: nowhere, and no picture.
            const auto empty = [&](std::byte* element) {
                std::memset(element, 0, stride);
                for (const auto& field : fields)
                    if (field.Pictured()) std::memset(element + field.offset, 0xFF, 4);
            };
            const auto pictured = static_cast<std::size_t>(std::ranges::count_if(fields, [](const auto& field) { return field.Pictured(); }));
            // Laid out here, between frames, and kept: what changed is written into the copy the frame reads as it begins
            // (WriteStaged).
            const bool fresh = instances.version == 0;
            if (fresh) {
                instances.staged.assign(capacity * stride, std::byte {});
                instances.rowKey.assign(capacity, 0);
                instances.rowOwned.assign(capacity, 0);
                instances.rowChanged.assign(capacity, 0);
                instances.rowPictures.assign(capacity * pictured, nullptr);
                instances.pictureCount.clear();
            }
            const auto found = built.find(instances.source);
            const EntityList* list = found != built.end() && found->second.status.ok && found->second.listed && world ? &*found->second.listed : nullptr;
            const std::size_t rows = list ? list->Rows() : 0;
            // More than there is room for: another buffer, and every Draw that shares this one built again with it. Until
            // then, as many as there is room for: this frame's draws read this buffer still.
            if (rows > capacity) {
                instances.full = true;
                for (const auto& id : instances.draws)
                    if (const auto draw = built.find(id); draw != built.end()) draw->second.signature.clear();
                stale = true;
            }
            const std::size_t laid = std::min(rows, capacity);

            // What could have changed since it was last written. Nothing: none of them is looked at.
            const std::uint64_t moves = world ? world->MoveClock() : 0;
            const bool owned = world && instances.component != kecs::NoComponent;
            const bool materialed = world && !instances.materials.empty();
            // What it reads of each entity, changed: its own component, or one of its materials.
            std::uint64_t owns = owned ? world->ChangesOf(instances.component) : 0;
            if (materialed)
                for (const kecs::ComponentId kind : instances.materials) owns += world->ChangesOf(kind);
            // An entity is drawn only with every material kind its fields are of: without one, it is laid out as no one's.
            const std::uint32_t needed = ((1u << instances.materials.size()) - 1u) << 1;
            const std::uint64_t listed = list ? list->version : 0;
            const bool replaced = pictured > 0 && replacements != instances.seenReplacements;
            if (!fresh && !replaced && list == instances.seenList && listed == instances.seenListed && moves == instances.seenMoves
                && owns == instances.seenOwns && laid == instances.laid)
                return;
            const bool lookMoved = moves != instances.seenMoves;
            const bool lookOwn = (owned || materialed) && owns != instances.seenOwns;
            const kecs::Tick ownSince = instances.seenTick;
            // What is written from here on is written after this: at a tick of its own.
            if ((owned || materialed) && (fresh || lookOwn)) {
                instances.seenTick = world->CurrentTick();
                world->AdvanceTick();
            }

            const std::uint64_t version = instances.version + 1;
            bool changed = fresh, picturesChanged = false;
            // What an element's TEXTURE and STORAGE fields name, counted: the pictures the passes read are those named at
            // least once by a TEXTURE, and the images they write those named at least once by a STORAGE.
            std::vector<bool> writes;
            for (const auto& field : fields)
                if (field.Pictured()) writes.push_back(field.from == Instancing::Field::From::eStorage);
            const auto picture = [&](const std::size_t slot, Member member) {
                const void*& was = instances.rowPictures[slot];
                if (was == member.id) return;
                const bool written = writes[slot % pictured];
                if (was)
                    if (const auto counted = instances.pictureCount.find(was); counted != instances.pictureCount.end()) {
                        std::size_t& uses = written ? counted->second.writes : counted->second.reads;
                        if (uses > 0 && --uses == 0) picturesChanged = true;
                        if (counted->second.reads == 0 && counted->second.writes == 0) instances.pictureCount.erase(counted);
                    }
                was = member.id;
                if (member.id) {
                    auto& counted = instances.pictureCount.try_emplace(member.id, Instances::Named { std::move(member) }).first->second;
                    std::size_t& uses = written ? counted.writes : counted.reads;
                    picturesChanged = picturesChanged || uses == 0;
                    ++uses;
                }
            };
            // An element no entity is in: nowhere, and no picture.
            const auto unlay = [&](const std::size_t row) {
                if (!fresh && instances.rowKey[row] == 0) return;
                empty(instances.staged.data() + row * stride);
                for (std::size_t p = 0; p < pictured; ++p) picture(row * pictured + p, {});
                instances.rowKey[row] = 0;
                instances.rowOwned[row] = 0;
                instances.rowChanged[row] = version;
                changed = true;
            };
            for (std::size_t row = laid; row < (fresh ? capacity : instances.laid); ++row) unlay(row);

            // Which elements are to be laid out again, looked for on every core: what is looked at is only read. One whose
            // entity is gone is marked so (Gone), to be laid out as no one's. And of theirs, whose place in the world is
            // still to be worked out this frame: of those under nothing, on every core, each its own; of the rest, one at
            // a time, since working one out works out what is above it, which another may share.
            constexpr std::uint32_t Gone = 1u << 31;
            constexpr std::size_t Grain = 8192;
            const bool placing = std::ranges::any_of(fields, [](const auto& field) {
                return field.from == Instancing::Field::From::eMatrix || field.from == Instancing::Field::From::eNormalMatrix || field.from == Instancing::Field::From::ePosition;
            });
            struct Looked {
                std::vector<std::uint32_t> again;
                std::vector<kecs::Entity> roots, nested;
            };
            const std::size_t parts = (laid + Grain - 1) / Grain;
            std::vector<Looked> looked(parts);
            const kecs::World& reading = *world;
            // Which of what it reads an entity has: its own component (bit 0), each material kind (from bit 1).
            const auto had = [&](const kecs::Entity entity) {
                std::uint32_t mask = owned && reading.Get(entity, instances.component) ? 1u : 0u;
                for (std::size_t m = 0; m < instances.materials.size(); ++m)
                    if (reading.Get(entity, instances.materials[m])) mask |= 2u << m;
                return mask;
            };
            const auto look = [&](const std::size_t index) {
                Looked& part = looked[index];
                for (std::size_t row = index * Grain; row < std::min(laid, (index + 1) * Grain); ++row) {
                    const kecs::Entity entity = list->entities[row / list->repeat];
                    if (!reading.Alive(entity)) { if (fresh || instances.rowKey[row] != 0) part.again.push_back(static_cast<std::uint32_t>(row) | Gone); continue; }
                    bool due = fresh || replaced || instances.rowKey[row] != KeyOf(entity);
                    if (!due && lookMoved) due = reading.MovedAt(entity) > instances.seenMoves;
                    if (!due && lookOwn) {
                        const std::uint32_t mask = had(entity);
                        due = mask != instances.rowOwned[row];
                        if (!due && (mask & 1u)) due = reading.ChangedTickOf(entity, instances.component) > ownSince;
                        for (std::size_t m = 0; !due && m < instances.materials.size(); ++m)
                            if (mask & (2u << m)) due = reading.ChangedTickOf(entity, instances.materials[m]) > ownSince;
                    }
                    if (!due) continue;
                    part.again.push_back(static_cast<std::uint32_t>(row));
                    if (placing && (entity.index >= placedAt.size() || placedAt[entity.index] != placedFrame))
                        (list->repeat == 1 && reading.ParentOf(entity).IsNull() ? part.roots : part.nested).push_back(entity);
                }
            };
            if (parts > 1) kor::ParallelFor(0, parts, look, 1).Wait();
            else if (parts == 1) look(0);
            std::vector<std::uint32_t> again;
            std::vector<kecs::Entity> roots, nested;
            for (Looked& part : looked) {
                for (const std::uint32_t row : part.again)
                    if (row & Gone) unlay(row & ~Gone);
                    else again.push_back(row);
                roots.insert(roots.end(), part.roots.begin(), part.roots.end());
                nested.insert(nested.end(), part.nested.begin(), part.nested.end());
                part = {};
            }
            if (!again.empty()) changed = true;
            // Room is made for all of them first: Placed grows its lists.
            std::size_t room = placedAt.size();
            for (const auto* placed : { &roots, &nested })
                for (const kecs::Entity entity : *placed) room = std::max<std::size_t>(room, std::size_t { entity.index } + 1);
            if (room > placedAt.size()) { placedAt.resize(room, NeverPlaced); placedMatrices.resize(room); }
            for (const kecs::Entity entity : nested) (void)Placed(entity);
            const auto place = [&](const std::size_t first, const std::size_t last) { for (std::size_t i = first; i < last; ++i) (void)Placed(roots[i]); };
            if (roots.size() > Grain) kor::ParallelForRanges(0, roots.size(), place, Grain).Wait();
            else place(0, roots.size());
            // Laid out on every core: each element is its own.
            const auto layOut = [&](const std::size_t first, const std::size_t last) {
                for (std::size_t i = first; i < last; ++i) {
                    const std::uint32_t row = again[i];
                    const kecs::Entity entity = list->entities[row / list->repeat];
                    const auto* own = owned ? static_cast<const std::byte*>(reading.Get(entity, instances.component)) : nullptr;
                    std::byte* element = instances.staged.data() + std::size_t { row } * stride;
                    empty(element);
                    instances.rowKey[row] = KeyOf(entity);
                    const std::uint32_t mask = had(entity);
                    instances.rowOwned[row] = mask;
                    instances.rowChanged[row] = version;
                    if ((mask & needed) != needed) continue;    // without one of its materials: no one's, and not drawn
                    static const kecs::Mat4 nowhere {};
                    const kecs::Mat4& placed = placing ? placedMatrices[entity.index] : nowhere;
                    for (const auto& field : fields) {
                        std::byte* at = element + field.offset;
                        switch (field.from) {
                        case Instancing::Field::From::eComponent:
                            if (own) std::memcpy(at, own + field.at, field.size);
                            break;
                        case Instancing::Field::From::eMatrix: std::memcpy(at, placed.m.data(), 64); break;
                        case Instancing::Field::From::eNormalMatrix: {
                            const kecs::Mat4 inverse = kecs::Inverse(placed);
                            kecs::Mat4 normal;
                            for (int column = 0; column < 4; ++column)
                                for (int r = 0; r < 4; ++r) normal.At(column, r) = inverse.At(r, column);
                            std::memcpy(at, normal.m.data(), 64);
                            break;
                        }
                        case Instancing::Field::From::ePosition: {
                            const float position[4] { placed.At(3, 0), placed.At(3, 1), placed.At(3, 2), 1.f };
                            std::memcpy(at, position, std::min<std::size_t>(field.size, 16));
                            break;
                        }
                        case Instancing::Field::From::eId: {
                            const std::uint32_t id = entity.index + 1;
                            std::memcpy(at, &id, 4);
                            break;
                        }
                        case Instancing::Field::From::eMaterial:
                            if (const auto* material = static_cast<const std::byte*>(reading.Get(entity, field.component)))
                                WriteMaterialField(field.source, material + field.at, field.type, at, field.size);
                            break;
                        case Instancing::Field::From::eTexture:
                        case Instancing::Field::From::eStorage:
                        case Instancing::Field::From::eMaterialTexture: break;     // below, one at a time: what they name is counted
                        }
                    }
                }
            };
            if (again.size() > Grain) kor::ParallelForRanges(0, again.size(), layOut, Grain).Wait();
            else layOut(0, again.size());

            // The pictures they hold, by the index a shader reaches each at: ~0 for none.
            if (pictured > 0)
                for (const std::uint32_t row : again) {
                    const kecs::Entity entity = list->entities[row / list->repeat];
                    const auto* own = owned ? static_cast<const std::byte*>(reading.Get(entity, instances.component)) : nullptr;
                    std::byte* element = instances.staged.data() + std::size_t { row } * stride;
                    const bool drawn = (instances.rowOwned[row] & needed) == needed;
                    std::size_t texture = 0;
                    for (const auto& field : fields) {
                        if (!field.Pictured()) continue;
                        std::uint32_t index = ~0u;
                        const std::byte* from = !drawn ? nullptr : field.Material() ? static_cast<const std::byte*>(reading.Get(entity, field.component)) : own;
                        const Held* held = from ? reinterpret_cast<const Held*>(from + field.at) : nullptr;
                        // A picture read since it was given is what was made in its place: its stand-in is what it was.
                        const Resource* now = held ? held->get() : nullptr;
                        while (now && now->replacedBy) now = now->replacedBy.get();
                        Member member;
                        if (now && now->handle) {
                            const Resource& given = *now;
                            kor::DescriptorIndex handle;
                            const bool written = field.from == Instancing::Field::From::eStorage;
                            MadeWithout(given, written ? kor::Image::Usage::eStorage : kor::Image::Usage::eSampled);    // said, for its image to be made for it
                            if (given.kind == "Image") handle = written ? kor::Bindless::Storage(kor::capi::ImageOf(given.handle)) : kor::Bindless::Sampled(kor::capi::ImageOf(given.handle));
                            else if (given.kind == "ImageView") handle = written ? kor::Bindless::Storage(kor::capi::ImageViewOf(given.handle)) : kor::Bindless::Sampled(kor::capi::ImageViewOf(given.handle));
                            if (handle.Valid()) {
                                index = handle.index;
                                member = MemberOf(now);
                            }
                        }
                        picture(std::size_t { row } * pictured + texture++, std::move(member));
                        std::memcpy(element + field.offset, &index, 4);
                    }
                }
            if (changed && materialed) {
                std::size_t skipped = 0;
                for (std::size_t row = 0; row < laid; ++row) skipped += instances.rowKey[row] != 0 && (instances.rowOwned[row] & needed) != needed;
                instances.skipped = skipped;
            }
            instances.laid = laid;
            instances.seenList = list;
            instances.seenListed = listed;
            instances.seenMoves = moves;
            instances.seenOwns = owns;
            instances.seenReplacements = replacements;
            if (changed) instances.written = instances.version = version;
            if (picturesChanged) {
                std::vector<Member> pictures, storages;
                for (const auto& counted : instances.pictureCount | std::views::values) {
                    if (counted.reads > 0) pictures.push_back(counted.member);
                    if (counted.writes > 0) storages.push_back(counted.member);
                }
                std::ranges::sort(pictures, {}, &Member::id);
                std::ranges::sort(storages, {}, &Member::id);
                instances.pictures = std::move(pictures);
                instances.storages = std::move(storages);
                ++instances.named;
            }
        }

        /**
         * A Draw's `whenChanged`: what it is to draw, made one number — the same meshes, of the same entities where they
         * are, with the same sets, push constants, instances and matrix to cull by — and whether that is not what it drew
         * last, as many times in a row as there are copies of what it draws into. Whether it draws is settled once every
         * pass is readied (WhichDrawAgain): with the others that draw into what it does.
         */
        void Unchanged(EntityRun& run, const std::optional<kor::Mat4>& sight)
        {
            if (!run.whenChanged) return;
            std::uint64_t hash = 1469598103934665603ull;
            const auto mix = [&hash](const void* bytes, const std::size_t size) { Mix(hash, bytes, size); };
            const auto mixed = [&](const auto& value) { mix(&value, sizeof value); };
            if (sight) for (int column = 0; column < 4; ++column) mixed((*sight)[column]);
            for (const Item& item : run.items) {
                const std::uint64_t key = item.entity.IsNull() ? 0 : KeyOf(item.entity);
                mixed(key);
                if (!item.entity.IsNull()) mix(Placed(item.entity).m.data(), 64);
                const void* handles[3] { item.mesh.Get(), item.pipeline.Get(), item.set.Get() };
                mixed(handles);
                const std::uint32_t range[6] { item.part, item.indexed, item.firstIndex, item.indexCount, static_cast<std::uint32_t>(item.vertexOffset), item.groups[0] };
                mixed(range);
            }
            mix(run.pushed.data(), run.pushed.size());
            // What each instance is given, where it draws instances: written from the entities, and not in its items.
            for (const auto& instances : run.instances) mixed(instances->written);
            run.drawingAs = hash;
            run.due = hash != run.drawnAs || run.drawnTimes < run.copies;
        }

        /**
         * Which Draws `whenChanged` draw this frame. One whose view another draws into is drawn with it: those that draw
         * into one view after another — a cascade a quarter of an atlas, after the one that clears it — clear nothing of
         * their own, and one drawn again alone would draw over what it drew before. So a view any of them is due in is
         * drawn again by all of them, and one a Draw that draws every frame draws into, every frame.
         */
        void WhichDrawAgain()
        {
            std::vector<EntityRun*> runs;
            std::unordered_map<const void*, bool> drawnInto;     // a view, and whether everything drawing into it draws
            for (const auto& id : wanted) {
                EntityRun* run = built.at(id).pass->entities.get();
                if (!run || !run->draws || run->idle) continue;
                for (const void* view : run->targets) drawnInto[view] = drawnInto[view] || !run->whenChanged || run->due;
                if (run->whenChanged) runs.push_back(run);
            }
            for (bool more = true; more;) {
                more = false;
                for (EntityRun* run : runs) {
                    if (run->due || std::ranges::none_of(run->targets, [&](const void* view) { return drawnInto[view]; })) continue;
                    run->due = more = true;
                    for (const void* view : run->targets) drawnInto[view] = true;
                }
            }
            for (EntityRun* run : runs) {
                if (!run->due) {
                    run->idle = true;
                    run->report.unchanged = true;
                    continue;
                }
                run->drawnTimes = run->drawingAs == run->drawnAs ? run->drawnTimes + 1 : 1;
                run->drawnAs = run->drawingAs;
            }
        }

        /** Readies a Draw's or a Dispatch's draws for this frame: one, or one an element of its ForEach, and what each is made of. */
        void Prepare(EntityRun& run)
        {
            // Idle this frame: what it draws into stays as it is, nothing is readied for it, and what it last said stands.
            run.idle = false;
            run.due = true;     // unless what it draws is found to be what it drew (Unchanged)
            if (!run.idleNode.empty())
                if (const auto source = built.find(run.idleNode); source != built.end())
                    if (const auto output = source->second.outputs.find(run.idleOutput); output != source->second.outputs.end()) {
                        const Value& said = output->second.value;
                        if (const auto* numbers = said.Items(); numbers && numbers->size() > static_cast<std::size_t>(run.cullIndex)) {
                            if (const double* number = (*numbers)[static_cast<std::size_t>(run.cullIndex)].Number()) run.idle = *number != 0.0;
                        } else if (const double* number = said.Number()) run.idle = *number != 0.0;     // one, for every turn
                        else if (const bool* flag = said.Bool()) run.idle = *flag;
                    }
            run.items.clear();
            run.holding.clear();
            run.pushed.clear();
            // Its buffer of instances follows the entities, idle or not: another pass may read it while this one rests
            // (the meshlets read the forward Draw's). Whether it is due again is said by what it last drew, not by this.
            for (const auto& instances : run.instances) FillInstances(*instances);
            if (run.idle) return;
            // The pictures they name by index are read by the draws all the same: the frame graph is told of them. So are
            // those of another Draw's buffer of instances, wired here: what samples them is this pass as much.
            std::vector<const Instances*> named;
            for (const auto& instances : run.instances) named.push_back(instances.get());
            for (const Source& slot : run.slots)
                if (slot.kind == Source::Kind::eWired && slot.resource)
                    for (const Impl* owner : { static_cast<const Impl*>(source), static_cast<const Impl*>(this) })
                        if (owner)
                            for (const auto& entry : owner->sharedInstances | std::views::values)
                                if (const auto shared = entry.lock(); shared && shared->held == slot.resource) named.push_back(shared.get());
            // Gathered again only when one of them names others than it did: thousands of images, sorted, each frame otherwise.
            std::vector<std::pair<const void*, std::uint64_t>> seen;
            for (const Instances* instances : named) seen.emplace_back(instances, instances->named);
            if (!named.empty() && seen != run.namedSeen) {
                run.namedSeen = std::move(seen);
                // And the images they write by index — a STORAGE field's — are written by it.
                for (auto [list, held] : { std::pair { &Instances::pictures, &run.pictures }, std::pair { &Instances::storages, &run.storages } }) {
                    std::vector<Member> members;
                    for (const Instances* instances : named) members.insert(members.end(), (instances->*list).begin(), (instances->*list).end());
                    std::ranges::sort(members, {}, &Member::id);
                    members.erase(std::ranges::unique(members, {}, &Member::id).begin(), members.end());
                    if (!std::ranges::equal(members, *held, {}, &Member::id, &Member::id)) {
                        *held = std::move(members);
                        run.changed = true;
                    }
                }
            }
            run.report = {};
            const PipelineInfo& info = *run.info;
            const std::size_t slots = info.slots.size();
            for (auto& cached : run.cache | std::views::values) cached.seen = false;

            // Whose elements: its ForEach's, as of this frame; or the one draw.
            Built* each = nullptr;
            if (!run.each.empty()) {
                const auto found = built.find(run.each);
                if (found == built.end() || !found->second.status.ok || !found->second.listed) return;
                each = &found->second;
            }
            const std::size_t rows = each ? each->listed->Rows() : 1;
            const std::size_t repeat = each ? std::max<std::size_t>(each->listed->repeat, 1) : 1;

            // What is out of sight: outside the box the Draw's `cull` matrix makes of what it sees. Read as it is now.
            std::optional<kor::Mat4> sight;
            if (run.draws && !run.cullNode.empty())
                if (const auto source = built.find(run.cullNode); source != built.end())
                    if (const auto output = source->second.outputs.find(run.cullOutput); output != source->second.outputs.end())
                        if (const auto* numbers = output->second.value.Items(); numbers && numbers->size() >= static_cast<std::size_t>(run.cullIndex + 1) * 16) {
                            kor::Mat4 matrix;
                            for (int i = 0; i < 16; ++i) matrix[i / 4][i % 4] = static_cast<float>((*numbers)[static_cast<std::size_t>(run.cullIndex * 16 + i)].Number() ? *(*numbers)[static_cast<std::size_t>(run.cullIndex * 16 + i)].Number() : 0.0);
                            sight = matrix;
                        }

            const auto graphics = run.draws ? kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf(info.resource->handle)) : kor::ResourceRef<const kor::GraphicsPipeline>();

            // What one of them is, for the element at hand: a resource, or a value.
            const auto output = [&](const Source& source) -> const Output* {
                const auto from = built.find(source.node);
                if (from == built.end()) return nullptr;
                const auto it = from->second.outputs.find(source.output);
                return it == from->second.outputs.end() ? nullptr : &it->second;
            };
            // Where it is held rather than a copy of it, for each element of every pass every frame: what holds it — the
            // node, the entity's component, the output — stays as it is while the element is readied.
            static const Held none;
            const auto resource = [&](const Source& source, const kecs::Entity entity) -> const Held& {
                const Held* held = &none;
                switch (source.kind) {
                case Source::Kind::eGiven:
                case Source::Kind::eWired: held = &source.resource; break;
                case Source::Kind::eField:
                    if (const auto* bytes = entity.IsNull() ? nullptr : static_cast<const std::byte*>(world->Get(entity, source.component)))
                        held = reinterpret_cast<const Held*>(bytes + source.field.offset);
                    break;
                case Source::Kind::eWhere:
                case Source::Kind::eChain:
                    if (const auto* given = output(source)) held = &given->resource;
                    break;
                default: break;
                }
                // A resource whose node was built again since: the new one, from here on.
                while (*held && (*held)->replacedBy) held = &(*held)->replacedBy;
                return *held;
            };
            std::vector<const Held*> held(slots);

            for (std::size_t row = 0; row < rows; ++row) {
                const kecs::Entity entity = each && row / repeat < each->listed->entities.size() ? each->listed->entities[row / repeat] : kecs::NullEntity;
                if (each && run.rows) {
                    SetRow(*each, row, &run.asked);
                    for (const auto& id : run.chain) Refresh(id);
                }
                const auto skip = [&](const std::string& why) {
                    ++run.report.skipped;
                    if (run.report.why.empty()) run.report.why = entity.IsNull() ? why : std::format("entity {}: {}", entity.index, why);
                    if (run.report.passedBy.size() < 512) run.report.passedBy.emplace_back(entity, why);
                };
                Item item;
                item.entity = entity;
                item.pipeline = graphics;

                if (run.draws && run.mesh.kind != Source::Kind::eNone) {
                    const Held& mesh = resource(run.mesh, entity);
                    // A model's mesh is a part of the one mesh the model is: that is what is bound.
                    const Resource* whole = mesh && mesh->part ? mesh->source.get() : mesh.get();
                    if (!whole || !whole->handle || mesh->kind != "Mesh") { skip(mesh && mesh->node.starts_with('@') ? "its mesh is still being read" : "it has no mesh to draw"); continue; }
                    item.mesh = kor::capi::MeshOf(whole->handle);
                    if (run.holding.empty() || run.holding.back() != mesh) run.holding.push_back(mesh);
                    item.part = mesh->part;
                    item.firstIndex = mesh->firstIndex;
                    item.indexCount = mesh->indexCount;
                    item.vertexOffset = mesh->vertexOffset;
                    // Drawn with the pipeline made for its mesh's layout: any mesh that has what the vertex shader reads.
                    auto& known = run.fits[item.mesh.Get()];
                    if (!known.mesh.Alive() || known.mesh.Get() != item.mesh.Get() || (known.why.empty() && !known.pipeline.Alive())) {
                        auto pipeline = PipelineFor(info, *item.mesh);
                        known = pipeline ? EntityRun::Fit { item.mesh, *pipeline, {} } : EntityRun::Fit { item.mesh, {}, pipeline.error() };
                    }
                    if (!known.why.empty()) { skip(known.why); continue; }
                    item.pipeline = known.pipeline;
                    const bool indices = item.mesh->IndexCount().has_value();
                    if (run.command == DrawWith::eDrawIndexed && !indices) { skip("its mesh has no indices, and the node draws indexed"); continue; }
                    item.indexed = run.command == DrawWith::eDrawIndexed || (run.command == DrawWith::eAuto && indices);
                    if (sight && mesh->bounded) {
                        // Its box's eight corners, where the matrix puts them — placed as its entity is: all past one side is out of sight.
                        const kor::Mat4 placed = entity.IsNull() ? *sight : *sight * ToKor(Placed(entity));
                        int past[6] { 0, 0, 0, 0, 0, 0 };
                        // The least corner where the matrix puts it, and each edge of the box from there: every corner is
                        // the one and some of the others, which is four products rather than eight.
                        const kor::Vec4 least = placed * kor::Vec4(mesh->bounds[0], mesh->bounds[1], mesh->bounds[2], 1.f);
                        const kor::Vec4 edges[3] { placed * kor::Vec4(mesh->bounds[3] - mesh->bounds[0], 0.f, 0.f, 0.f),
                                                   placed * kor::Vec4(0.f, mesh->bounds[4] - mesh->bounds[1], 0.f, 0.f),
                                                   placed * kor::Vec4(0.f, 0.f, mesh->bounds[5] - mesh->bounds[2], 0.f) };
                        for (int corner = 0; corner < 8; ++corner) {
                            kor::Vec4 at = least;
                            for (int axis = 0; axis < 3; ++axis) if (corner & (1 << axis)) at += edges[axis];
                            past[0] += at.x < -at.w; past[1] += at.x > at.w; past[2] += at.y < -at.w; past[3] += at.y > at.w; past[4] += at.z < 0.f; past[5] += at.z > at.w;
                        }
                        if (std::ranges::any_of(past, [](const int count) { return count == 8; })) {
                            // Out of sight — but one that could not be drawn in sight either says so all the same: what is
                            // wrong with an entity is not a matter of where the camera is.
                            std::string lacking;
                            for (std::size_t i = 0; i < slots && lacking.empty(); ++i)
                                if (const Held& given = resource(run.slots[i], entity); !given || (!given->handle && !given->part)) lacking = Lacks(run.slots[i]);
                            if (!lacking.empty()) skip(lacking);
                            else ++run.report.culled;
                            continue;
                        }
                    }
                }

                if (slots > 0) {
                    std::string lacking;
                    for (std::size_t i = 0; i < slots && lacking.empty(); ++i) {
                        const Slot& slot = info.slots[i];
                        held[i] = &resource(run.slots[i], entity);
                        if (!*held[i] || !(*held[i])->handle) lacking = Lacks(run.slots[i]);
                        else if (!Takes(slot.type, (*held[i])->kind)) lacking = std::format("'param.{}' takes a {}, and was given a {}", slot.field, KindOf(slot.type), (*held[i])->kind);
                    }
                    if (!lacking.empty()) { skip(lacking); continue; }

                    auto& cached = run.cache[{ entity.IsNull() ? 0 : KeyOf(entity), row % repeat }];
                    bool same = cached.set.Valid() && cached.key.size() == slots;
                    for (std::size_t i = 0; same && i < slots; ++i) same = cached.key[i] == held[i]->get();
                    if (!same) {
                        kor::DescriptorSet::Builder set(info.pipeline, 1);
                        std::string failed;
                        for (std::size_t i = 0; i < slots && failed.empty(); ++i) {
                            const Slot& slot = info.slots[i];
                            if (slot.sampler) continue;
                            const Resource* sampler = nullptr;
                            if (slot.type == kor::DescriptorType::eCombinedImageSampler) {
                                if (i + 1 < slots && info.slots[i + 1].sampler) sampler = held[i + 1]->get();
                                else if (const auto shared = info.samplers.find(slot.parameter); shared != info.samplers.end()) sampler = shared->second.get();
                            }
                            if (const auto written = WriteParameter(set, slot.parameter, slot.type, **held[i], sampler, "param." + slot.field); !written) failed = written.error();
                        }
                        kor::Resource<kor::DescriptorSet> made;
                        if (failed.empty()) {
                            made = set.Build();
                            if (!made.Valid()) failed = made.Failure() ? made.Failure()->message : std::string("its descriptor set could not be built");
                        }
                        // The set it had may be in what this frame has recorded: kept until that frame is done.
                        retiredSets.push_back(std::exchange(cached, EntityRun::Cached {}));
                        if (!failed.empty()) { run.cache.erase({ entity.IsNull() ? 0 : KeyOf(entity), row % repeat }); run.changed = true; skip(failed); continue; }
                        cached.set = std::move(made);
                        cached.key.assign(slots, nullptr);
                        cached.held.clear();
                        for (const Held* given : held) cached.held.push_back(*given);
                        cached.members.assign(slots, Member {});
                        for (std::size_t i = 0; i < slots; ++i) {
                            cached.key[i] = held[i]->get();
                            if (!info.slots[i].sampler) cached.members[i] = MemberOf(held[i]->get());
                        }
                        run.changed = true;
                    }
                    cached.seen = true;
                    item.set = cached.set;
                    if (run.cover >= 0 && cached.members[static_cast<std::size_t>(run.cover)].image.Get()) {
                        const auto extent = cached.members[static_cast<std::size_t>(run.cover)].image->Extent();
                        for (int axis = 0; axis < 3; ++axis)
                            item.groups[axis] = std::max<kor::u32>((static_cast<kor::u32>(extent[axis]) + run.groupSize[axis] - 1) / run.groupSize[axis], 1);
                    } else {
                        std::ranges::copy(run.groups, item.groups);
                    }
                } else {
                    std::ranges::copy(run.groups, item.groups);
                }

                // Its push constants, each from where it comes: zero, for one given nothing.
                if (info.pushSize) {
                    item.push = run.pushed.size();
                    run.pushed.resize(run.pushed.size() + info.pushSize, std::byte { 0 });
                    std::byte* const pushed = run.pushed.data() + item.push;
                    for (std::size_t i = 0; i < info.pushes.size(); ++i) {
                        const PushMember& push = info.pushes[i];
                        const Source& source = run.pushes[i];
                        std::byte* const at = pushed + push.offset;
                        const bool matrix = push.scalar == 0 && push.rows == 4 && push.columns == 4 && push.size == 64;
                        switch (source.kind) {
                        case Source::Kind::eNone: break;
                        case Source::Kind::eGiven: PackPush(push, source.value, at); break;
                        case Source::Kind::eField: {
                            const auto* bytes = entity.IsNull() ? nullptr : static_cast<const std::byte*>(world->Get(entity, source.component));
                            if (!bytes) break;
                            if (source.raw) std::memcpy(at, bytes + source.field.offset, std::min<std::size_t>(source.field.size, push.size));
                            else PackPush(push, FieldValue(source.field, bytes + source.field.offset), at);
                            break;
                        }
                        case Source::Kind::eWhere:
                            // Where it is and which it is, straight from the world.
                            if (source.output == "matrix" && matrix) { if (!entity.IsNull()) std::memcpy(at, Placed(entity).m.data(), 64); break; }
                            if (source.output == "normalMatrix" && matrix) {
                                if (entity.IsNull()) break;
                                const kecs::Mat4 inverse = kecs::Inverse(Placed(entity));
                                kecs::Mat4 normal;
                                for (int column = 0; column < 4; ++column)
                                    for (int r = 0; r < 4; ++r) normal.At(column, r) = inverse.At(r, column);
                                std::memcpy(at, normal.m.data(), 64);
                                break;
                            }
                            if (source.output == "entityId" && !run.asked.contains("entityId")) { PackPush(push, Value(entity.IsNull() ? 0.0 : entity.index + 1.0), at); break; }
                            if (const auto* given = output(source)) PackPush(push, given->value, at);
                            break;
                        case Source::Kind::eWired:
                        case Source::Kind::eChain:
                            if (const auto* given = output(source)) PackPush(push, given->value, at);
                            break;
                        }
                    }
                }
                run.items.push_back(std::move(item));
            }
            // Anything else wired to the ForEach has the first element, as it does without a draw.
            if (each && run.rows && rows > 1) {
                SetRow(*each, 0);
                for (const auto& id : run.chain) Refresh(id);
            }
            // Those of one mesh one after another: a mesh is then bound once for all of them.
            if (run.draws) std::ranges::stable_sort(run.items, {}, [](const Item& item) { return item.mesh.Get(); });
            // A Dispatch of a Filter's entities: its groups along z as many times over as there are of them, the element
            // SV_GroupID.z / groups.z. None of them: nothing to run. More than a dispatch's z counts: said, and not run.
            if (!run.batchOf.empty()) {
                const auto found = built.find(run.batchOf);
                const std::size_t elements = found != built.end() && found->second.listed ? found->second.listed->Rows() : 0;
                const kor::u64 most = 65535;
                for (Item& item : run.items) {
                    if (static_cast<kor::u64>(item.groups[2]) * elements > most) {
                        ++run.report.skipped;
                        if (run.report.why.empty())
                            run.report.why = std::format("{} of '{}' at {} groups along z each is {} groups along z, past the {} a dispatch can count", elements, run.batchOf, item.groups[2],
                                                         static_cast<kor::u64>(item.groups[2]) * elements, most);
                        item.groups[2] = 0;
                        continue;
                    }
                    item.groups[2] *= static_cast<kor::u32>(elements);
                }
                std::erase_if(run.items, [](const Item& item) { return item.groups[2] == 0; });
            }
            run.report.ran = run.items.size();
            if (run.draws) Unchanged(run, sight);

            // Elements that are gone let go of what was made for them.
            for (auto it = run.cache.begin(); it != run.cache.end();) {
                if (it->second.seen) { ++it; continue; }
                retiredSets.push_back(std::move(it->second));
                it = run.cache.erase(it);
                run.changed = true;
            }
            if (!run.changed) return;
            // Every element's resources, a slot at a time and once each: what the frame graph orders this pass by.
            run.members.resize(slots);
            for (std::size_t i = 0; i < slots; ++i) {
                run.members[i].clear();
                if (info.slots[i].sampler) continue;
                std::set<const void*> had;
                for (const auto& cached : run.cache | std::views::values)
                    if (cached.members[i].id && had.insert(cached.members[i].id).second) run.members[i].push_back(cached.members[i]);
                std::ranges::sort(run.members[i], {}, &Member::id);
            }
        }

        /** A node's inputs as they stand: its own values, and for each wire what the other end gives now. Nothing when one is missing. */
        [[nodiscard]] std::optional<Inputs> Gather(const Node& node) const
        {
            Inputs inputs;
            for (const auto& [name, input] : node.inputs) {
                Resolved resolved;
                if (input.link) {
                    const auto source = built.find(input.link->node);
                    if (source == built.end() || !source->second.status.ok) return std::nullopt;
                    const auto output = source->second.outputs.find(input.link->output);
                    if (output == source->second.outputs.end()) return std::nullopt;
                    resolved.linked = true;
                    resolved.value = output->second.value;
                    resolved.resource = output->second.resource;
                } else {
                    resolved.value = input.value;
                }
                inputs.emplace(name, std::move(resolved));
            }
            return inputs;
        }

        /**
         * What follows the world, brought up to date: each node that reads it, and — in the order they depend on one
         * another — every value worked out from one, and every buffer one is wired to. Nothing is built again:
         * a value changes where it is, and a buffer is written.
         */
        void RunLive()
        {
            // What is for setup alone, on a frame setup records — or one when entities or components came or went, so
            // that what is counted of them (a buffer's size) follows.
            bool all = setupRecords;
            if (world && world->StructureVersion() != structureSeen) { structureSeen = world->StructureVersion(); all = true; }
            for (const std::string& id : live)
                if (all || !setupOnly.contains(id)) Refresh(id);
            for (const std::string& id : sized) {
                const Node* node = current ? current->Find(id) : nullptr;
                const auto found = built.find(id);
                if (!node || found == built.end() || !found->second.status.ok) continue;
                if (WiredValues(*node) != found->second.madeWith) { found->second.signature.clear(); stale = true; }
            }
        }

        /** Whether a node is a resource made by the framework's builder — an Image, a Buffer, a Sampler — of the values it is given. */
        bool IsMadeResource(const std::string_view type) const
        {
            const NodeType* found = FindType(type);
            return found && found->builder && !IsPipeline(found->name);
        }

        /** The values wired into a node, as they are now: what a resource made of them is compared with. */
        std::string WiredValues(const Node& node) const
        {
            std::string values;
            for (const auto& [name, input] : node.inputs) {
                if (!input.link) continue;
                const auto source = built.find(input.link->node);
                if (source == built.end()) continue;
                const auto output = source->second.outputs.find(input.link->output);
                if (output != source->second.outputs.end() && !output->second.resource) values += std::format("{}={};", name, output->second.value.ToJson());
            }
            return values;
        }

        /** One node that follows the world, brought up to date: read again, worked out again, or written. */
        void Refresh(const std::string& id)
        {
            const Node* node = current ? current->Find(id) : nullptr;
            const auto found = built.find(id);
            if (!node || found == built.end() || !found->second.status.ok) return;
            Built& out = found->second;
            if (IsMapped(node->type)) {
                if (out.mapped) Fill(*node, out);
                return;
            }
            if (node->type == "Collect") { CollectAll(*node, out); return; }
            auto inputs = Gather(*node);
            if (!inputs || !InParts(*node, *inputs)) return;
            if (ReadsTheWorld(node->type)) {
                (void)BuildLive(*node, *inputs, out);
            } else if (node->type == "MeshPack") {
                // Other meshes than it packed — an entity given another, one arrived from its file: packed again.
                if (!out.packing) return;
                // Looked at again only when something they are found from could have changed: the entities, their meshes,
                // a resource made again in its place. Otherwise thousands of entities are walked each frame for nothing.
                const auto wired = node->inputs.find("meshes");
                const Node* origin = wired != node->inputs.end() && wired->second.link ? Origin(wired->second.link->node) : nullptr;
                const auto source = origin ? built.find(origin->id) : built.end();
                const EntityList* list = source != built.end() && source->second.listed ? &*source->second.listed : nullptr;
                const auto read = list ? std::ranges::find(list->fields, wired->second.link->output, &EntityList::Read::output) : std::vector<EntityList::Read>::const_iterator {};
                const bool listed = list && read != list->fields.end() && read->resource && world;
                const MeshPacking::Seen seen { list, list ? list->version : 0, listed ? world->ChangesOf(read->component) : 0, replacements };
                if (listed && out.packing->seen == seen) return;
                if (PackedNow(*node) != out.packing->of) { out.signature.clear(); stale = true; return; }
                if (listed) out.packing->seen = seen;
            } else if (node->type == "IndirectDraws") {
                if (!out.packed) return;
                auto commands = IndirectCommands(*inputs, OnTheGpu(*inputs));
                if (!commands) { out.signature.clear(); stale = true; return; }
                auto& [bytes, instances] = *commands;
                // More commands, or more instances than `visible` has room for: another buffer.
                if (bytes.size() != out.packed->bytes.size() || (out.packed->everyFrame && instances > out.packed->room)) { out.signature.clear(); stale = true; return; }
                if (!out.packed->everyFrame && bytes == out.packed->bytes) return;
                // On the GPU's, its pass writes them in each frame: what it writes is kept here.
                if (!bytes.empty() && !out.packed->everyFrame) out.packed->buffer->Write(std::span<const std::byte>(bytes));
                out.packed->bytes = std::move(bytes);
            } else if (node->type == "Reference") {
                if (const auto target = node->inputs.find("node"); target != node->inputs.end() && target->second.value.String())
                    if (const auto source = built.find(*target->second.value.String()); source != built.end())
                        for (const auto& [name, output] : source->second.outputs)
                            if (!output.resource) out.outputs[name].value = output.value;
            } else {
                FillDefaults(*node, *inputs);
                Built fresh;
                if (!BuildValue(*node, *inputs, fresh)) return;
                for (auto& [name, output] : fresh.outputs)
                    if (!output.resource) out.outputs[name].value = std::move(output.value);
            }
        }

        /**
         * A pass of a setup page records on the frame after it is built — on as many frames as there are copies of a
         * per-frame resource it writes, so that each copy is made — and rests from then on, until it is built again.
         * It stays in the frame graph all the while, so that what reads what it made is still after it.
         */
        void RunSetup()
        {
            // A copy of what a setup page begins with is made while setup is: a new one makes setup be made again. As does
            // the scene starting or stopping.
            bool again = std::exchange(setupAgain, false);
            for (const auto& id : before)
                if (const auto copy = built.find(id); copy != built.end() && copy->second.pass) {
                    const Node* node = current ? current->Find(id) : nullptr;
                    if (!node || !current->IsSetup(node->pass) || copy->second.ranFor == copy->second.stamp) continue;
                    copy->second.ranFor = copy->second.stamp;
                    again = true;
                }
            // Setup is one sequence: a pass of it built again runs again with all the rest of it, not alone — what one
            // adds up, another clears first.
            for (const auto& id : wanted) {
                if (again) break;
                if (id.starts_with('$')) continue;
                const Built& out = built.at(id);
                const Node* node = current ? current->Find(id) : nullptr;
                if (node && current->IsSetup(node->pass) && (!out.setup || out.ranFor != out.stamp)) again = true;
            }
            setupRecords = false;
            std::uint32_t runs = 0;
            for (const auto& id : wanted) {
                if (id.starts_with('$')) continue;      // a watch's copy, which is copied as its watch says
                Built& out = built.at(id);
                const Node* node = current ? current->Find(id) : nullptr;
                const bool setup = node && current->IsSetup(node->pass);
                if (!setup) {
                    out.setup = false;
                    // A page of the fixed step runs on a frame with a step in it, and rests on one without.
                    out.pass->resting->store(node && current->IsFixed(node->pass) && fixedSteps == 0);
                    continue;
                }
                if (!out.setup || out.ranFor != out.stamp || again) {
                    out.setup = true;
                    out.ranFor = out.stamp;
                    out.runsLeft = 1;
                    for (const auto& use : out.pass->uses)
                        if (use.write && ((use.what.image.Get() && use.what.image->IsPerFrame()) || (use.what.buffer.Get() && use.what.buffer->IsPerFrame())))
                            out.runsLeft = std::max(out.runsLeft, kor::Context::HasScheduler() ? kor::Context::Scheduler().ImageCount() : 1u);
                    runs = std::max(runs, out.runsLeft);
                }
            }
            // And every pass of it as many times as the one that runs most: each time, all of it, in its order.
            for (const auto& id : wanted) {
                if (id.starts_with('$')) continue;
                Built& out = built.at(id);
                if (!out.setup) continue;
                if (again) out.runsLeft = runs;
                out.pass->resting->store(out.runsLeft == 0);
                if (out.runsLeft > 0) { --out.runsLeft; setupRecords = true; }
            }
        }

        /** What is done every frame, before anything is recorded: the scene's buffers written, and each pass readied. */
        void EachFrame()
        {
            // As many as the graph runs at most: the rest are lost.
            const auto most = static_cast<std::uint32_t>(std::max(current ? current->fixedStepsAtMost : 4, 1));
            const std::uint32_t taken = std::exchange(fixedPending, 0);
            fixedSteps = std::min(taken, most);
            fixedLost += taken - fixedSteps;
            fixedNow->store(fixedSteps);
            fixedCount += fixedSteps;
            // The time goes on by what a frame took — at most a quarter of a second, so that a frame held up does not jump.
            const auto now = std::chrono::steady_clock::now();
            timeDelta = timeLast ? std::min(std::chrono::duration<double>(now - *timeLast).count(), 0.25) : 0.0;
            timeLast = now;
            timeSeconds += timeDelta;
            if (std::exchange(timeBegun, true)) ++timeFrame;
            RunSetup();
            // A watch that is copied now and then rests on the frames between, each on frames of its own.
            ++watchFrame;
            for (auto& [id, watching] : watched) {
                const auto copy = built.find(std::format("$peek {}", id));
                if (copy == built.end() || !copy->second.pass) continue;
                if (watching.since == 0) watching.since = watchFrame;
                const std::uint64_t phase = std::hash<std::string>{}(id);
                const bool fresh = watchFrame - watching.since < 2;     // nothing copied yet: copied now, whatever its turn
                bool rests = !fresh && watching.every > 1 && (watchFrame + phase) % watching.every != 0;
                // What a setup page begins with is there only while setup is made.
                if (before.contains(copy->first))
                    if (const Node* node = current ? current->Find(copy->first) : nullptr; node && current->IsSetup(node->pass)) rests = !setupRecords;
                copy->second.pass->resting->store(rests);
            }
            RunLive();
            // The scenes rays are traced through, made again where an entity of theirs moved, came or went.
            for (auto& entry : built)
                if (entry.second.rays && entry.second.status.ok) UpdateRays(entry.first, entry.second);
            for (const auto& id : wanted)
                if (const auto& plan = built.at(id).pass; plan && plan->prepare) plan->prepare();
        }

        // Where each entity is in the world, worked out once a frame however many passes ask.
        std::vector<kecs::Mat4> placedMatrices;
        std::vector<std::uint64_t> placedAt;        // the frame each was worked out in: NeverPlaced, not yet
        std::uint64_t placedFrame = 0;
        // No frame's: what an entity not yet placed has, so that the first frame — 0, as a buffer of instances made while the
        // graph is built is filled in — works out where each is rather than taking it as worked out already.
        static constexpr std::uint64_t NeverPlaced = ~0ull;
        // How many resources have been made again in their places, by any runtime: an index a picture is read by may be another.
        static inline std::atomic<std::uint64_t> replacements = 0;
        const kecs::Mat4& Placed(const kecs::Entity entity)
        {
            if (entity.index >= placedAt.size()) { placedAt.resize(entity.index + 1, NeverPlaced); placedMatrices.resize(entity.index + 1); }
            if (placedAt[entity.index] != placedFrame) { placedMatrices[entity.index] = world->WorldMatrix(entity); placedAt[entity.index] = placedFrame; }
            return placedMatrices[entity.index];
        }

        /** Readies every pass that runs per entity. Whether what any of them uses changed. */
        bool PrepareAll()
        {
            bool changed = false;
            ++placedFrame;
            for (const auto& id : wanted) {
                const auto& plan = built.at(id).pass;
                if (!plan->entities) continue;
                Prepare(*plan->entities);
                changed = changed || plan->entities->changed;
                plan->entities->changed = false;
            }
            RestUnread();
            WhichDrawAgain();
            return changed;
        }

        /**
         * Rests what nothing that runs reads: a pass whose resources other passes read, but none of them one that runs
         * this frame — the shadows a renderer draws with, while another renderer makes the picture — records nothing, as
         * if idle, until one does. Kept are what writes the screen, memory the CPU reads, a resource no other pass reads
         * (what the graph is for, read by whoever runs it), or nothing at all; a page's setup, or its fixed step, whose
         * state goes on from one run to the next; and a watch's copy, with what it copies. A Draw rested so is not taken
         * as drawn: `whenChanged`, it is due again once read again.
         */
        void RestUnread()
        {
            restingUnread.clear();
            if (usesOf.empty()) return;
            const auto rests = [&](const std::string& id) {
                const auto& plan = *built.at(id).pass;
                return plan.resting->load() || (plan.idle && plan.idle->load()) || (plan.entities && plan.entities->idle);
            };
            // Each resource, and the passes that read it, resting or not.
            std::map<std::string_view, std::vector<std::string_view>> readers;
            for (const auto& [id, uses] : usesOf)
                for (const Declared& use : uses)
                    if (use.read) readers[use.name].push_back(id);
            const auto readByAnother = [&](const std::string& id, const Declared& use) {
                const auto by = readers.find(use.name);
                return by != readers.end() && std::ranges::any_of(by->second, [&](const std::string_view reader) { return reader != id; });
            };
            std::vector<const std::string*> awake;
            std::map<std::string_view, std::vector<const std::string*>> writers;     // a resource, and the passes that run and write it
            std::set<std::string_view> needed;
            std::vector<std::string_view> next;
            for (const auto& id : wanted) {
                // A watch's copy keeps what it copies going on the frames between its copies too: one that goes on from
                // what it made — a simulation looked at — would otherwise go on only when looked at.
                if (id.starts_with('$') && needed.insert(id).second) next.push_back(id);
                if (rests(id)) continue;
                awake.push_back(&id);
                const auto uses = usesOf.find(id);
                if (uses != usesOf.end())
                    for (const Declared& use : uses->second)
                        if (use.write) writers[use.name].push_back(&id);
                const Node* node = current ? current->Find(id) : nullptr;
                const bool kept = uses == usesOf.end() || id.starts_with('$') || built.at(id).pass->writesScreen
                                  || (node && (current->IsSetup(node->pass) || current->IsFixed(node->pass)))
                                  || std::ranges::none_of(uses->second, &Declared::write)
                                  || std::ranges::any_of(uses->second, [&](const Declared& use) {
                                         return use.write && (seenByTheCpu.contains(use.name) || !readByAnother(id, use));
                                     });
                if (kept && needed.insert(id).second) next.push_back(id);
            }
            // What a pass that is needed reads, the passes that write it are needed for.
            while (!next.empty()) {
                const std::string_view id = next.back();
                next.pop_back();
                const auto uses = usesOf.find(id);
                if (uses == usesOf.end()) continue;
                for (const Declared& use : uses->second) {
                    if (!use.read) continue;
                    const auto by = writers.find(use.name);
                    if (by == writers.end()) continue;
                    for (const std::string* writer : by->second)
                        if (needed.insert(*writer).second) next.push_back(*writer);
                }
            }
            for (const std::string* id : awake) {
                if (needed.contains(*id)) continue;
                const auto& plan = *built.at(*id).pass;
                plan.resting->store(true);
                if (plan.entities) plan.entities->idle = true;
                restingUnread.insert(*id);
            }
        }

        // ---- the frame graph ------------------------------------------------------------------------------------

        /**
         * The order passes go to the frame graph in. It runs the writers of a resource in the order they were added,
         * and what they use says more than that: a pass that only writes a resource makes what one that reads and
         * writes it works on, so it goes first. Where usage does not say, the graph's own order stands.
         */
        [[nodiscard]] static std::vector<std::size_t> OverwritesFirst(const std::vector<std::vector<Declared>>& declared, const std::vector<double>& pages)
        {
            const std::size_t count = declared.size();
            std::vector<std::vector<std::size_t>> after(count);
            std::vector<std::size_t> waits(count, 0);
            for (std::size_t first = 0; first < count; ++first)
                for (std::size_t second = 0; second < count; ++second) {
                    // Of one page, or an earlier one: what a later page overwrites, it overwrites after what is before it.
                    if (first == second || pages[first] > pages[second]) continue;
                    const bool ordered = std::ranges::any_of(declared[first], [&](const Declared& made) {
                        return made.write && !made.read && std::ranges::any_of(declared[second], [&](const Declared& used) { return used.name == made.name && used.read && used.write; });
                    });
                    if (ordered) { after[first].push_back(second); ++waits[second]; }
                }
            std::vector<std::size_t> order;
            std::vector<bool> placed(count, false);
            while (order.size() < count) {
                // The earliest in the graph that waits for nothing; in a circle of them, simply the earliest.
                std::size_t next = count;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i] && waits[i] == 0) next = i;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i]) next = i;
                placed[next] = true;
                order.push_back(next);
                for (const std::size_t follower : after[next]) if (waits[follower] > 0) --waits[follower];
            }
            return order;
        }

        /**
         * Tells the frame graph what each pass uses, and puts the passes in it.
         *
         * A resource a pass names by a wire is imported by itself. The resources entities hold are not known one by
         * one — they come and go — so every parameter a pass fills per entity is a set of them. Two uses that have
         * a resource in common are uses of the same thing, and are given the same name: that is what lets a pass
         * sampling the images entities hold be ordered after the one that writes them.
         */
        void Resolve()
        {
            struct Used {
                std::size_t pass = 0;
                bool read = false, write = false, dynamic = false;
                std::vector<Member> members;
                std::string key;
            };
            std::vector<Used> uses;
            for (std::size_t pass = 0; pass < wanted.size(); ++pass) {
                const auto& plan = built.at(wanted[pass]).pass;
                for (const auto& use : plan->uses) uses.push_back({ pass, use.read, use.write, false, { use.what }, {} });
                if (!plan->entities) continue;
                const auto& slots = plan->entities->info->slots;
                for (std::size_t i = 0; i < slots.size() && i < plan->entities->members.size(); ++i)
                    if (!slots[i].sampler) uses.push_back({ pass, slots[i].read, slots[i].write, true, plan->entities->members[i], wanted[pass] + "/" + slots[i].field });
                if (!plan->entities->pictures.empty()) uses.push_back({ pass, true, false, true, plan->entities->pictures, wanted[pass] + "/pictures" });
                if (!plan->entities->storages.empty()) uses.push_back({ pass, true, true, true, plan->entities->storages, wanted[pass] + "/storages" });
            }

            // Uses with a resource in common, together.
            std::vector<std::size_t> parent(uses.size());
            for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = i;
            const std::function<std::size_t(std::size_t)> root = [&](const std::size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
            std::unordered_map<const void*, std::size_t> firstUse;
            for (std::size_t i = 0; i < uses.size(); ++i)
                for (const auto& member : uses[i].members) {
                    const auto [known, fresh] = firstUse.try_emplace(member.id, i);
                    if (!fresh) parent[root(i)] = root(known->second);
                }

            struct Group {
                bool dynamic = false;
                std::string name;
                std::vector<Member> members;
                std::set<const void*> had;
            };
            std::map<std::size_t, Group> groups;
            for (std::size_t i = 0; i < uses.size(); ++i) {
                Group& group = groups[root(i)];
                if (uses[i].dynamic && !group.dynamic) { group.dynamic = true; group.name = "kgraph.set." + uses[i].key; }
                for (const auto& member : uses[i].members)
                    if (group.had.insert(member.id).second) group.members.push_back(member);
            }
            std::set<std::string> live;
            for (auto& group : groups | std::views::values) {
                if (!group.dynamic) {
                    const Member& member = group.members.front();
                    group.name = std::format("kgraph.{:x}", reinterpret_cast<std::uintptr_t>(member.id));
                    if (member.image.Get()) frameGraph.Import(group.name, member.image);
                    else frameGraph.Import(group.name, member.buffer);
                    continue;
                }
                live.insert(group.name);
                Set& set = sets[group.name];
                if (!set.set) {
                    set.set = std::make_shared<kor::ResourceSet>();
                    frameGraph.ImportSet(group.name, set.set);
                }
                std::vector<const void*> ids(group.had.begin(), group.had.end());
                if (ids == set.members) continue;
                set.set->Clear();
                for (const auto& member : group.members) {
                    if (member.image.Get()) set.set->Add(member.image);
                    else set.set->Add(member.buffer);
                }
                set.members = std::move(ids);
            }
            // A set nothing uses any more holds on to nothing.
            for (auto& [name, set] : sets)
                if (!live.contains(name) && !set.members.empty()) { set.set->Clear(); set.members.clear(); }

            std::vector<std::vector<Declared>> declared(wanted.size());
            for (std::size_t i = 0; i < uses.size(); ++i) {
                const std::string& name = groups.at(root(i)).name;
                auto& list = declared[uses[i].pass];
                const auto known = std::ranges::find(list, name, &Declared::name);
                if (known == list.end()) list.push_back({ name, uses[i].read, uses[i].write });
                else { known->read = known->read || uses[i].read; known->write = known->write || uses[i].write; }
            }
            usesOf.clear();
            for (std::size_t pass = 0; pass < wanted.size(); ++pass) usesOf[wanted[pass]] = declared[pass];
            seenByTheCpu.clear();
            for (const auto& group : groups | std::views::values)
                if (std::ranges::any_of(group.members, [](const Member& member) { return member.buffer.Get() && member.buffer->IsHostVisible(); }))
                    seenByTheCpu.insert(group.name);

            // The frame graph runs everything that writes a resource before anything that only reads it. But a pass
            // may read one so as to make what a later writer of it takes — the picture, traced for reflections that
            // are then added to the picture — and after every writer is then after itself. Such a reader takes its
            // place among the writers, which run in the order the graph has them: after those before it.
            {
                const std::size_t count = declared.size();
                // What each pass feeds: the passes that read something it writes.
                std::vector<std::vector<std::size_t>> feeds(count);
                for (std::size_t from = 0; from < count; ++from)
                    for (std::size_t to = 0; to < count; ++to) {
                        if (from == to) continue;
                        const bool fed = std::ranges::any_of(declared[from], [&](const Declared& made) {
                            return made.write && std::ranges::any_of(declared[to], [&](const Declared& used) { return used.name == made.name && used.read; });
                        });
                        if (fed) feeds[from].push_back(to);
                    }
                std::vector<std::pair<std::size_t, std::size_t>> among;     // a pass, and which of its uses
                for (std::size_t pass = 0; pass < count; ++pass)
                    for (std::size_t use = 0; use < declared[pass].size(); ++use) {
                        if (declared[pass][use].write) continue;
                        std::vector<bool> reached(count, false);
                        std::vector<std::size_t> next { pass };
                        reached[pass] = true;
                        bool feedsAWriter = false;
                        while (!next.empty() && !feedsAWriter) {
                            const std::size_t from = next.back();
                            next.pop_back();
                            for (const std::size_t to : feeds[from]) {
                                if (reached[to]) continue;
                                reached[to] = true;
                                next.push_back(to);
                                if (std::ranges::any_of(declared[to], [&](const Declared& theirs) { return theirs.name == declared[pass][use].name && theirs.write; })) { feedsAWriter = true; break; }
                            }
                        }
                        if (feedsAWriter) among.emplace_back(pass, use);
                    }
                for (const auto& [pass, use] : among) declared[pass][use].write = true;
            }

            // A copy of what a page begins with takes its place among the writers, before the page's own: as a reader it
            // would be after every one of them.
            std::vector<double> pages(wanted.size(), current ? static_cast<double>(current->passes.size()) : 0.0);   // in none: after them all
            for (std::size_t pass = 0; pass < wanted.size(); ++pass) {
                if (const Node* node = current ? current->Find(wanted[pass]) : nullptr)
                    if (const auto at = std::ranges::find(current->passes, node->pass); at != current->passes.end())
                        pages[pass] = static_cast<double>(at - current->passes.begin());
                if (before.contains(wanted[pass])) {
                    pages[pass] -= 0.5;
                    for (auto& use : declared[pass]) use.write = true;
                }
            }
            std::vector<std::string> order;
            std::vector<std::vector<Declared>> ordered;
            for (const std::size_t index : OverwritesFirst(declared, pages)) {
                order.push_back(wanted[index]);
                ordered.push_back(std::move(declared[index]));
            }
            StepAgain(order, ordered);
            if (order != passOrder) {
                // Which passes there are, or their order, changed: the order they are added in breaks ties, so all go in again.
                for (auto* pass : passes | std::views::values) frameGraph.Remove(*pass);
                passes.clear();
                for (std::size_t i = 0; i < order.size(); ++i) {
                    GraphPass& pass = frameGraph.Add<GraphPass>(order[i], built.at(StepOf(order[i]).first).pass, std::move(ordered[i]));
                    if (const std::uint32_t step = StepOf(order[i]).second; step > 0) { pass.step = step; pass.steps = fixedNow; }
                    passes.emplace(order[i], &pass);
                }
                passOrder = std::move(order);
                return;
            }
            bool declarations = false;
            for (std::size_t i = 0; i < order.size(); ++i) {
                GraphPass* pass = passes.at(order[i]);
                const auto& plan = built.at(StepOf(order[i]).first).pass;
                if (pass->plan != plan) {
                    declarations = declarations || pass->plan->readsScreen != plan->readsScreen || pass->plan->writesScreen != plan->writesScreen
                                || pass->plan->async != plan->async;   // which queue it is on is said in its Setup
                    pass->plan = plan;
                }
                if (pass->declared != ordered[i]) {
                    pass->declared = std::move(ordered[i]);
                    declarations = true;
                }
            }
            if (declarations) frameGraph.Invalidate();
        }

        /**
         * The passes of the fixed step once more for each further step a frame can have (Graph::fixedStepsAtMost), each
         * step's after the last's: `node\nstep 2`, and so on. Within them, what one reads that one of them writes is read
         * among the writers — which run in the order they are added, step after step — and not after every one of them,
         * as what only reads is: so that a step reads what the step before it wrote, and not what the last step will.
         */
        void StepAgain(std::vector<std::string>& order, std::vector<std::vector<Declared>>& ordered) const
        {
            if (!current || current->fixed.empty() || current->fixedStepsAtMost < 1) return;
            std::vector<std::size_t> fixedAt;
            for (std::size_t i = 0; i < order.size(); ++i)
                if (const Node* node = current->Find(order[i]); node && !order[i].starts_with('$') && current->IsFixed(node->pass)) fixedAt.push_back(i);
            if (fixedAt.empty()) return;
            std::set<std::string> written;
            for (const std::size_t i : fixedAt)
                for (const auto& use : ordered[i]) if (use.write) written.insert(use.name);
            for (const std::size_t i : fixedAt)
                for (auto& use : ordered[i]) if (written.contains(use.name)) use.write = true;
            std::vector<std::string> steppedOrder;
            std::vector<std::vector<Declared>> stepped;
            for (std::size_t i = 0; i < order.size(); ++i) {
                steppedOrder.push_back(order[i]);
                stepped.push_back(ordered[i]);
                if (i != fixedAt.back()) continue;
                for (int step = 1; step < current->fixedStepsAtMost; ++step)
                    for (const std::size_t j : fixedAt) {
                        steppedOrder.push_back(std::format("{}\nstep {}", order[j], step + 1));
                        stepped.push_back(ordered[j]);
                    }
            }
            order = std::move(steppedOrder);
            ordered = std::move(stepped);
        }

        /** A pass's name in the frame graph as its node's id and which fixed step it is for, from 0 (the first, or not a step's). */
        static std::pair<std::string, std::uint32_t> StepOf(const std::string& name)
        {
            const auto at = name.find("\nstep ");
            if (at == std::string::npos) return { name, 0 };
            return { name.substr(0, at), static_cast<std::uint32_t>(std::stoul(name.substr(at + 6)) - 1) };
        }

        /** Puts the passes that built in the frame graph, and takes out those that are gone. */
        void Install(const Graph& graph)
        {
            wanted.clear();
            for (const auto& node : graph.nodes)
                if (const auto it = built.find(node.id); it != built.end() && it->second.status.ok && it->second.pass) wanted.push_back(node.id);
            // In the order of the passes they are part of: what is drawn first where what is used does not say. One
            // that is in no pass keeps its place among the rest of those.
            const auto place = [&](const std::string& id) {
                const Node* node = graph.Find(id);
                const auto at = node ? std::ranges::find(graph.passes, node->pass) : graph.passes.end();
                return at == graph.passes.end() ? graph.passes.size() : static_cast<std::size_t>(at - graph.passes.begin());
            };
            std::ranges::stable_sort(wanted, {}, place);
            // A pass of the fixed step rests until a frame says it has a step: the frame it is put in is not readied as
            // others are (EachFrame), and would run it once whether or not it had one.
            for (const auto& id : wanted)
                if (const Node* node = graph.Find(id); node && graph.IsFixed(node->pass)) built.at(id).pass->resting->store(true);
            // What the passes used is told again below: what was told of the graph before is of other resources, and
            // nothing rests for want of a reader until it is.
            usesOf.clear();
            PrepareAll();
            Resolve();
        }

        /**
         * After a shader was edited: what is wired to a parameter it no longer has, or has as another kind, is taken
         * off the node — and a constant it no longer has. What keeps its name and kind stays as it is.
         */
        void ResetStale(Graph& graph)
        {
            // Each pipeline's parameters and push constants as its shaders have them now: what its draws can be given.
            std::map<std::string, std::pair<Interface, std::set<std::string>>> now;
            for (auto& node : graph.nodes) {
                const NodeType* type = FindType(node.type);
                if (!type || !type->builder || !IsPipeline(type->name)) continue;
                std::vector<kor::ResourceRef<const kor::Shader>> shaders;
                const auto outputOf = [&](const Input& input) -> Held {
                    if (!input.link) return nullptr;
                    const auto source = built.find(input.link->node);
                    if (source == built.end()) return nullptr;
                    const auto output = source->second.outputs.find(input.link->output);
                    return output == source->second.outputs.end() ? nullptr : output->second.resource;
                };
                for (const auto& input : node.inputs | std::views::values)
                    if (const Held resource = outputOf(input); resource && resource->kind == "Shader" && resource->handle)
                        shaders.push_back(kor::capi::ShaderOf(resource->handle));
                // And those it names itself, as they were when it was last built.
                if (const auto it = built.find(node.id); it != built.end() && it->second.pipeline)
                    for (const Held& named : it->second.pipeline->named)
                        if (named && named->handle) shaders.push_back(kor::capi::ShaderOf(named->handle));
                if (shaders.empty() || !std::ranges::all_of(shaders, [](const auto& shader) { return shader.Valid(); })) continue;   // one that does not compile says nothing yet
                const Interface interface = Reflect(shaders, Kinds(), world);
                std::set<std::string> pushes;
                for (const auto& shader : shaders)
                    for (const auto& block : shader->BlockLayout().pushConstants | std::views::values)
                        for (const auto& member : block.members) pushes.insert("push." + member.name);
                now[node.id] = { interface, std::move(pushes) };
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    const std::string& name = it->first;
                    bool stale = false;
                    if (name.starts_with("param.") || name.starts_with("constant.")) {
                        const Port* port = interface.Find(name);
                        stale = !port;
                        if (port && port->kind == PortKind::eResource)
                            if (const Held given = outputOf(it->second); given && !given->screen)
                                stale = port->type == "Image" ? given->kind != "Image" && given->kind != "ImageView" : given->kind != port->type;
                    }
                    if (!stale) { ++it; continue; }
                    reset.push_back(std::format("{}.{}", node.id, name));
                    it = node.inputs.erase(it);
                }
            }
            // A draw's own, the same way: what its pipeline's shaders no longer have is taken off it.
            for (auto& node : graph.nodes) {
                if (node.type != "Draw" && node.type != "Dispatch" && node.type != "TraceRays") continue;
                const auto pipeline = node.inputs.find("pipeline");
                const Node* of = pipeline != node.inputs.end() && pipeline->second.link ? Origin(pipeline->second.link->node) : nullptr;
                const auto found = of ? now.find(of->id) : now.end();
                if (found == now.end()) continue;
                const auto& [interface, pushes] = found->second;
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    const std::string& name = it->first;
                    const bool stale = (name.starts_with("param.") && !interface.Find(name)) || (name.starts_with("push.") && !pushes.contains(name));
                    if (!stale) { ++it; continue; }
                    reset.push_back(std::format("{}.{}", node.id, name));
                    it = node.inputs.erase(it);
                }
            }
        }
    };

    Runtime::Runtime(kor::FrameGraph& frameGraph, kecs::World* world) : _impl(std::make_unique<Impl>(frameGraph, world))
    {
    }
    Runtime::Runtime(kor::FrameGraph& frameGraph, kecs::World* world, Runtime& source) : _impl(std::make_unique<Impl>(frameGraph, world))
    {
        _impl->source = source._impl->source ? source._impl->source : source._impl.get();
    }
    Runtime::~Runtime() = default;

    // A node that cannot be built keeps why (Status), and an entity that cannot be run is reported (ReportOf): the
    // framework's own account of each failure is kept out of the log, which a graph being edited would fill with it.

    void Runtime::Set(Graph graph)
    {
        const kor::log::Quiet quiet;
        _graph = std::move(graph);
        ++_revision;
        _impl->SetMeshImports(_graph.meshImports);
        _impl->SetTextureImports(_graph.textureImports, _graph.texturePacks);
        _impl->SetDefaultMeshTangents(_graph.defaultMeshTangents);
        Run();
    }

    TexelLayout TexelLayoutOf(const std::string_view format)
    {
        // `eRGBA16_SFLOAT`: its channels, how many bits each, and what the bits are.
        TexelLayout layout;
        std::size_t at = format.starts_with('e') ? 1 : 0;
        int channels = 0;
        while (at < format.size() && std::string_view("RGBAD").find(format[at]) != std::string_view::npos) { layout.depth = format[at] == 'D'; ++channels; ++at; }
        int bits = 0;
        while (at < format.size() && std::isdigit(static_cast<unsigned char>(format[at]))) bits = bits * 10 + (format[at++] - '0');
        const std::string_view kind = at < format.size() ? format.substr(at + 1) : std::string_view();
        layout.kind = kind == "UNORM" || kind == "SRGB" ? 'u' : kind == "SNORM" ? 's' : kind == "UINT" ? 'U' : kind == "SINT" ? 'I' : kind == "SFLOAT" ? 'f' : 0;
        if (channels == 0 || layout.kind == 0 || (bits != 8 && bits != 16 && bits != 32)) return {};
        layout.channels = channels;
        layout.bits = bits;
        return layout;
    }

    namespace {
        /** A format's name, as a graph has it: `eRGBA8_UNORM`. */
        std::string FormatName(const kor::Image::Format format)
        {
            const kb::Description* image = kb::Find("Image");
            const kb::Setting* setting = image ? image->FindSetting("format") : nullptr;
            if (!setting || setting->arguments.empty() || !setting->arguments.front().type->enumeration) return {};
            for (const auto& entry : setting->arguments.front().type->enumeration->values)
                if (entry.value == static_cast<std::uint64_t>(format)) return std::string(entry.name);
            return {};
        }
    }

    void Runtime::SetAssetDirectory(std::filesystem::path directory) { _impl->assetDirectory = std::move(directory); }
    const std::filesystem::path& Runtime::AssetDirectory() const { return _impl->Assets(); }
    std::string Runtime::AssetId(const std::filesystem::path& file, const int mesh) const { return _impl->AssetId(file, mesh); }
    std::shared_ptr<const Model> Runtime::ModelOf(const std::filesystem::path& path, std::string* error) const
    {
        Impl& files = _impl->source ? *_impl->source : *_impl;
        const std::filesystem::path file = files.AssetFile(path.generic_string());
        const std::string key = file.lexically_normal().generic_string();
        if (const auto it = files.models.find(key); it != files.models.end()) {
            if (!it->second && error) *error = files.unread[key];
            return it->second;
        }
        files.WantModel(file);
        return nullptr;
    }

    bool Runtime::MakeMesh(const std::string& id, const VertexData& data, std::string* error)
    {
        Impl& files = _impl->source ? *_impl->source : *_impl;
        auto made = files.MeshOf(data);
        if (!made) { if (error) *error = made.error(); return false; }
        (*made)->node = id;
        (*made)->kind = "Mesh";
        Held& kept = files.assets[id];
        if (kept && kept != *made) { kept->replacedBy = *made; ++Impl::replacements; }
        kept = std::move(*made);
        // A generator's, kept in the project's cache: there when the scene is opened again without its scripts.
        if (Impl::Generated(id))
            if (const auto cache = Impl::CacheBeside(files.Assets())) (void)cache->Write("meshes", Impl::GeneratedKey(id), ".kmesh", kasset::Serialize(data));
        return true;
    }

    bool Runtime::MakeImage(const std::string& id, const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba, std::string* error)
    {
        Impl& files = _impl->source ? *_impl->source : *_impl;
        auto made = Impl::PictureOf(width, height, rgba);
        if (!made) { if (error) *error = made.error(); return false; }
        (*made)->node = id;
        Held& kept = files.assets[id];
        if (kept && kept != *made) { kept->replacedBy = *made; ++Impl::replacements; }
        kept = std::move(*made);
        if (Impl::Generated(id))
            if (const auto cache = Impl::CacheBeside(files.Assets())) (void)cache->Write("images", Impl::GeneratedKey(id), ".kimage", Impl::PictureBytes(width, height, rgba));
        return true;
    }

    std::optional<VertexData> Runtime::ReadMesh(const std::string_view id, std::string* error) const
    {
        const auto fail = [&](std::string why) -> std::optional<VertexData> { if (error) *error = std::move(why); return std::nullopt; };
        Held held = ResourceOf(id);
        while (held && held->replacedBy) held = held->replacedBy;
        if (!held || held->kind != "Mesh") return fail(std::format("'{}' is no mesh", id));
        if (held->pending) return fail(std::format("'{}' is still being read", id));
        const Resource* whole = held->part ? held->source.get() : held.get();
        if (!whole || !whole->handle) return fail(std::format("'{}' has no mesh on the device", id));
        const auto mesh = kor::capi::MeshOf(whole->handle);
        const auto& layout = mesh->Layout();
        const auto buffers = mesh->VertexBuffers();
        const std::uint32_t first = held->part ? static_cast<std::uint32_t>(held->vertexOffset) : 0;
        const std::uint32_t count = held->part ? held->vertexCount : static_cast<std::uint32_t>(mesh->VertexCount());
        // Each buffer, and the indices, copied where they can be read, all at once.
        const auto readable = [](const kor::ResourceRef<const kor::Buffer>& from) {
            return kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(from->size())).SetUsage(kor::Buffer::Usage::eTransferDst)
                .SetType(kor::Buffer::Type::eReadback).Build();
        };
        std::vector<kor::Resource<kor::Buffer>> copies;
        for (const auto& buffer : buffers) copies.push_back(readable(buffer));
        std::optional<kor::Resource<kor::Buffer>> indexCopy;
        if (const auto indices = mesh->IndexBuffer()) indexCopy = readable(*indices);
        kor::CommandBuffer::SingleTimeCommand([&](kor::CommandBuffer& cb) {
            for (std::size_t b = 0; b < buffers.size(); ++b) cb.CopyBuffer(buffers[b], kor::ResourceRef<const kor::Buffer>(copies[b]));
            if (indexCopy) cb.CopyBuffer(*mesh->IndexBuffer(), kor::ResourceRef<const kor::Buffer>(*indexCopy));
        }, kor::CommandBuffer::Usage::eTransfer).Wait();

        VertexData data;
        data.vertexCount = count;
        std::map<kor::u32, std::vector<std::string>> byBuffer;
        for (const auto& attribute : layout.attributes) {
            if (attribute.channelType != kor::ChannelType::eFloat && attribute.channelType != kor::ChannelType::eInt && attribute.channelType != kor::ChannelType::eUInt)
                return fail(std::format("'{}' of '{}' is not of four-byte numbers, and is not read back", attribute.semantic, id));
            const AttributeType type = attribute.channelType == kor::ChannelType::eInt ? AttributeType::eInt
                                     : attribute.channelType == kor::ChannelType::eUInt ? AttributeType::eUInt : AttributeType::eFloat;
            if (attribute.binding >= copies.size() || attribute.locations != 1) return fail(std::format("'{}' of '{}' is not read back", attribute.semantic, id));
            const auto binding = std::ranges::find(layout.bindings, attribute.binding, &kor::VertexInputBindingDescription::binding);
            if (binding == layout.bindings.end()) return fail(std::format("'{}' has no buffer {}", id, attribute.binding));
            const auto words = copies[attribute.binding]->Read<std::uint32_t>();
            VertexAttribute out { attribute.semantic, type, attribute.channelCount, {} };
            out.words.reserve(static_cast<std::size_t>(count) * attribute.channelCount);
            for (std::uint32_t v = first; v < first + count; ++v) {
                const std::size_t at = (static_cast<std::size_t>(v) * binding->stride + attribute.offset) / 4;
                for (std::uint32_t c = 0; c < attribute.channelCount; ++c) out.words.push_back(at + c < words.size() ? words[at + c] : 0u);
            }
            data.attributes.push_back(std::move(out));
            byBuffer[attribute.binding].push_back(attribute.semantic);
        }
        if (byBuffer.size() > 1) for (auto& semantics : byBuffer | std::views::values) data.buffers.push_back(std::move(semantics));
        if (indexCopy) {
            const auto indices = (*indexCopy)->Read<std::uint32_t>();
            const std::size_t from = held->part ? held->firstIndex : 0, n = held->part ? held->indexCount : indices.size();
            data.indices.assign(indices.begin() + static_cast<std::ptrdiff_t>(std::min(from, indices.size())),
                                indices.begin() + static_cast<std::ptrdiff_t>(std::min(from + n, indices.size())));
        }
        return data;
    }

    std::optional<std::vector<Model::Attribute>> Runtime::ModelAttributes(const std::filesystem::path& path, std::string* error) const
    {
        const std::filesystem::path file = _impl->AssetFile(path.generic_string());
        const std::string key = file.lexically_normal().generic_string();
        // Read already: what it has is known.
        if (const auto it = _impl->models.find(key); it != _impl->models.end() && it->second) return it->second->attributes;
        if (const auto it = _impl->probed.find(key); it != _impl->probed.end()) {
            if (!it->second && error) *error = _impl->unread[key];
            return it->second;
        }
        _impl->probed[key] = std::nullopt;     // being probed: asked again, it is not probed twice
        _impl->reading.push_back(Impl::ProbeModelFile(_impl.get(), _impl->alive, key, file));
        return std::nullopt;
    }

    std::vector<std::string> Runtime::Assets() const
    {
        std::vector<std::string> ids;
        for (const auto& [id, held] : _impl->assets)
            if (held && id.starts_with('@')) ids.push_back(id);
        return ids;
    }

    std::vector<std::string> Runtime::Pictures() const
    {
        std::vector<std::string> ids;
        for (const auto& [id, held] : _impl->source ? _impl->source->assets : _impl->assets)
            if (held && id.starts_with('@') && id.find('#') == std::string::npos && held->kind != "Mesh") ids.push_back(id);
        return ids;
    }
    bool Runtime::Cooked(const std::string_view id) const { return (_impl->source ? *_impl->source : *_impl).Cooked(id); }
    void Runtime::Cook(const std::vector<std::string>& ids) { _impl->Cook(ids); }
    const Runtime::CookProgress& Runtime::Cooking() const { return (_impl->source ? _impl->source : _impl.get())->cooking; }
    std::filesystem::path Runtime::TextureCache() const { return _impl->TextureCache(); }

    std::size_t Runtime::Reading() const { return _impl->reading.size() + _impl->arrivedMeshes.size(); }
    std::vector<std::string> Runtime::Unread() const
    {
        std::vector<std::string> said;
        for (const auto& why : _impl->unread | std::views::values) said.push_back(why);
        return said;
    }

    void Runtime::Watch(const std::string_view id)
    {
        if (_impl->watched.try_emplace(std::string(id)).second) _impl->rewatch = true;
    }
    void Runtime::Watch(const std::string_view id, const std::uint32_t mip, const std::uint32_t layer)
    {
        auto [it, made] = _impl->watched.try_emplace(std::string(id));
        if (made || it->second.mip != mip || it->second.layer != layer) _impl->rewatch = true;
        it->second.mip = mip;
        it->second.layer = layer;
    }
    void Runtime::WatchEvery(const std::string_view id, const std::uint32_t frames)
    {
        auto [it, made] = _impl->watched.try_emplace(std::string(id));
        if (made) _impl->rewatch = true;
        it->second.every = std::max<std::uint32_t>(frames, 1);
    }

    void Runtime::WatchOutput(const std::string_view id, std::string output)
    {
        auto [it, made] = _impl->watched.try_emplace(std::string(id));
        if (made || it->second.output != output) _impl->rewatch = true;
        it->second.output = std::move(output);
    }

    std::string Runtime::ShownOutput(const std::string_view id) const
    {
        if (_impl->calls.contains(id) || _impl->aliases.contains(id)) return "out";
        const auto* node = _impl->Find(id);
        if (!node) return "out";
        if (const auto watch = _impl->watched.find(id); watch != _impl->watched.end() && !watch->second.output.empty())
            if (const auto it = node->outputs.find(watch->second.output); it != node->outputs.end() && it->second.resource) return watch->second.output;
        if (node->outputs.contains("out")) return "out";
        // In the order the node gives them: a Script node's as its script declares them.
        std::string first;
        for (const auto& port : OutputsOf(id)) {
            const auto it = node->outputs.find(port.name);
            if (it == node->outputs.end() || !it->second.resource) continue;
            if (ImageRefOf(it->second.resource.get()).Get()) return port.name;
            if (first.empty()) first = port.name;
        }
        return first.empty() ? std::string("out") : first;
    }

    void Runtime::Unwatch(const std::string_view id)
    {
        if (const auto it = _impl->watched.find(id); it != _impl->watched.end()) { _impl->watched.erase(it); _impl->rewatch = true; }
    }

    // What a watched node is copied out with: a buffer the size of what it holds that can be read back, and the pass
    // that copies into it. Two nodes like any other, added to the graph that runs and to nothing that is saved.
    static std::string PeekId(const std::string_view id) { return std::format("$peek {}", id); }
    static std::string PeekBufferId(const std::string_view id) { return std::format("$peek buffer {}", id); }
    static std::string PeekSourceId(const std::string_view id) { return std::format("$peek source {}", id); }

    namespace {
        /** Which level and layer of an image held as @p held a watch copies: what was asked, within what there is. */
        struct Subresource {
            kor::u32 base = 0, levels = 1, level = 0, layer = 0;
            kor::UVec3 size {};
        };
        Subresource SubresourceOf(const Resource& held, const kor::Image& image, const std::uint32_t mip, const std::uint32_t layer)
        {
            Subresource at;
            at.base = held.kind == "ImageView" ? kor::capi::ImageViewOf(held.handle)->BaseMipLevel() : 0;
            at.levels = std::max<kor::u32>(image.MipLevels() - std::min(at.base, image.MipLevels() - 1), 1);
            at.level = at.base + std::min<kor::u32>(mip, at.levels - 1);
            at.layer = std::min<kor::u32>(layer, std::max<kor::u32>(image.ArrayLayers(), 1) - 1);
            const auto extent = image.Extent();
            at.size = { std::max<kor::u32>(extent.x >> at.level, 1), std::max<kor::u32>(extent.y >> at.level, 1), std::max<kor::u32>(extent.z >> at.level, 1) };
            return at;
        }

        std::vector<std::string> UsageNames(const kor::Flags<kor::Image::Usage> usage)
        {
            std::vector<std::string> names;
            for (const auto& [flag, name] : { std::pair { kor::Image::Usage::eTransferSrc, "eTransferSrc" }, { kor::Image::Usage::eTransferDst, "eTransferDst" },
                                              { kor::Image::Usage::eSampled, "eSampled" }, { kor::Image::Usage::eStorage, "eStorage" },
                                              { kor::Image::Usage::eColorAttachment, "eColorAttachment" }, { kor::Image::Usage::eDepthStencilAttachment, "eDepthStencilAttachment" } })
                if (usage & flag) names.emplace_back(name);
            return names;
        }
    }

    std::string Runtime::WatchProblem(const std::string_view id) const
    {
        if (!_impl->watched.contains(id)) return "it is not watched";
        const auto held = ResourceOf(id);
        if (held && held->pending) return "it is still being read";
        if (!held || (!ImageRefOf(held.get()).Get() && !BufferRefOf(held.get()).Get())) return "it makes no image and no buffer";
        if (const auto image = ImageRefOf(held.get()); image.Get() && TexelLayoutOf(FormatName(image->PixelFormat())).channels == 0)
            return std::format("an image of {} cannot be looked at", FormatName(image->PixelFormat()));
        for (const std::string& node : { PeekSourceId(id), PeekBufferId(id), PeekId(id) })
            if (const auto it = _impl->built.find(node); it != _impl->built.end() && !it->second.status.ok) return it->second.status.error;
        return {};
    }

    std::optional<Peek> Runtime::Watched(const std::string_view id) const
    {
        const auto watch = _impl->watched.find(id);
        if (watch == _impl->watched.end()) return std::nullopt;
        // A node, or a picture of a file's that no node is (`@wall.png`).
        const auto* node = _impl->Find(id);
        const auto held = ResourceOf(id);
        if ((!node && !id.starts_with('@')) || !held || held->pending) return std::nullopt;
        Peek peek;
        std::string now;
        if (const auto image = ImageRefOf(held.get()); image.Get()) {
            const Subresource at = SubresourceOf(*held, *image, watch->second.mip, watch->second.layer);
            const auto extent = image->Extent();
            peek.image = true;
            peek.format = FormatName(image->PixelFormat());
            peek.extent = at.size;
            peek.fullExtent = { std::max<kor::u32>(extent.x >> at.base, 1), std::max<kor::u32>(extent.y >> at.base, 1), std::max<kor::u32>(extent.z >> at.base, 1) };
            peek.mips = at.levels;
            peek.layers = std::max<kor::u32>(image->ArrayLayers(), 1);
            peek.samples = static_cast<std::uint32_t>(image->Samples());
            peek.mip = at.level - at.base;
            peek.layer = at.layer;
            peek.type = image->ImageType() == kor::Image::Type::e1D ? "1D" : image->ImageType() == kor::Image::Type::e3D ? "3D" : "2D";
            peek.usage = UsageNames(image->UsageFlags());
            const TexelLayout texel = TexelLayoutOf(peek.format);
            for (kor::u32 level = at.base; level < at.base + at.levels; ++level)
                peek.byteSize += static_cast<std::uint64_t>(std::max<kor::u32>(extent.x >> level, 1)) * std::max<kor::u32>(extent.y >> level, 1)
                               * std::max<kor::u32>(extent.z >> level, 1) * texel.Bytes() * peek.layers;
            if (texel.channels == 0) return std::nullopt;     // WatchProblem says why
            now = std::format("image {} {}x{}x{} {} {} of {}", peek.format, peek.extent.x, peek.extent.y, peek.extent.z, at.level, at.layer, ShownOutput(id));
        } else if (const auto buffer = BufferRefOf(held.get()); buffer.Get()) {
            now = std::format("buffer {} of {}", buffer->size(), ShownOutput(id));
            peek.byteSize = buffer->size();
            if (node && node->mapped) {
                peek.stride = node->mapped->layout.stride;
                peek.count = static_cast<std::uint32_t>(node->mapped->count);
                for (const auto& field : node->mapped->layout.fields) peek.fields.push_back({ field.name, field.offset, field.scalar, field.Numbers() });
            } else if (const Node* drawn = _impl->current ? _impl->current->Find(id) : nullptr; node && node->packed && drawn && drawn->type == "IndirectDraws") {
                // An IndirectDraws': a VkDrawIndexedIndirectCommand a mesh — how many of it are drawn, from which instance.
                peek.stride = 20;
                peek.count = static_cast<std::uint32_t>(node->packed->bytes.size() / 20);
                peek.fields = { { "indexCount", 0, 'u', 1 }, { "instanceCount", 4, 'u', 1 }, { "firstIndex", 8, 'u', 1 },
                                { "vertexOffset", 12, 'i', 1 }, { "firstInstance", 16, 'u', 1 } };
            }
        } else {
            return std::nullopt;
        }
        // Not what it was when the copy was set up — another size, or only now built: set up again, next frame.
        if (now != watch->second.setUp) { _impl->rewatch = true; return std::nullopt; }
        const auto copied = _impl->built.find(PeekBufferId(id));
        const auto copy = _impl->built.find(PeekId(id));
        if (copied == _impl->built.end() || copy == _impl->built.end() || !copied->second.status.ok || !copy->second.status.ok) return std::nullopt;
        const auto out = copied->second.outputs.find("out");
        if (out == copied->second.outputs.end() || !out->second.resource || !out->second.resource->handle) return std::nullopt;
        peek.bytes = kor::capi::BufferOf(out->second.resource->handle)->Read<std::byte>();
        return peek;
    }

    void Runtime::Run()
    {
        // What a ForEach's element holds: where it is, and its component's fields — of `with`'s too, by their own names,
        // its component's coming first — as the world describes them now.
        const ElementOf elementOf = [this](const Node& node) {
            std::vector<std::pair<std::string, std::string>> members;
            for (const char* place : { "entityId", "matrix", "normalMatrix", "previousMatrix", "worldPosition", "forward", "up", "right", "position", "rotation", "orientation", "scale" })
                members.emplace_back(place, place);
            kecs::World* world = _impl->world;
            if (!world) return members;
            const auto add = [&](const std::string& component, const bool own) {
                const kecs::ComponentId id = world->Find(component);
                if (id == kecs::NoComponent) return;
                const kecs::ComponentDescription& description = world->Describe(id);
                for (const auto& field : description.fields) {
                    if (field.type == "string") continue;   // nothing a buffer holds
                    const std::string output = own ? "field." + field.name : description.name + "." + field.name;
                    if (const auto had = std::ranges::find(members, field.name, &std::pair<std::string, std::string>::first); had != members.end()) {
                        if (own) had->second = output;     // its own component's, before where it is
                    } else {
                        members.emplace_back(field.name, output);
                    }
                }
            };
            if (const auto component = node.inputs.find("component"); component != node.inputs.end() && component->second.value.String()) add(*component->second.value.String(), true);
            for (const char* listed : { "with" })
                if (const auto with = node.inputs.find(listed); with != node.inputs.end() && with->second.value.Items())
                    for (const auto& name : *with->second.value.Items())
                        if (name.String()) add(*name.String(), false);
            return members;
        };
        // What of a component a ComponentBuffer can hold: each field that is a number, a vector or a matrix.
        const FieldsOf fieldsOf = [this](const std::string_view component) -> std::optional<std::vector<std::pair<std::string, std::string>>> {
            kecs::World* world = _impl->world;
            const kecs::ComponentId id = world ? world->Find(component) : kecs::NoComponent;
            if (id == kecs::NoComponent) return std::nullopt;
            std::vector<std::pair<std::string, std::string>> fields;
            for (const auto& field : world->Describe(id).fields)
                if (const std::string_view type = LayoutTypeOf(field.type); !type.empty()) fields.emplace_back(field.name, type);
            return fields;
        };
        Expansion expansion = Expand(_graph, elementOf, fieldsOf);
        _impl->before.clear();
        // What is watched is copied out by two nodes more, sized by what the node made when last it was built.
        for (auto& [id, watching] : _impl->watched) {
            std::string& setUp = watching.setUp;
            setUp.clear();
            const auto held = ResourceOf(id);
            if (!held || held->pending || expansion.graph.Find(PeekId(id))) continue;
            std::size_t bytes = 0;
            Node copy;
            copy.id = PeekId(id);
            // The node itself, or — for one of a loop — what it is on its first turn; a file's picture, by a node of its own.
            std::string source = expansion.graph.Find(id) ? std::string(id) : TurnId(id, 0);
            if (!expansion.graph.Find(source) && id.starts_with('@') && !id.contains('#')) {
                source = PeekSourceId(id);
                expansion.graph.Add(source, "ImageFile").Set("file", id.substr(1));
            }
            if (!expansion.graph.Find(source)) continue;
            if (const auto image = ImageRefOf(held.get()); image.Get()) {
                const std::string format = FormatName(image->PixelFormat());
                const TexelLayout texel = TexelLayoutOf(format);
                if (texel.channels == 0) continue;
                const Subresource at = SubresourceOf(*held, *image, watching.mip, watching.layer);
                bytes = static_cast<std::size_t>(at.size.x) * at.size.y * at.size.z * texel.Bytes();
                copy.type = "CopyImageToBuffer";
                copy.Wire("image", source, ShownOutput(id)).Wire("buffer", PeekBufferId(id));
                if (at.level > 0) copy.Set("mip", at.level);
                if (at.layer > 0) copy.Set("layer", at.layer);
                setUp = std::format("image {} {}x{}x{} {} {} of {}", format, at.size.x, at.size.y, at.size.z, at.level, at.layer, ShownOutput(id));
            } else if (const auto buffer = BufferRefOf(held.get()); buffer.Get()) {
                bytes = static_cast<std::size_t>(buffer->size());
                copy.type = "CopyBuffer";
                copy.Wire("source", source, ShownOutput(id)).Wire("destination", PeekBufferId(id));
                setUp = std::format("buffer {} of {}", bytes, ShownOutput(id));
            } else {
                continue;
            }
            Node& into = expansion.graph.Add(PeekBufferId(id), "Buffer");
            into.Set("instanceCount", static_cast<double>(bytes)).Set("usage", Value::List { "eTransferDst" }).Set("type", "eReadback");
            // A Reference's is of what it stands for as it is where the Reference is: when its page begins, first of it.
            if (const Node* reference = expansion.graph.Find(id); reference && reference->type == "Reference" && !reference->pass.empty()) {
                copy.pass = reference->pass;
                _impl->before.insert(copy.id);
                const auto first = std::ranges::find(expansion.graph.nodes, copy.pass, &Node::pass);
                expansion.graph.nodes.insert(first, std::move(copy));
            } else {
                expansion.graph.nodes.push_back(std::move(copy));
            }
        }
        _impl->rewatch = false;
        _impl->unwritten.clear();
        for (auto& [id, why] : expansion.errors) _impl->unwritten[id] = NodeStatus { false, std::move(why), 0, {} };
        _impl->turns = std::move(expansion.turns);
        _impl->calls = std::move(expansion.calls);
        _impl->aliases = std::move(expansion.aliases);
        _impl->bundles = std::move(expansion.bundles);
        _impl->fromBundle = std::move(expansion.fromBundle);
        _impl->bundleNotes = std::move(expansion.notes);
        _impl->batched = std::move(expansion.batched);
        _impl->unbatched = std::move(expansion.unbatched);
        _running = std::move(expansion.graph);
        _impl->Evaluate(_running);
    }

    void Runtime::Retire(std::shared_ptr<Resource> resource)
    {
        if (resource) _impl->retiredResources.push_back(std::move(resource));
    }

    void Runtime::Update()
    {
        const kor::log::Quiet quiet;
        const Impl::Using using_(*_impl, std::string(Impl::EntitiesUse));
        // A frame, counted before anything can end it early: a buffer of instances this runtime did not fill in it — a
        // frame it was built again in — is filled by a runtime that follows it.
        ++*_impl->frames;
        _impl->LetGo();
        if (_impl->reloaded.exchange(false)) {
            _impl->ResetStale(_graph);
            // Which shader it was is not said: every shader node counts as built again, and what is wired to one follows.
            // A pipeline that names its shaders is itself one of them: it is built again, whatever its inputs say.
            for (auto& node : _impl->built | std::views::values) {
                if (node.watches.empty()) continue;
                node.stamp = ++_impl->counter;
                if (node.pipeline || node.mapped) node.signature.clear();
            }
            Run();
            return;
        }
        if (_impl->shaderFailed && std::chrono::steady_clock::now() - _impl->lastRetry > std::chrono::milliseconds(500)) {
            _impl->lastRetry = std::chrono::steady_clock::now();
            _impl->shaderFailed = false;    // set again by whichever still fails
            for (auto& node : _impl->built | std::views::values)
                if (!node.status.ok) node.signature.clear();
            _impl->Evaluate(_running);
            return;
        }
        if (_impl->rewatch) { Run(); return; }
        if (_impl->editorViewAsked != _impl->editorView || _impl->debugViewAsked != _impl->debugView || _impl->shaderDebugAsked != _impl->shaderDebug) {
            _impl->editorView = _impl->editorViewAsked;
            _impl->debugView = _impl->debugViewAsked;
            _impl->shaderDebug = _impl->shaderDebugAsked;
            _impl->Evaluate(_running);
            return;
        }
        // A resource script set again: what it fills is made and filled again — here, and in a runtime that follows this one.
        if (const std::uint64_t version = _impl->ScriptsVersion(); version != _impl->scriptsSeen) {
            _impl->scriptsSeen = version;
            _impl->Evaluate(_running);
            return;
        }
        _impl->Arrive();
        _impl->Watch();
        WriteBack();
        _impl->EachFrame();
        if (std::exchange(_impl->stale, false)) { _impl->Evaluate(_running); return; }
        if (_impl->PrepareAll()) _impl->Resolve();
    }

    void Runtime::WriteBack()
    {
        // A view that follows another writes nothing back: the one it follows does, of the same world.
        if (_impl->source || !_impl->world || !_impl->current) return;
        kecs::World& world = *_impl->world;
        // Setup still being recorded (once for each copy of what it makes): it is made from the world as it is, and what is
        // read back is of what it is about to make again — written back, setup would be made from that instead.
        const bool remaking = _impl->setupAgain || std::ranges::any_of(_impl->built | std::views::values, [](const auto& built) { return built.setup && built.runsLeft > 0; });
        std::set<std::string> seen;
        for (const Node& node : _impl->current->nodes) {
            if (node.type != "WriteBack") continue;
            seen.insert(node.id);
            const NodeStatus* status = Status(node.id);
            const auto buffer = node.inputs.find("buffer");
            const auto each = node.inputs.find("forEach");
            if (!status || !status->ok || buffer == node.inputs.end() || !buffer->second.link || each == node.inputs.end() || !each->second.value.String()) continue;
            const std::string& from = buffer->second.link->node;
            // Idle: what it watches is let be — and what it would take as moved by something else is not looked at.
            if (const auto idle = node.inputs.find("idle"); idle != node.inputs.end()) {
                Value said = idle->second.value;
                if (idle->second.link)
                    if (const auto source = _impl->built.find(idle->second.link->node); source != _impl->built.end())
                        if (const auto output = source->second.outputs.find(idle->second.link->output); output != source->second.outputs.end()) said = output->second.value;
                if (said.Bool() ? *said.Bool() : said.Number() && *said.Number() != 0.0) { _impl->writingBack[node.id] = {}; seen.insert(node.id); continue; }
            }
            Watch(from);        // asked again each frame: what someone else stopped watching is watched again
            auto& state = _impl->writingBack[node.id];
            if (remaking) { state.bytes.clear(); continue; }
            const auto peek = Watched(from);
            if (!peek || peek->bytes.empty()) continue;
            const auto listed = _impl->built.find(*each->second.value.String());
            if (listed == _impl->built.end() || !listed->second.listed) continue;
            const std::vector<kecs::Entity>& entities = listed->second.listed->entities;
            const auto number = [&](const char* name) -> std::optional<std::size_t> {
                const auto it = node.inputs.find(name);
                const double* n = it == node.inputs.end() ? nullptr : it->second.value.Number();
                return n ? std::optional(static_cast<std::size_t>(*n)) : std::nullopt;
            };
            const std::size_t stride = number("stride").value_or(0);
            const auto position = number("position"), rotation = number("rotation"), follows = number("follows");
            const auto velocity = number("velocity"), spin = number("spin");
            // What is read back is of the steps before this frame's, which go on before it is drawn (and before what a script
            // moves this frame — a kinematic blade — is drawn where it was put): each element sent on by as long, as it goes.
            const float ahead = static_cast<float>(std::min(_impl->fixedPending, static_cast<std::uint32_t>(std::max(_impl->current->fixedStepsAtMost, 1)))
                                                   * _impl->fixedSeconds);
            if (stride == 0) continue;
            // An element that follows its entity: the entity says where it is, and is neither written nor taken as moved.
            const auto following = [&](const std::size_t i) {
                if (!follows || (i + 1) * stride > peek->bytes.size() || *follows + 4 > stride) return false;
                float flag;
                std::memcpy(&flag, peek->bytes.data() + i * stride + *follows, sizeof flag);
                return flag != 0.f;
            };
            const auto key = [](const kecs::Entity e) { return (std::uint64_t { e.index } << 32) | e.generation; };
            // One moved by something else since it was last written — the gizmo, a script, the inspector: setup is made
            // again, from the world as it is, and what is read back from before that is let pass.
            bool moved = false;
            for (std::size_t i = 0; i < entities.size(); ++i) {
                const kecs::Entity e = entities[i];
                if (following(i)) continue;
                const auto put = state.put.find(key(e));
                if (put == state.put.end() || !world.Alive(e)) continue;
                const kecs::Vec3 at = world.PositionOf(e);
                const kecs::Quat turn = world.OrientationOf(e);
                const auto far = [](const float a, const float b) { return std::abs(a - b) > 1e-4f; };
                if ((position && (far(at.x, put->second.first.x) || far(at.y, put->second.first.y) || far(at.z, put->second.first.z)))
                    || (rotation && (far(turn.x, put->second.second.x) || far(turn.y, put->second.second.y) || far(turn.z, put->second.second.z) || far(turn.w, put->second.second.w)))) {
                    moved = true;
                    break;
                }
            }
            if (moved) {
                RunSetupAgain();
                state.put.clear();
                state.skip = static_cast<int>(kor::Context::HasScheduler() ? kor::Context::Scheduler().ImageCount() : 1) + 2;
                continue;
            }
            if (state.skip > 0) { --state.skip; state.bytes.clear(); continue; }   // from before: not written, and not taken as written
            if (peek->bytes == state.bytes) continue;       // nothing new: nothing stepped — and nothing written over an edit
            state.bytes = peek->bytes;
            const auto floats = [&](const std::size_t at, float* into, const int n) { std::memcpy(into, peek->bytes.data() + at, sizeof(float) * n); };
            std::vector<std::tuple<kecs::ComponentId, const kecs::FieldDescription*, std::size_t>> fields;
            if (const auto given = node.inputs.find("fields"); given != node.inputs.end())
                if (const auto* members = given->second.value.Named())
                    for (const auto& [name, offset] : *members) {
                        const auto dot = name.find('.');
                        const kecs::ComponentId id = world.Find(name.substr(0, dot));
                        const kecs::FieldDescription* field = id == kecs::NoComponent ? nullptr : world.Describe(id).FindField(name.substr(dot + 1));
                        if (field && offset.Number()) fields.emplace_back(id, field, static_cast<std::size_t>(*offset.Number()));
                    }
            const std::size_t count = std::min(entities.size(), peek->bytes.size() / stride);
            for (std::size_t i = 0; i < count; ++i) {
                const kecs::Entity e = entities[i];
                if (!world.Alive(e) || following(i)) continue;
                const std::size_t base = i * stride;
                if (position) {
                    float p[3];
                    floats(base + *position, p, 3);
                    if (velocity && ahead > 0.f && base + *velocity + 12 <= peek->bytes.size()) {
                        float v[3];
                        floats(base + *velocity, v, 3);
                        for (int k = 0; k < 3; ++k) p[k] += v[k] * ahead;
                    }
                    world.SetPosition(e, { p[0], p[1], p[2] });
                }
                if (rotation) {
                    float q[4];
                    floats(base + *rotation, q, 4);
                    if (spin && ahead > 0.f && base + *spin + 12 <= peek->bytes.size()) {
                        // Turned on by the spin (about the world's axes) for as long: q + ½ (w, 0) q t, made length 1 again.
                        float w[3];
                        floats(base + *spin, w, 3);
                        const float h = 0.5f * ahead;
                        const float d[4] = { h * (w[0] * q[3] + w[1] * q[2] - w[2] * q[1]), h * (w[1] * q[3] + w[2] * q[0] - w[0] * q[2]),
                                             h * (w[2] * q[3] + w[0] * q[1] - w[1] * q[0]), h * (-w[0] * q[0] - w[1] * q[1] - w[2] * q[2]) };
                        float l = 0.f;
                        for (int k = 0; k < 4; ++k) { q[k] += d[k]; l += q[k] * q[k]; }
                        l = std::sqrt(l);
                        if (l > 1e-12f) for (float& c : q) c /= l;
                    }
                    world.SetOrientation(e, { q[0], q[1], q[2], q[3] });
                }
                for (const auto& [id, field, offset] : fields) {
                    if (!world.Has(e, id) || offset + field->size > stride) continue;
                    auto* bytes = static_cast<std::byte*>(const_cast<void*>(std::as_const(world).Get(e, id)));
                    std::memcpy(bytes + field->offset, peek->bytes.data() + base + offset, field->size);
                }
                state.put[key(e)] = { world.PositionOf(e), world.OrientationOf(e) };
            }
        }
        std::erase_if(_impl->writingBack, [&](const auto& entry) { return !seen.contains(entry.first); });
    }

    void Runtime::SetView(std::optional<View> view) { _impl->viewOverride = std::move(view); }
    void Runtime::SetHidden(std::set<kecs::Entity> hidden)
    {
        if (hidden == _impl->hidden) return;
        _impl->hidden = std::move(hidden);
        ++_impl->hiddenVersion;     // each list of what is drawn found again
    }
    void Runtime::Share(const std::set<std::string>& nodes)
    {
        if (!_impl->source || std::ranges::equal(nodes, _impl->shared)) return;
        _impl->shared = std::set<std::string, std::less<>>(nodes.begin(), nodes.end());
        _impl->stale = true;    // built again, next frame: those taken from the source, and the rest as their own
    }
    void Runtime::SetSelected(const kecs::Entity entity) { _impl->selectedId->store(entity.IsNull() ? 0 : entity.index + 1); }
    void Runtime::SetEditorView(const bool editor) { _impl->editorViewAsked = editor; }
    bool Runtime::EditorView() const { return _impl->editorViewAsked; }
    void Runtime::ShowDebugView(std::string id) { _impl->debugViewAsked = std::move(id); }
    const std::string& Runtime::ShownDebugView() const { return _impl->debugViewAsked; }
    void Runtime::SetShaderDebugView(const int channel) { _impl->shaderDebugAsked = channel; }
    int Runtime::ShaderDebugView() const { return _impl->shaderDebugAsked; }

    std::vector<Runtime::DebugViewOf> Runtime::DebugViews() const
    {
        std::vector<DebugViewOf> views;
        for (const auto& node : _running.nodes) {
            if (node.type != "DebugView") continue;
            const auto name = node.inputs.find("name");
            const std::string* named = name != node.inputs.end() ? name->second.value.String() : nullptr;
            views.push_back({ node.id, named && !named->empty() ? *named : node.id });
        }
        return views;
    }

    std::vector<std::string> Runtime::WithoutShaderDebugView() const
    {
        std::vector<std::string> without;
        const std::string constant = std::format("constant.{}", ShaderDebugConstant);
        for (const auto& [id, built] : _impl->built) {
            if (!built.pipeline || !built.status.ok || built.pipeline->numThreads[0] != 0) continue;     // a graphics pipeline, built
            // One that draws no colour — a depth prepass, a shadow — has nothing of a material to show.
            const Held& target = built.pipeline->framebuffer;
            if (!target || !target->handle || kor::capi::FramebufferOf(target->handle)->ColorAttachmentCount() == 0) continue;
            if (std::ranges::find(built.pipeline->ports, constant, &Port::name) == built.pipeline->ports.end()) without.push_back(id);
        }
        return without;
    }
    void Runtime::FixedStep(const float seconds)
    {
        ++_impl->fixedPending;
        if (seconds > 0.f) _impl->fixedSeconds = seconds;
    }
    std::uint32_t Runtime::FixedSteps() const { return _impl->fixedSteps; }
    void Runtime::SetFixedFraction(const float fraction) { _impl->fixedFraction = std::clamp(static_cast<double>(fraction), 0.0, 1.0); }
    std::map<std::string, Runtime::Timing, std::less<>> Runtime::Timings() const
    {
        std::map<std::string, Timing, std::less<>> timings;
        for (const auto& pass : _impl->frameGraph.PassTimings()) {
            if (!_impl->passes.contains(pass.name)) continue;      // another's: not one of this graph's passes
            Timing& of = timings[Impl::StepOf(pass.name).first];
            of.gpuMs += pass.gpuMs;
            of.recordMs += pass.recordMs;
            of.prepareMs += pass.prepareMs;
            of.gpuMeasured = of.gpuMeasured || pass.gpuMeasured;
            ++of.runs;
        }
        return timings;
    }
    Runtime::FrameReport Runtime::Frame() const
    {
        FrameReport report;
        const kor::FrameGraph& graph = _impl->frameGraph;
        // What each resource the passes use is, by the node that made it: the frame graph knows it by where it is.
        std::unordered_map<const void*, std::string> madeBy;
        for (const auto& [id, made] : _impl->built)
            for (const auto& output : made.outputs | std::views::values) {
                if (!output.resource || output.resource->node != id) continue;
                if (const auto image = ImageRefOf(output.resource.get()); image.Get()) madeBy.try_emplace(image.Get(), id);
                else if (const auto buffer = BufferRefOf(output.resource.get()); buffer.Get()) madeBy.try_emplace(buffer.Get(), id);
            }
        const auto named = [&](const std::string& name) -> std::string {
            if (name.starts_with("kgraph.set.")) return std::format("the entities' {}", name.substr(11));
            if (name.starts_with("kgraph.")) {
                const auto at = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(std::stoull(name.substr(7), nullptr, 16)));
                if (const auto found = madeBy.find(at); found != madeBy.end()) return found->second;
            }
            return name;
        };
        std::map<std::string, kor::FrameGraph::PassTiming, std::less<>> timings;
        for (auto& timing : graph.PassTimings()) timings.emplace(timing.name, timing);
        for (const auto& scheduled : graph.Schedule()) {
            const auto ours = _impl->passes.find(scheduled.name);
            if (ours == _impl->passes.end()) continue;     // another's: not one of this graph's passes
            FrameReport::Pass pass { scheduled.name, Impl::StepOf(scheduled.name).first, scheduled.level, scheduled.async };
            if (const auto timing = timings.find(scheduled.name); timing != timings.end()) {
                pass.gpuMs = timing->second.gpuMs;
                pass.recordMs = timing->second.recordMs;
                pass.gpuMeasured = timing->second.gpuMeasured;
                pass.cpuPrepareStartMs = timing->second.cpuPrepareStartMs;
                pass.cpuPrepareMs = timing->second.cpuPrepareLastMs;
                pass.cpuRecordStartMs = timing->second.cpuRecordStartMs;
                pass.cpuRecordMs = timing->second.cpuRecordLastMs;
                pass.recordThread = timing->second.recordThread;
                pass.gpuStartMs = timing->second.gpuStartMs;
                pass.gpuLastMs = timing->second.gpuLastMs;
                pass.gpuOnCpuClock = timing->second.gpuOnCpuClock;
                pass.work = timing->second.work;
            }
            for (const auto& use : ours->second->declared) pass.uses.emplace_back(named(use.name), use.read && use.write ? 'b' : use.write ? 'w' : 'r');
            if (ours->second->plan->readsScreen || ours->second->plan->writesScreen)
                pass.uses.emplace_back("the screen", ours->second->plan->readsScreen && ours->second->plan->writesScreen ? 'b' : ours->second->plan->writesScreen ? 'w' : 'r');
            report.passes.push_back(std::move(pass));
        }
        for (const auto& skipped : graph.SkippedPasses())
            if (_impl->passes.contains(skipped.name))
                report.leftOut.emplace_back(Impl::StepOf(skipped.name).first, std::format("skipped: what it needs, {}, is made by {}, which is left out", named(skipped.resource), skipped.source));
        for (const auto& culled : graph.CulledPasses())
            if (_impl->passes.contains(culled)) report.leftOut.emplace_back(Impl::StepOf(culled).first, "culled: nothing needs what it makes");
        for (const auto& id : _impl->restingUnread) report.leftOut.emplace_back(id, "resting: nothing that runs this frame reads what it makes");
        report.bytes = graph.Memory().bytes;
        report.unsharedBytes = graph.Memory().unsharedBytes;
        return report;
    }

    Runtime::FrameTimes Runtime::Times() const
    {
        auto times = _impl->frameGraph.Times();
        return { std::move(times.frameMs), std::move(times.gpuMs) };
    }

    void Runtime::RunSetupAgain() { _impl->setupAgain = true; }
    void Runtime::SetDebugDraw(kor::DebugDraw* debug) { _impl->debug = debug; }
    kor::Mat4 Runtime::ViewProjection() const { return _impl->viewProjection; }
    View Runtime::CurrentView() const { return _impl->currentView; }
    float Runtime::Aspect() const { return _impl->currentAspect; }

    void Runtime::SetScreenExtent(const kor::UVec2 extent)
    {
        if (extent == _impl->screenExtent || extent.x == 0 || extent.y == 0) return;
        _impl->screenExtent = extent;
        const kor::log::Quiet quiet;
        _impl->Evaluate(_running);    // only a Screen node, and what is sized by one, is built again
    }

    void Runtime::SetScreenLook(std::string format, const float scale)
    {
        if (format == _impl->screenFormat && scale == _impl->screenScale) return;
        _impl->screenFormat = std::move(format);
        _impl->screenScale = scale;
        const kor::log::Quiet quiet;
        _impl->Evaluate(_running);
    }

    bool Runtime::Pick(const float u, const float v, std::function<void(kecs::Entity)> then)
    {
        const bool there = std::ranges::any_of(_graph.nodes, [&](const Node& node) { return node.type == "Pick" && Status(node.id) && Status(node.id)->ok; });
        if (!there) return false;
        _impl->pick->asked = std::pair { std::clamp(u, 0.f, 1.f), std::clamp(v, 0.f, 1.f) };
        _impl->pick->then = [world = _impl->world, then = std::move(then)](const kor::u32 id, const PickHit&) {
            then(world && id != 0 ? world->EntityAt(id - 1) : kecs::NullEntity);
        };
        return true;
    }

    bool Runtime::PickAt(const float u, const float v, std::function<void(const PickHit&)> then)
    {
        const bool there = std::ranges::any_of(_graph.nodes, [&](const Node& node) { return node.type == "Pick" && Status(node.id) && Status(node.id)->ok; });
        if (!there) return false;
        _impl->pick->asked = std::pair { std::clamp(u, 0.f, 1.f), std::clamp(v, 0.f, 1.f) };
        _impl->pick->then = [world = _impl->world, then = std::move(then)](const kor::u32 id, const PickHit& hit) {
            PickHit found = hit;
            found.entity = world && id != 0 ? world->EntityAt(id - 1) : kecs::NullEntity;
            then(found);
        };
        return true;
    }

    const NodeStatus* Runtime::Status(const std::string_view id) const
    {
        if (const auto it = _impl->built.find(id); it != _impl->built.end()) return &it->second.status;
        if (const auto it = _impl->unwritten.find(id); it != _impl->unwritten.end()) return &it->second;
        // A node of a loop fared as its turns did: as the first of them that failed, or as its first.
        if (const auto of = _impl->turns.find(id); of != _impl->turns.end()) {
            const NodeStatus* first = nullptr;
            for (const auto& turn : of->second) {
                const auto it = _impl->built.find(turn);
                if (it == _impl->built.end()) continue;
                if (!it->second.status.ok) return &it->second.status;
                if (!first) first = &it->second.status;
            }
            return first;
        }
        // A call fared as its nodes did: as the first of them that failed, said with which.
        if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) {
            NodeStatus fared { true, {}, 1 };
            for (const auto& inner : call->second.nodes)
                if (const NodeStatus* status = Status(inner); status && !status->ok) {
                    fared = NodeStatus { false, std::format("{}: {}", inner.substr(call->second.called.size() + 1), status->error), status->builds, {} };
                    break;
                }
            return &(_impl->callStatus[std::string(id)] = std::move(fared));
        }
        // An edge of a function opened in place stands for what it is given: there is nothing of it to fail.
        static const NodeStatus edge { true, {}, 1 };
        if (_impl->aliases.contains(id)) return &edge;
        // A Repeat is a count and nothing more: there is nothing of it to fail once it is one.
        static const NodeStatus counted { true, {}, 1 };
        const Node* node = _graph.Find(id);
        return node && node->type == "Repeat" ? &counted : nullptr;
    }

    bool Runtime::Ok() const
    {
        // (What copies a watched node out is no part of the graph: that it cannot is WatchProblem's to say.)
        return _impl->unwritten.empty() && std::ranges::all_of(_impl->built, [](const auto& node) { return node.second.status.ok || node.first.starts_with("$peek"); });
    }

    std::vector<std::string> Runtime::Problems() const
    {
        std::vector<std::string> problems;
        for (const auto& node : _graph.nodes)
            if (const auto* status = Status(node.id); status && !status->ok) problems.push_back(std::format("{}: {}", node.id, status->error));
        return problems;
    }

    std::vector<std::string> Runtime::TurnsOf(const std::string_view id) const
    {
        const auto of = _impl->turns.find(id);
        return of == _impl->turns.end() ? std::vector<std::string> {} : of->second;
    }

    std::vector<Preset> Runtime::Presets() const
    {
        std::vector<Preset> presets;
        // A Script node of each of the project's ResourceScripts: what it makes, at once.
        for (const auto& script : ResourceScripts()) {
            std::string makes;
            for (const auto& resource : script.resources) makes += std::format("{}{} ({})", makes.empty() ? "" : ", ", resource.name, resource.kind);
            presets.push_back({ script.name, "Script", {}, makes.empty() ? std::format("{}: a ResourceScript that makes nothing yet.", script.name)
                                                                          : std::format("What {} makes, filled by it: {}.", script.name, makes),
                                { { "script", Value(script.name) } } });
        }
        const kecs::World* world = _impl->world;
        if (!world) { std::ranges::sort(presets, {}, &Preset::name); return presets; }
        // Every component of the world, as a ForEach of it: its buffer is one ComponentBuffer node, told which.
        for (kecs::ComponentId id = 0; id < world->ComponentCount(); ++id) {
            const kecs::ComponentDescription& type = world->Describe(id);
            if (type.IsTag() || type.name == "Name" || type.name.empty()) continue;
            std::string fields;
            for (const auto& field : type.fields)
                if (!LayoutTypeOf(field.type).empty()) fields += std::format("{}{}", fields.empty() ? "" : ", ", field.name);
            if (fields.empty()) continue;
            presets.push_back({ type.name, "ForEach", type.name,
                                std::format("Every entity with a {}, an element at a time: {}, and where it is.", type.name, fields),
                                { { "component", Value(type.name) } } });
        }
        std::ranges::sort(presets, {}, &Preset::name);
        return presets;
    }

    std::string Runtime::EachOf(const std::string_view id) const
    {
        std::string source;
        const auto found = _impl->built.find(std::string(id));
        if (const auto* made = _impl->Find(id); made && made->pass && made->pass->entities) {
            source = made->pass->entities->each;     // a Draw's or a Dispatch's — of a loop, its first turn's: what it draws an element of
        } else if (found != _impl->built.end() && found->second.mapped) {
            source = found->second.mapped->source;
        } else {
            std::set<std::string> sources;
            std::vector<std::string> chain;
            std::map<std::string, bool> seen;
            if (!_impl->Follows(std::string(id), sources, chain, seen) || sources.size() != 1) return {};
            source = *sources.begin();
        }
        // One that chooses one of them is no loop.
        if (const Node* each = _impl->current && !source.empty() ? _impl->current->Find(source) : nullptr)
            if (each->type == "Entity") return {};
        return source;
    }

    std::shared_ptr<Resource> Runtime::ResourceOf(const std::string_view id, const std::string_view given) const
    {
        const std::string shown = given.empty() ? ShownOutput(id) : std::string();
        const std::string_view output = given.empty() ? std::string_view(shown) : given;
        if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) {
            const auto gives = call->second.gives.find(output);
            return gives == call->second.gives.end() || gives->second.node.empty() ? nullptr : ResourceOf(gives->second.node, gives->second.output);
        }
        if (const auto edge = _impl->aliases.find(id); edge != _impl->aliases.end())
            return edge->second.link ? ResourceOf(edge->second.link->node, edge->second.link->output) : nullptr;
        const auto* node = _impl->Find(id);
        // A default mesh is there without a node of its own: `$cube`.
        if (!node) {
            // A file's, and a generator's: made this run, or kept from one before.
            if (id.starts_with('@') || Impl::Generated(id)) return _impl->Asset(id);
            if (!id.starts_with('$')) return nullptr;
            if (auto mesh = _impl->DefaultMesh(id.substr(1))) return mesh;
            return _impl->DefaultImage(id.substr(1));
        }
        const auto out = node->outputs.find(output);
        return out == node->outputs.end() ? nullptr : out->second.resource;
    }

    kor::ResourceRef<const kor::Image> Runtime::ImageOf(const std::string_view id, const std::string_view output) const
    {
        return ImageRefOf(ResourceOf(id, output).get());
    }

    Value Runtime::ValueOf(const std::string_view id, const std::string_view output) const
    {
        // A call's output is what it is written out to; an edge of a function opened in place, what it stands for.
        if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) {
            const auto gives = call->second.gives.find(output);
            return gives == call->second.gives.end() || gives->second.node.empty() ? Value {} : ValueOf(gives->second.node, gives->second.output);
        }
        if (const auto edge = _impl->aliases.find(id); edge != _impl->aliases.end())
            return edge->second.link ? ValueOf(edge->second.link->node, edge->second.link->output) : edge->second.value;
        const auto* node = _impl->Find(id);
        if (!node) return {};
        const auto out = node->outputs.find(output);
        return out == node->outputs.end() ? Value {} : out->second.value;
    }

    namespace {
        std::string KindOf(const Node& node)
        {
            const auto kind = node.inputs.find("kind");
            return kind != node.inputs.end() && kind->second.value.String() ? *kind->second.value.String() : std::string("eValue");
        }
    }

    std::vector<Port> Runtime::PortsOf(const std::string_view id) const
    {
        const Node* node = _graph.Find(id);
        if (!node) return {};
        std::vector<Port> ports;
        if (const NodeType* type = FindType(node->type)) ports = PortsFor(*type, *node);
        // A call takes what its function's inputs are; an edge, a `value` of its kind.
        if (node->type == "Function") {
            if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) ports.insert(ports.end(), call->second.inputs.begin(), call->second.inputs.end());
            return ports;
        }
        if (node->type == "FunctionInput" || node->type == "FunctionOutput") {
            for (auto& port : ports)
                if (port.name == "value") {
                    Port typed = FunctionPort("value", KindOf(*node), port.doc);
                    typed.optional = port.optional;
                    port = std::move(typed);
                }
            return ports;
        }
        const auto* made = _impl->Find(id);
        if (made && made->pipeline) ports.insert(ports.end(), made->pipeline->ports.begin(), made->pipeline->ports.end());
        if (made) ports.insert(ports.end(), made->ports.begin(), made->ports.end());
        // A ComputePass has its pipeline's besides: the parameters and constants it gives the pipeline written out of it.
        if (node->type == "ComputePass")
            if (const auto* pipeline = _impl->Find(std::string(id) + "/pipeline")) {
                std::vector<Port> its = pipeline->ports;
                if (pipeline->pipeline) its.insert(its.end(), pipeline->pipeline->ports.begin(), pipeline->pipeline->ports.end());
                for (const Port& port : its)
                    if ((port.name.starts_with("param.") || port.name.starts_with("constant.")) && !std::ranges::contains(ports, port.name, &Port::name)) ports.push_back(port);
            }
        return ports;
    }

    std::vector<Port> Runtime::OutputsOf(const std::string_view id) const
    {
        const Node* node = _graph.Find(id);
        if (!node) node = _running.Find(id);     // a node a call was written out into: `<call>/<node>`
        if (!node) return {};
        std::vector<Port> ports;
        // A call gives what its function's outputs are, each as the node inside gives it — and a Reference to a call, the
        // same; an edge, an `out` of its kind.
        if (node->type == "Function" || _impl->calls.contains(id)) {
            const auto call = _impl->calls.find(id);
            if (call == _impl->calls.end()) return ports;
            for (const auto& name : call->second.outputs) {
                Port port { name, PortKind::eValue, "value", false, {}, nullptr };
                if (const auto gives = call->second.gives.find(name); gives != call->second.gives.end() && !gives->second.node.empty())
                    for (const auto& inner : OutputsOf(gives->second.node))
                        if (inner.name == gives->second.output) { port = inner; port.name = name; break; }
                ports.push_back(std::move(port));
            }
            return ports;
        }
        if (node->type == "FunctionInput" || node->type == "FunctionOutput") return { FunctionPort("out", KindOf(*node), "What it stands for: what it is given.") };
        // An Unbundle gives each member of what it takes apart.
        if (node->type == "Unbundle") {
            for (const auto& name : BundleOf(id, "of")) ports.push_back({ name, PortKind::eValue, "value", false, std::format("The member '{}'.", name), nullptr });
            return ports;
        }
        // What a node says of itself comes first: an Entities node is its component's fields before anything else.
        if (const auto* made = _impl->Find(id)) ports = made->gives;
        if (const NodeType* type = FindType(node->type)) ports.insert(ports.end(), type->outputs.begin(), type->outputs.end());
        return ports;
    }

    std::vector<std::string> Runtime::BundleOf(const std::string_view id, const std::string_view output) const
    {
        std::vector<std::string> names;
        const auto found = _impl->bundles.find(std::format("{}.{}", id, output));
        if (found != _impl->bundles.end())
            for (const auto& name : found->second | std::views::keys) names.push_back(name);
        return names;
    }

    std::vector<std::string> Runtime::PerEntityParameters(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        return node && node->pipeline ? node->pipeline->perEntity : std::vector<std::string> {};
    }

    kecs::ComponentId Runtime::ComponentOf(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        return node && node->status.ok && node->pipeline ? node->pipeline->component : kecs::NoComponent;
    }

    std::vector<std::string> Runtime::MaterialsOf(const std::string_view id) const
    {
        std::vector<std::string> kinds;
        const auto* node = _impl->Find(id);
        if (!node || !node->status.ok || !node->pipeline) return kinds;
        for (const auto& slot : node->pipeline->slots)
            if (slot.instancing)
                for (const auto& field : slot.instancing->fields)
                    if (field.Material() && !std::ranges::contains(kinds, field.kind)) kinds.push_back(field.kind);
        return kinds;
    }

    const kecs::Migration* Runtime::MigrationOf(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        return node && node->migration ? &*node->migration : nullptr;
    }

    std::vector<std::pair<kecs::Entity, std::string>> Runtime::PassedBy() const
    {
        std::vector<std::pair<kecs::Entity, std::string>> all;
        for (const auto& id : _impl->wanted) {
            const auto it = _impl->built.find(id);
            if (it == _impl->built.end() || !it->second.pass || !it->second.pass->entities) continue;
            for (const auto& [entity, why] : it->second.pass->entities->report.passedBy) all.emplace_back(entity, std::format("{}: {}", id, why));
        }
        return all;
    }

    void Runtime::SetResourceScript(ResourceScript script)
    {
        Impl::Scripted& at = _impl->scripts[script.name];
        if (script.size == 0)
            for (const auto& field : script.fields) script.size = std::max(script.size, field.offset + field.size);
        at.script = std::move(script);
        at.generation = ++_impl->scriptsVersion;
    }

    void Runtime::RemoveResourceScript(const std::string_view name)
    {
        if (const auto it = _impl->scripts.find(name); it != _impl->scripts.end()) {
            _impl->scripts.erase(it);
            ++_impl->scriptsVersion;
        }
    }

    std::vector<ResourceScript> Runtime::ResourceScripts() const
    {
        std::vector<ResourceScript> out;
        const Impl* from = _impl.get();
        while (from->source) from = from->source;
        for (const auto& each : from->scripts | std::views::values) out.push_back(each.script);
        return out;
    }

    kecs::ComponentId Runtime::DeclareMaterialKind(MaterialKind kind, std::string* error)
    {
        kecs::World* world = _impl->world;
        if (!world) { if (error) *error = "there is no world to make the kind a component of"; return kecs::NoComponent; }
        const kecs::ComponentId existing = world->Find(kind.name);
        const auto known = std::ranges::find(_impl->materialKinds, kind.name, &MaterialKind::name);
        if (existing != kecs::NoComponent && known == _impl->materialKinds.end()) {
            if (error) *error = std::format("there is a component named '{}' already, which is no material kind: the kind needs another name", kind.name);
            return kecs::NoComponent;
        }
        // What a new one's pictures start as: found now, as a stand-in until a file is read.
        std::vector<Held> starting;
        for (const auto& [field, picture] : kind.textures) starting.push_back(ResourceOf(picture));
        kecs::ComponentDescription description = MaterialComponentOf(kind, std::move(starting));
        kecs::ComponentId id = existing;
        if (existing == kecs::NoComponent) id = world->Register(std::move(description));
        else {
            try { (void)world->Reshape(existing, std::move(description)); }
            catch (const std::exception& e) { if (error) *error = e.what(); return kecs::NoComponent; }
        }
        if (known != _impl->materialKinds.end()) *known = std::move(kind);
        else {
            _impl->materialKinds.push_back(std::move(kind));
            std::ranges::sort(_impl->materialKinds, {}, &MaterialKind::name);
        }
        _impl->WriteMaterials();
        // What a shader takes of a kind is worked out as its pipeline is built — a pipeline node's, or a pass's own: the
        // graph is built again, the next frame. A kind is declared seldom: when a project's scripts are read.
        for (auto& built : _impl->built | std::views::values) built.signature.clear();
        _impl->stale = true;
        return id;
    }

    const std::vector<MaterialKind>& Runtime::MaterialKinds() const { return _impl->materialKinds; }

    const MaterialKind* Runtime::MaterialKindOf(const std::string_view name) const
    {
        const auto it = std::ranges::find(_impl->materialKinds, name, &MaterialKind::name);
        return it == _impl->materialKinds.end() ? nullptr : &*it;
    }

    void Runtime::SetGeneratedShaderDirectory(std::filesystem::path directory)
    {
        if (directory == _impl->generatedShaders) return;
        kor::Shader::AddSearchPath(directory);
        _impl->generatedShaders = std::move(directory);
        _impl->WriteMaterials();
    }

    const std::filesystem::path& Runtime::GeneratedShaderDirectory() const { return _impl->generatedShaders; }

    const std::vector<std::string>& Runtime::MaterialNotes() const { return _impl->materialNotes; }

    std::vector<PassUse> Runtime::UsesOf(const std::string_view id, const std::string_view output) const
    {
        std::vector<PassUse> uses;
        const auto resource = ResourceOf(id, output);
        const void* of = resource ? MemberOf(resource.get()).id : nullptr;
        if (!of) return uses;
        for (const auto& pass : _impl->wanted) {
            if (pass.starts_with('$')) continue;    // a watch's copy, which is no node of the graph's
            const auto& plan = _impl->built.at(pass).pass;
            PassUse use { pass, {}, false, false };
            for (const auto& each : plan->uses)
                if (each.what.id == of) { use.read = use.read || each.read; use.write = use.write || each.write; }
            if (!use.read && !use.write) continue;
            if (const Node* node = _impl->current ? _impl->current->Find(pass) : nullptr) use.pass = node->pass;
            uses.push_back(std::move(use));
        }
        return uses;
    }

    const PassReport* Runtime::ReportOf(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        if (!node || !node->status.ok || !node->pass || !node->pass->entities) return nullptr;
        // One that draws once is no report of entities — unless it could not draw, and says why.
        const auto& run = *node->pass->entities;
        return run.each.empty() && run.report.skipped == 0 ? nullptr : &run.report;
    }

    void Runtime::ReachedFromTheCpu(const std::string_view id)
    {
        if (!_impl->reachedFromTheCpu.emplace(id).second) return;
        // Made where only passes reach it: made again where the CPU does.
        if (const auto it = _impl->built.find(std::string(id)); it != _impl->built.end() && it->second.automaticType == "eDeviceLocal") {
            it->second.signature.clear();
            _impl->stale = true;
        }
    }

    std::vector<std::string> Runtime::UsageOf(const std::string_view id) const
    {
        const Node* node = _running.Find(id);
        const auto it = _impl->built.find(std::string(id));
        const kb::Enumeration* usages = node && Impl::MadeForItsUse(node->type) ? Impl::UsagesOf(node->type) : nullptr;
        if (!usages || it == _impl->built.end() || !it->second.status.ok) return {};
        std::vector<std::string> names;
        for (const auto& entry : usages->values)
            if (entry.value != 0 && (it->second.madeFor & entry.value) == entry.value) names.emplace_back(entry.name);
        return names;
    }

    std::optional<Runtime::Batching> Runtime::BatchingOf(const std::string_view id) const
    {
        const Node* node = _running.Find(id);
        if (!node || (node->type != "Draw" && node->type != "Dispatch")) return std::nullopt;
        const auto rowsOf = [&](const std::string& filter) -> std::size_t {
            const auto* built = _impl->Find(filter);
            return built && built->listed ? built->listed->Rows() : 0;
        };
        const auto* built = _impl->Find(id);
        const EntityRun* run = built && built->status.ok && built->pass ? built->pass->entities.get() : nullptr;
        Batching batching;
        if (node->type == "Dispatch") {
            if (run && !run->batchOf.empty()) {
                batching.batched = true;
                batching.elements = rowsOf(run->batchOf);
                return batching;
            }
            if (!run || run->each.empty()) return std::nullopt;
            batching.elements = run->report.ran + run->report.skipped;
            if (batching.elements < 2) return std::nullopt;
            batching.why = std::format("wired to a Filter's entities ('entities'), it would run once for all of them, one after another along z, each "
                                       "element SV_GroupID.z: what it is wired of each of '{}' its shader would read from its [entity(\"PER_INSTANCE\")] buffer "
                                       "instead, and the images each holds by their bindless indices (TEXTURE, STORAGE)", run->each);
            return batching;
        }
        if (_impl->batched.contains(id)) {
            batching.batched = true;
            batching.elements = rowsOf(std::string(id) + "/batch");
            return batching;
        }
        // Of how many: what it drew one at a time; or, one that could not draw them so, the Filter or ForEach its mesh is of.
        const auto mesh = node->inputs.find("mesh");
        const Node* source = mesh != node->inputs.end() && mesh->second.link ? _impl->Origin(mesh->second.link->node) : nullptr;
        batching.elements = run && !run->each.empty() ? run->report.ran + run->report.culled + run->report.skipped : source ? rowsOf(source->id) : 0;
        if (batching.elements < 2 || !source || (source->type != "Filter" && source->type != "ForEach")) return std::nullopt;
        // Whether its shader reads each instance's from a buffer of them: then `batched` is all it takes.
        bool perInstance = false;
        if (const auto pipeline = node->inputs.find("pipeline"); pipeline != node->inputs.end() && pipeline->second.link)
            if (const Node* origin = _impl->Origin(pipeline->second.link->node))
                if (const auto* made = _impl->Find(origin->id); made && made->pipeline)
                    perInstance = std::ranges::any_of(made->pipeline->slots, [](const Slot& slot) { return slot.instancing != nullptr; });
        std::vector<std::string> each;
        const std::string why = WhyNotBatched(_running, *node, &each);
        if (const auto asked = _impl->unbatched.find(id); asked != _impl->unbatched.end()) {
            batching.why = asked->second;
        } else if (why.empty() && perInstance) {
            batching.offered = true;
        } else if (!each.empty() || why.empty()) {
            std::string list;
            for (const auto& name : each) list += std::format("{}'{}'", list.empty() ? "" : ", ", name);
            batching.why = std::format("batched, it would be one draw: {}its shader would read what each element is from a [entity(\"PER_INSTANCE\")] buffer — "
                                       "where it is as [entity(\"MATRIX\")], its own fields as they are", list.empty() ? std::string() : list + (each.size() == 1 ? ", which is each element's now — " : ", which are each element's now — "));
        } else {
            batching.why = why;
        }
        return batching;
    }

    std::vector<std::string> Runtime::TakeReset() { return std::exchange(_impl->reset, {}); }
}
