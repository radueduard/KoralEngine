// kgraph: the runtime's own — what its parts share. Private to kgraph's runtime*.cpp.

#pragma once

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
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
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

    // What the runtime's parts share: each inline, one of each in the program.
    namespace runtime_detail {
        namespace kb = kor::builders;

        /** A format's name, as a graph has it: `eRGBA8_UNORM`. */
        inline std::string FormatName(const kor::Image::Format format)
        {
            const kb::Description* image = kb::Find("Image");
            const kb::Setting* setting = image ? image->FindSetting("format") : nullptr;
            if (!setting || setting->arguments.empty() || !setting->arguments.front().type->enumeration) return {};
            for (const auto& entry : setting->arguments.front().type->enumeration->values)
                if (entry.value == static_cast<std::uint64_t>(format)) return std::string(entry.name);
            return {};
        }

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
        inline std::unexpected<std::string> Fail(std::string message) { return std::unexpected(std::move(message)); }

        using Held = std::shared_ptr<Resource>;

        /** An image or a buffer a pass uses: what the frame graph orders passes by. Told apart by which object it is. */
        struct Member {
            const void* id = nullptr;
            kor::ResourceRef<const kor::Image> image;
            kor::ResourceRef<const kor::Buffer> buffer;
            std::string transient;      // a transient's: the node it is of, which the frame graph knows it by
        };
        struct Use {
            Member what;
            bool read = true, write = false;
        };

        /** The image a resource is, or is a view of; empty for anything else. */
        inline kor::ResourceRef<const kor::Image> ImageRefOf(const Resource* resource)
        {
            if (!resource || !resource->handle) return {};
            if (resource->kind == "Image") return kor::capi::ImageOf(resource->handle);
            if (resource->kind == "ImageView") return kor::capi::ImageViewOf(resource->handle)->SourceImage();
            return {};
        }
        inline kor::ResourceRef<const kor::Buffer> BufferRefOf(const Resource* resource)
        {
            if (!resource || !resource->handle) return {};
            if (resource->kind == "Buffer") return kor::capi::BufferOf(resource->handle);
            if (resource->kind == "BufferView") return kor::capi::BufferViewOf(resource->handle)->SourceBuffer();
            return {};
        }
        inline Member MemberOf(const Resource* resource)
        {
            Member member;
            if ((member.image = ImageRefOf(resource)).Get()) member.id = member.image.Get();
            else if ((member.buffer = BufferRefOf(resource)).Get()) member.id = member.buffer.Get();
            // A transient's, or a view of one.
            if (resource && resource->transient) member.transient = resource->node;
            else if (resource && resource->source && resource->source->transient) member.transient = resource->source->node;
            return member;
        }
        inline Member MemberOf(kor::ResourceRef<const kor::Image> image)
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
        inline thread_local std::function<void(const Resource&, std::uint32_t)>* usedAs = nullptr;
        /** Whether an image (or a view of one) was made without what it is used as here. Said either way: it is made so next time. */
        inline bool MadeWithout(const Resource& resource, const kor::Image::Usage usage)
        {
            if (usedAs) (*usedAs)(resource, static_cast<std::uint32_t>(usage));
            const auto image = ImageRefOf(&resource);
            return !image.Get() || !(image->UsageFlags() & usage);
        }
        inline bool MadeWithout(const Resource& resource, const kor::Buffer::Usage usage)
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

        inline std::size_t SlotCount(const kecs::ComponentDescription& type) { return static_cast<const EntityLayout*>(type.context.get())->slots.size(); }

        inline void ConstructEntity(void* at, const kecs::ComponentDescription& type)
        {
            std::memset(at, 0, type.size);
            const auto& initial = static_cast<const EntityLayout*>(type.context.get())->initial;
            if (!initial.empty()) std::memcpy(at, initial.data(), std::min(initial.size(), type.size));     // no Held has been made in it yet
            for (std::size_t i = 0; i < SlotCount(type); ++i) ::new (static_cast<Held*>(at) + i) Held();
        }
        inline void CopyEntity(void* to, const void* from, const kecs::ComponentDescription& type)
        {
            std::memcpy(to, from, type.size);
            for (std::size_t i = 0; i < SlotCount(type); ++i) ::new (static_cast<Held*>(to) + i) Held(static_cast<const Held*>(from)[i]);
        }
        inline void RelocateEntity(void* to, void* from, const kecs::ComponentDescription& type)
        {
            std::memcpy(to, from, type.size);
            for (std::size_t i = 0; i < SlotCount(type); ++i) {
                Held& source = static_cast<Held*>(from)[i];
                ::new (static_cast<Held*>(to) + i) Held(std::move(source));
                source.~Held();
            }
        }
        inline void DestroyEntity(void* at, const kecs::ComponentDescription& type)
        {
            for (std::size_t i = 0; i < SlotCount(type); ++i) static_cast<Held*>(at)[i].~Held();
        }
        inline void AssignHeld(void* to, const void* from, const kecs::FieldDescription&) { *static_cast<Held*>(to) = *static_cast<const Held*>(from); }

        inline bool IsImage(const kor::DescriptorType type)
        {
            return type == kor::DescriptorType::eSampledImage || type == kor::DescriptorType::eStorageImage || type == kor::DescriptorType::eCombinedImageSampler;
        }
        /** The kind of resource a parameter takes, as a node's output and a component's field are typed. */
        inline const char* KindOf(const kor::DescriptorType type)
        {
            return IsImage(type) ? "Image"
                : type == kor::DescriptorType::eSampler ? "Sampler"
                : type == kor::DescriptorType::eAccelerationStructure ? "AccelerationStructure"
                : type == kor::DescriptorType::eUniformTexelBuffer || type == kor::DescriptorType::eStorageTexelBuffer ? "BufferView" : "Buffer";
        }
        inline bool Takes(const kor::DescriptorType type, const std::string& kind)
        {
            return IsImage(type) ? kind == "Image" || kind == "ImageView" : kind == KindOf(type);
        }

        /** What a push constant is called as a component's field, and how many bytes of it are the value. */
        inline std::pair<std::string, std::size_t> ConstantType(const kor::Pipeline::PushConstantMember& member)
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
            // What each row of `bytes` was worked out of: its entity and what of it was read, a key a row. A row whose key
            // is as it was — and what the chain reads besides (`context`) is too — is the row it was, not worked out again.
            std::vector<std::byte> rowKeys;
            std::size_t keySize = 0, keyedRows = 0, keyedBefore = 0;
            std::vector<Value> context;
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
        inline void WriteStaged(Instances& instances)
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
        inline void Mix(std::uint64_t& hash, const void* bytes, const std::size_t size)
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
        inline std::string InstancesKey(const std::string& source, const Instancing& instancing, const kecs::ComponentId component)
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

        /**
         * What the frame graph made of each transient, by the node it is of: as the pass that creates it found it, in its
         * Initialize. What a node made in its place stands in until what is wired from it is built again with this.
         */
        struct TransientBoard {
            std::mutex mutex;
            std::map<std::string, Member, std::less<>> made;
        };

        /** A transient: how the frame graph is to make it, and which object of the frame graph's what is wired from it was last built with (null: what stands in). */
        struct Transient {
            std::optional<kor::ImageDesc> image;
            std::optional<kor::BufferDesc> buffer;
            const void* made = nullptr;
            const void* standIn = nullptr;      // what its node made in its place
        };

        inline bool SameDesc(const std::optional<kor::ImageDesc>& a, const std::optional<kor::ImageDesc>& b)
        {
            if (a.has_value() != b.has_value()) return false;
            return !a || (a->format == b->format && a->usage == b->usage && a->scale == b->scale && a->sizeOf == b->sizeOf && a->extent == b->extent && a->mipLevels == b->mipLevels);
        }
        inline bool SameDesc(const std::optional<kor::BufferDesc>& a, const std::optional<kor::BufferDesc>& b)
        {
            if (a.has_value() != b.has_value()) return false;
            return !a || (a->size == b->size && a->usage == b->usage && a->type == b->type);
        }

        /** What a pass declares to the frame graph, under the name the resource — or the set it is in — is imported by. */
        struct Declared {
            std::string name;
            bool read = false, write = false;
            /// A transient's, by its node: the frame graph makes it, in the one pass of them that is given how (image, buffer).
            std::string transient;
            std::optional<kor::ImageDesc> image;
            std::optional<kor::BufferDesc> buffer;
            bool operator==(const Declared& other) const
            {
                return name == other.name && read == other.read && write == other.write && transient == other.transient
                    && SameDesc(image, other.image) && SameDesc(buffer, other.buffer);
            }
        };

        class GraphPass final : public kor::RenderPass {
        public:
            GraphPass(std::string name, std::shared_ptr<const PassPlan> plan, std::vector<Declared> declared)
                : RenderPass(std::move(name)), plan(std::move(plan)), declared(std::move(declared)) {}

            void Setup(kor::PassBuilder& builder) override
            {
                bool writes = plan->writesScreen;
                for (const auto& use : declared) {
                    // A transient, made here: by the frame graph, for the frame.
                    if (use.image) { builder.Create(use.name, *use.image); writes = true; continue; }
                    if (use.buffer) { builder.Create(use.name, *use.buffer); writes = true; continue; }
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
                // What the frame graph made of the transients made here, for what is wired from them to be built with.
                if (!board) return;
                for (const auto& use : declared) {
                    if (!use.image && !use.buffer) continue;
                    Member made;
                    made.transient = use.transient;
                    if (use.image) made.id = (made.image = resources.ImageNamed(use.name)).Get();
                    else made.id = (made.buffer = resources.BufferNamed(use.name)).Get();
                    const std::scoped_lock lock(board->mutex);
                    board->made[use.transient] = std::move(made);
                }
            }
            /** Whether a transient this pass was built with is not what the frame graph has made of it now: it records nothing then. */
            [[nodiscard]] bool HoldsAStandIn() const
            {
                if (!board || holds.empty()) return false;
                const std::scoped_lock lock(board->mutex);
                return std::ranges::any_of(holds, [&](const auto& held) {
                    const auto made = board->made.find(held.first);
                    return made == board->made.end() || made->second.id != held.second;
                });
            }
            void Prepare() override
            {
                if (plan->entities)
                    for (const auto& instances : plan->entities->instances) WriteStaged(*instances);
            }
            void Record(kor::CommandBuffer& commands) const override
            {
                if (steps && step >= steps->load()) return;     // a further fixed step this frame does not have
                // Built with what stood in for a transient: the frame it is built again with the frame graph's, it records.
                if (HoldsAStandIn()) return;
                if (plan->record && !plan->resting->load()) plan->record(commands, _context);
            }

            std::shared_ptr<const PassPlan> plan;
            std::vector<Declared> declared;
            std::shared_ptr<TransientBoard> board;      // the runtime's: what the frame graph made of each transient
            std::vector<std::pair<std::string, const void*>> holds;    // the transients it was built with: by node, which object
            /// A pass of the fixed step again, for the frame's step after the first: which (1 the second), and how many
            /// the frame being made has. Null for any other.
            std::uint32_t step = 0;
            std::shared_ptr<std::atomic<std::uint32_t>> steps;

        private:
            PassContext _context;
        };

        /** Puts what a parameter is given in a descriptor set being built. */
        inline Expected<void> WriteParameter(kor::DescriptorSet::Builder& set, const std::string& parameter, const kor::DescriptorType type,
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
        inline Shape ShapeOfScalar(const unsigned scalar, const unsigned components)
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
        inline void Annotate(Port& port, const kor::Shader::Annotation& annotation)
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

        inline bool NamesColor(std::string name)
        {
            std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name.find("color") != std::string::npos || name.find("colour") != std::string::npos || name.find("tint") != std::string::npos;
        }

        /** A parameter its shader marks for the engine: a buffer of each instance's, as its element's fields are marked. */
        inline void Instanced(const kor::Shader::Descriptor& descriptor, Parameter& parameter, const std::vector<MaterialKind>& kinds, const kecs::World* world)
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

        inline Interface Reflect(const std::vector<kor::ResourceRef<const kor::Shader>>& shaders, const std::vector<MaterialKind>& kinds = {}, const kecs::World* world = nullptr)
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

        inline std::uint64_t KeyOf(const kecs::Entity entity) { return (std::uint64_t { entity.index } << 32) | entity.generation; }

        // ---- values -------------------------------------------------------------------------------------------

        /** Numbers, or vectors a component at a time: a number goes with every component of a vector. */
        inline Expected<Value> Each(const std::vector<const Value*>& values, const std::function<double(const double*)>& apply)
        {
            // Of structs, member by member — in the first one's order — and a number or a vector with every member.
            if (const auto shape = std::ranges::find_if(values, [](const Value* value) { return value->Named() != nullptr; }); shape != values.end()) {
                Value::Members out;
                for (const auto& name : *(*shape)->Named() | std::views::keys) {
                    std::vector<const Value*> parts;
                    for (const Value* value : values) {
                        if (!value->Named()) { parts.push_back(value); continue; }
                        const Value* member = value->Find(name);
                        if (!member) return Fail(std::format("one struct has '{}' and another has not: of two, each has what the other has", name));
                        parts.push_back(member);
                    }
                    auto each = Each(parts, apply);
                    if (!each) return Fail(std::format("'{}': {}", name, each.error()));
                    out.emplace_back(name, std::move(*each));
                }
                return Value(std::move(out));
            }
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
        inline const std::map<std::string, std::pair<int, Operation>, std::less<>>& Operations()
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
        inline Expected<std::vector<double>> NumbersOf(const Value& value, const char* name)
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

        inline Expected<std::uint64_t> Enumerator(const kb::Type& type, const Value& value)
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

        inline Expected<kb::Value> Convert(const kb::Type& type, const Resolved& input);

        inline Expected<kb::Value> ConvertValue(const kb::Type& type, const Value& value)
        {
            return Convert(type, Resolved { value, nullptr, false });
        }

        inline Expected<kb::Value> Convert(const kb::Type& type, const Resolved& input)
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
        inline std::string Fits(const kor::Mesh& mesh, const kor::GraphicsPipeline& pipeline, const kor::ResourceRef<const kor::Shader>& vertexShader)
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
        inline Value LayoutValueOf(const kor::VertexLayout& layout)
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
        inline std::string LayoutSignature(const kor::VertexLayout& layout)
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
        inline Expected<Value> LayoutValue(const Value& attributes)
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
        inline std::string AssetName(const std::filesystem::path& file, const std::filesystem::path& directory, const int mesh)
        {
            std::filesystem::path name = file;
            std::error_code ec;
            if (!directory.empty() && file.is_absolute())
                if (const auto whole = std::filesystem::absolute(directory, ec); !ec)
                    if (const auto relative = file.lexically_normal().lexically_relative(whole.lexically_normal()); !relative.empty() && *relative.begin() != "..") name = relative;
            return mesh < 0 ? std::format("@{}", name.generic_string()) : std::format("@{}#{}", name.generic_string(), mesh);
        }
    }
    using namespace runtime_detail;

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
        std::map<std::string, std::set<std::string>, std::less<>> referencesRead;   // of `current`: ReadFromReferences
        // Its nodes by id, and what each pipeline's draws take from elsewhere: worked out once for each graph evaluated,
        // not once for each node that asks.
        std::unordered_map<std::string_view, const Node*> currentById;
        /** A node of the graph as last set, by its id: looked up, as this is done for each entity of a list, each frame. */
        [[nodiscard]] const Node* CurrentNode(const std::string_view id) const;
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
        std::map<std::string, Held, std::less<>> defaultMeshes;    // the procedural meshes held, by name: `sphere?rings=24`
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
        std::vector<bool> restsBefore;                      // RestUnread's: which passes rested when it last worked it out,
        std::vector<const std::string*> unreadBefore;       // what it then rested for nothing reading it, of `wanted`,
        std::size_t unreadOf = 0;                           // and how many passes' uses it was of
        std::set<std::string, std::less<>> batched;
        std::map<std::string, std::string, std::less<>> unbatched;
        /// The Repeats whose count is wired, as the graph was written out (Expansion::wiredCounts), and how many turns each
        /// was written out with — as what it is wired from last gave.
        std::map<std::string, Link, std::less<>> wiredCounts;
        std::map<std::string, int, std::less<>> loopCounts;

        /** Whether a Bundle gave node @p id its input @p name, rather than a wire or a value of its own. */
        [[nodiscard]] bool FromBundle(const std::string_view id, const std::string& name) const;
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
        [[nodiscard]] const Scripted* ScriptNamed(const std::string_view name) const;
        [[nodiscard]] std::uint64_t ScriptsVersion() const;

        // The material kinds of the world, by name, and where their Slang modules are written.
        std::vector<MaterialKind> materialKinds;
        /** The kinds a shader's attributes name: this runtime's, or — one that follows another, with none of its own — the other's. */
        [[nodiscard]] const std::vector<MaterialKind>& Kinds() const;
        std::filesystem::path generatedShaders;
        bool generatedOwned = true;     // false: the folder is another runtime's, which keeps what else is in it
        std::vector<std::string> materialNotes;
        bool materialsToWrite = false;
        /**
         * What the kinds' modules are to say has changed: written once they are all known, before anything is built of
         * them. Written a kind at a time, a kind not declared yet would be taken away and made again, and every shader
         * that imports it built again, each time a project is opened.
         */
        void WriteMaterials();
        void WriteMaterialsNow();
        const std::filesystem::path& GeneratedFolder();
        /**
         * The module that sorts what @p element names (`compute.element`), written beside the material kinds' — where a
         * search path is already, so that no other shader is built again for it. It does not begin as theirs do, which
         * are taken away when they are of no kind. What it imports is seen whole, a shader's own `push` with it, so that
         * its own names are ones a shader would not have. Its name, or why there is none.
         */
        Expected<std::string> WriteSortModule(const std::string& element);

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
        [[nodiscard]] const Built* Find(const std::string_view id) const;
        std::atomic<bool> reloaded = false;
        // A shader could not be made — its file is not there yet, or does not compile and nothing is watching it:
        // the nodes that failed are tried again every so often, for the file to turn up.
        bool shaderFailed = false;
        std::chrono::steady_clock::time_point lastRetry = std::chrono::steady_clock::now();

        Impl(kor::FrameGraph& graph, kecs::World* entities) : frameGraph(graph), world(entities)
        {
            if (world) world->Id<Mesh>();      // what a ForEach of meshes is of, before anything has one
        }

        void Unwatch(Built& node);

        // What an edit replaced: kept until the frame that may be drawing with it is done. An edit made from an
        // interface comes in the middle of a frame, after the scene has recorded commands with these and before the
        // frame is finished with them. Let go of at the second Update after: by then that frame is long handed over.
        std::vector<Built> retired, retiring;
        std::vector<Held> retiredResources, retiringResources;
        std::vector<EntityRun::Cached> retiredSets, retiringSets;   // an entity's descriptor set, and what it was made of
        void Retire(Built&& old);
        void LetGo();

        void Watch(Built& node, kor::ResourceRef<const kor::Shader> shader);

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
                                              const std::function<bool(std::string_view)>& handledElsewhere);

        Expected<std::shared_ptr<Resource>> BuildWith(const kb::Description& description, const std::string& nodeId, const std::vector<kb::Value>& constructor,
                                                      const std::vector<kb::Call>& calls);

        /**
         * What a mesh is drawn with by a pipeline node: the node's own pipeline when the mesh is laid out as it is, and
         * otherwise the same pipeline made for the mesh's layout — when the mesh has everything the vertex shader reads,
         * found by semantic as the shader names it. Why not, when it has not.
         */
        Expected<kor::ResourceRef<const kor::GraphicsPipeline>> PipelineFor(const PipelineInfo& info, const kor::Mesh& mesh);

        /** A field's value as its bytes, as a script lays it out. Why not, when the value is not one the field can be. */
        static std::string FieldBytes(const kecs::FieldDescription& field, const Value& value, std::byte* at);

        /**
         * A Script node: the script it names, its settings — what it is given for each field, and what the script's source
         * says for the rest — as the script lays them out, and the resources it makes, each an output by its name. Each is
         * made as a node of its kind would be, of what the source gives it and what the node is given for it
         * (`<name>.<input>`). Those that are of no other of the script's resources — its images and buffers — are made
         * first, filled by the script, and made again with what it wrote; then those made of them, a view of an image.
         */
        Expected<void> BuildScript(const Node& node, const Inputs& inputs, Built& out);

        /** Told by MadeWithout that a resource is used as `usage`, by what is using resources now: the node that made it is made so. */
        void UsedAs(const Resource& resource, const std::uint32_t usage);
        static bool MadeForItsUse(const std::string_view type);
        /** An Image's or a Buffer's usage flags, by name, as its builder has them. */
        static const kb::Enumeration* UsagesOf(const std::string_view type);
        /** The usage an Image or a Buffer node is given: what it has besides what it is used as. */
        static std::uint32_t GivenUsage(const std::string_view type, const Inputs& inputs);
        /** What an Image or a Buffer node's resource is made for: what uses it, and to be copied from, which a watch does. */
        std::uint32_t UsageFor(const std::string_view id) const;
        /** Sets what is using resources, as UsedAs is told, for as long as it lasts; and a runtime's MadeWithout reaching it. */
        struct Using {
            Using(Impl& impl, std::string user) : impl(impl), user(std::exchange(impl.user, std::move(user))), hook(std::exchange(usedAs, &impl.usedAsHook)) {}
            ~Using() { impl.user = std::move(user); usedAs = hook; }
            Impl& impl;
            std::string user;
            std::function<void(const Resource&, std::uint32_t)>* hook;
        };

        Expected<void> BuildBuilder(const Node& node, const kb::Description& description, const Inputs& given, Built& out);

        /**
         * How the frame graph is to make a transient: as @p standIn — what its node made in its place — is. A 2D image of one
         * layer, of one sample; a buffer on the device (one the CPU writes is one for each frame in flight, never shared).
         */
        static Expected<Transient> TransientOf(const Resource& standIn);

        /** The shaders wired to a pipeline node, in the order of its inputs. */
        static std::vector<kor::ResourceRef<const kor::Shader>> ShadersOf(const Inputs& inputs, kor::ResourceRef<const kor::Shader>* vertex = nullptr);

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
        Expected<Inputs> AsTheBuilderTakesThem(const Node& node, const kb::Description& description, const Inputs& given, PipelineInfo& info, Built& out);

        Expected<void> BuildPipeline(const Node& node, const kb::Description& description, const Inputs& given, Built& out);

        /** What a link comes from, past the References between: the node, by id. */
        [[nodiscard]] const Node* Origin(const std::string& id) const;

        /**
         * What the draws of pipeline @p id — the Draws and Dispatches it is wired to — take from elsewhere than the
         * pipeline's own component: each `param.` and `push.` given on the node, or wired from anything but a ForEach of
         * that component's field of the same name. What is left is the component.
         */
        [[nodiscard]] std::set<std::string> TakenElsewhere(const std::string& id) const;

        /** What a draw of pipeline @p id takes from elsewhere than its component, added to @p taken. */
        void TakenBy(const Node& node, const std::string& id, std::set<std::string>& taken) const;

        /** Makes the world's component of a pipeline's layout, or gives the one it has its new shape. */
        Expected<void> Register(const std::string& name, PipelineInfo& info, Built& out);

        Expected<void> BuildPass(const Node& node, const Inputs& inputs, Built& out);

        /** A matrix of four by four from its sixteen numbers, column after column. */
        static Expected<kor::Mat4> MatrixOf(const Value& value, const char* name);

        /** What the editor's gizmos and the debug shapes go by: the main View's matrix and the main lens's. */
        void SeenThrough();

        /**
         * A view, as glm's lookAt makes one: from a position, towards a target, up as up. The main one is the scene's:
         * in an editor, the editor's own in its place.
         */
        Expected<void> BuildView(const Inputs& inputs, Built& out);

        /**
         * A projection: a Perspective, an Orthographic, a Frustum, or an Oblique's near plane moved. Each has depth 0 at
         * its near plane and 1 at its far one, and Y pointing down as the device has it unless told otherwise. It knows
         * nothing of where it is seen from. The main one is the scene's lens: a Perspective's, in an editor, the editor's.
         */
        Expected<void> BuildLens(const Node& node, const Inputs& inputs, Built& out);

        /** Half of all round a point, for a shader to bend into a picture: what it takes, and the turn that faces a view that way. */
        static Expected<void> BuildParaboloid(const Inputs& inputs, Built& out);

        /** One face of a cube round a point, as a cube map's layers are: towards +x, -x, +y, -y, +z, -z. */
        static Expected<void> BuildCubeFace(const Inputs& inputs, Built& out);

        /**
         * A light's shadow maps: four cascades, each fitted to a slice of what a camera sees — nearer slices smaller, so
         * that a shadow is sharp where it is looked at closely.
         */
        static Expected<void> BuildCascades(const Inputs& inputs, Built& out);

        /**
         * The point lights' shadow maps: a cube round each, a face a quarter turn wide towards each way along each axis,
         * from just off the light to as far as it reaches. Which of them are drawn again this frame: `each` of them, a
         * light after another — and whichever is not where, or as far-reaching, as it was when it was drawn last.
         */
        Expected<void> BuildPointShadows(const Node& node, const Inputs& inputs, Built& out);
        Expected<void> BuildSpotShadows(const Node& node, const Inputs& inputs, Built& out);

        /**
         * The shape of the input @p name of @p consumer, when it is said: of its type's, or a draw's `push.` and `param.`
         * as its pipeline has them, or a pipeline's `constant.` as its shaders had them when it was last built.
         */
        [[nodiscard]] Shape InputShape(const Node& consumer, const std::string& name) const;

        /**
         * What the inputs @p id 's output is wired to take, the first that says: through a node that takes any kind and gives
         * what it is given (a Math node's Add, Lerp), what that one's output is wired to. None when nothing says.
         */
        [[nodiscard]] Shape WiredShape(const std::string& id, const int depth = 0) const;

        /** What a Random gives: what its `type` says, or for Auto what it is wired to takes, and a Float when that says nothing. */
        [[nodiscard]] Shape RandomShape(const Node& node) const;

        /**
         * A Random's number, or vector of them, of its node's distribution and of RandomShape's: each component drawn in turn
         * from one generator seeded with its seed, by the distribution's inputs — each a number for every component, or a
         * vector, one for each. One not given is what the node type says it starts as.
         */
        Expected<void> BuildRandom(const Node& node, const random::Distribution distribution, const Inputs& inputs, Value& result);

        /** A colour said one way, said another: picked (sRGB), linear, by the power 2.2, as hue, saturation and value, or a temperature. */
        Expected<void> BuildColorConvert(const Inputs& inputs, Built& out);

        /** Turns as quaternions — x, y, z, w — made, put together, and turned into other things. */
        Expected<void> BuildQuaternion(const std::string& type, const Inputs& inputs, Built& out);

        /** A place within the pixel for a frame, of a Halton (2, 3) sequence, and a projection moved by it. */
        static Expected<void> BuildJitter(const Inputs& inputs, Built& out);

        Expected<void> BuildValue(const Node& node, const Inputs& inputs, Built& out);

        /** What the default meshes have of their surface's directions: the source's, for a runtime that follows another. */
        [[nodiscard]] Tangents DefaultTangents() const;

        /**
         * How the default meshes' vertices are laid out: POSITION, NORMAL, the tangents the graph asks for and UV, in one
         * buffer — as a mesh of their attributes is (MeshOf).
         */
        static kor::VertexLayout DefaultLayout(const Tangents tangents);

        /** Which attributes, of what, in which buffers: meshes alike in this are drawn by one pipeline, and can be one mesh. */
        static std::string LayoutKey(const VertexData& data);

        /** The kor type of an attribute's numbers. */
        static kor::ChannelType ChannelOf(const AttributeType type);

        /**
         * A mesh of any attributes on the device: a vertex buffer for each of its buffers, the attributes it names side
         * by side in each vertex, and one more for what none names. What it has not is not anywhere.
         */
        static Expected<Held> MeshOf(const VertexData& data);

        /**
         * A mesh as its buffers will hold it, and the box it is in: worked out of its attributes with nothing of the device,
         * so off the frame — where a model is read — leaving the frame only the buffers to make (MeshOf).
         */
        struct Interleaved {
            std::string problem;
            kor::VertexLayout layout;
            std::vector<std::vector<std::uint32_t>> buffers;
            std::vector<std::uint32_t> indices;
            float bounds[6] {};
            bool bounded = false;
        };

        static Interleaved Interleave(const VertexData& data);

        /** The mesh @p ready says, on the device: its buffers made and their data handed over, on the frame. */
        static Expected<Held> MeshOf(Interleaved ready);

        /** The box vertices @p first to @p first + @p count of @p data are in, as @p held 's. */
        static void Bound(Resource& held, const VertexData& data, const std::uint32_t first, const std::uint32_t count);

        /**
         * A procedural mesh, by its name (`sphere`, `sphere?rings=24`): made the first time it is asked for, and kept, so
         * that all who name the same numbers hold the one mesh. Null for a name of no shape.
         */
        Held DefaultMesh(const std::string_view asked);

        // ---- what is in files ---------------------------------------------------------------------------------------

        std::filesystem::path assetDirectory;
        std::map<std::string, Held, std::less<>> assets;        // what was asked for so far, by the name it is found by
        std::map<std::string, std::shared_ptr<const Model>, std::less<>> models;    // by file

        /** Where files named without their whole path are: the source's, for one that follows another. */
        [[nodiscard]] const std::filesystem::path& Assets() const;
        /** A file named @p name — without the `@` — where it is: under @p directory, unless the name is a whole path. */
        static std::filesystem::path FileNamed(const std::string_view name, const std::filesystem::path& directory);
        [[nodiscard]] std::filesystem::path AssetFile(const std::string_view name) const;
        [[nodiscard]] std::string AssetId(const std::filesystem::path& file, const int mesh) const;

        /** A picture of one texel: `white`, `black`, or `normal` — a surface's own normal, as a normal map has it. */
        Held DefaultImage(const std::string_view name);

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
        struct Whole { VertexData all; std::vector<Held> parts; Interleaved ready; };    // ready: all, as its buffers hold it
        struct Arrived { std::vector<Whole> wholes; };     // a model's meshes, read and put together off the frame

        /** A model's meshes put together into wholes, each part bounded: the work of it, done where the file was read. */
        static Arrived Together(std::vector<std::pair<std::string, VertexData>> meshes, const std::map<std::string, std::string>& names);
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
        std::vector<kor::Task<void>> reading;                   // the files being read
        /// A PointShadows' or a SpotShadows' lights as their maps were last drawn — where each was, how far it reached,
        /// and a spot's way and cone — and whose turn it is to be drawn again. Forgotten when the graph is built again:
        /// the maps are then new.
        struct ShadowsDrawn { std::vector<std::array<float, 8>> as; int next = 0; };
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
        /// The transients, by node: how the frame graph is to make each — as what its node made in its place is — and which
        /// object of the frame graph's what is wired from it was last built with (null: what stands in).
        std::map<std::string, Transient, std::less<>> transients;
        std::shared_ptr<TransientBoard> board = std::make_shared<TransientBoard>();
        std::map<std::string, std::vector<std::pair<std::string, const void*>>, std::less<>> holdsOf;   // each pass's transients, by node, as the objects it was built with
        std::deque<Arrived> arrivedMeshes;                      // models read, and not yet the device's: each, the next frame
        std::optional<std::chrono::steady_clock::time_point> modelsWaiting;  // since when a model read waits for the others
        std::vector<Held> arrivedPictures;                      // the stand-ins whose pictures came, for the nodes that hold one

        /** Where what is made of the project's files is kept: `.koral/cache` beside its assets. None without them. */
        static std::optional<kasset::Cache> CacheBeside(const std::filesystem::path& assetDirectory);

        /**
         * A model file's numbers, as the importer made them — from the cache, when it was read so before — with what it
         * names named as the runtime names files. Touches nothing of the runtime's, so it is read on any thread.
         */
        static std::optional<ReadModel> Read(const std::filesystem::path& file, const std::filesystem::path& assetDirectory, const MeshImport& import);

        /** Reads a model file on a background thread, and hands what it holds back on the frame's. */
        static kor::Task<void> ReadModelFile(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string key, std::filesystem::path file,
                                             std::filesystem::path assetDirectory, MeshImport import);

        /** What a model file's meshes have, read off the frame without anything of them made: what an import asks about. */
        static kor::Task<void> ProbeModelFile(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string key, std::filesystem::path file);

        /**
         * Reads a picture off the frame — its file, or the file cooked of it, kept as @p format — and leads whoever holds
         * @p standIn to it.
         */
        static kor::Task<void> ReadPicture(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string id, std::filesystem::path file, Held standIn,
                                           const TextureFormat format = TextureFormat::eAsRead);

        // ---- pictures kept block-compressed ----------------------------------------------------------------------

        std::map<std::string, TextureImport, std::less<>> textureImports;     // as the graph has them, by file
        std::map<std::string, TexturePack, std::less<>> texturePacks;         // by pack
        std::map<std::string, std::pair<std::string, int>, std::less<>> packedIn;   // a file a pack is made of: the pack, and the channel it went into
        std::deque<cook::Recipe> toCook;                        // asked to be cooked, and not yet begun
        std::vector<kor::Task<void>> cookingTasks;              // the one cooking them, while it is
        Runtime::CookProgress cooking;

        /** Where pictures are cooked to: the project's `.koral/cache/textures`, beside its `assets`. */
        [[nodiscard]] std::filesystem::path TextureCache() const;
        /** Where a picture file of an asset id is, worked out by a copy of the directory: for a thread off the frame. */
        [[nodiscard]] cook::FileOf FileOf() const;
        /** What picture @p id is cooked of, and how, as the graph says: a pack's channels, a file's import, or as it is read. */
        [[nodiscard]] cook::Recipe RecipeOf(const std::string_view id) const;
        /** Whether picture @p id has a file cooked of it as it is now, with the settings as they are now. */
        [[nodiscard]] bool Cooked(const std::string_view id) const;

        /** A view of picture @p image whose channels are read as @p mapping says: it holds the picture, which stays as long. */
        static Held ViewOf(const Held& image, const kor::ImageView::ComponentMapping& mapping);
        /** A picture kept as @p format, as it is to be read: BC4's one channel in red, green and blue; BC5's blue — a z it has not — 1. */
        static Held Spread(Held image, const TextureFormat format);
        /** A file a pack is made of, as whoever holds it reads it: the pack's channel it went into, in red, green and blue. */
        static Held Channel(const Held& pack, const int channel);

        /** Whoever holds @p from — a stand-in, or picture @p id as it was read before — holds @p to from here on. */
        void Lead(const std::string& id, const Held& from, Held to);
        /** Picture @p id is here: what held @p standIn holds it, and each file it is a pack of reads its channel of it. */
        void PictureArrived(const std::string& id, const Held& standIn, Held real);

        /**
         * Starts reading picture @p id into the place of @p standIn: of a file a pack is made of, nothing — it is the
         * pack's channel, once the pack is there; the file cooked of it, when there is one of it as it is; a pack's
         * files, put together; or its file.
         */
        void StartPicture(const std::string& id, const Held& standIn);

        /** A pack not cooked yet: its files read and put together off the frame, made a picture of four bytes a texel, with its mips. */
        static kor::Task<void> ReadPack(Impl* self, std::shared_ptr<std::atomic<bool>> alive, cook::Recipe recipe, cook::FileOf fileOf, Held standIn);

        /**
         * How picture files are kept, as the graph says: each picture whose settings changed — its import, its pack, or
         * the pack it is in — is read again, as it is now to be.
         */
        void SetTextureImports(const std::map<std::string, TextureImport>& imports, const std::map<std::string, TexturePack>& packs);

        /** Cooks what is asked, one picture after another, off the frame; each read again from what was cooked of it. */
        static kor::Task<void> CookAll(Impl* self, std::shared_ptr<std::atomic<bool>> alive);
        void Cook(const std::vector<std::string>& ids);

        /** Starts reading a model file, unless it is read or being read. */
        // ---- files written again while they are had ------------------------------------------------------------

        /// Each file read — a picture, a model, and what a model names beside it — by its path: when it had last been
        /// written, as it was read, and the model it is of (empty for a picture).
        struct Stamp { std::filesystem::file_time_type written; std::string model; };
        std::map<std::string, Stamp, std::less<>> stamps;
        std::chrono::steady_clock::time_point lookedAt {};

        static std::optional<std::filesystem::file_time_type> WrittenAt(const std::filesystem::path& file);
        /** Remembers when @p file was written, as it is read now: of model @p model, or a picture. */
        void Remember(const std::filesystem::path& file, const std::string& model = {});

        /**
         * A second at most: each file read that was written since — by another program, an export from Blender — is read
         * again, and what held what was of it holds what is of it now: a model's meshes, a picture.
         */
        void Watch();

        void WantModel(const std::filesystem::path& file);

        /**
         * What the default meshes are to have of their surface's directions, as the graph says: changed, those made are
         * made again, and what held them holds the new; a Primitive's is built again.
         */
        void SetDefaultMeshTangents(const Tangents wanted);

        /** How model files are to be read, as the graph says: a file read already whose settings changed is read again. */
        void SetMeshImports(const std::map<std::string, MeshImport>& wanted);

        /** A picture of four bytes a texel, its smaller levels made of it: what a generator or a script makes. */
        static std::expected<Held, std::string> PictureOf(const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba);
        /** A generator's picture as the cache keeps it: its width and height, then its texels. */
        static std::vector<std::byte> PictureBytes(const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba);

        /** Whether @p id is a mesh a generator made: `$script/gen/<script>/<what it was made of>`. */
        static bool Generated(const std::string_view id);
        /** What a generator's mesh is kept by in the cache: its id, which is what it was made of. */
        static kasset::Key GeneratedKey(const std::string_view id);

        /** What is in a file, by the name it is found by: `@wall.png`, a picture; `@model.gltf#3`, a mesh. A stand-in, until it is read. */
        Held Asset(const std::string_view id);

        /** Each frame: what was read since is made the device's, and what holds a stand-in is led on. */
        /** The transients node @p id is wired from, by way of anything between: a view, a framebuffer, a pipeline, a Reference. */
        const std::set<std::string>& TransientsUpstream(const std::string& id, std::map<std::string, std::set<std::string>, std::less<>>& memo) const;

        /**
         * Each frame: what the frame graph has made of each transient since — the first time, or made again — is what its node
         * gives from now on, and what is wired from it is built again with that. What stood in goes once nothing holds it.
         */
        void ArriveTransients();

        /**
         * Each frame: whether a Repeat whose count is wired was written out with another count than what it is wired from
         * gives now — a whole number, from 0 to 256 — and so is to be written out again (Run). Said once for each change.
         */
        bool Recount();

        void Arrive();

        // ---- what is read from the world, each frame --------------------------------------------------------

        /** Whether a node is a ForEach: every entity with a component, an element at a time. An Entities node is one, by its old name. */
        static bool IsEach(const std::string_view type);
        /** Whether a node is a buffer of elements laid out field by field: a shader's struct, or one set out on the node. */
        static bool IsMapped(const std::string_view type);

        /** Whether a node's outputs are read from the world every frame, rather than worked out once. */
        static bool ReadsTheWorld(const std::string_view type);

        static Value Matrix(const kor::Mat4& m);
        static Value Three(const kor::Vec3 v);
        /** A colour as picked — as the display shows it — in the linear terms light adds up in. */

        /** The entity a node is of: the one so named, or — with no name — the first that has the component. */
        [[nodiscard]] kecs::Entity EntityFor(const Inputs& inputs, const kecs::ComponentId component) const;

        /** A field of a component as a value: a number, or a list of them. Null for one that is neither. */
        /** Whether a component's field is a shared reference to a resource — a mesh, a picture — and no value. */
        static bool IsResourceField(const kecs::FieldDescription& field);

        static Value FieldValue(const kecs::FieldDescription& field, const std::byte* at);

        /** What a Readback copied frames enough ago that the GPU is done with it, as a value. */
        static void ReadBack(Built& out);

        /** The box round the entities wired to a SceneBounds: each one's mesh's box where it is, or where it is when it has none. */
        Expected<void> BuildBounds(const Node& node, Built& out);

        /** A WriteComponent's value written into its entity's field, when the field does not hold it already. */
        Expected<void> WriteComponent(const Inputs& inputs, Built& out);

        /** A DebugShapes' shapes, drawn this frame into the scene's debug shapes: as many as its values have. */
        Expected<void> DrawShapes(const Inputs& inputs);

        /**
         * A node that reads the world: its outputs, as the world is now. Called when the graph is set, and again
         * every frame, so that what is wired from one follows the scene.
         */
        Expected<void> BuildLive(const Node& node, const Inputs& inputs, Built& out);

        /**
         * What a ForEach gives, as of one of its elements: the @p index th, which is entity `index / repeat`'s turn
         * `index % repeat`. With none there, a component of nothing at the origin. A buffer its fields are wired to goes
         * through them all, an element each, and asks for only what it reads (@p only).
         */
        void SetRow(Built& out, const std::size_t index, const std::set<std::string, std::less<>>* only = nullptr) const;

        /**
         * Where an entity was the frame before, of where it is now (@p now): what it was when it was last asked of on the
         * frame before this one. Asked of on no frame before — or not on the last — where it is now: it did not move.
         */
        kor::Mat4 MatrixBefore(const kecs::Entity entity, const kor::Mat4& now) const;

        /** Whether a node's values are worked out of other nodes' values, so that they follow whatever those follow. */
        static bool WorksOut(const Node& node);

        /**
         * Whether what node @p id gives is one entity's of many: it is an Entities node (which is added to
         * @p sources), or is worked out of one (and is added to @p chain, after what it is worked out of).
         */
        bool Follows(const std::string& id, std::set<std::string>& sources, std::vector<std::string>& chain, std::map<std::string, bool>& seen) const;

        /**
         * A Collect's list: the value wired to it, as each element of the ForEach it follows has it, one after another —
         * worked out again for each, as a buffer's elements are. Of a value that follows none, that one value.
         */
        void CollectAll(const Node& node, Built& out);

        /** One field of an element, written where the shader reads it: as many numbers as it takes, and nothing for the rest. */
        static void Put(const BlockField& field, const Value& value, std::byte* element);

        /** The outputs of an element that are worked out of where its entity is: its world matrix is in its key when one is asked. */
        static constexpr std::array<std::string_view, 10> PlacedOutputs { "matrix", "normalMatrix", "worldPosition", "forward", "up", "right", "position", "rotation", "orientation", "scale" };

        /**
         * How many bytes a row of @p source is keyed by, for buffer @p node: its entity, what is read of it — each field asked
         * for, as its bytes; a resource as the one it is now — its world matrix when anything placed is asked, and its group.
         * 0 when a row cannot be keyed so: a field that owns what it holds (a string), the matrix of the frame before, or a
         * node between that reads the world itself. What the chain reads besides — wires from nodes that follow no entity,
         * values given, how many there are — is @p context: a row is reused only while that is as it was too.
         */
        std::size_t RowKeySize(const Node& node, const Mapped& mapped, const Built& source, const std::set<std::string, std::less<>>& asked, std::vector<Value>& context) const;

        /** Row @p index's key, as RowKeySize lays it out, into @p key. */
        void RowKey(const Built& source, const std::size_t index, const std::set<std::string, std::less<>>& asked, std::byte* key) const;

        /**
         * A ShaderBuffer's elements as they stand, written when they are not what was last written: those of the
         * buffer it comes after, then one for each entity its fields follow — each field as that entity has it,
         * through whatever is worked out in between — or the one element, when they follow none.
         */
        void Fill(const Node& node, Built& out);

        /** A field of an element as a shader writes its type: `float3`, `float4x4`, `uint[4]`. */
        static std::string FieldKind(const BlockField& field);

        /** A buffer's element as inputs to its node: `field.<name>` for each of its fields. What it has, by name, for what is said of one it has not. */
        static std::string FieldPorts(const BlockLayout& layout, Built& out);

        /**
         * An element laid out as a shader lays out a struct of these fields, one after another: std430 for a storage
         * buffer, std140 for a uniform block. A number takes four bytes, on four; a vector of two eight, on eight; of
         * three or four, on sixteen; a matrix is its columns, each a vector of four's room. An array's elements are each
         * on their own alignment — on sixteen, in std140 — and so is the struct, whose size is a whole number of it.
         */
        static Expected<BlockLayout> LayOut(const std::vector<std::tuple<std::string, std::string, std::uint32_t>>& fields, const bool storage);

        /**
         * A buffer of elements laid out on the node: its `layout` says each field. As many elements as its count says, or
         * as the ForEach its fields follow has, or one.
         */
        Expected<void> BuildBufferBuilder(const Node& node, const Inputs& inputs, Built& out);

        /**
         * A buffer a shader declares, laid out as its compiled code lays it out: a field of the struct an input,
         * by name. The shader is the node's to name — a module, and an entry point that uses the buffer — since the
         * pipeline that reads the buffer is wired from this node, and cannot also be what it is worked out of.
         */
        Expected<void> BuildShaderBuffer(const Node& node, const Inputs& inputs, Built& out);

        /**
         * What a buffer of elements is made of, once its element is laid out — by its shader or on the node: where its
         * elements come from (a ForEach its fields follow, or none), what comes before them, and room for
         * them all. @p has is the fields it has, for what is said of one it has not.
         */
        Expected<void> MakeMapped(const Node& node, const Inputs& inputs, Built& out, std::shared_ptr<Mapped> mapped, const std::string& has);

        /**
         * What output @p output of node @p id holds of each of its elements: a resource for each element of a ForEach (a
         * Filter's `Mesh.mesh`), each led on to what replaced it — or the one it holds, of a node that follows none.
         */
        std::vector<Held> EachResource(const std::string& id, const std::string& output) const;

        /** A MeshPack's meshes as they are now: the distinct ones of its elements, in the order they come first. */
        std::vector<Held> PackedMeshes(const Node& node) const;

        /**
         * The same meshes as PackedMeshes, as which they are and nothing more: what a MeshPack asks each frame, to know
         * whether to pack again — without holding each entity's mesh on the way, thousands of them.
         */
        std::vector<const Resource*> PackedNow(const Node& node) const;

        Expected<void> BuildMeshPack(const Node& node, const Inputs& inputs, Built& out);

        /**
         * A MeshPack's meshes cut into meshlets on the CPU: each mesh's triangles taken in their order, a meshlet ending
         * where one more would take it past so many vertices or triangles. Each has a sphere round its vertices and a
         * cone of the ways its triangles face (Meshlets' doc says how a shader tests them).
         */
        Expected<void> BuildMeshlets(const Inputs& inputs, Built& out);

        /** A RayScene: its bottom-level structures, made now; its top-level one, made now and whenever its instances change. */
        Expected<void> BuildRayScene(const Node& node, const Inputs& inputs, Built& out);

        /** A RayScene's top-level structure made again, when its instances are not what they were: where each is, which, how many. */
        void UpdateRays(const std::string& id, Built& node, const bool building = false);

        /** An IndirectDraws' commands, as their bytes, and how many instances its groups have. */
        static Expected<std::pair<std::vector<std::byte>, std::size_t>> IndirectCommands(const Inputs& inputs, const bool gpu);

        static bool OnTheGpu(const Inputs& inputs);

        Expected<void> BuildIndirectDraws(const Node& node, const Inputs& inputs, Built& out);

        Expected<void> BuildPrimitive(const Node& node, const Inputs& inputs, Built& out);

        /** An input given more than once, as its type lists it: `field[2].value` is `field[].value`. */
        static std::string Listed(std::string name);

        /**
         * What a node a Bundle gave inputs to says of it: the names it had that the node has no input for, and the inputs of
         * its kind the node has that it had not — what the expansion said, for a BufferBuilder, whose fields it knew.
         */
        std::string BundleNote(const Node& node, const Built& made) const;

        Expected<void> Build(const Node& node, const Inputs& inputs, Built& out);

        /**
         * What a node gives besides what it makes: everything about it that another node might want. Each value it
         * was given, under the input's name; the parts of one thing as one (`extent` of `extent.x`, `.y`, `.z`); what
         * an input is when it is not given; and what the object it made says of itself — an image's size, a buffer's.
         */
        static void AddOutputs(const Node& node, const Inputs& inputs, Built& out);

        /**
         * An input given whole is given in its parts: `groups` of [8, 8, 1] is `groups.x`, `.y` and `.z`, which is how
         * everything after this reads it. As many parts as the vector has numbers: an image's size, of two, leaves z
         * as it is when not given.
         */
        static Expected<void> InParts(const Node& node, Inputs& inputs);

        /** A struct given some of its fields has the rest as the node type says they are, not as nothing. */
        static void FillDefaults(const Node& node, Inputs& inputs);

        // ---- the whole graph ----------------------------------------------------------------------------------

        void Evaluate(const Graph& graph);

        // ---- each frame: the entities ---------------------------------------------------------------------------

        /** A value as a push constant's bytes: as many numbers as it takes, each of the shader's kind, and zero for the rest. */
        static void PackPush(const PushMember& push, const Value& value, std::byte* at);

        /** What is said of a draw given nothing for a resource: of the entity's field, when that is where it comes from. */
        static std::string Lacks(const Source& source);

        /**
         * A buffer of each instance's, for a Draw that is given nothing for it: made here, with room for more than there
         * are, and filled each frame from the entities of the Filter whose meshes the Draw's MeshPack packs — in the order
         * the pack's indirect commands draw them. A TraceRays' is of the Filter wired to its `entities`, in its order: the
         * InstanceID() a RayScene of the same Filter gives each.
         */
        Expected<Source> InstancesFor(const Node& draw, const PipelineInfo& info, const Slot& slot, EntityRun& run);

        /**
         * Each instance's element as its entity is now: where it is, which it is, and its own fields. Once a frame, and
         * only what changed: an element is written again when another entity is in its place, when its entity moved
         * (kecs::World::MovedAt) or when its entity's own fields were written (ChangedTickOf) since it was last written.
         * A frame in which nothing of theirs changed looks at none of them.
         */
        void FillInstances(Instances& instances, const bool now = false);

        /**
         * A Draw's `whenChanged`: what it is to draw, made one number — the same meshes, of the same entities where they
         * are, with the same sets, push constants, instances and matrix to cull by — and whether that is not what it drew
         * last, as many times in a row as there are copies of what it draws into. Whether it draws is settled once every
         * pass is readied (WhichDrawAgain): with the others that draw into what it does.
         */
        void Unchanged(EntityRun& run, const std::optional<kor::Mat4>& sight);

        /**
         * Which Draws `whenChanged` draw this frame. One whose view another draws into is drawn with it: those that draw
         * into one view after another — a cascade a quarter of an atlas, after the one that clears it — clear nothing of
         * their own, and one drawn again alone would draw over what it drew before. So a view any of them is due in is
         * drawn again by all of them, and one a Draw that draws every frame draws into, every frame.
         */
        void WhichDrawAgain();

        /** Readies a Draw's or a Dispatch's draws for this frame: one, or one an element of its ForEach, and what each is made of. */
        void Prepare(EntityRun& run);

        /** A node's inputs as they stand: its own values, and for each wire what the other end gives now. Nothing when one is missing. */
        [[nodiscard]] std::optional<Inputs> Gather(const Node& node) const;

        /**
         * What follows the world, brought up to date: each node that reads it, and — in the order they depend on one
         * another — every value worked out from one, and every buffer one is wired to. Nothing is built again:
         * a value changes where it is, and a buffer is written.
         */
        void RunLive();

        /** Whether a node is a resource made by the framework's builder — an Image, a Buffer, a Sampler — of the values it is given. */
        bool IsMadeResource(const std::string_view type) const;

        /** The values wired into a node, as they are now: what a resource made of them is compared with. */
        std::string WiredValues(const Node& node) const;

        /** Which outputs of each Reference are wired to something: what it has to keep up to date. */
        void ReadFromReferences(const Graph& graph);

        /** One node that follows the world, brought up to date: read again, worked out again, or written. */
        void Refresh(const std::string& id);

        /**
         * A pass of a setup page records on the frame after it is built — on as many frames as there are copies of a
         * per-frame resource it writes, so that each copy is made — and rests from then on, until it is built again.
         * It stays in the frame graph all the while, so that what reads what it made is still after it.
         */
        void RunSetup();

        /** What is done every frame, before anything is recorded: the scene's buffers written, and each pass readied. */
        void EachFrame();

        // Where each entity is in the world, worked out once a frame however many passes ask.
        std::vector<kecs::Mat4> placedMatrices;
        std::vector<std::uint64_t> placedAt;        // the frame each was worked out in: NeverPlaced, not yet
        std::uint64_t placedFrame = 0;
        // No frame's: what an entity not yet placed has, so that the first frame — 0, as a buffer of instances made while the
        // graph is built is filled in — works out where each is rather than taking it as worked out already.
        static constexpr std::uint64_t NeverPlaced = ~0ull;
        // How many resources have been made again in their places, by any runtime: an index a picture is read by may be another.
        static inline std::atomic<std::uint64_t> replacements = 0;
        const kecs::Mat4& Placed(const kecs::Entity entity);

        /** Readies every pass that runs per entity. Whether what any of them uses changed. */
        bool PrepareAll();

        /**
         * Rests what nothing that runs reads: a pass whose resources other passes read, but none of them one that runs
         * this frame — the shadows a renderer draws with, while another renderer makes the picture — records nothing, as
         * if idle, until one does. Kept are what writes the screen, memory the CPU reads, a resource no other pass reads
         * (what the graph is for, read by whoever runs it), or nothing at all; a page's setup, or its fixed step, whose
         * state goes on from one run to the next; and a watch's copy, with what it copies. A Draw rested so is not taken
         * as drawn: `whenChanged`, it is due again once read again.
         */
        void RestUnread();

        // ---- the frame graph ------------------------------------------------------------------------------------

        /**
         * The order passes go to the frame graph in. It runs the writers of a resource in the order they were added,
         * and what they use says more than that: a pass that only writes a resource makes what one that reads and
         * writes it works on, so it goes first. Where usage does not say, the graph's own order stands.
         */
        [[nodiscard]] static std::vector<std::size_t> OverwritesFirst(const std::vector<std::vector<Declared>>& declared, const std::vector<double>& pages);

        /**
         * Tells the frame graph what each pass uses, and puts the passes in it.
         *
         * A resource a pass names by a wire is imported by itself. The resources entities hold are not known one by
         * one — they come and go — so every parameter a pass fills per entity is a set of them. Two uses that have
         * a resource in common are uses of the same thing, and are given the same name: that is what lets a pass
         * sampling the images entities hold be ordered after the one that writes them.
         */
        void Resolve();

        /**
         * The passes of the fixed step once more for each further step a frame can have (Graph::fixedStepsAtMost), each
         * step's after the last's: `node\nstep 2`, and so on. Within them, what one reads that one of them writes is read
         * among the writers — which run in the order they are added, step after step — and not after every one of them,
         * as what only reads is: so that a step reads what the step before it wrote, and not what the last step will.
         */
        void StepAgain(std::vector<std::string>& order, std::vector<std::vector<Declared>>& ordered) const;

        /** A pass's name in the frame graph as its node's id and which fixed step it is for, from 0 (the first, or not a step's). */
        static std::pair<std::string, std::uint32_t> StepOf(const std::string& name);

        /** Puts the passes that built in the frame graph, and takes out those that are gone. */
        void Install(const Graph& graph);

        /**
         * After a shader was edited: what is wired to a parameter it no longer has, or has as another kind, is taken
         * off the node — and a constant it no longer has. What keeps its name and kind stays as it is.
         */
        void ResetStale(Graph& graph);
    };
}
