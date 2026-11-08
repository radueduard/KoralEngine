//
// kgraph: the kinds of node. One a builder the framework describes, and the engine's own.
//

#include <kgraph/meshes.h>
#include <kgraph/catalog.h>
#include <kgraph/graph.h>
#include <kgraph/random.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

#include <builderDescriptions.h>

#include "names.h"

namespace kgraph
{
    namespace {
        using kor::builders::Kind;

        std::string TypeText(const kor::builders::Type& type)
        {
            switch (type.kind) {
            case Kind::eBool: return "bool";
            case Kind::eInt:
            case Kind::eUInt:
            case Kind::eFloat: return "number";
            case Kind::eString: return "string";
            case Kind::eEnum: return std::format("enum {}", type.name);
            case Kind::eFlags: return std::format("flags {}", type.name);
            case Kind::eChoice: return std::format("choice {}", type.name);
            case Kind::eResource: return std::string(type.name);
            case Kind::eArray: return "vector";
            case Kind::eList: return "list";
            case Kind::eStruct: return std::format("struct {}", type.name);
            case Kind::eUnion: return "union";
            case Kind::eBytes: return "bytes";
            }
            return {};
        }

        Port PortOf(std::string name, const kor::builders::Field& field, const bool repeatable, const std::string_view doc)
        {
            Port port;
            port.name = std::move(name);
            port.kind = field.type->kind == Kind::eResource ? PortKind::eResource : PortKind::eValue;
            port.type = TypeText(*field.type);
            port.repeatable = repeatable;
            port.doc = std::string(doc);
            port.builder = field.type;
            return port;
        }

        Port Number(std::string name, std::string doc = {}) { return { std::move(name), PortKind::eValue, "number", false, std::move(doc), nullptr }; }
        Port Any(std::string name, std::string doc = {}) { return { std::move(name), PortKind::eValue, "value", false, std::move(doc), nullptr }; }
        Port ResourcePort(std::string name, std::string kind, std::string doc = {}) { return { std::move(name), PortKind::eResource, std::move(kind), false, std::move(doc), nullptr }; }

        /**
         * What a number an input takes is kept within, for an editor, and how it is dragged: a whole number a builder
         * takes is dragged a unit at a time, and an unsigned one is none or more; and, by the node and the input — the
         * whole of one in parts as each of its parts — what makes sense of it. A size, a distance, a count that may be
         * a few or thousands, is dragged by so much of what it is.
         */
        void Limit(const std::string_view type, Port& port)
        {
            if (port.kind != PortKind::eValue) return;
            using kor::builders::Kind;
            if (port.builder && (port.builder->kind == Kind::eInt || port.builder->kind == Kind::eUInt)) port.integer = true;
            if (port.builder && port.builder->kind == Kind::eUInt) port.min = 0.f;
            // Of one in parts, the whole's: `extent.x` is an extent's.
            const std::string_view name = port.name;
            const bool part = name.size() > 2 && name[name.size() - 2] == '.' && std::string_view("xyzw").find(name.back()) != std::string_view::npos;
            const std::string_view whole = part ? name.substr(0, name.size() - 2) : name;
            // A count, an index, a level or a layer: whole, whatever node it is of.
            static const std::set<std::string_view> whole_ { "count", "index", "face", "which", "repeat", "most", "each", "resolution", "mip", "layer", "layers",
                                                             "sourceMip", "destinationMip", "sourceLayer", "destinationLayer", "vertexCount", "firstVertex",
                                                             "vertexOffset", "instances", "firstInstance", "drawCount", "stride", "cullIndex", "tasks",
                                                             "groups", "groupSize", "size", "bytes", "offset", "range", "sourceOffset", "destinationOffset",
                                                             "sourceExtent", "destinationExtent", "bufferOffset", "bufferRowLength", "bufferImageHeight",
                                                             "imageOffset", "imageExtent", "indirectOffset", "countOffset", "stencilReference",
                                                             "stencilCompareMask", "stencilWriteMask", "clearStencil", "scissor" };
            static const std::set<std::string_view> anywhere { "count", "index", "face", "which" };
            struct Kept { float min, max; bool logarithmic = false; };
            constexpr float none = 0.f, any = std::numeric_limits<float>::infinity();
            constexpr bool log = true;
            // Of every Vec, IVec and UVec, the components: an unsigned one's none or more.
            if (type.starts_with("UVec") || type == "UInt") { port.min = 0.f; port.integer = true; }
            if (type.starts_with("IVec") || type == "Int") port.integer = true;
            static const std::map<std::pair<std::string_view, std::string_view>, Kept> known {
                // How big a resource is: at least one of it.
                { { "Image", "extent" }, { 1.f, 16384.f, log } }, { { "Image", "mipLevels" }, { 1.f, 16.f } }, { { "Image", "arrayLayers" }, { 1.f, 2048.f } },
                { { "Buffer", "instanceCount" }, { 1.f, any, log } }, { { "BufferBuilder", "count" }, { none, any, log } },
                { { "ImageView", "baseMipLevel" }, { none, 15.f } }, { { "ImageView", "mipLevelCount" }, { 1.f, 16.f } },
                { { "ImageView", "baseArrayLayer" }, { none, 2047.f } }, { { "ImageView", "arrayLayerCount" }, { 1.f, 2048.f } },
                { { "BufferView", "offset" }, { none, any } }, { { "BufferView", "range" }, { none, any, log } },
                { { "Sampler", "maxAnisotropy" }, { 1.f, 16.f, log } }, { { "Sampler", "mipLodBias" }, { -16.f, 16.f } },
                { { "Sampler", "minLod" }, { none, 1000.f } }, { { "Sampler", "maxLod" }, { none, 1000.f } },
                { { "GraphicsPipeline", "patchControlPoints" }, { 1.f, 32.f } }, { { "RayTracingPipeline", "maxRecursionDepth" }, { none, 31.f } },
                { { "Mesh", "vertexBuffer.binding" }, { none, 31.f } },
                { { "Framebuffer", "depthStencil.depth" }, { none, 1.f } }, { { "Framebuffer", "depthStencil.stencil" }, { none, 255.f } },
                // How many times, and which of them.
                { { "Repeat", "count" }, { 1.f, 256.f } }, { { "ForEach", "repeat" }, { 1.f, 256.f } },
                { { "Element", "index" }, { none, any } }, { { "Switch", "index" }, { none, any } }, { { "CubeFace", "face" }, { none, 5.f } },
                { { "PointShadows", "most" }, { 1.f, any } }, { { "PointShadows", "each" }, { none, any } }, { { "PointShadows", "near" }, { 0.001f, any, log } },
                // What is drawn and run: none or more of it; a group, of one or more.
                { { "Draw", "vertexCount" }, { none, any, log } }, { { "Draw", "firstVertex" }, { none, any } }, { { "Draw", "vertexOffset" }, { -any, any } }, { { "Draw", "instances" }, { none, any, log } },
                { { "Draw", "firstInstance" }, { none, any } }, { { "Draw", "tasks" }, { none, any, log } }, { { "Draw", "indirectOffset" }, { none, any } },
                { { "Draw", "drawCount" }, { none, any, log } }, { { "Draw", "stride" }, { none, any } }, { { "Draw", "countOffset" }, { none, any } },
                { { "Draw", "clearDepth" }, { none, 1.f } }, { { "Draw", "clearStencil" }, { none, 255.f } }, { { "Draw", "lineWidth" }, { none, 64.f } },
                { { "Draw", "stencilReference" }, { none, 255.f } }, { { "Draw", "stencilCompareMask" }, { none, 255.f } },
                { { "Draw", "stencilWriteMask" }, { none, 255.f } }, { { "Draw", "cullIndex" }, { none, any } }, { { "Draw", "scissor" }, { none, any } },
                { { "Dispatch", "groups" }, { none, any, log } }, { { "Dispatch", "groupSize" }, { 1.f, 1024.f } }, { { "Dispatch", "indirectOffset" }, { none, any } },
                { { "TraceRays", "size" }, { 1.f, any, log } },
                // Copies: from somewhere inside, something big, of a level and a layer there are.
                { { "Blit", "sourceOffset" }, { none, any } }, { { "Blit", "destinationOffset" }, { none, any } },
                { { "Blit", "sourceExtent" }, { 1.f, any, log } }, { { "Blit", "destinationExtent" }, { 1.f, any, log } },
                { { "Blit", "sourceMip" }, { none, 15.f } }, { { "Blit", "destinationMip" }, { none, 15.f } },
                { { "Blit", "sourceLayer" }, { none, 2047.f } }, { { "Blit", "destinationLayer" }, { none, 2047.f } }, { { "Blit", "layers" }, { 1.f, 2048.f } },
                { { "Resolve", "sourceOffset" }, { none, any } }, { { "Resolve", "destinationOffset" }, { none, any } },
                { { "Resolve", "sourceExtent" }, { 1.f, any, log } }, { { "Resolve", "destinationExtent" }, { 1.f, any, log } },
                { { "Resolve", "sourceMip" }, { none, 15.f } }, { { "Resolve", "destinationMip" }, { none, 15.f } },
                { { "Resolve", "sourceLayer" }, { none, 2047.f } }, { { "Resolve", "destinationLayer" }, { none, 2047.f } }, { { "Resolve", "layers" }, { 1.f, 2048.f } },
                { { "CopyBuffer", "bytes" }, { none, any, log } }, { { "CopyBuffer", "sourceOffset" }, { none, any } }, { { "CopyBuffer", "destinationOffset" }, { none, any } },
                { { "ClearBuffer", "offset" }, { none, any } }, { { "ClearBuffer", "bytes" }, { none, any, log } },
                { { "CopyBufferToImage", "bufferOffset" }, { none, any } }, { { "CopyBufferToImage", "bufferRowLength" }, { none, any } },
                { { "CopyBufferToImage", "bufferImageHeight" }, { none, any } }, { { "CopyBufferToImage", "imageOffset" }, { none, any } },
                { { "CopyBufferToImage", "imageExtent" }, { 1.f, any, log } }, { { "CopyBufferToImage", "mip" }, { none, 15.f } },
                { { "CopyBufferToImage", "layer" }, { none, 2047.f } }, { { "CopyBufferToImage", "layers" }, { 1.f, 2048.f } },
                { { "CopyImageToBuffer", "bufferOffset" }, { none, any } }, { { "CopyImageToBuffer", "bufferRowLength" }, { none, any } },
                { { "CopyImageToBuffer", "bufferImageHeight" }, { none, any } }, { { "CopyImageToBuffer", "imageOffset" }, { none, any } },
                { { "CopyImageToBuffer", "imageExtent" }, { 1.f, any, log } }, { { "CopyImageToBuffer", "mip" }, { none, 15.f } },
                { { "CopyImageToBuffer", "layer" }, { none, 2047.f } }, { { "CopyImageToBuffer", "layers" }, { 1.f, 2048.f } },
                // Lenses: an angle of view that sees something, a plane in front of the eye, a picture some wide.
                { { "Perspective", "fieldOfView" }, { 1.f, 179.f } }, { { "Perspective", "aspect" }, { 0.01f, 100.f, log } },
                { { "Perspective", "near" }, { 0.0001f, any, log } }, { { "Perspective", "far" }, { 0.001f, any, log } },
                { { "Orthographic", "size" }, { 0.001f, any, log } }, { { "Orthographic", "aspect" }, { 0.01f, 100.f, log } },
                { { "Frustum", "near" }, { 0.0001f, any, log } }, { { "Frustum", "far" }, { 0.001f, any, log } },
                { { "Paraboloid", "near" }, { 0.0001f, any, log } }, { { "Paraboloid", "far" }, { 0.001f, any, log } },
                { { "ShadowCascades", "resolution" }, { 1.f, 16384.f, log } }, { { "ShadowCascades", "distance" }, { 0.01f, any, log } },
                { { "ShadowCascades", "lambda" }, { none, 1.f } },
                { { "Refract", "eta" }, { none, any } },
                // Numbers of a kind: one unsigned is none or more.
                { { "UInt", "value" }, { none, any } },
            };
            if (anywhere.contains(whole)) port.integer = true;
            if (const auto it = known.find({ type, whole }); it != known.end()) {
                if (whole_.contains(whole) && type != "Orthographic") port.integer = true;
                port.min = it->second.min;
                port.max = it->second.max;
                port.logarithmic = it->second.logarithmic;
            }
        }

        /**
         * Where each input of a type is in an inspector, and when it does anything: a Draw's in what it is about — what
         * is drawn, from what buffer, into what, how — a Sampler's likewise; a builder's struct a part of its own, and
         * what several inputs share the start of the name of (`color[].view`, `.load`, `.store`), a part of their own.
         */
        /**
         * How abstract a type is: by its category, but for those whose category says what they make rather than what
         * they are about — a pack of the scene's meshes is a resource, and about the scene.
         */
        /**
         * Exactly what each of a type's values is, where that is said: a number node's and a vector's, its parts' and what it
         * gives; a builder's argument of a number's kind. Not what takes any kind, nor a whole of parts.
         */
        void Shaped(NodeType& type)
        {
            const auto set = [](Port& port, const Shape shape) { if (!port.shape.Known()) port.shape = shape; };
            for (auto& port : type.inputs)
                if (port.kind == PortKind::eValue && port.builder) {
                    using kor::builders::Kind;
                    switch (port.builder->kind) {
                        case Kind::eBool: set(port, { Scalar::eBool, 1 }); break;
                        case Kind::eInt: set(port, { Scalar::eInt, 1 }); break;
                        case Kind::eUInt: set(port, { Scalar::eUInt, 1 }); break;
                        case Kind::eFloat: set(port, { port.builder->bits == 64 ? Scalar::eDouble : Scalar::eFloat, 1 }); break;
                        default: break;
                    }
                }
            const Shape whole = ShapeNamed(type.name);
            if (whole.Known() && type.category == "Values") {
                // Its parts, of its kind; a number node's value is the number.
                for (auto& port : type.inputs)
                    if (port.kind == PortKind::eValue && port.type == "number") set(port, { whole.scalar, 1 });
                for (auto& port : type.outputs)
                    if (port.name == "out") set(port, whole);
            }
            if (type.name == "Bool") {
                for (auto& port : type.inputs) set(port, { Scalar::eBool, 1 });
                for (auto& port : type.outputs) set(port, { Scalar::eBool, 1 });
            }
            if (type.name == "Color") {
                for (auto& port : type.inputs) set(port, { Scalar::eFloat, 4 });
                for (auto& port : type.outputs) set(port, { Scalar::eFloat, 4 });
            }
        }

        Level LevelOf(const NodeType& type)
        {
            static const std::set<std::string_view> scene { "MeshPack", "IndirectDraws", "Meshlets", "RayScene", "ShaderBuffer", "BufferBuilder", "ComponentBuffer" };
            if (scene.contains(type.name) || type.category == "Scene") return Level::eScene;
            if (type.category == "Functions") return type.name == "Function" ? Level::eTechnique : Level::eValue;   // its inputs and outputs are a function's ends
            if (type.category == "Passes") return Level::eCommand;
            if (type.category == "Resources" || type.category == "Pipelines") return type.name == "Reference" ? Level::eValue : Level::eResource;
            return Level::eValue;      // Values, Math, Vectors, Logic, Cameras, Flow
        }

        void Group(NodeType& type)
        {
            using V = Value;
            const auto set = [&](const std::string_view name, const std::string_view group, Port::Condition when = {}) {
                for (auto& port : type.inputs)
                    if (port.name == name || (port.name.size() > name.size() + 1 && port.name.starts_with(name) && port.name[name.size()] == '.'
                                              && port.name.size() == name.size() + 2)) {
                        port.group = std::string(group);
                        if (!when.input.empty()) port.when = when;
                    }
            };
            if (type.name == "Draw") {
                const Port::Condition drawn { "command", { V("eAuto"), V("eDraw"), V("eDrawIndexed") }, "for draws of vertices" };
                const Port::Condition indexed { "command", { V("eAuto"), V("eDrawIndexed") }, "for indexed draws" };
                const Port::Condition tasks { "command", { V("eAuto"), V("eDrawMeshTasks") }, "for draws of mesh tasks" };
                const Port::Condition indirect { "command", { V("eDrawIndirect"), V("eDrawIndexedIndirect"), V("eDrawIndirectCount"), V("eDrawIndexedIndirectCount"),
                                                              V("eDrawMeshTasksIndirect") }, "for indirect draws" };
                const Port::Condition counted { "command", { V("eDrawIndirectCount"), V("eDrawIndexedIndirectCount") }, "for draws counted by a buffer" };
                const Port::Condition depth { "depthTest", { V(true) }, "while its depth test is on" };
                const Port::Condition stencil { "stencilTest", { V(true) }, "while its stencil test is on" };
                for (const char* name : { "batched", "compacted" }) set(name, "What is drawn");
                for (const char* name : { "vertexCount", "firstVertex", "instances", "firstInstance" }) set(name, "What is drawn", drawn);
                set("vertexOffset", "What is drawn", indexed);
                set("tasks", "What is drawn", tasks);
                for (const char* name : { "indirect", "indirectOffset", "drawCount", "stride" }) set(name, "Indirect", indirect);
                for (const char* name : { "count", "countOffset" }) set(name, "Indirect", counted);
                for (const char* name : { "load", "colorLoad", "depthLoad", "stencilLoad", "colorStore", "depthStore", "stencilStore", "clearColor", "clearDepth", "clearStencil" })
                    set(name, "Attachments");
                for (const char* name : { "viewport", "scissor" }) set(name, "Viewport");
                for (const char* name : { "cullMode", "frontFace", "depthBias", "lineWidth", "rasterizerDiscard", "primitiveRestart" }) set(name, "Rasterization");
                set("depthTest", "Depth");
                for (const char* name : { "depthWrite", "depthCompare" }) set(name, "Depth", depth);
                set("stencilTest", "Stencil");
                for (const char* name : { "stencilReference", "stencilCompareMask", "stencilWriteMask" }) set(name, "Stencil", stencil);
                set("blendConstants", "Blending");
                for (const char* name : { "cull", "cullIndex", "idle", "whenChanged" }) set(name, "Culling");
                return;
            }
            if (type.name == "Dispatch") {
                for (const char* name : { "groups", "cover", "groupSize" }) set(name, "How many");
                for (const char* name : { "indirect", "indirectOffset" }) set(name, "Indirect");
                set("entities", "How many");
                return;
            }
            if (type.name == "Sampler") {
                for (const char* name : { "minFilter", "magFilter", "mipmapMode", "anisotropyEnable" }) set(name, "Filtering");
                set("maxAnisotropy", "Filtering", { "anisotropyEnable", { V(true) }, "while anisotropy is on" });
                for (const char* name : { "addressModeU", "addressModeV", "addressModeW" }) set(name, "Addressing");
                for (const char* name : { "mipLodBias", "minLod", "maxLod" }) set(name, "Level of detail");
                set("compareEnable", "Comparison");
                set("compareOp", "Comparison", { "compareEnable", { V(true) }, "while it compares" });
                return;
            }
            if (!type.builder) return;
            // A struct, a part of its own; what several share the start of the name of, likewise.
            std::map<std::string, int> prefixes;
            const auto prefixOf = [](const std::string& name) {
                const auto dot = name.find('.');
                if (dot == std::string::npos) return std::string();
                const std::string rest = name.substr(dot + 1);
                return rest.size() == 1 && std::string_view("xyzw").find(rest[0]) != std::string_view::npos ? std::string() : name.substr(0, dot);
            };
            for (const auto& port : type.inputs) if (const auto prefix = prefixOf(port.name); !prefix.empty()) ++prefixes[prefix];
            for (auto& port : type.inputs) {
                if (!port.group.empty()) continue;
                if (port.builder && port.builder->kind == kor::builders::Kind::eStruct) port.group = port.name;
                else if (const auto prefix = prefixOf(port.name); !prefix.empty() && prefixes[prefix] >= 3) port.group = prefix;
            }
        }

        /** The inputs a node of a type cannot do without: the rest are there to be asked for. */
        bool Essential(const std::string_view type, const std::string_view input)
        {
            static const std::map<std::string_view, std::set<std::string_view>> essential {
                { "Buffer", { "instanceCount" } },
                { "Image", { "format", "extent.x", "extent.y" } },
                { "ImageView", { "image" } },
                { "BufferView", { "buffer" } },
                { "Sampler", {} },
                { "Shader", { "entryPoint.module", "entryPoint.entry" } },
                { "Framebuffer", { "color[].view" } },
                { "Mesh", { "vertexBuffer.buffer", "vertexLayout" } },
                { "AccelerationStructure", { "mesh[]" } },
                { "ComputePipeline", { "compute.module", "compute.entry" } },
                { "GraphicsPipeline", { "stages", "vertex.module", "vertex.entry", "mesh.module", "mesh.entry",
                                        "fragment.module", "fragment.entry", "framebuffer" } },
                { "RayTracingPipeline", { "raygen.module", "raygen.entry" } },
            };
            const auto it = essential.find(type);
            return it != essential.end() && it->second.contains(input);
        }

        /** What an input is to begin with: what a new node has for the ones it needs, and what the rest are until they are set. */
        Value Initial(const std::string_view type, const std::string_view input)
        {
            using L = Value::List;
            using M = Value::Members;
            static const std::map<std::string_view, std::map<std::string_view, Value>> initial {
                { "Image", {
                    { "format", "eRGBA8_UNORM" }, { "extent.x", 512 }, { "extent.y", 512 }, { "extent.z", 1 }, { "type", "e2D" },
                    { "mipLevels", 1 }, { "arrayLayers", 1 }, { "sampleCount", "e1" },
                    { "isPerFrame", false }, { "sharedAcrossQueues", false } } },
                { "Buffer", {
                    { "instanceCount", 256 }, { "type", "eDeviceLocal" }, { "isPerFrame", false }, { "sharedAcrossQueues", false } } },
                { "ImageView", {
                    { "viewType", "e2D" }, { "baseMipLevel", 0 }, { "mipLevelCount", 1 }, { "baseArrayLayer", 0 }, { "arrayLayerCount", 1 },
                    { "componentMapping", L { "eIdentity", "eIdentity", "eIdentity", "eIdentity" } } } },
                { "Sampler", {
                    { "minFilter", "eLinear" }, { "magFilter", "eLinear" }, { "mipmapMode", "eLinear" }, { "addressModeU", "eRepeat" },
                    { "addressModeV", "eRepeat" }, { "addressModeW", "eRepeat" }, { "mipLodBias", 0 }, { "anisotropyEnable", false }, { "maxAnisotropy", 1 },
                    { "compareEnable", false }, { "compareOp", "eLess" }, { "minLod", 0 }, { "maxLod", 16 }, { "unnormalizedCoordinates", false } } },
                { "BufferView", { { "format", "eRGBA8_UNORM" }, { "offset", 0 } } },
                { "Framebuffer", {
                    { "color[].name", "color" }, { "depthStencil.which", "Depth" }, { "depthStencil.name", "depth" }, { "depthStencil.depth", 1 },
                    { "depthStencil.stencil", 0 }, { "resolveMode", "eNone" },
                    { "color[].clear", M { { "scalarType", "Float" }, { "components", 4 }, { "value", M { { "f", L { 0, 0, 0, 1 } } } } } } } },
                { "Mesh", { { "vertexBuffer.binding", 0 }, { "indexBuffer.indexType", "eUInt" } } },
                { "GraphicsPipeline", {
                    { "stages", "eVertex" }, { "patchControlPoints", 3 },
                    { "inputAssemblyState", M { { "topology", "eTriangleList" }, { "primitiveRestartEnable", false } } },
                    { "rasterizationState", M { { "polygonMode", "eFill" }, { "cullMode", L {} }, { "frontFace", "eCounterClockwise" }, { "lineWidth", 1 } } },
                    { "multisampleState", M { { "sampleCount", "e1" }, { "sampleShadingEnable", false }, { "minSampleShading", 1 } } },
                    { "depthStencilState", M { { "depthTestEnable", true }, { "depthWriteEnable", true }, { "depthCompareOp", "eLess" }, { "minDepth", 0 }, { "maxDepth", 1 } } },
                    { "colorBlendState", M { { "enableLogicOp", false }, { "logicOp", "eCopy" }, { "attachments", L {} }, { "blendConstants", L { 0, 0, 0, 0 } } } } } },
                { "RayTracingPipeline", { { "maxRecursionDepth", 1 } } },
                { "Bool", { { "value", false } } }, { "String", { { "value", "" } } },
                { "Float", { { "value", 0 } } }, { "Double", { { "value", 0 } } }, { "Int", { { "value", 0 } } }, { "UInt", { { "value", 0 } } },
                { "Vec2", { { "x", 0 }, { "y", 0 } } }, { "Vec3", { { "x", 0 }, { "y", 0 }, { "z", 0 } } }, { "Vec4", { { "x", 0 }, { "y", 0 }, { "z", 0 }, { "w", 0 } } },
                { "IVec2", { { "x", 0 }, { "y", 0 } } }, { "IVec3", { { "x", 0 }, { "y", 0 }, { "z", 0 } } }, { "IVec4", { { "x", 0 }, { "y", 0 }, { "z", 0 }, { "w", 0 } } },
                { "UVec2", { { "x", 0 }, { "y", 0 } } }, { "UVec3", { { "x", 0 }, { "y", 0 }, { "z", 0 } } }, { "UVec4", { { "x", 0 }, { "y", 0 }, { "z", 0 }, { "w", 0 } } },
                { "Color", { { "color", L { 1, 1, 1, 1 } } } },
                // What is worked out starts as something it can work out: an Add of nothing is 0 + 0, shown on the node.
                { "Add", { { "a", 0 }, { "b", 0 } } }, { "Subtract", { { "a", 0 }, { "b", 0 } } }, { "Multiply", { { "a", 1 }, { "b", 1 } } },
                { "Divide", { { "a", 1 }, { "b", 1 } } }, { "Min", { { "a", 0 }, { "b", 0 } } }, { "Max", { { "a", 0 }, { "b", 0 } } }, { "Mod", { { "a", 0 }, { "b", 1 } } },
                { "Negate", { { "a", 0 } } }, { "Abs", { { "a", 0 } } }, { "Floor", { { "a", 0 } } }, { "Ceil", { { "a", 0 } } }, { "Round", { { "a", 0 } } },
                { "Fract", { { "a", 0 } } }, { "Sign", { { "a", 0 } } }, { "Saturate", { { "a", 0 } } }, { "Sqrt", { { "a", 1 } } }, { "Exp", { { "a", 0 } } },
                { "Log", { { "a", 1 } } }, { "Sin", { { "a", 0 } } }, { "Cos", { { "a", 0 } } }, { "Tan", { { "a", 0 } } }, { "Radians", { { "a", 0 } } }, { "Degrees", { { "a", 0 } } },
                { "Clamp", { { "x", 0 }, { "min", 0 }, { "max", 1 } } }, { "Lerp", { { "a", 0 }, { "b", 1 }, { "t", 0.5 } } },
                { "Smoothstep", { { "edge0", 0 }, { "edge1", 1 }, { "x", 0.5 } } }, { "Step", { { "edge", 0.5 }, { "x", 0 } } }, { "Pow", { { "a", 1 }, { "b", 2 } } },
                { "Atan2", { { "y", 0 }, { "x", 1 } } },
                { "Length", { { "a", L { 1, 0, 0 } } } }, { "Normalize", { { "a", L { 1, 0, 0 } } } }, { "Dot", { { "a", L { 1, 0, 0 } }, { "b", L { 1, 0, 0 } } } },
                { "Cross", { { "a", L { 1, 0, 0 } }, { "b", L { 0, 1, 0 } } } }, { "Distance", { { "a", L { 0, 0, 0 } }, { "b", L { 1, 0, 0 } } } },
                { "Append", { { "a", L { 0, 0, 0 } }, { "b", 1 } } }, { "Slice", { { "of", L { 0, 0, 0 } }, { "first", 0 }, { "count", 1 } } },
                { "Less", { { "a", 0 }, { "b", 0 } } }, { "LessOrEqual", { { "a", 0 }, { "b", 0 } } }, { "Greater", { { "a", 0 }, { "b", 0 } } },
                { "GreaterOrEqual", { { "a", 0 }, { "b", 0 } } }, { "Equal", { { "a", 0 }, { "b", 0 } } }, { "NotEqual", { { "a", 0 }, { "b", 0 } } },
                { "Not", { { "a", false } } }, { "And", { { "a", false }, { "b", false } } }, { "Or", { { "a", false }, { "b", false } } }, { "Xor", { { "a", false }, { "b", false } } },
                { "Select", { { "condition", true }, { "a", 0 }, { "b", 0 } } }, { "Split", { { "vector", L { 0, 0, 0, 0 } } } },
                { "Primitive", { { "shape", "eCube" } } },
                { "VertexLayout", { { "attributes", L { M { { "semantic", "POSITION" }, { "type", "eFloat" }, { "count", 3 } } } } } },
                { "Clear", { { "color", L { 0, 0, 0, 1 } } } },
                { "Dispatch", { { "groups.x", 1 }, { "groups.y", 1 }, { "groups.z", 1 },
                                { "with", L {} }, { "without", L {} } } },
                { "Draw", { { "command", "eAuto" }, { "instances", 1 }, { "load", false }, { "with", L {} }, { "without", L {} }, { "vertexCount", 3 }, { "firstVertex", 0 },
                            { "tasks.x", 1 }, { "tasks.y", 1 }, { "tasks.z", 1 }, { "drawCount", 1 }, { "clearColor", L { 0, 0, 0, 1 } }, { "clearDepth", 1 },
                            { "depthTest", true }, { "depthWrite", true }, { "lineWidth", 1 }, { "depthBias", L { 0, 0, 0 } }, { "blendConstants", L { 0, 0, 0, 0 } } } },
                { "TraceRays", { { "size.x", 1 }, { "size.y", 1 }, { "size.z", 1 }, { "with", L {} }, { "without", L {} } } },
                { "Blit", { { "filter", "eNearest" } } }, { "Resolve", {} },
            };
            const auto of = initial.find(type);
            if (of == initial.end()) return {};
            const auto it = of->second.find(input);
            return it == of->second.end() ? Value {} : it->second;
        }

        /** Other words for a node type: what someone who does not know its name types to find it. */
        std::string Keywords(const std::string_view type)
        {
            static const std::map<std::string_view, std::string_view> keywords {
                { "Image", "texture render target picture attachment" }, { "ImageView", "texture view mip layer slice" },
                { "Buffer", "data uniform storage array memory" }, { "BufferView", "texel buffer view" },
                { "Sampler", "filter texture sampling wrap" }, { "Framebuffer", "render target attachments output" },
                { "Mesh", "geometry vertices indices model" }, { "Primitive", "cube sphere plane cylinder cone capsule torus quad triangle square box ball mesh geometry shape default" },
                { "AccelerationStructure", "ray tracing bvh blas tlas" },
                { "ComputePipeline", "compute shader kernel gpu program" }, { "GraphicsPipeline", "vertex fragment mesh shader material render program" },
                { "RayTracingPipeline", "ray tracing shader raygen hit miss" },
                { "Screen", "window swapchain present output backbuffer" }, { "Reference", "alias link portal reroute" },
                { "Clear", "fill colour color wipe erase" }, { "Blit", "copy scale show present stretch resize" },
                { "Copy", "duplicate image transfer" }, { "CopyBuffer", "duplicate buffer transfer" }, { "Keep", "previous history last step frame before snapshot copy state ping pong" }, { "MeshBuffers", "mesh vertices indices triangles storage collider compute read" },
                { "GenerateMipmaps", "mip mipmap levels downsample" },
                { "Dispatch", "run compute execute kernel indirect" },
                { "Draw", "render rasterize mesh entities triangle fullscreen indirect instanced viewport scissor mesh tasks" },
                { "TraceRays", "ray tracing raygen rtx path trace" },
                                
                
                { "Entities", "all every world entity list each scene query" }, { "Entity", "one first single named camera sky sun component get find" }, { "Filter", "with without components tags keep query select entities meshes group groupby sort batch instances" },
                { "Selected", "picked selection outline entity id editor" }, { "FixedStep", "fixed step physics simulation time delta dt tick rate" }, { "Time", "time seconds elapsed clock delta dt animate" }, { "WriteBack", "read back readback entities transforms physics results gpu to cpu write" },
                { "ForEach", "each every all loop iterate list component lights ecs world query scene entities instances" },
                { "BufferBuilder", "ssbo struct storage structured uniform constant buffer layout fields instances array lights cameras pack" },
                { "ComponentBuffer", "ssbo struct storage structured buffer component fields entities lights cameras instances array pack" },
                { "ColorConvert", "colour color gamma srgb linear convert hsv hue saturation kelvin temperature luminance" },
                { "Any", "or reduce some components bool vector" }, { "All", "and reduce every components bool vector" },
                { "Jitter", "taa halton subpixel offset antialiasing temporal projection fsr dlss" }, { "LastFrame", "previous frame before history temporal value reprojection" },
                { "DeviceSupports", "feature capability mesh shader ray tracing fallback device gpu supports" },
                { "SceneBounds", "aabb box bounds extent size scene all entities min max radius" },
                { "WriteComponent", "set write field entity component value script" },
                { "DebugShapes", "debug draw lines box sphere frustum axes gizmo visualize cascades" },
                { "History", "previous last frame temporal taa feedback ping pong" },
                { "Readback", "read back gpu cpu value counter download async" },
                { "ComputePass", "compute shader kernel pipeline dispatch pass run" },
                { "RenderTarget", "image texture target render screen sized transient attachment" },
                { "QuatFromEuler", "quaternion rotation euler degrees" }, { "QuatFromAxisAngle", "quaternion rotation axis angle" },
                { "QuatToEuler", "quaternion rotation euler degrees" }, { "QuatToMatrix", "quaternion rotation matrix mat4" },
                { "QuatMultiply", "quaternion compose combine rotation" }, { "QuatInverse", "quaternion invert conjugate rotation" },
                { "QuatSlerp", "quaternion interpolate blend rotation spherical" }, { "QuatRotate", "quaternion rotate vector turn" },
                { "MeshPack", "pack merge combine batch meshes one mesh geometry indirect gpu driven bindless vertices indices" },
                { "Meshlets", "meshlet cluster mesh shader task shader amplification cull cone bounds nanite" },
                { "RayScene", "ray tracing raytracing tlas blas acceleration structure scene instances bvh rtx" },
                { "Function", "call subgraph group macro reuse function nodes as one" }, { "FunctionInput", "parameter argument port edge in" },
                { "FunctionOutput", "result return port edge out" },
                { "IndirectDraws", "indirect draw commands multi draw gpu driven culling batch instances drawcount args arguments" },
                { "View", "camera look at lookat eye target up view matrix" },
                { "Perspective", "camera projection lens field of view fov frustum reversed z infinite" },
                { "Orthographic", "ortho camera projection box parallel sun directional shadow 2d" },
                { "Frustum", "off-centre off-axis asymmetric projection stereo eye portal" },
                { "Oblique", "near plane clip mirror water reflection portal projection" },
                { "Paraboloid", "dual paraboloid hemisphere environment shadow point light projection" },
                { "CubeFace", "cube map face direction point light shadow environment probe six" },
                { "Collect", "gather list array all elements foreach values" },
                { "Bundle", "bundle struct record make pack group named values camera" }, { "Unbundle", "unbundle split unpack struct members fields" },
                { "ShadowCascades", "csm cascaded shadow map sun directional light matrices" },
                { "PointShadows", "cube map omnidirectional shadow point light lamp six faces matrices" },
                { "ImageFile", "texture picture png jpg hdr environment map load import asset" },
                { "Script", "fill init initial data cpu generate noise table lut bytes texels code" },
                { "Repeat", "loop for each times iterate count mip chain" }, { "Previous", "loop last turn before feedback chain" },
                { "Element", "loop index pick one turn result" }, { "Note", "comment text label remark annotate" }, { "Switch", "branch choose select if either option toggle" }, { "DebugView", "debug view visualize visualise buffer inspect scene view editor heat" },
                { "ShaderBuffer", "ssbo struct storage structured uniform constant buffer map fields entities array lights" },
                { "Append", "join concatenate vec4 combine" }, { "Slice", "part range sub list some numbers matrix of several cascade" }, { "MatrixMultiply", "mat4 transform compose" }, { "MatrixInverse", "mat4 invert" }, { "Pick", "select click id entity mouse read back" },
                { "DebugDraw", "gizmo lines helpers debug overlay" }, { "Resolve", "multisample msaa antialias samples" },
                { "ClearBuffer", "zero fill buffer reset" }, { "CopyBufferToImage", "upload texels buffer image transfer" },
                { "CopyImageToBuffer", "download read back texels image buffer transfer" },
                { "Bool", "boolean flag true false constant" }, { "String", "text name constant" },
                { "Color", "colour rgba picker constant" },
                { "Float", "number scalar constant value single real decimal f32" }, { "Double", "number scalar constant value real decimal f64 precise" },
                { "Int", "number integer whole constant value signed i32 count" }, { "UInt", "number integer whole unsigned constant value u32 count index" },
                { "Vec2", "vector float2 combine compose make uv size" }, { "Vec3", "vector float3 combine compose make position direction colour" },
                { "Vec4", "vector float4 combine compose make colour rgba plane" },
                { "IVec2", "vector int2 integer whole combine compose make offset" }, { "IVec3", "vector int3 integer whole combine compose make" },
                { "IVec4", "vector int4 integer whole combine compose make" },
                { "UVec2", "vector uint2 unsigned integer whole combine compose make size extent" }, { "UVec3", "vector uint3 unsigned integer whole combine compose make extent groups" },
                { "UVec4", "vector uint4 unsigned integer whole combine compose make" },
                { "Split", "break separate components xyz swizzle" },
                { "ImageExtent", "size width height resolution dimensions get" }, { "VertexLayout", "vertex format attributes input" },
                { "Add", "plus sum +" }, { "Subtract", "minus difference -" }, { "Multiply", "times product scale *" }, { "Divide", "over quotient /" },
                { "Mod", "modulo remainder wrap %" }, { "Pow", "power exponent" }, { "Sqrt", "square root" }, { "Lerp", "mix interpolate blend" },
                { "Clamp", "limit range" }, { "Saturate", "clamp 0 1" }, { "Smoothstep", "ease interpolate" }, { "Fract", "fractional part" },
                { "Length", "magnitude norm" }, { "Normalize", "unit direction" }, { "Dot", "inner product" }, { "Cross", "outer product perpendicular" },
                { "Select", "if choose branch ternary switch" }, { "Equal", "compare same ==" }, { "NotEqual", "compare different !=" },
                { "Less", "compare smaller <" }, { "Greater", "compare larger >" }, { "LessOrEqual", "compare <=" }, { "GreaterOrEqual", "compare >=" },
                { "And", "both logic &&" }, { "Or", "either logic ||" }, { "Not", "invert negate logic !" }, { "Xor", "either not both logic" },
                { "Radians", "degrees to radians angle convert" }, { "Degrees", "radians to degrees angle convert" },
                { "Atan2", "angle arctangent" }, { "Negate", "minus opposite" }, { "Abs", "absolute magnitude" },
                { "Asin", "arcsine angle inverse sine" }, { "Acos", "arccosine angle inverse cosine" }, { "Atan", "arctangent angle inverse tangent" },
                { "Sinh", "hyperbolic sine" }, { "Cosh", "hyperbolic cosine" }, { "Tanh", "hyperbolic tangent" }, { "Exp2", "power of two exponent" },
                { "Log2", "logarithm base 2" }, { "Log10", "logarithm base 10" }, { "InverseSqrt", "rsqrt reciprocal square root" },
                { "Trunc", "truncate integer part towards zero" }, { "Reciprocal", "one over inverse 1/x rcp" }, { "OneMinus", "invert complement 1-x" },
                { "InverseLerp", "unlerp inverse mix fraction between" }, { "Remap", "map range rescale fit convert" }, { "MultiplyAdd", "fma mad multiply add" },
                { "Reflect", "bounce mirror reflection vector" }, { "Refract", "refraction glass water snell vector" }, { "Project", "projection onto vector component along" },
                { "Constant", "pi tau e sqrt2 golden ratio phi number" }, { "MatrixTranspose", "mat4 transpose rows columns" },
                { "MatrixDeterminant", "mat4 determinant volume" }, { "MatrixTranslate", "mat4 translation move offset" },
                { "MatrixRotate", "mat4 rotation euler degrees turn" }, { "MatrixAxisAngle", "mat4 rotation axis angle turn" },
                { "MatrixScale", "mat4 scaling size" }, { "MatrixCompose", "mat4 trs transform position rotation scale model" },
                { "TransformPoint", "mat4 multiply point position project unproject" }, { "TransformPlane", "mat4 plane clip mirror water oblique" }, { "TransformDirection", "mat4 multiply direction vector normal" },
            };
            const auto it = keywords.find(type);
            return it == keywords.end() ? std::string() : std::string(it->second);
        }

        /** The icon of a node type: its own, or its kind's. A Material icon, by name. */
        std::string IconOf(const std::string_view type, const std::string_view category)
        {
            static const std::map<std::string_view, std::string_view> icons {
                // What is made.
                { "Image", "image" }, { "ImageView", "crop" }, { "Buffer", "storage" }, { "BufferView", "view_list" }, { "Sampler", "texture" },
                { "Framebuffer", "layers" }, { "Mesh", "category" }, { "Shader", "code" }, { "AccelerationStructure", "account_tree" },
                { "Primitive", "view_in_ar" }, { "ImageFile", "insert_photo" }, { "Script", "code" }, { "Screen", "monitor" }, { "Reference", "link" },
                { "VertexLayout", "reorder" }, { "ShaderBuffer", "table_chart" }, { "BufferBuilder", "view_module" }, { "RenderTarget", "wallpaper" }, { "ComponentBuffer", "dataset" }, { "MeshPack", "inventory_2" }, { "Meshlets", "grid_view" }, { "RayScene", "flare" }, { "Function", "functions" }, { "FunctionInput", "login" }, { "FunctionOutput", "logout" }, { "IndirectDraws", "format_list_numbered" },
                { "ComputePipeline", "memory" }, { "GraphicsPipeline", "brush" }, { "RayTracingPipeline", "flare" },
                // What is run.
                { "Clear", "format_color_reset" }, { "Blit", "open_in_full" }, { "Resolve", "blur_on" }, { "Copy", "content_copy" },
                { "CopyBuffer", "file_copy" }, { "Keep", "history" }, { "MeshBuffers", "data_array" }, { "ClearBuffer", "clear_all" }, { "CopyBufferToImage", "upload" }, { "CopyImageToBuffer", "download" },
                { "GenerateMipmaps", "photo_size_select_small" }, { "Dispatch", "play_arrow" }, { "TraceRays", "flare" }, { "Draw", "draw" },
                { "Pick", "touch_app" }, { "DebugDraw", "bug_report" }, { "History", "history_toggle_off" }, { "Readback", "download_for_offline" },
                { "ComputePass", "memory" }, { "DebugShapes", "pentagon" },
                // Values.
                { "Bool", "toggle_on" }, { "String", "text_fields" }, { "Color", "palette" },
                { "Float", "pin" }, { "Double", "pin" }, { "Int", "tag" }, { "UInt", "tag" },
                { "Vec2", "north_east" }, { "Vec3", "north_east" }, { "Vec4", "north_east" }, { "IVec2", "grid_4x4" }, { "IVec3", "grid_4x4" }, { "IVec4", "grid_4x4" },
                { "UVec2", "grid_4x4" }, { "UVec3", "grid_4x4" }, { "UVec4", "grid_4x4" },
                { "Constant", "tag" }, { "RandomDevice", "shuffle" }, { "ColorConvert", "palette" }, { "DeviceSupports", "developer_board" },
                { "Jitter", "grain" }, { "LastFrame", "skip_previous" }, { "Any", "rule" }, { "All", "rule" }, { "Split", "call_split" }, { "Append", "playlist_add" }, { "Slice", "content_cut" },
                { "ImageExtent", "aspect_ratio" }, { "Select", "alt_route" },
                // Cameras.
                { "View", "visibility" }, { "Perspective", "videocam" }, { "Orthographic", "crop_square" }, { "Frustum", "change_history" },
                { "Oblique", "flip" }, { "Paraboloid", "panorama_fish_eye" }, { "CubeFace", "view_in_ar" }, { "ShadowCascades", "wb_sunny" },
                { "PointShadows", "lightbulb" },
                // The world.
                { "Entities", "public" }, { "Filter", "filter_alt" }, { "ForEach", "loop" }, { "Entity", "place" }, 
                { "Selected", "near_me" }, { "FixedStep", "timer" }, { "Time", "schedule" }, { "WriteBack", "move_down" }, { "Collect", "collections" }, { "SceneBounds", "fit_screen" }, { "WriteComponent", "edit" }, { "Bundle", "inventory_2" }, { "Unbundle", "unarchive" },
                // What is done more than once, and one of several ways.
                { "Repeat", "repeat" }, { "Previous", "skip_previous" }, { "Element", "format_list_numbered" }, { "Switch", "call_split" }, { "DebugView", "visibility" },
                { "Note", "sticky_note_2" },
            };
            if (const auto it = icons.find(type); it != icons.end()) return std::string(it->second);
            if (type.starts_with("Matrix") || type.starts_with("Transform")) return "grid_on";
            if (type.starts_with("Random")) return "casino";
            if (type.starts_with("Quat")) return "3d_rotation";
            static const std::map<std::string_view, std::string_view> kinds {
                { "Math", "calculate" }, { "Vectors", "north_east" }, { "Logic", "rule" }, { "Cameras", "videocam" }, { "Values", "tune" },
                { "Passes", "play_arrow" }, { "Resources", "inventory_2" }, { "Pipelines", "memory" }, { "Scene", "public" }, { "Flow", "repeat" } };
            const auto kind = kinds.find(category);
            return kind == kinds.end() ? std::string("widgets") : std::string(kind->second);
        }

        std::vector<NodeType> Build()
        {
            std::vector<NodeType> types;

            // ---- one a builder ----------------------------------------------------------------------------------
            for (const auto& description : kor::builders::All()) {
                NodeType type;
                type.name = std::string(description.name);
                const bool pipeline = IsPipeline(description.name);
                type.category = pipeline ? "Pipelines" : "Resources";
                type.doc = std::string(description.doc);
                type.builder = &description;
                // A pipeline names its shaders itself: a Shader node still loads and runs, and is not offered.
                type.listed = description.name != "Shader";
                const auto add = [&](Port port, const std::string_view mode = {}) {
                    port.optional = !Essential(description.name, port.name);
                    port.mode = std::string(mode);
                    port.initial = Initial(description.name, port.name);
                    type.inputs.push_back(std::move(port));
                };
                if (description.name == "GraphicsPipeline") {
                    Port stages { "stages", PortKind::eValue, "string", false,
                                  "How it is drawn: eVertex, with a vertex shader and, when asked for, tessellation and geometry shaders; or eMesh, with a mesh shader and, when asked for, a task shader.", nullptr };
                    stages.choices = { "eVertex", "eMesh" };
                    add(std::move(stages));
                }
                if (description.name == "Image" || description.name == "Buffer") {
                    Port transient { "transient", PortKind::eValue, "bool", false,
                                     "Made by the frame graph for the frame, its memory shared with others of the frame graph's that are never needed at the same time: "
                                     "what it holds lasts from the pass that writes it to the last that uses it, this frame, and no further. "
                                     "A 2D image of one layer, or a buffer on the device.", nullptr };
                    add(std::move(transient));
                }
                for (const auto& field : description.constructor) add(PortOf(std::string(field.name), field, false, field.doc));
                for (const auto& setting : description.settings) {
                    // A pipeline's bindings and constants are the engine's to give, from what is wired: its
                    // `param.` and `constant.` inputs stand for them.
                    if (pipeline && HiddenPipelineSetting(setting.name)) continue;
                    const std::string_view mode = pipeline ? StageMode(setting.name) : std::string_view {};
                    for (const auto& argument : setting.arguments) {
                        if (argument.type->kind == Kind::eBytes) continue;   // initial data is not a graph's to hold
                        if (!pipeline) { add(PortOf(InputName(setting, argument), argument, setting.repeatable, setting.doc)); continue; }
                        const std::string name = PipelineInputName(setting, argument);
                        if (!IsShader(argument)) { add(PortOf(name, argument, setting.repeatable, setting.doc), mode); continue; }
                        // A shader, by where it is: the module it is in and the function of it that is the entry point.
                        Port module { name + ".module", PortKind::eValue, "string", setting.repeatable,
                                      "The shader module: a .slang file among the project's shaders, by its name without the ending.", nullptr };
                        module.editor = "module";
                        Port entry { name + ".entry", PortKind::eValue, "string", setting.repeatable,
                                     "The function of the module that is this stage's entry point.", nullptr };
                        entry.editor = "entry";
                        add(std::move(module), mode);
                        add(std::move(entry), mode);
                        if (description.name == "ComputePipeline") {
                            Port element { name + ".element", PortKind::eValue, "string", false,
                                           "What the elements of koral_compare_sort are: a shader's float or uint, or a struct of a module that is IComparable, as module.Type.", nullptr };
                            element.optional = true;
                            add(std::move(element), mode);
                        }
                    }
                }
                type.outputs.push_back(ResourcePort("out", std::string(description.result)));
                types.push_back(std::move(type));
            }

            // ---- values, and what is worked out from them ---------------------------------------------------------
            const auto optional = [](Port port) { port.optional = true; return port; };
            const auto hinted = [](Port port, std::string editor) { port.editor = std::move(editor); return port; };
            const auto chosen = [](Port port, std::vector<std::string> choices) { port.choices = std::move(choices); return port; };
            const auto value = [&](std::string name, std::vector<Port> inputs, std::string out, std::string doc) {
                types.push_back({ std::move(name), "Values", std::move(inputs), { { "out", PortKind::eValue, std::move(out), false, {}, nullptr } }, std::move(doc), nullptr });
            };
            value("Float", { Number("value") }, "number", "A number as a shader's float has it: 32 bits, about seven digits.");
            value("Double", { Number("value") }, "number", "A number as a double has it: 64 bits, as it is given.");
            value("Int", { Number("value") }, "number", "A whole number, as a 32-bit int has it: what is given rounded toward zero, kept within what an int holds.");
            value("UInt", { Number("value") }, "number", "A whole number of none or more, as a 32-bit uint has it: what is given rounded toward zero, kept within what a uint holds.");
            value("Bool", { { "value", PortKind::eValue, "bool", false, {}, nullptr } }, "bool", "True or false.");
            value("String", { { "value", PortKind::eValue, "string", false, {}, nullptr } }, "string", "Text.");
            // Vectors of a size and a kind, as a shader declares them: float2 to float4, int2 to int4, uint2 to uint4.
            for (const auto& [kind, what] : { std::pair { "Vec", "floats" }, { "IVec", "whole numbers, as ints" }, { "UVec", "whole numbers of none or more, as uints" } })
                for (int size = 2; size <= 4; ++size) {
                    std::vector<Port> components;
                    for (int i = 0; i < size; ++i) components.push_back(Number(std::string(1, "xyzw"[i])));
                    value(std::format("{}{}", kind, size), std::move(components), "vector", std::format("A vector of {} {}: x first.", size, what));
                }
            value("Color", { hinted({ "color", PortKind::eValue, "vector", false, "Red, green, blue, alpha.", nullptr }, "color") }, "vector",
                  "A colour, picked: four numbers, for a Clear or anything else that takes a vector.");
            const std::pair<const char*, const char*> two[] {
                { "Add", "a + b." }, { "Subtract", "a - b." }, { "Multiply", "a * b." }, { "Divide", "a / b." }, { "Min", "The lesser of a and b." },
                { "Max", "The greater of a and b." }, { "Pow", "a to the power b." }, { "Mod", "What is left of a after taking b from it as often as it goes: it wraps." } };
            for (const auto& [name, doc] : two)
                value(name, { Any("a"), Any("b") }, "value", std::string(doc) + " Of two numbers, or of two vectors a component at a time; a number goes with every component of a vector.");
            const std::pair<const char*, const char*> one[] {
                { "Negate", "-a." }, { "Abs", "a without its sign." }, { "Floor", "The whole number at or below a." }, { "Ceil", "The whole number at or above a." },
                { "Round", "The nearest whole number." }, { "Fract", "What is left of a after its whole part." }, { "Sign", "-1, 0 or 1, as a is below, at or above zero." },
                { "Saturate", "a, kept between 0 and 1." }, { "Sqrt", "The square root of a." }, { "Exp", "e to the power a." }, { "Log", "The natural logarithm of a." },
                { "Sin", "The sine of a, in radians." }, { "Cos", "The cosine of a, in radians." }, { "Tan", "The tangent of a, in radians." },
                { "Radians", "a, in degrees, as radians." }, { "Degrees", "a, in radians, as degrees." } };
            for (const auto& [name, doc] : one) value(name, { Any("a") }, "value", std::string(doc) + " Of a number, or of each component of a vector.");
            value("Clamp", { Any("x"), Any("min"), Any("max") }, "value", "x, kept between min and max.");
            value("Lerp", { Any("a"), Any("b"), Any("t") }, "value", "From a to b: a at t 0, b at t 1, and between in between.");
            value("Smoothstep", { Any("edge0"), Any("edge1"), Any("x") }, "value", "0 below edge0, 1 above edge1, and eased between.");
            value("Step", { Any("edge"), Any("x") }, "value", "0 where x is below edge, 1 where it is not.");
            value("Atan2", { Any("y"), Any("x") }, "value", "The angle, in radians, of the point (x, y).");
            value("Length", { Any("a") }, "number", "How long a vector is.");
            value("Normalize", { Any("a") }, "vector", "A vector, made one long.");
            value("Dot", { Any("a"), Any("b") }, "number", "The dot product of two vectors.");
            value("Cross", { Any("a"), Any("b") }, "vector", "The cross product of two vectors of 3.");
            value("Distance", { Any("a"), Any("b") }, "number", "How far apart two points are.");
            for (const char* name : { "Less", "LessOrEqual", "Greater", "GreaterOrEqual" })
                value(name, { Any("a"), Any("b") }, "value", "True or false: how a stands to b. Of two vectors, or a vector and a number, a component at a time: "
                                                              "true or false for each, which an Any or an All says as one, and a Select picks by.");
            value("Equal", { Any("a"), Any("b") }, "bool", "Whether a and b are the same.");
            value("NotEqual", { Any("a"), Any("b") }, "bool", "Whether a and b differ.");
            const Port flag { "a", PortKind::eValue, "bool", false, {}, nullptr };
            Port other = flag;
            other.name = "b";
            value("Not", { flag }, "bool", "True when a is false.");
            value("And", { flag, other }, "bool", "True when both are.");
            value("Or", { flag, other }, "bool", "True when either is.");
            value("Xor", { flag, other }, "bool", "True when one is and the other is not.");
            value("Any", { Any("a") }, "bool", "True when any of a is: of the components a comparison of vectors gives, one at least.");
            value("All", { Any("a") }, "bool", "True when all of a is: of the components a comparison of vectors gives, every one.");
            value("Select", { Any("condition"), Any("a"), Any("b") }, "value", "a when the condition is true, b when it is not. "
                                                                             "Of a condition for each component — a comparison of vectors — a component at a time.");
            value("Split", { { "vector", PortKind::eValue, "vector", false, {}, nullptr } }, "vector", "A vector's components, each an output of its own: x, y, z, w.");
            for (const char* part : { "x", "y", "z", "w" }) types.back().outputs.push_back({ part, PortKind::eValue, "number", false, {}, nullptr });
            value("ImageExtent", { ResourcePort("image", "Image") }, "vector", "An image's width, height and depth.");
            value("Append", { Any("a"), Any("b") }, "vector", "The numbers of a, then those of b: a colour and its alpha as one vector of four.");
            value("Slice", { Any("of"), Number("first"), Number("count") }, "vector", "count of the numbers of a list, from the first-th (0 the first): of a ShadowCascades' matrices, sixteen from 16 times a cascade's number is its matrix.");
            value("MatrixMultiply", { Any("a"), Any("b") }, "matrix", "a times b, as matrices of four by four: what b does, then what a does.");
            value("MatrixInverse", { Any("a") }, "matrix", "What undoes a matrix of four by four.");
            // More of what is worked out of numbers, a component at a time.
            const std::pair<const char*, const char*> more[] {
                { "Asin", "The angle, in radians, whose sine is a." }, { "Acos", "The angle, in radians, whose cosine is a." },
                { "Atan", "The angle, in radians, whose tangent is a." }, { "Sinh", "The hyperbolic sine of a." }, { "Cosh", "The hyperbolic cosine of a." },
                { "Tanh", "The hyperbolic tangent of a." }, { "Exp2", "2 to the power a." }, { "Log2", "The logarithm of a, base 2." },
                { "Log10", "The logarithm of a, base 10." }, { "InverseSqrt", "One over the square root of a." }, { "Trunc", "a without what is after its point: towards zero." },
                { "Reciprocal", "One over a." }, { "OneMinus", "1 - a." } };
            for (const auto& [name, doc] : more) value(name, { Any("a") }, "value", std::string(doc) + " Of a number, or of each component of a vector.");
            value("InverseLerp", { Any("a"), Any("b"), Any("value") }, "value", "How far value is from a to b: 0 at a, 1 at b. What undoes a Lerp.");
            value("Remap", { Any("value"), Any("fromMin"), Any("fromMax"), Any("toMin"), Any("toMax") }, "value",
                  "value, from between fromMin and fromMax to as far between toMin and toMax. Not kept between them: a Clamp after, for that.");
            value("MultiplyAdd", { Any("a"), Any("b"), Any("c") }, "value", "a * b + c.");
            value("Reflect", { Any("incident"), Any("normal") }, "vector", "The way incident goes after it bounces off a surface facing normal (one long).");
            value("Refract", { Any("incident"), Any("normal"), Number("eta") }, "vector",
                  "The way incident goes through a surface facing normal, both one long, eta the ratio of the two sides' indices: none, all zero, when it is all reflected.");
            value("Project", { Any("a"), Any("onto") }, "vector", "The part of a that is along onto.");
            value("Constant", { chosen({ "value", PortKind::eValue, "string", false, "ePi, eTau (two pi), eE, eSqrt2, ePhi (the golden ratio).", nullptr },
                                       { "ePi", "eTau", "eE", "eSqrt2", "ePhi" }) }, "number", "A number with a name.");
            types.back().inputs.front().initial = Value("ePi");
            {
                // ---- random numbers: a node for each distribution, of the kind of what it is wired to ---------------------
                std::vector<std::string> kinds { "Auto", "Float", "Double", "Int", "UInt", "Vec2", "Vec3", "Vec4", "IVec2", "IVec3", "IVec4", "UVec2", "UVec3", "UVec4" };
                for (const std::string_view name : random::DistributionNames()) {
                    const random::Distribution distribution = *random::DistributionNamed(name);
                    Port kind = chosen({ "type", PortKind::eValue, "string", false, "What kind of number it gives. Auto: what the input it is wired to takes "
                                         "(a shader's float4, an Int), and a Float when that says nothing. A vector's components are drawn each by "
                                         "itself; a whole kind's are whole: a uniform one each from min to max, any other rounded to the nearest.", nullptr },
                                       kinds);
                    kind.initial = Value("Auto");
                    Port seed = Number("seed", "Where the numbers start: the same seed gives the same numbers, on every machine. A RandomDevice's, for "
                                               "numbers no one chose.");
                    seed.initial = Value(0);
                    seed.integer = true;
                    seed.min = 0.f;
                    seed.shape = { Scalar::eUInt, 1 };
                    std::vector<Port> inputs { std::move(kind), std::move(seed) };
                    for (const auto& input : random::InputsOf(distribution)) {
                        Port port = Any(std::string(input.name), std::string(input.doc) + " A number, or a vector of one for each component.");
                        port.initial = Value(input.initial);
                        if (input.name == "trials") port.integer = true;
                        if (input.name == "trials" || input.name == "successes") port.min = 0.f;
                        inputs.push_back(std::move(port));
                    }
                    types.push_back({ std::string(random::NodeOf(distribution)), "Random", std::move(inputs),
                                      { { "out", PortKind::eValue, "value", false, "A number, or a vector of them, of the kind it gives.", nullptr } },
                                      std::string(random::DocOf(distribution)) + " Drawn once, when it is built: the same seed gives the same numbers, here and on any machine.",
                                      nullptr });
                    types.back().keywords = std::format("random rng noise seed chance distribution {}", random::KeywordsOf(distribution));
                }
                Port draw = Number("draw", "Changed to draw a new seed. It is kept while the graph runs otherwise, whatever else is edited.");
                draw.initial = Value(0);
                draw.integer = true;
                draw.optional = true;
                types.push_back({ "RandomDevice", "Random", { std::move(draw) },
                                  { { "out", PortKind::eValue, "number", false, "The seed: a UInt.", nullptr } },
                                  "A seed no one chose, as std::random_device draws one: for a Random's seed. A new one each time the graph is opened; kept while it runs.", nullptr });
                types.back().outputs.front().shape = { Scalar::eUInt, 1 };
                types.back().keywords = "random device seed entropy true hardware nondeterministic unpredictable";
            }
            value("MatrixTranspose", { Any("a") }, "matrix", "A matrix of four by four with its rows made its columns.");
            value("MatrixDeterminant", { Any("a") }, "number", "A matrix of four by four's determinant: how much it scales a volume by, negative when it mirrors.");
            value("MatrixTranslate", { Any("offset") }, "matrix", "A matrix that moves a point by offset.");
            value("MatrixRotate", { Any("rotation") }, "matrix", "A matrix that turns as an entity's rotation does: x, y and z in degrees.");
            value("MatrixAxisAngle", { Any("axis"), Number("angle") }, "matrix", "A matrix that turns angle degrees round axis.");
            value("MatrixScale", { Any("scale") }, "matrix", "A matrix that scales by a vector of three, or by one number every way.");
            value("MatrixCompose", { Any("position"), Any("rotation"), Any("scale") }, "matrix",
                  "Where something is, how it is turned (x, y and z in degrees) and how big, as one matrix: an entity's own, worked out as the world works it out.");
            value("TransformPoint", { Any("matrix"), Any("point") }, "vector", "A point a matrix has moved: divided by its w, so that a projection's is where it is in the picture.");
            value("TransformDirection", { Any("matrix"), Any("direction") }, "vector", "A direction a matrix has turned and scaled: not moved, as a point is.");
            value("TransformPlane", { Any("matrix"), Any("plane") }, "vector",
                  "A plane (a, b, c, d) where a matrix that moves points puts it: by the matrix's inverse, transposed. A plane of the world's, as a View sees it.");
            {
                const auto in = [&](std::string name, std::string type, std::string doc) {
                    Port port { std::move(name), PortKind::eValue, std::move(type), false, std::move(doc), nullptr };
                    port.optional = true;
                    return port;
                };
                const auto number = [&](std::string name, std::string doc, const double initial) {
                    Port port = in(std::move(name), "number", std::move(doc));
                    port.initial = Value(initial);
                    return port;
                };
                // ---- what a camera is: a view, from where it is towards where it looks, and a lens --------------------
                // None of them knows of a camera or a light: what they are given is numbers, from whatever holds them.
                Port main = in("main", "bool", "Whether it is the view the scene is seen through: in an editor, the editor's own view takes its place, and the "
                                               "editor's gizmos and debug shapes are seen as it sees.");
                main.initial = Value(false);
                Port up = in("up", "vector", "Which way is up, as it is seen: (0, 1, 0) when not given. What a ForEach's `up` is, for a camera that rolls as its entity does.");
                types.push_back({ "View", "Cameras",
                                  { in("position", "vector", "Where it is seen from: (0, 0, 0) when not given. A ForEach's `worldPosition`."),
                                    in("target", "vector", "A point it looks at: a step along -z from where it is, when not given. A ForEach's `worldPosition` added to its `forward`."),
                                    std::move(up), main },
                                  { { "out", PortKind::eValue, "matrix", false, "The view: the world, as seen from where it is.", nullptr },
                                    in("view", "matrix", "The view: the world, as seen from where it is."),
                                    in("inverseView", "matrix", "What undoes the view: where it is in the world, as a matrix."),
                                    in("position", "vector", "Where it is seen from."), in("forward", "vector", "The way it looks, one long."),
                                    in("up", "vector", "Up, as it is seen: square to the way it looks."), in("right", "vector", "Right, as it is seen.") },
                                  "A view matrix, as glm's lookAt makes one: from a position, towards a target, with up as up. The main one is the scene's: in an editor, "
                                  "where the editor's view is takes its place.", nullptr });
                // What each lens gives: its projection, and nothing of where it is seen from — a MatrixMultiply of it and a
                // View's matrix is the two as one. The main one is the scene's: what the editor's gizmos go by, and for a
                // Perspective, the editor's own lens in its place.
                const auto lens = [&](std::string name, std::vector<Port> inputs, std::string doc, std::vector<Port> outputs = {}, const bool flips = true) {
                    if (flips) {
                        Port lensMain = in("main", "bool", "Whether it is the lens the scene is seen through: what an editor's gizmos go by, and for a Perspective, "
                                                           "in an editor, the editor's own field of view, near and far in place of its own.");
                        lensMain.initial = Value(false);
                        inputs.push_back(std::move(lensMain));
                        Port flip = in("flipY", "bool", "Whether Y points down, as the device has it: true when not given. False for a projection as glm makes it.");
                        flip.initial = Value(true);
                        inputs.push_back(std::move(flip));
                    }
                    std::vector<Port> all {
                        { "out", PortKind::eValue, "matrix", false, "The projection.", nullptr },
                        in("projection", "matrix", "The projection: from what is seen to where it is in the picture, depth 0 at the near plane and 1 at the far one."),
                        in("inverseProjection", "matrix", "What undoes the projection: from the picture back to what is seen.") };
                    for (auto& port : outputs) all.push_back(std::move(port));
                    types.push_back({ std::move(name), "Cameras", std::move(inputs), std::move(all), std::move(doc), nullptr });
                };
                const auto aspect = [&] { return number("aspect", "Width over height: a Screen's `aspect`, for a picture the window's size.", 1); };
                lens("Perspective",
                     { number("fieldOfView", "Top to bottom, in degrees. A camera component's `fieldOfView`.", 60), aspect(),
                       number("near", "How near it starts.", 0.1), number("far", "How far it sees: 0 for no far plane, as far as there is.", 100),
                       in("reversedZ", "bool", "Depth 1 at the near plane and 0 at the far one: far finer depth far off, with a float depth image cleared to 0 and drawn "
                                               "with eGreater.") },
                     "What is seen through a lens: nearer is bigger. The main one, in an editor, has the editor's field of view, near and far in place of its own.",
                     { in("fieldOfView", "number", "Top to bottom, in degrees."), in("aspect", "number", {}), in("near", "number", {}), in("far", "number", {}) });
                lens("Orthographic",
                     { in("left", "number", "Where the box it sees starts, to the left: half its width, when not given."), in("right", "number", {}),
                       in("bottom", "number", {}), in("top", "number", {}),
                       number("size", "How high the box is, when left, right, bottom and top are not given: as wide as that times the aspect.", 10), aspect(),
                       number("near", "How near it starts: behind where it is seen from, when less than 0.", 0), number("far", "How far it sees.", 100) },
                     "What is seen in a box: as big however far. A sun's, a map's, a drawing's.",
                     { in("near", "number", {}), in("far", "number", {}) });
                lens("Frustum",
                     { number("left", "Where the near plane's rectangle starts, to the left.", -0.1), number("right", {}, 0.1), number("bottom", {}, -0.1), number("top", {}, 0.1),
                       number("near", "How near it starts: where that rectangle is.", 0.1), number("far", "How far it sees.", 100) },
                     "A perspective whose middle is off the middle of the picture, as glm's frustum makes: one eye's of two, a part of a bigger picture, a portal.",
                     { in("near", "number", {}), in("far", "number", {}) });
                lens("Oblique",
                     { in("projection", "matrix", "The projection whose near plane is moved: a Perspective's or an Orthographic's."),
                       in("plane", "vector", "The plane that is its near plane: its normal and its distance, (a, b, c, d), with ax + by + cz + d = 0 on it and what is seen "
                                             "where it is more, as it is seen: a TransformPlane of the world's plane by the view. Where it is seen from is to be where it is less.") },
                     "A projection whose near plane is a plane of the world's: what is behind a mirror or under water is left out, and depth is kept as it was.", {}, false);
                types.push_back({ "Paraboloid", "Cameras",
                                  { number("near", "How near it starts.", 0.1), number("far", "How far it sees.", 100),
                                    chosen(in("hemisphere", "string", "Which half of all round it: eFront, the way the view looks; eBack, the other."), { "eFront", "eBack" }) },
                                  { { "out", PortKind::eValue, "vector", false, "Its params.", nullptr },
                                    { "params", PortKind::eValue, "vector", false, "near, far, 1 over (far - near), and 1 for the front or -1 for the back: what a shader's paraboloid takes.", nullptr },
                                    { "turn", PortKind::eValue, "matrix", false, "What turns a view to face the half asked for — none for the front, half a turn for the back: a MatrixMultiply of it and a View's matrix.", nullptr },
                                    in("near", "number", {}), in("far", "number", {}) },
                                  "Half of all there is round a point, in one picture: two of them, front and back, are a dual-paraboloid map. A paraboloid is no matrix — the "
                                  "vertex shader bends what it draws, from a view and `params` — so this gives what that takes.", nullptr });
                types.back().inputs[2].initial = Value("eFront");
                types.push_back({ "CubeFace", "Cameras", { number("face", "Which face, from 0: towards +x, -x, +y, -y, +z, -z, as a cube map's layers are.", 0) },
                                  { { "direction", PortKind::eValue, "vector", false, "The way the face looks.", nullptr },
                                    { "up", PortKind::eValue, "vector", false, "Up, as the face is seen.", nullptr } },
                                  "One face of a cube round a point, as a cube map has it: a View's target is the point and the direction added, its up the face's up, "
                                  "and a Perspective of 90 degrees, square, sees the face.", nullptr });

                Port direction { "direction", PortKind::eValue, "vector", false, "The way the light travels: a directional light's entity's `forward`.", nullptr };
                types.push_back({ "ShadowCascades", "Cameras",
                                  { in("view", "matrix", "The view of the camera the cascades are fitted to: a View's."),
                                    in("projection", "matrix", "Its projection: a Perspective's. Its near and far planes are where the cascades start and end."),
                                    std::move(direction),
                                    in("resolution", "number", "How many pixels across one cascade's shadow map is: half the atlas's width, with four of them two by two."),
                                    in("distance", "number", "How far from the camera shadows are cast: 200 when not given, and never past the camera's far plane."),
                                    in("lambda", "number", "How the distance is divided among the four: 0 evenly, 1 each as many times farther as the last. 0.85 when not given."),
                                    in("boundsMin", "vector", "The least corner of the box everything that casts a shadow is in."),
                                    in("boundsMax", "vector", "Its greatest corner.") },
                                  { { "matrices", PortKind::eValue, "vector", false, "Four matrices, sixteen numbers each: what a position in the world is multiplied by to be where each cascade's map has it.", nullptr },
                                    in("splits", "vector", "How far from the camera each cascade reaches."),
                                    in("worldTexel", "vector", "How much of the world one pixel of each cascade's map covers."),
                                    in("tileTexel", "number", "One over the resolution."),
                                    in("direction", "vector", "The way the light travels, made one long.") },
                                  "A light's shadow maps, as values: four cascades fitted to what a camera sees, nearer ones smaller and so sharper. "
                                  "Each is drawn into a quarter of one depth image, from its matrix.", nullptr });
                Port lights { "lights", PortKind::eValue, "vector", false,
                              "The lights, four numbers each: where it is and how far it reaches — a Collect of an Append of a ForEach of PointLight's `worldPosition` and `field.range`.", nullptr };
                types.push_back({ "PointShadows", "Cameras",
                                  { std::move(lights),
                                    in("most", "number", "How many lights have a shadow map at the most: the first that many. 8 when not given."),
                                    in("near", "number", "How near a light its map starts: 0.05 when not given. It ends at the light's range."),
                                    in("each", "number", "How many lights' maps are drawn again each frame, a light after another: the rest stay as they were drawn last. Every one each frame, when 0 or not given. A light that moved, or reaches another distance, is drawn again at once.") },
                                  { { "matrices", PortKind::eValue, "vector", false, "Six matrices a light, sixteen numbers each — towards +x, -x, +y, -y, +z, -z: what a position in the world is multiplied by to be where that face of the light's cube has it. For a light there is none of, one nothing is inside.", nullptr },
                                    in("count", "number", "How many lights have a map: no more than `most`."),
                                    in("faces", "number", "Six times `most`: how many matrices there are, and how many layers an image of every face has."),
                                    in("idle", "vector", "A number a face: 0 for the faces that are drawn this frame, 1 for those left as they are — a Draw's `idle`.") },
                                  "The point lights' shadow maps, a cube each: six square views from where the light is, a quarter turn wide, as far as its range. Its matrices are a Draw's `cull`, a face at a time, and the shader's.", nullptr });
            }
            value("VertexLayout", { hinted({ "attributes", PortKind::eValue, "list", false,
                                      "What a vertex has, in order: { \"semantic\": \"POSITION\", \"type\": \"eFloat\", \"count\": 3 }, and optionally \"binding\", \"instanced\", \"namespace\".", nullptr }, "attributes") },
                  "struct KoralVertexLayout", "How a mesh's vertices are laid out, packed in the order given: what a pipeline's vertex.layout and a Mesh's vertexLayout take.");

            // ---- the screen, and a node by name -------------------------------------------------------------------
            {
                const auto given = [](std::string name, std::string type, std::string doc) {
                    Port port { std::move(name), PortKind::eValue, std::move(type), false, std::move(doc), nullptr };
                    port.optional = true;
                    return port;
                };
                types.push_back({ "Screen", "Resources", {},
                                  { ResourcePort("out", "Image"), given("extent", "vector", "How big the window is: its width, its height, and 1. An image sized by it follows the window."),
                                    given("size", "vector", "The window's width and height."), given("width", "number", "How many pixels across the window is."),
                                    given("height", "number", "How many pixels down."), given("aspect", "number", "Its width over its height: a lens's `aspect`."),
                                    given("format", "string", "What a pixel of it is: eBGRA8_SRGB, or whatever the window was given."),
                                    given("scale", "number", "How many of its pixels a point of the desktop is: 2 on a screen of twice the density. What a user interface is scaled by.") },
                                  "The scene's window: what a pass writes to show something.", nullptr });
// What a scene's own components hold, as values: read from the world every frame, to be worked on by
                // other nodes and packed into the buffers shaders read.
                const auto out = [&](std::string name, std::string type, std::string doc, const bool shown = false) {
                    Port port { std::move(name), PortKind::eValue, std::move(type), false, std::move(doc), nullptr };
                    port.optional = !shown;
                    return port;
                };
                const auto in = [&](std::string name, std::string type, std::string doc, std::string editor = {}) {
                    Port port { std::move(name), PortKind::eValue, std::move(type), false, std::move(doc), nullptr };
                    port.optional = true;
                    port.editor = std::move(editor);
                    return port;
                };
                types.push_back({ "Selected", "Scene", {},
                                  { out("entityId", "number", "Which entity is picked, from 1: 0 for none. What a ForEach's `entityId` is compared with.", true),
                                    out("present", "number", "1 when one is picked."),
                                    out("picking", "number", "1 on a frame a pick is asked of (Runtime::Pick): when what draws the ids it reads has to draw them.") },
                                  "The entity picked in an editor, as a number: what a pass that draws a line round it takes, once for every draw.", nullptr });
                {
                    Port of = ResourcePort("buffer", "Buffer", "What was worked out on the GPU, an element for each of the ForEach's: read back, a frame or two late.");
                    Port each { "forEach", PortKind::eValue, "string", false, "The ForEach whose elements the buffer's are, by its id: the first element is its first entity's.", nullptr };
                    Port stride { "stride", PortKind::eValue, "number", false, "How many bytes an element is.", nullptr };
                    Port position { "position", PortKind::eValue, "number", false, "Where in an element its position is, in bytes: three floats, where the entity is relative to what it is under.", nullptr };
                    Port rotation { "rotation", PortKind::eValue, "number", false, "Where in an element its rotation is, in bytes: a quaternion of four floats, x y z w.", nullptr };
                    Port fields { "fields", PortKind::eValue, "struct", false, "Fields of components it writes, by `Component.field`, each where it is in an element, in bytes: { \"Spin.velocity\": 32 }.", nullptr };
                    Port idle { "idle", PortKind::eValue, "number", false, "While not zero (or true), it writes nothing back: a simulation another runs, for now.", nullptr };
                    Port follows { "follows", PortKind::eValue, "number", false, "Where in an element a float says it follows its entity, in bytes: where it is not 0, the "
                                   "entity says where it is — a kinematic body a script moves — so it is not written back, nor taken as moved by something else.", nullptr };
                    Port velocity { "velocity", PortKind::eValue, "number", false, "Where in an element its velocity is, in bytes: three floats. Given, what is "
                                    "read back — of the steps before this frame's — is sent on by this frame's fixed steps, as it goes, so that it is drawn "
                                    "with what a script moved this frame.", nullptr };
                    Port spin { "spin", PortKind::eValue, "number", false, "Where in an element its spin is, in bytes: three floats, radians a second about "
                                "the world's axes. Given, it is turned on as `velocity` sends it on.", nullptr };
                    position.optional = rotation.optional = fields.optional = idle.optional = follows.optional = velocity.optional = spin.optional = true;
                    types.push_back({ "WriteBack", "Scene", { std::move(of), std::move(each), std::move(stride), std::move(position), std::move(rotation), std::move(fields), std::move(idle), std::move(follows),
                                                              std::move(velocity), std::move(spin) }, {},
                                      "Writes what the GPU worked out back into the entities it was worked out for — where each is, how it is turned, fields of its "
                                      "components — when what is read back changes: a simulation's bodies, moved. One moved by something else (the gizmo, a script) "
                                      "makes setup run again, from the world as it is.", nullptr });
                }
                types.push_back({ "Time", "Scene", {},
                                  { out("seconds", "number", "How long the graph has run, in seconds: what moves by itself, a cloud on the wind, goes by. It goes on whether the scene plays or not.", true),
                                    out("dt", "number", "How long the last frame took, in seconds: at most a quarter of one, so that a frame held up — a breakpoint, a window dragged — does not jump."),
                                    out("frame", "number", "Which frame this is, from 0: one more each frame the graph runs. What a Jitter takes.") },
                                  "The time, as numbers: how long the graph has run, and how long a frame takes. Frame by frame, not on the fixed step.", nullptr });
                types.push_back({ "FixedStep", "Scene", {},
                                  { out("dt", "number", "How long a fixed step is, in seconds: what a simulation on a fixed page moves by each time it runs.", true),
                                    out("steps", "number", "How many fixed steps this frame has in it: its fixed pages run when it is 1 or more, and rest when 0."),
                                    out("count", "number", "How many fixed steps there have been, this frame's among them: a seed that changes every frame with a step."),
                                    out("lost", "number", "How many steps frames had beyond the most a frame runs (the graph's fixedStepsAtMost): the simulation fell behind by so many."),
                                    out("alpha", "number", "How far this frame is from the last step to the next, 0 to 1: what a frame's drawing mixes the last two steps' states by, so that it moves smoothly between steps.") },
                                  "The fixed step, as numbers: what a pass on a fixed page goes by. Steps are taken while the scene plays.", nullptr });

                // Every entity that has a component, one after another, and a buffer a shader declares with an element
                // for each: the fields of the one wired to the fields of the other.
                {
                    Port component = hinted({ "component", PortKind::eValue, "string", false, "The component every one of them has: a light, a script's, a pipeline's.", nullptr }, "component");
                    Port with = hinted({ "with", PortKind::eValue, "list", false, "Components, by name, an entity must have as well. Their fields are given too, as `<Component>.<field>`.", nullptr }, "components");
                    Port without = hinted({ "without", PortKind::eValue, "list", false, "Components, by name, an entity must not have.", nullptr }, "components");
                    with.optional = without.optional = true;
                    Port where { "where", PortKind::eValue, "struct", false, "Fields an entity's must hold to be one of them, each by its output's name and what it holds: "
                                 "{ \"field.kind\": 2 } (a number, or true or false). Found again when the components they are of change.", nullptr };
                    where.optional = true;
                    Port repeat { "repeat", PortKind::eValue, "number", false, "How many elements each entity is, one after another: 6 for a point light's six faces. 1 when not given.", nullptr };
                    repeat.optional = true;
                    repeat.initial = Value(1);
                    Port named = hinted({ "entity", PortKind::eValue, "string", false, "Which entity, by its name: the first that has the component, when not given.", nullptr }, "entity");
                    named.optional = true;
                    const std::vector<Port> each {
                        out("index", "number", "Which element this is, from 0: there to be asked for, where the order matters."), out("count", "number", "How many elements there are: the entities, times `repeat`."),
                        out("turn", "number", "Which of the entity's `repeat` elements this is, from 0: a point light's face."),
                        out("entityIndex", "number", "Which of the entities this is, from 0."), out("entities", "number", "How many entities there are."),
                        out("entityId", "number", "Which entity it is, from 1: what a pipeline's `entityId` is given."),
                        out("matrix", "matrix", "Where it is in the world: its own transform under those of everything above it. A Draw's `push.model`."),
                        out("normalMatrix", "matrix", "The matrix's inverse, transposed: what turns a normal as the matrix turns a surface."),
                        out("previousMatrix", "matrix", "Where it was the frame before: the matrix it had then — its own, the first frame it is seen. "
                                                        "What a shader works out how it moved from, for motion vectors."),
                        out("worldPosition", "vector", "Where it is in the world."), out("forward", "vector", "The way it faces."),
                        out("up", "vector", "Its up: +y, turned as it is."), out("right", "vector", "Its right: +x, turned as it is."),
                        out("position", "vector", "Where it is, relative to what it is under."), out("rotation", "vector", "How it is turned, in degrees."),
                                    out("orientation", "vector", "How it is turned, as a quaternion: x, y, z, w — relative to what it is under, as a WriteBack's `rotation` is written."),
                        out("scale", "vector", {}),
                        out("present", "number", "1 when there is such an element, 0 when there is none."),
                        out("element", "bundle", "The element as one Bundle: its component's fields and `with`'s, by their names, and where it is (matrix, "
                                                 "worldPosition, forward...). Wired to a buffer's `fields`, it fills each field of the same name.") };
                    types.push_back({ "ForEach", "Scene", { component, with, without, where, repeat }, each,
                                      "Every entity that has a component, an element at a time: each field of it, its index, and where the entity is. "
                                      "Every node wired from it is worked out once an element, and a BufferBuilder or ShaderBuffer wired from those holds an element for each.", nullptr });
                    // One entity, not a loop: the first with a component, or the one named — its fields, and where it is.
                    std::vector<Port> one;
                    for (const auto& port : each)
                        if (port.name != "index" && port.name != "count" && port.name != "turn" && port.name != "entityIndex" && port.name != "entities") one.push_back(port);
                    one.back().doc = "1 when there is such an entity: whether it was found.";
                    Port first = component;
                    first.doc = "The component it has: a camera, a sky, a script's. Not given, it is the entity `entity` names, whatever it has: where it is.";
                    first.optional = true;
                    types.push_back({ "Entity", "Scene", { std::move(first), with, named }, one,
                                      "One entity: the first that has a component, or the one named — the camera, the sky, the sun. Each field of its component "
                                      "and of `with`'s, where it is and which it is. What is wired from it is that one's: it is no loop.", nullptr });
                    // Every entity, and those of them a filter keeps: a ForEach in steps, each wired to the next.
                    std::vector<Port> every = each;
                    every.insert(every.begin(), out("out", "entities", "Them: what a Filter takes, to keep some of.", true));
                    every.erase(std::ranges::find(every, "turn", &Port::name));
                    types.push_back({ "Entities", "Scene", {}, every,
                                      "Every entity in the world, an element at a time: where each is, and which. A Filter keeps those with the components it says; "
                                      "what is wired from either is worked out once an element, as from a ForEach.", nullptr });
                    Port of { "entities", PortKind::eValue, "entities", false, "The entities it keeps some of: an Entities node's, or another Filter's.", nullptr };
                    Port keep = with, drop = without;
                    keep.doc = "Components, by name, an entity must have to be kept. Their fields are given, as `<Component>.<field>`: `Mesh.mesh`.";
                    drop.doc = "Components, by name, an entity must not have.";
                    Port groupBy = hinted({ "groupBy", PortKind::eValue, "string", false, "One of `with`'s components, by name: those it keeps come in groups, "
                                            "each entity next to those whose component is the same as its own — the same mesh — each group where its first one was.", nullptr }, "component");
                    groupBy.optional = true;
                    Port only { "entityId", PortKind::eValue, "number", false, "One entity alone, by its id from 1 (a Selected node's `entityId`): only it is kept, "
                                "when it is among them; 0, none. All of them when not given.", nullptr };
                    only.optional = true;
                    std::vector<Port> kept = every;
                    kept.push_back(out("group", "number", "Grouped: which group this element is of, from 0. 0 when it is not grouped."));
                    kept.push_back(out("groups", "list", "Grouped: { first, count } for each group, in order — where its elements start, and how many. What an IndirectDraws takes."));
                    kept.push_back(out("groupCount", "number", "Grouped: how many groups there are."));
                    types.push_back({ "Filter", "Scene", { std::move(of), std::move(keep), std::move(drop), where, std::move(groupBy), std::move(only) }, kept,
                                      "The entities it is wired, those with every component of `with` and none of `without`: an element at a time, with the fields of "
                                      "`with`'s components and where each is. A Draw wired from it draws each. With `groupBy`, those of the same mesh are next to each other.", nullptr });

                    Port module { "module", PortKind::eValue, "string", false, "The shader module that declares the buffer: a .slang file among the project's shaders, by its name without the ending.", nullptr };
                    module.editor = "module";
                    Port entry { "entry", PortKind::eValue, "string", false, "An entry point of the module that uses the buffer.", nullptr };
                    entry.editor = "entry";
                    Port parameter { "parameter", PortKind::eValue, "string", false, "What the shader calls the buffer: the `lights` of `StructuredBuffer<Light> lights`.", nullptr };
                    Port after = ResourcePort("after", "Buffer", "Another ShaderBuffer of the same struct, whose elements come first in this one: several kinds of entity in one buffer.");
                    after.optional = true;
                    Port fields { "fields", PortKind::eValue, "bundle", false, "A Bundle — a ForEach's element, a Bundle node's — that fills each field of the same name. "
                                  "A field wired by itself takes its own wire; what has no field of its name is said on the node.", nullptr };
                    fields.optional = true;
                    types.push_back({ "ShaderBuffer", "Resources", { std::move(module), std::move(entry), std::move(parameter), std::move(after), fields },
                                      { ResourcePort("out", "Buffer"), out("count", "number", "How many elements it holds: what the shader loops to."),
                                        out("stride", "number", "The size of one element, in bytes.") },
                                      "A buffer a shader declares, laid out as the shader lays it out: an input for each field of its struct, by name. With fields wired from "
                                      "a ForEach it holds an element for each entity; with none, one element.", nullptr });

                    // A buffer of a component, laid out as it is: a ForEach of it and a BufferBuilder of its fields, as one node.
                    Port whose = component;
                    whose.doc = "The component every entity of it has: an element for each, its fields laid out as the component has them.";
                    Port ticked = hinted({ "fields", PortKind::eValue, "list", false, "Which of its fields it holds, by name, and which of where the entity is "
                                        "(worldPosition, forward, up, right, position, rotation, scale, orientation, matrix, normalMatrix, previousMatrix, entityId). "
                                        "Laid out in that order whatever order they are given in: the component's first, as it declares them. "
                                        "Not given: every field of it, and worldPosition, forward and matrix.", nullptr }, "componentFields");
                    ticked.optional = true;
                    Port laidOut { "usage", PortKind::eValue, "string", false, "eStorage: a StructuredBuffer, laid out as std430 has it. eUniform: a ConstantBuffer, laid out as std140 has it.", nullptr };
                    laidOut.optional = true;
                    laidOut.choices = { "eStorage", "eUniform" };
                    laidOut.initial = Value("eStorage");
                    Port most { "count", PortKind::eValue, "number", false, "How many elements it has room for: the first that many entities. One for each when not given.", nullptr };
                    most.optional = true;
                    Port before = ResourcePort("after", "Buffer", "Another buffer of the same layout, whose elements come first in this one: several kinds of entity in one buffer.");
                    before.optional = true;
                    types.push_back({ "ComponentBuffer", "Resources", { std::move(whose), std::move(ticked), with, without, where, std::move(laidOut), std::move(most), std::move(before) },
                                      { ResourcePort("out", "Buffer"), out("count", "number", "How many elements it holds: what the shader loops to.", true),
                                        out("stride", "number", "The size of one element, in bytes."), out("bytes", "number", "The size of the whole buffer, in bytes.") },
                                      "Every entity that has a component, as a buffer: an element for each, of the fields ticked. "
                                      "A field wired by itself, `field.<name>`, takes its wire instead of the entity's.", nullptr });
                }


                // A buffer whose element is laid out on the node: its fields, by name and type, and as many elements as it is told.
                {
                    Port layout = hinted({ "layout", PortKind::eValue, "list", false,
                                           "One element's fields, in order: { \"name\": \"position\", \"type\": \"eVec3\" }, and \"count\" for a fixed array of them. "
                                           "Laid out as the shader's struct of the same fields is.", nullptr }, "fields");
                    Port count { "count", PortKind::eValue, "number", false,
                                 "How many elements it has room for. Not given: one for each element of the ForEach its fields follow, or one. Given, with fields that follow a ForEach, the first that many; with none, every one the same.", nullptr };
                    count.optional = true;
                    Port rules { "usage", PortKind::eValue, "string", false,
                                 "eStorage: a StructuredBuffer, laid out as std430 has it. eUniform: a ConstantBuffer, laid out as std140 has it — every array element and struct on sixteen bytes.", nullptr };
                    rules.optional = true;
                    rules.choices = { "eStorage", "eUniform" };
                    rules.initial = Value("eStorage");
                    Port after = ResourcePort("after", "Buffer", "Another BufferBuilder of the same layout, whose elements come first in this one: several kinds of entity in one buffer.");
                    after.optional = true;
                    Port fields { "fields", PortKind::eValue, "bundle", false, "A Bundle — a ForEach's element, a Bundle node's — that fills each field of the "
                                  "layout of the same name. A field wired by itself takes its own wire; what matches nothing, on either side, is said on the node.", nullptr };
                    fields.optional = true;
                    types.push_back({ "BufferBuilder", "Resources", { std::move(layout), std::move(count), std::move(rules), std::move(after), std::move(fields) },
                                      { ResourcePort("out", "Buffer"), out("count", "number", "How many elements it holds: what the shader loops to.", true),
                                        out("stride", "number", "The size of one element, in bytes."), out("bytes", "number", "The size of the whole buffer, in bytes.") },
                                      "A buffer of elements laid out on the node: an input for each field, `field.<name>`. "
                                      "Wired from a ForEach, an element for each of its elements; from values of no ForEach, one.", nullptr });
                }
                        }
            {
                // An image sized by the window, or by another image: the Screen, the scale and the rounding written out for it.
                Port format { "format", PortKind::eValue, "string", false, "What a pixel of it is: eRGBA16_SFLOAT, eD32_SFLOAT...", nullptr };
                format.editor = "format";
                format.initial = Value("eRGBA16_SFLOAT");
                Port scale { "scale", PortKind::eValue, "value", false, "How big, of what it is sized by: 1 as big, 0.5 half as big each way. A vector for each way.", nullptr };
                scale.optional = true;
                scale.initial = Value(1);
                Port sizeOf = ResourcePort("sizeOf", "Image", "The image it is sized by: the window's, when not wired.");
                sizeOf.optional = true;
                Port extent { "extent", PortKind::eValue, "vector", false, "Its size in pixels, given: instead of being sized by something.", nullptr };
                extent.optional = true;
                Port mips = Number("mipLevels", "How many mip levels: 1 when not given; 0 for as many as there are down to a pixel.");
                mips.optional = true;
                Port layers = Number("arrayLayers", "How many layers: 1 when not given.");
                layers.optional = true;
                Port transient { "transient", PortKind::eValue, "bool", false,
                                 "Made by the frame graph for the frame, its memory shared with others of the frame graph's that are never needed at the same time, "
                                 "as an Image's: what it holds lasts this frame, from the pass that writes it to the last that uses it.", nullptr };
                transient.optional = true;
                types.push_back({ "RenderTarget", "Resources", { std::move(format), std::move(scale), std::move(sizeOf), std::move(extent), std::move(mips), std::move(layers), std::move(transient) },
                                  { ResourcePort("out", "Image"), { "extent", PortKind::eValue, "vector", false, "How big it is: width, height and 1.", nullptr } },
                                  "An image a pass draws into, sized by the window — or by another image — times a scale: made again when that changes size. "
                                  "Its usage is what it is used as, as an Image's is.", nullptr });
            }
            types.push_back({ "MeshBuffers", "Resources", { ResourcePort("mesh", "Mesh", "A mesh: a Primitive's, a default one, a model's, a script's.") },
                              { ResourcePort("vertices", "Buffer", "The vertex buffer its positions are in (eStorage): what a compute shader reads them from."),
                                ResourcePort("indices", "Buffer", "Its indices, 32 bits each (eStorage); none for a mesh that has none."),
                                Number("stride", "How many bytes a vertex is, in that buffer."), Number("position", "Where in a vertex its position is, in bytes: three floats."),
                                Number("vertexCount"), Number("indexCount"),
                                Number("firstIndex", "Of a model's mesh, which is a part of the one buffer its model is on the device: the first of its indices."),
                                Number("vertexOffset", "...and what they count from."),
                                ResourcePort("normals", "Buffer", "The vertex buffer its normals are in, when it has them."), Number("normalsStride"), Number("normalsOffset"),
                                ResourcePort("uvs", "Buffer", "The vertex buffer its first UVs are in, when it has them."), Number("uvsStride"), Number("uvsOffset") },
                              "A mesh's buffers, as buffers: for a compute shader to read its triangles — a mesh collider, a mesh to scatter on.", nullptr });
            types.push_back({ "Primitive", "Resources",
                              { chosen({ "shape", PortKind::eValue, "string", false,
                                         "One of the default meshes, each about a unit across and centred on the origin: eCube, eSphere, ePlane (facing up), eCylinder, eCone, "
                                         "eCapsule, eTorus, eQuad (facing +z), eTriangle; eIcosphere (even triangles), eGrid (the plane cut 64 by 64), eInsideCube and "
                                         "eInsideSphere (facing in: a sky), eRoundedBox, eWedge, ePyramid, eHemisphere, ePipe, eArch, eStairs, eTeapot, eKnot, eArrow (up +y), "
                                         "eAxes (from the origin) and eSkinnedCylinder (with BONE_IDS and BONE_WEIGHTS).", nullptr },
                                       [] {
                                           std::vector<std::string> shapes;
                                           for (const auto& name : DefaultMeshNames()) shapes.push_back("e" + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])))) + name.substr(1));
                                           return shapes;
                                       }()) },
                              { ResourcePort("out", "Mesh"), { "layout", PortKind::eValue, "struct KoralVertexLayout", false, "Its vertices: POSITION, NORMAL, the tangents the graph asks for (defaultMeshTangents) and UV. What a pipeline that draws it takes as vertex.layout.", nullptr } },
                              "A mesh of a simple shape, with positions, normals and texture coordinates. The same shapes are there for any entity's Mesh without a node, as the default meshes.", nullptr });
            // ---- many meshes drawn as one: packed, and drawn by the commands a buffer holds ------------------------------
            {
                Port meshes = ResourcePort("meshes", "Mesh", "Each element's mesh: a Filter's `Mesh.mesh`. The distinct ones are packed, in the order the Filter has them first — grouped by Mesh, a group's.");
                Port positions { "positions", PortKind::eValue, "bool", false, "Only where their vertices are: three floats each, whatever else each mesh has and however it lays them out "
                                 "— for a compute shader that reads triangles (a mesh collider), not for drawing. Read back and packed on the CPU when it is built.", nullptr };
                Port attributes { "attributes", PortKind::eValue, "list", false, "Only these of their vertices, by semantic (\"POSITION\", \"NORMAL\", \"UV\"): a buffer each, as the first mesh "
                                  "that has it has it, and zeros where a mesh has not — so that meshes laid out otherwise, of other files, are one. What the pipeline reads. "
                                  "Read back and packed on the CPU when it is built.", nullptr };
                attributes.optional = true;
                types.push_back({ "MeshPack", "Resources", { std::move(meshes), std::move(positions), std::move(attributes) },
                                  { ResourcePort("mesh", "Mesh", "All of them as one mesh: what an indirect Draw draws."),
                                    { "layout", PortKind::eValue, "struct KoralVertexLayout", false, "Its vertices, as the meshes have them: what a pipeline that draws it takes as vertex.layout.", nullptr },
                                    { "ranges", PortKind::eValue, "list", false, "Each mesh's part of it: { firstIndex, indexCount, vertexOffset, boundsMin, boundsMax }, in order. What an IndirectDraws takes.", nullptr },
                                    ResourcePort("rangeBuffer", "Buffer", "The same, for a shader: 48 bytes a mesh — uint firstIndex, indexCount; int vertexOffset; uint; float4 boundsMin; float4 boundsMax."),
                                    { "count", PortKind::eValue, "number", false, "How many meshes it holds.", nullptr } },
                                  "Many meshes as one: their vertices one after another in one buffer, and their indices in another, each mesh copied once on the device. "
                                  "Built again when the meshes are other ones. Their vertices must be laid out alike, unless only their positions are packed.", nullptr });
                // ---- the same meshes cut into meshlets, for task and mesh shaders ----------------------------------------
                {
                    Port packed = ResourcePort("mesh", "Mesh", "The meshes as one: a MeshPack's mesh. Its POSITION and its indices are read back.");
                    Port packedRanges { "ranges", PortKind::eValue, "list", false, "Each mesh's part of it: the MeshPack's ranges. Each mesh is cut apart from the rest.", nullptr };
                    Port mostVertices { "maxVertices", PortKind::eValue, "number", false, "The most vertices a meshlet has: what a mesh shader's group writes out.", nullptr };
                    mostVertices.optional = true;
                    mostVertices.initial = Value(64);
                    Port mostTriangles { "maxTriangles", PortKind::eValue, "number", false, "The most triangles a meshlet has.", nullptr };
                    mostTriangles.optional = true;
                    mostTriangles.initial = Value(124);
                    types.push_back({ "Meshlets", "Resources", { std::move(packed), std::move(packedRanges), std::move(mostVertices), std::move(mostTriangles) },
                                      { ResourcePort("meshlets", "Buffer", "48 bytes a meshlet: uint vertexOffset, vertexCount, triangleOffset, triangleCount; float4 sphere (its middle "
                                                     "and how far it reaches); float4 cone (the way most of its triangles face, and the cosine past which none faces the eye)."),
                                        ResourcePort("vertices", "Buffer", "A uint a vertex of a meshlet: which vertex of the mesh it is, its mesh's vertexOffset in it."),
                                        ResourcePort("triangles", "Buffer", "A uint a triangle of a meshlet: its three corners, among the meshlet's vertices, a byte each."),
                                        ResourcePort("meshRanges", "Buffer", "16 bytes a mesh, in the pack's order: uint firstMeshlet, meshletCount; and two left over."),
                                        { "count", PortKind::eValue, "number", false, "How many meshlets there are.", nullptr },
                                        { "most", PortKind::eValue, "number", false, "How many meshlets the mesh with the most has.", nullptr } },
                                      "The meshes of a MeshPack cut into meshlets: runs of their triangles of a few dozen vertices each, each with a sphere round it and a cone of "
                                      "the ways it faces — what a task shader culls and a mesh shader draws. Cut again when the pack is packed again.", nullptr });
                }
                // ---- the same entities as a scene rays are traced through ----------------------------------------------
                {
                    Port each = ResourcePort("meshes", "Mesh", "Each element's mesh: the same Filter's `Mesh.mesh` the MeshPack packs, grouped by Mesh. Its elements are the instances, "
                                             "in its order: the rows of the buffer of instances a Draw of the pack fills.");
                    Port packed = ResourcePort("mesh", "Mesh", "The meshes as one: the MeshPack's mesh. Each of its meshes is a bottom-level structure of its own.");
                    Port packedRanges { "ranges", PortKind::eValue, "list", false, "Each mesh's part of it: the MeshPack's ranges.", nullptr };
                    Port masked { "masked", PortKind::eValue, "string", false, "A component — a tag — whose entities are cut out by a picture's alpha: their triangles are not opaque, "
                                  "and an any-hit shader runs for each. The rest are opaque.", nullptr };
                    masked.optional = true;
                    Port unshadowing { "noShadow", PortKind::eValue, "string", false, "A component whose entities cast no shadow — a lamp's own ball: their instances' mask is 2, "
                                       "so that a ray traced with a mask of 1 (a shadow's, an occlusion's) passes through them. Every other's is 3.", nullptr };
                    unshadowing.optional = true;
                    types.push_back({ "RayScene", "Resources", { std::move(each), std::move(packed), std::move(packedRanges), std::move(masked), std::move(unshadowing) },
                                      { ResourcePort("structure", "AccelerationStructure", "The top-level structure: every instance, where its entity is. Made again when one moves "
                                                     "— what is wired to it is built again then; a shader that reads it by its bindless index from `scene` is not."),
                                        ResourcePort("scene", "Buffer", "16 bytes: uint the structure's bindless index (kor::accelerationStructure), uint how many instances; two left over. "
                                                     "Written again whenever the structure is made again. The index is ~0u while there is nothing to trace "
                                                     "through — no entity yet, or none left: a shader looks before it traces."),
                                        ResourcePort("meshOf", "Buffer", "A uint an instance, by its row (InstanceID()): which mesh of the pack it is — its range, its meshlets."),
                                        { "count", PortKind::eValue, "number", false, "How many instances there are.", nullptr } },
                                      "The entities of a Filter as a scene rays are traced through: a bottom-level structure for each mesh of their pack, and a top-level one placing "
                                      "an instance of it where each entity is — its InstanceID() its row among the Filter's elements. Made again when one moves, comes or goes.", nullptr });
                }
                Port ranges { "ranges", PortKind::eValue, "list", false, "Each mesh's part of the mesh drawn: a MeshPack's ranges.", nullptr };
                Port groups { "groups", PortKind::eValue, "list", false, "How many of each to draw, and from where: a Filter's groups, grouped by Mesh, so its groups and the pack's meshes are in one order.", nullptr };
                Port mode { "mode", PortKind::eValue, "string", false, "eCPU: each command draws its whole group. eGPU: each command starts at 0 instances every frame, for a compute pass to count the ones it draws, "
                            "and 'visible' to say which they are.", nullptr };
                mode.optional = true;
                mode.choices = { "eCPU", "eGPU" };
                mode.initial = Value("eCPU");
                types.push_back({ "IndirectDraws", "Resources", { std::move(ranges), std::move(groups), std::move(mode) },
                                  { ResourcePort("commands", "Buffer", "A VkDrawIndexedIndirectCommand for each mesh, 20 bytes each: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance. A Draw's `indirect`."),
                                    { "drawCount", PortKind::eValue, "number", false, "How many commands: a Draw's `drawCount`.", nullptr },
                                    { "stride", PortKind::eValue, "number", false, "The bytes from one command to the next: 20.", nullptr },
                                    ResourcePort("visible", "Buffer", "eGPU: room for a uint for every instance, for the compute pass to write which instance each drawn one is: a group's from its firstInstance on.") },
                                  "The commands an indirect Draw reads: one for each mesh of a MeshPack, drawing as many as its group has, from the group's first. "
                                  "A shader finds its instance at SV_StartInstanceLocation + SV_InstanceID.", nullptr });
            }
            // ---- functions: many nodes as one, called wherever they are wanted ------------------------------------------
            {
                Port function = hinted({ "function", PortKind::eValue, "string", false, "Which function it calls: one of the project's functions, by name.", nullptr }, "function");
                types.push_back({ "Function", "Functions", { std::move(function) }, {},
                                  "A call: the nodes of a function, as one. It takes what the function's inputs are and gives what its outputs are, "
                                  "and everything in it runs in the call's pass. Pressed twice, the function is opened, to be changed for every call.", nullptr });
                const auto edge = [&](const char* doc) {
                    Port name { "name", PortKind::eValue, "string", false, doc, nullptr };
                    return name;
                };
                Port kind { "kind", PortKind::eValue, "string", false, "What it carries: a value (eValue), entities a Filter keeps some of (eEntities), "
                            "or a resource — eMesh, eBuffer, eImage, or any one (eResource).", nullptr };
                kind.choices = { "eValue", "eEntities", "eMesh", "eBuffer", "eImage", "eResource" };
                kind.initial = Value("eValue");
                Port initial { "default", PortKind::eValue, "value", false, "What a call that is given nothing there takes: a value's.", nullptr };
                initial.optional = true;
                Port order { "order", PortKind::eValue, "number", false, "Where it is among the call's ports of its side, from 0: those that say none come after, as they are on the page, top to bottom.", nullptr };
                order.optional = true;
                Port given { "value", PortKind::eValue, "value", false, "What it is given, while the function is opened in place: what the call is given there.", nullptr };
                given.optional = true;
                types.push_back({ "FunctionInput", "Functions", { edge("The input's name: a port of every call, where its order says (or, saying none, as the inputs are on the page, top to bottom)."), kind, std::move(initial), given, order },
                                  { { "out", PortKind::eValue, "value", false, "What the call is given there.", nullptr } },
                                  "An edge of a function: what a call is given at one of its inputs, for the function's nodes to take.", nullptr });
                Port wired = given;
                wired.optional = false;
                wired.doc = "What it gives: wired from a node of the function.";
                types.push_back({ "FunctionOutput", "Functions", { edge("The output's name: a port of every call, where its order says (or, saying none, as the outputs are on the page, top to bottom)."), kind, wired, order },
                                  { { "out", PortKind::eValue, "value", false, "What it gives, for what is wired from it while the function is opened in place.", nullptr } },
                                  "The other edge of a function: what a call gives at one of its outputs, wired from inside it.", nullptr });
            }
            // ---- what is done more than once, and one of several ways -------------------------------------------------
            {
                const auto given = [](std::string name, std::string type, std::string doc) {
                    Port port { std::move(name), PortKind::eValue, std::move(type), false, std::move(doc), nullptr };
                    port.optional = true;
                    return port;
                };
                Port count { "count", PortKind::eValue, "number", false, "How many turns: a whole number, given here.", nullptr };
                count.initial = Value(4);
                types.push_back({ "Repeat", "Flow", { std::move(count) },
                                  { { "index", PortKind::eValue, "number", false, "The turn's number, from 0.", nullptr },
                                    given("reverse", "number", "The turn's number counted from the last: count - 1 on the first turn, 0 on the last."),
                                    given("count", "number", "How many turns there are."),
                                    given("first", "bool", "True on the first turn."), given("last", "bool", "True on the last turn.") },
                                  "A loop: every node wired from it, straight or through other nodes, is there once for each turn — a view of each level of an image, "
                                  "a pass for each — and what it takes from here is that turn's number.", nullptr });
                Port of = hinted({ "of", PortKind::eValue, "string", false, "A node of a loop, by its id.", nullptr }, "node");
                Port first = hinted({ "first", PortKind::eValue, "string", false, "The node it is on the first turn, when there is no turn before: by its id.", nullptr }, "node");
                first.optional = true;
                types.push_back({ "Previous", "Flow", { of, std::move(first) }, { ResourcePort("out", "Resource") },
                                  "What a node of a loop made the turn before: how each turn carries on from the last. On the first turn it is the node 'first' names.", nullptr });
                Port output { "output", PortKind::eValue, "string", false, "Of a function's call: which of its outputs. The first, when not given.", nullptr };
                output.optional = true;
                types.push_back({ "Element", "Flow", { of, given("index", "number", "Which turn, from 0: the last, when not given."), std::move(output) }, { ResourcePort("out", "Resource") },
                                  "What a node of a loop made on one turn: how what a loop made is used outside it, or by another loop. Of a function's call, what one of "
                                  "its outputs is.", nullptr });
                Port text { "text", PortKind::eValue, "string", false, "What it says.", nullptr };
                text.initial = Value("A note.");
                Port value { "value", PortKind::eValue, "value", false, "A value worked out once an element of a ForEach: a field of it, or what is made of one.", nullptr };
                types.push_back({ "Collect", "Flow", { std::move(value) },
                                  { { "out", PortKind::eValue, "vector", false, "The value of every element, one after another, as one list of numbers.", nullptr },
                                    given("count", "number", "How many elements there are.") },
                                  "Every element's value at once: what is worked out once an element of a ForEach, as one list — the lights' places, for a node that "
                                  "takes them all.", nullptr });
                Port names = hinted({ "names", PortKind::eValue, "list", false, "Its members' names, in order: an input of each, `field.<name>`, to wire or to give.", nullptr }, "names");
                names.initial = Value(Value::List {});
                Port base { "base", PortKind::eValue, "bundle", false, "Another Bundle it is made on: its members, with this one's own in place of those of the same name.", nullptr };
                base.optional = true;
                types.push_back({ "Bundle", "Flow", { std::move(names), std::move(base) },
                                  { { "out", PortKind::eValue, "bundle", false, "The Bundle: every member, by its name.", nullptr } },
                                  "Many values as one wire, each by its name: a camera's view, projection and position, for a buffer whose fields have those names. "
                                  "A buffer's `fields`, or a draw's `push`, takes it and fills what has the same name.", nullptr });
                Port taken { "of", PortKind::eValue, "bundle", false, "The Bundle to take apart: a Bundle node's, a ForEach's element, a function's.", nullptr };
                types.push_back({ "Unbundle", "Flow", { std::move(taken) }, {},
                                  "A Bundle taken apart: an output for each of its members, by its name.", nullptr });
                types.push_back({ "Note", "Flow", { std::move(text) }, {}, "Words on the graph, beside what they are about: why a pass is there, what a number means. It makes nothing.", nullptr });
                Port index { "index", PortKind::eValue, "number", false, "Which of the options, from 0. True is 1 and false 0.", nullptr };
                index.initial = Value(0);
                Port option { "option[]", PortKind::eResource, "Resource", true, "One of the things it can be: a resource, or a value.", nullptr };
                types.push_back({ "Switch", "Flow", { std::move(index), std::move(option) }, { ResourcePort("out", "Resource") },
                                  "One of several things wired to it, by number: a picture or another to show, a pass's way of working or another.", nullptr });
                Port viewName { "name", PortKind::eValue, "string", false, "What an editor's scene view lists it as: \"Cluster heat\". Not given, the node's id.", nullptr };
                viewName.optional = true;
                Port viewPicture { "picture", PortKind::eResource, "Image", false, "The picture shown in its place while it is chosen.", nullptr };
                types.push_back({ "DebugView", "Flow", { std::move(viewName), std::move(viewPicture) },
                                  { ResourcePort("out", "Image"), { "shown", PortKind::eValue, "bool", false, "True while an editor shows it: wired to the 'enabled' of the passes "
                                                                    "that make its picture, they run only then.", nullptr } },
                                  "A picture an editor's scene view can be switched to, in place of what the graph shows: the clusters' heat, a buffer of normals. "
                                  "It is chosen in that view alone — the game, and a project that is run, never show it.", nullptr });
            }
            types.push_back({ "ImageFile", "Resources",
                              { { "file", PortKind::eValue, "string", false, "The picture's file — png, jpg, hdr and the like: under the project's assets folder, or by its whole path.", nullptr } },
                              { ResourcePort("out", "Image") },
                              "A picture from a file, with its smaller levels made: a sky to look up, a pattern to draw with. An entity's own pictures need no node: its fields name their files.", nullptr });
            types.push_back({ "Script", "Resources", { hinted({ "script", PortKind::eValue, "string", false,
                                  "The class of one of the project's ResourceScripts: what makes the resources this node gives.", nullptr }, "script") },
                              {},
                              "One of the project's ResourceScripts: the Images, Buffers, views and Samplers it declares, each an output by its name, the "
                              "images and buffers filled on the CPU by its Init as they are made. Its fields are inputs, `field.<name>`; and what each "
                              "resource is made with, `<resource>.<input>` — its source's to begin with.", nullptr });
            types.push_back({ "Reference", "Resources", { hinted({ "node", PortKind::eValue, "string", false, "The id of the node it stands for.", nullptr }, "node") },
                              { ResourcePort("out", "Resource") },
                              "Another node's output, by the node's id: a resource made elsewhere in the graph, without a wire across it.", nullptr });

            // ---- passes -------------------------------------------------------------------------------------------
            const auto Bool = [](std::string name, std::string doc) { return Port { std::move(name), PortKind::eValue, "bool", false, std::move(doc), nullptr }; };
            const auto Choice = [&](std::string name, std::vector<std::string> choices, std::string doc) {
                return chosen({ std::move(name), PortKind::eValue, "string", false, std::move(doc), nullptr }, std::move(choices));
            };
            const auto Vector = [&](std::string name, std::string doc) { return hinted({ std::move(name), PortKind::eValue, "vector", false, std::move(doc), nullptr }, "numbers"); };
            // A box of an image, a number each way: given whole as well, like anything in parts.
            const auto box = [&](std::vector<Port>& ports, const std::string& name, const std::string& doc) {
                ports.push_back(optional(Number(name + ".x", doc)));
                ports.push_back(optional(Number(name + ".y")));
                ports.push_back(optional(Number(name + ".z")));
            };
            types.push_back({ "Clear", "Passes", { ResourcePort("image", "Image"), hinted({ "color", PortKind::eValue, "vector", false, "Red, green, blue, alpha. Of an image of whole "
                                                                                                    "numbers (UINT, SINT), each the whole number it is.", nullptr }, "color"),
                                                   optional(Number("depth", "Of a depth image, the depth: 1 when not given — 0 for reversed depth.")),
                                                   optional(Number("stencil", "Of one with a stencil, its value: 0 to 255.")) }, {},
                              "Fills an image with one colour — every mip and layer of it — or a depth image with one depth.", nullptr });
            {
                std::vector<Port> ports { ResourcePort("source", "Image"), ResourcePort("destination", "Image"),
                                          optional(Choice("filter", { "eNearest", "eLinear" }, "How it is scaled: eNearest takes the one nearest texel, eLinear blends its neighbours.")) };
                box(ports, "sourceOffset", "Where in the source the box copied from starts: its corner, when not given.");
                box(ports, "sourceExtent", "How big the box copied from is: the whole source, when not given.");
                box(ports, "destinationOffset", "Where in the destination the box copied to starts.");
                box(ports, "destinationExtent", "How big the box copied to is: the whole destination, when not given. Not the source's size, it scales.");
                for (const char* name : { "sourceMip", "destinationMip", "sourceLayer", "destinationLayer" }) ports.push_back(optional(Number(name, "Which mip level, or array layer, of the image: the first, when not given.")));
                ports.push_back(optional(Number("layers", "How many array layers: 1 when not given.")));
                types.push_back({ "Blit", "Passes", ports, {}, "Copies an image onto another, scaled to fit: onto the screen, to show it.", nullptr });
                ports.erase(ports.begin() + 2);   // a resolve does not scale
                types.push_back({ "Resolve", "Passes", std::move(ports), {},
                                  "Makes an ordinary image of a multisampled one: each pixel the average of its samples. Onto the screen as well.", nullptr });
            }
            types.push_back({ "Copy", "Passes", { ResourcePort("source", "Image"), ResourcePort("destination", "Image") }, {},
                              "Copies an image onto another of the same size, as it is.", nullptr });
            types.push_back({ "CopyBuffer", "Passes",
                              { ResourcePort("source", "Buffer"), ResourcePort("destination", "Buffer"),
                                optional(Number("bytes", "How many bytes: as many as the smaller of the two holds, when not given.")),
                                optional(Number("sourceOffset", "The byte of the source it starts at.")), optional(Number("destinationOffset", "The byte of the destination it starts at.")), optional(Number("idle", "While not zero (or true), it records nothing this frame: wired from a value — a count that is 0, a step that is not due — it runs on a condition.")) }, {},
                              "Copies a buffer's bytes into another: as many as the smaller of the two holds.", nullptr });
            types.push_back({ "Keep", "Passes", { ResourcePort("of", "Resource", "A buffer or an image (with eTransferSrc): what is kept a copy of."), optional(Number("idle", "While not zero (or true), it records nothing this frame: wired from a value — a count that is 0, a step that is not due — it runs on a condition.")) },
                              { ResourcePort("out", "Resource", "The copy: a buffer or an image like it, as it was when this pass ran."),
                                { "valid", PortKind::eValue, "bool", false, "False until it has been copied into once, and again after it is made anew: what a "
                                  "temporal pass blends nothing of while it is false.", nullptr } },
                              "A copy of a buffer or an image as it is when this pass runs, made where it is among the passes: what those after it "
                              "read as it was before, while they write the original — the last step's state, on a page of the fixed step.", nullptr });
            // The frame before: a copy of what it is given made when every pass of the frame has run, read this frame.
            types.push_back({ "History", "Passes", { ResourcePort("of", "Resource", "A buffer or an image (with eTransferSrc): what is had of the frame before.") },
                              { ResourcePort("out", "Resource", "It as it was at the end of the frame before."),
                                { "valid", PortKind::eValue, "bool", false, "False on the first frame, and after it is made anew (another size): when there is no frame before to read.", nullptr } },
                              "A buffer or an image as it was when the frame before was over: what a temporal pass — antialiasing, an exposure, a "
                              "simulation — reads of the last frame while it writes this one. A Keep copied when the frame begins, before every page.", nullptr });
            {
                std::vector<Port> ports { optional(ResourcePort("buffer", "Buffer", "The buffer read: with eTransferSrc.")),
                                          optional(ResourcePort("image", "Image", "Or an image: one texel of it.")),
                                          optional(Number("offset", "Of a buffer, the byte it starts at.")),
                                          optional(hinted({ "texel", PortKind::eValue, "vector", false, "Of an image, which texel: x, y, and z or a layer.", nullptr }, "numbers")),
                                          optional(Number("mip", "Of an image, which mip.")),
                                          optional(Choice("type", { "eFloat", "eInt", "eUInt", "eVec2", "eVec3", "eVec4", "eIVec2", "eIVec4", "eUVec2", "eUVec4" },
                                                          "Of a buffer, what is there: eFloat when not given. An image's is its format's.")) };
                ports[5].initial = Value("eFloat");
                types.push_back({ "Readback", "Passes", std::move(ports),
                                  { { "out", PortKind::eValue, "value", false, "What was there: a number or a vector, as it was a frame or two before.", nullptr },
                                    { "ready", PortKind::eValue, "bool", false, "Whether anything has come back yet: false the first frames.", nullptr } },
                                  "A few numbers of a buffer, or a texel of an image, back on the CPU as a value: a count the GPU worked out, the light of a "
                                  "picture for its exposure. Copied where this pass runs, and read when the copy is done — a frame or two late.", nullptr });
            }
            {
                Port module = hinted({ "module", PortKind::eValue, "string", false, "The shader module: a .slang file among the project's shaders, by its name without the ending.", nullptr }, "module");
                Port entry = hinted({ "entry", PortKind::eValue, "string", false, "The function of it that is run: its [numthreads] entry point.", nullptr }, "entry");
                std::vector<Port> ports { std::move(module), std::move(entry),
                                          optional(Number("groups.x", "How many groups to run, each way.")), optional(Number("groups.y")), optional(Number("groups.z")),
                                          optional(hinted({ "cover", PortKind::eValue, "string", false, "One of its image parameters, by name: as many groups as cover it. Instead of groups.", nullptr }, "parameter")),
                                          optional(ResourcePort("indirect", "Buffer", "A buffer that says how many groups: three numbers of 32 bits. Instead of groups.")),
                                          optional(Number("indirectOffset", "The byte of the indirect buffer the three numbers are at.")),
                                          optional(Number("idle", "While not zero (or true), it records nothing this frame.")) };
                types.push_back({ "ComputePass", "Passes", std::move(ports), {},
                                  "A compute shader run: a ComputePipeline and a Dispatch of it as one node. Its shader's parameters are inputs, `param.<name>`, and "
                                  "its push constants, `push.<name>`, and its specialization constants, `constant.<name>`, as they would be on the two.", nullptr });
            }
            types.push_back({ "ClearBuffer", "Passes",
                              { ResourcePort("buffer", "Buffer"), optional(Number("offset", "The byte it starts at.")), optional(Number("bytes", "How many bytes: to the buffer's end, when not given.")), optional(Number("idle", "While not zero (or true), it records nothing this frame: wired from a value — a count that is 0, a step that is not due — it runs on a condition.")) }, {},
                              "Fills a buffer, or a part of it, with zeroes.", nullptr });
            for (const bool toImage : { true, false }) {
                std::vector<Port> ports { ResourcePort(toImage ? "buffer" : "image", toImage ? "Buffer" : "Image"), ResourcePort(toImage ? "image" : "buffer", toImage ? "Image" : "Buffer"),
                                          optional(Number("bufferOffset", "The byte of the buffer the texels start at.")),
                                          optional(Number("bufferRowLength", "How many texels a row takes in the buffer, when rows have padding between them: the box's width, when not given.")),
                                          optional(Number("bufferImageHeight", "How many rows a slice takes in the buffer: the box's height, when not given.")) };
                box(ports, "imageOffset", "Where in the image the box starts.");
                box(ports, "imageExtent", "How big the box is: the whole image, when not given.");
                ports.push_back(optional(Number("mip", "Which mip level of the image.")));
                ports.push_back(optional(Number("layer", "Which array layer it starts at.")));
                ports.push_back(optional(Number("layers", "How many array layers: 1 when not given.")));
                types.push_back({ toImage ? "CopyBufferToImage" : "CopyImageToBuffer", "Passes", std::move(ports), {},
                                  toImage ? "Copies a buffer's bytes into an image, as its texels." : "Copies an image's texels into a buffer, as bytes.", nullptr });
            }
            types.push_back({ "GenerateMipmaps", "Passes", { ResourcePort("image", "Image") }, {},
                              "Fills an image's smaller mip levels from its largest.", nullptr });
            types.push_back({ "Pick", "Passes", { ResourcePort("ids", "Image", "An image of which entity is where: one a pipeline draws its `entityId` push constant into, as a number of 32 bits."),
                                          optional(ResourcePort("depth", "Image", "The depth drawn with the ids, read round the same pixel: where in the scene it is, and which way the surface there faces.")) }, {},
                              "Lets a click on the picture pick an entity: reads back the pixel asked about. It costs nothing in a frame nobody asks.", nullptr });
            types.push_back({ "DebugDraw", "Passes",
                              { ResourcePort("image", "Image", "The picture they are drawn over."),
                                optional(ResourcePort("depth", "Image", "A depth image to test them against: on top of everything, when not wired.")) }, {},
                              "Draws the scene's debug shapes over a picture, seen as the Camera node sees it: an editor's gizmo, a camera's reach, a light's.", nullptr });
            types.push_back({ "Dispatch", "Passes",
                              { ResourcePort("pipeline", "ComputePipeline"),
                                optional(Number("groups.x", "How many groups to run, each way: rounded up, so an image's extent divided by the pipeline's numThreads covers the image.")), optional(Number("groups.y")), optional(Number("groups.z")),
                                optional(hinted({ "cover", PortKind::eValue, "string", false, "One of the pipeline's image parameters, by name: as many groups as cover it — each element's own image, when it is given here from a ForEach. Instead of groups.", nullptr }, "parameter")),
                                optional(Number("groupSize.x", "How much of the covered image one group works on: the pipeline's numThreads, when not given.")), optional(Number("groupSize.y")), optional(Number("groupSize.z")),
                                optional(ResourcePort("indirect", "Buffer", "A buffer that says how many groups, three numbers of 32 bits, written by a pass before this one: instead of groups. It needs eIndirect among its usage.")),
                                optional(Number("indirectOffset", "The byte of the indirect buffer the three numbers are at.")), optional(Number("idle", "While not zero (or true), it records nothing this frame: wired from a value — a count that is 0, a step that is not due — it runs on a condition.")),
                                optional({ "entities", PortKind::eValue, "entities", false, "A Filter's entities: its groups run once for each of them, one after another along z, in one dispatch — "
                                           "element SV_GroupID.z / groups.z. What each is to the shader is its [entity(\"PER_INSTANCE\")] buffer's, in the Filter's order: where it is, "
                                           "its own fields, and the images it holds — read (TEXTURE) and written (STORAGE) — by their bindless indices.", nullptr }),
                                optional({ "push", PortKind::eValue, "bundle", false, "A Bundle that gives each `push.<name>` of the same name the pipeline leaves it: a ForEach's element gives a draw its `matrix`, its `entityId`. One given by itself takes its own.", nullptr }) }, {},
                              "Runs a compute pipeline. What its pipeline leaves unwired is given here, `param.<name>` and `push.<name>`: wired from a ForEach, it "
                              "runs once for each of its elements, and otherwise once.", nullptr });
            types.push_back({ "TraceRays", "Passes",
                              { ResourcePort("pipeline", "RayTracingPipeline"),
                                optional(Number("size.x", "How many rays, each way: one for each pixel of the image they are for.")), optional(Number("size.y")), optional(Number("size.z")),
                                optional(hinted({ "cover", PortKind::eValue, "string", false, "One of the pipeline's image parameters, by name: a ray for each of its pixels. Instead of size.", nullptr }, "parameter")), optional(Number("idle", "While not zero (or true), it records nothing this frame: wired from a value — a count that is 0, a step that is not due — it runs on a condition.")),
                                optional({ "push", PortKind::eValue, "bundle", false, "A Bundle that gives each `push.<name>` of the same name the pipeline leaves it: a ForEach's element gives a draw its `matrix`, its `entityId`. One given by itself takes its own.", nullptr }),
                                optional({ "entities", PortKind::eValue, "entities", false, "The entities its rays hit: a Filter's, the one a RayScene of them is of. A buffer the shader marks "
                                          "`[entity(\"PER_INSTANCE\")]` and nothing is wired to has an element for each, in the Filter's order — InstanceID() — filled from the pipeline's component: its material.", nullptr }) }, {},
                              "Runs a ray tracing pipeline. What its pipeline leaves unwired is given here, as a Dispatch's is: once, or once for each element of a ForEach.", nullptr });
            {
                const std::vector<std::string> loads { "eClear", "eLoad", "eDontCare" }, stores { "eStore", "eDontCare" };
                const std::vector<std::string> compares { "eNever", "eLess", "eEqual", "eLessOrEqual", "eGreater", "eNotEqual", "eGreaterOrEqual", "eAlways" };
                std::vector<Port> ports {
                    ResourcePort("pipeline", "GraphicsPipeline"),
                    optional(ResourcePort("framebuffer", "Framebuffer", "What it draws into: the pipeline's own, when not wired.")),
                    optional(ResourcePort("mesh", "Mesh", "What is drawn: a ForEach of Mesh's `field.mesh`, a mesh for each element — or one mesh, a Primitive's. None, for a vertex shader that makes its own vertices.")),
                    optional(Bool("batched", "True to draw every element of the Filter or ForEach its mesh is wired from in one indirect draw, rather than a draw each: their meshes packed "
                                             "as one, grouped by mesh. What each is to the shader is its [entity(\"PER_INSTANCE\")] buffer's, not push constants; one that is wired a "
                                             "value of each element otherwise is drawn one at a time, and its note says why. Given `cull` too, what is out of its sight is "
                                             "left out on the GPU.")),
                    optional(Bool("compacted", "True to have its shader read its buffer of instances as a pass before it leaves it — what is in sight, one after another as "
                                               "it is drawn (CullInSight's `drawn`) — rather than as it is filled: given as `drawnInstances`. A batched Draw given `cull` is.")),
                    optional(Choice("command", { "eAuto", "eDraw", "eDrawIndexed", "eDrawMeshTasks", "eDrawIndirect", "eDrawIndexedIndirect", "eDrawIndirectCount",
                                                 "eDrawIndexedIndirectCount", "eDrawMeshTasksIndirect" },
                                    "Which draw. eAuto: indexed when the mesh has indices, mesh tasks when the pipeline's stages is eMesh, and plain vertices otherwise. The Indirect ones read what to draw from the buffer wired to 'indirect'.")),
                    optional(Number("vertexCount", "How many vertices to draw, or indices when it is drawn indexed: all the mesh has, when not given. A pipeline whose vertex shader takes no vertex inputs needs no mesh, and then this is all there is to draw: 3 when not given, a triangle the shader makes from its SV_VertexID.")),
                    optional(Number("firstVertex", "The first vertex drawn, or the first index when it is drawn indexed.")),
                    optional(Number("vertexOffset", "Drawn indexed: what is added to each index before its vertex is read.")),
                    optional(Number("instances", "How many of each: 1 when not given.")),
                    optional(Number("firstInstance", "The number of the first instance, as the shader sees it.")),
                    optional(Number("tasks.x", "With mesh shaders: how many groups of the task (or mesh) shader to run, each way.")), optional(Number("tasks.y")), optional(Number("tasks.z")),
                    optional(ResourcePort("indirect", "Buffer", "An Indirect command's buffer of draws, written by a pass before this one. It needs eIndirect among its usage.")),
                    optional(Number("indirectOffset", "The byte of the indirect buffer the first draw is at.")),
                    optional(Number("drawCount", "How many draws the indirect buffer holds: 1 when not given. With a count buffer, the most it runs.")),
                    optional(Number("stride", "Bytes from one draw in the indirect buffer to the next: packed one after another, when not given.")),
                    optional(ResourcePort("count", "Buffer", "For the IndirectCount commands: a buffer holding how many draws to run, a number of 32 bits.")),
                    optional(Number("countOffset", "The byte of the count buffer the number is at.")),
                    optional(Bool("load", "True to draw over what the framebuffer holds; false, or not given, clears it first.")),
                    optional(Choice("colorLoad", loads, "What the colour attachments start as: cleared, as they were (eLoad), or anything at all (eDontCare, when every pixel is drawn).")),
                    optional(Choice("depthLoad", loads, "What the depth attachment starts as.")),
                    optional(Choice("stencilLoad", loads, "What the stencil attachment starts as.")),
                    optional(Choice("colorStore", stores, "Whether what was drawn is kept: eDontCare when nothing reads it after.")),
                    optional(Choice("depthStore", stores, "Whether the depth drawn is kept.")),
                    optional(Choice("stencilStore", stores, "Whether the stencil drawn is kept.")),
                    optional(hinted({ "clearColor", PortKind::eValue, "vector", false, "What the colour attachments are cleared to, in place of what the framebuffer says: red, green, blue, alpha.", nullptr }, "color")),
                    optional(Number("clearDepth", "What the depth attachment is cleared to, in place of what the framebuffer says.")),
                    optional(Number("clearStencil", "What the stencil attachment is cleared to.")),
                    optional(Vector("viewport", "The part of the framebuffer drawn into: x, y, width, height. All of it, when not given.")),
                    optional(Vector("scissor", "Nothing is drawn outside this box: x, y, width, height. The whole framebuffer, when not given.")),
                    optional(Choice("cullMode", { "eNone", "eFront", "eBack", "eFrontAndBack" }, "Which faces are not drawn, in place of what the pipeline says.")),
                    optional(Choice("frontFace", { "eCounterClockwise", "eClockwise" }, "Which way round a face's vertices go when it faces the viewer, in place of what the pipeline says.")),
                    optional(Bool("depthTest", "Whether what is behind what was drawn is left out, in place of what the pipeline says.")),
                    optional(Bool("depthWrite", "Whether it writes depth, in place of what the pipeline says.")),
                    optional(Choice("depthCompare", compares, "How a pixel's depth is compared with what is there, in place of what the pipeline says.")),
                    optional(Vector("depthBias", "Pushes what is drawn away or nearer: a constant, a clamp, and a factor of its slope. For shadow maps.")),
                    optional(Number("lineWidth", "How wide lines are drawn.")),
                    optional(hinted({ "blendConstants", PortKind::eValue, "vector", false, "The constant colour blending is done with, where the pipeline's blending uses one.", nullptr }, "color")),
                    optional(Bool("stencilTest", "Whether the stencil test is on, in place of what the pipeline says.")),
                    optional(Number("stencilReference", "What the stencil test compares with, and writes.")),
                    optional(Number("stencilCompareMask", "Which bits of the stencil are compared.")),
                    optional(Number("stencilWriteMask", "Which bits of the stencil are written.")),
                    optional(Bool("rasterizerDiscard", "True to draw nothing: only the vertex stages run.")),
                    optional(Bool("primitiveRestart", "Whether an index of all ones starts a new strip.")),
                    optional(hinted({ "cull", PortKind::eValue, "matrix", false, "Wired from what the picture is seen with — a Camera's viewProjection, a ShadowCascades' matrices: an entity whose mesh is wholly out of its sight is not drawn. Only a mesh whose box is known is left out: a default mesh, a model's.", nullptr }, "numbers")),
                    optional(Number("cullIndex", "Which of them, from 0, when 'cull' is wired from several matrices one after another.")),
                    optional(hinted({ "idle", PortKind::eValue, "vector", false, "Wired from numbers, one for each of what 'cullIndex' counts — a PointShadows' idle: while this pass's is not zero it draws nothing, and what it draws into stays as it was. For what need not be drawn again every frame.", nullptr }, "matrix")),
                    optional(Bool("whenChanged", "True to draw only when what it would draw is not what it drew last — the same entities' meshes, where they are, with the same "
                                                 "'cull' matrix, sets and push constants — leaving what it draws into as it was otherwise: a shadow map of what holds still. "
                                                 "What the buffers it reads hold is not looked at: a matrix it is seen by is to be its 'cull' too. Drawn again into what "
                                                 "another Draw started over this frame.")),
                    optional({ "push", PortKind::eValue, "bundle", false, "A Bundle that gives each `push.<name>` of the same name the pipeline leaves it: a ForEach's element gives a draw its `matrix`, its `entityId`. One given by itself takes its own.", nullptr }) };
                types.push_back({ "Draw", "Passes", std::move(ports), {},
                                  "Draws with a graphics pipeline into a framebuffer. What a draw has of its own is given here — its `mesh`, and what the pipeline leaves "
                                  "unwired as `param.<name>` and `push.<name>`, a model matrix among them: wired from a ForEach, a draw for each of its elements; "
                                  "otherwise, one draw.", nullptr });
            }
            // ---- colour, turns, the frame before, and what the device can do ------------------------------------------
            {
                const std::vector<std::string> spaces { "eSrgb", "eLinear", "eGamma22", "eHsv", "eKelvin" };
                Port color = hinted(Any("color", "A colour: red, green, blue and, if given, alpha, which is passed on as it is. Of eKelvin, a temperature: 6500 is daylight."), "color");
                Port from = chosen({ "from", PortKind::eValue, "string", false, "What the colour is: eSrgb as a display shows it (a picked colour), eLinear in the terms light adds "
                                     "up in, eGamma22 the display's curve as the power 2.2, eHsv hue (0 to 1), saturation and value of an sRGB colour, eKelvin a temperature.", nullptr }, spaces);
                from.initial = Value("eSrgb");
                Port to = chosen({ "to", PortKind::eValue, "string", false, "What it is made: any of from's but eKelvin.", nullptr }, { "eLinear", "eSrgb", "eGamma22", "eHsv" });
                to.initial = Value("eLinear");
                types.push_back({ "ColorConvert", "Values", { std::move(color), std::move(from), std::move(to) },
                                  { { "out", PortKind::eValue, "vector", false, "The colour, as `to` says.", nullptr },
                                    { "luminance", PortKind::eValue, "number", false, "How bright it is, in linear terms (Rec. 709): what an exposure is worked out of.", nullptr } },
                                  "A colour from one way of saying it to another: a picked colour into the linear terms a shader adds light up in — by the sRGB curve, "
                                  "or by the power 2.2 — or back; to and from hue, saturation and value; and a temperature as the colour it glows.", nullptr });

                // Turns as quaternions: x, y, z and w, as an entity's `orientation` is.
                const auto quaternion = [&](std::string name) { return Port { std::move(name), PortKind::eValue, "vector", false, "A turn as a quaternion: x, y, z, w.", nullptr }; };
                value("QuatFromEuler", { Any("rotation", "x, y and z in degrees: about X, then Y, then Z, as an entity's rotation is.") }, "vector", "A turn of three angles in degrees as a quaternion.");
                value("QuatFromAxisAngle", { Any("axis", "The axis it turns about."), Number("angle", "How far, in degrees.") }, "vector", "A turn about an axis as a quaternion.");
                value("QuatToEuler", { quaternion("q") }, "vector", "A quaternion as x, y and z in degrees: what an entity's rotation would be.");
                value("QuatToMatrix", { quaternion("q") }, "matrix", "A quaternion as the matrix that turns as it does.");
                value("QuatMultiply", { quaternion("a"), quaternion("b") }, "vector", "a after b: the turn b, then a.");
                value("QuatInverse", { quaternion("q") }, "vector", "The turn back.");
                value("QuatSlerp", { quaternion("a"), quaternion("b"), Number("t") }, "vector", "From a to b by the shortest way: a at t 0, b at t 1, and turning evenly between.");
                value("QuatRotate", { quaternion("q"), Any("vector", "A vector of three.") }, "vector", "A vector turned by the quaternion.");

                Port size { "size", PortKind::eValue, "vector", false, "How big, in pixels, the picture it is for is: a Screen's `size`.", nullptr };
                Port phases = Number("phases", "How many places within a pixel it goes through before it starts again: 8 when not given.");
                phases.optional = true;
                phases.initial = Value(8);
                Port projection { "projection", PortKind::eValue, "matrix", false, "A projection to move by it: a Perspective's. `jittered` is it moved.", nullptr };
                projection.optional = true;
                types.push_back({ "Jitter", "Cameras", { Number("frame", "Which frame: a Time's `frame`."), std::move(phases), std::move(size), std::move(projection) },
                                  { { "pixels", PortKind::eValue, "vector", false, "Where in the pixel this frame looks, from its middle: x and y, each -0.5 to 0.5.", nullptr },
                                    { "clip", PortKind::eValue, "vector", false, "The same, in clip space: what a projection is moved by, 2 * pixels / size.", nullptr },
                                    { "jittered", PortKind::eValue, "matrix", false, "`projection`, moved by it: what a temporal antialiasing draws with.", nullptr },
                                    { "index", PortKind::eValue, "number", false, "Which of the places it is, from 0.", nullptr } },
                                  "A place within a pixel for each frame, from a Halton (2, 3) sequence: what moves the projection a little every frame, so that the frames "
                                  "a temporal antialiasing blends see between the pixels.", nullptr });

                Port last = Any("value", "What it is given: a number, a vector, a matrix.");
                types.push_back({ "LastFrame", "Flow", { std::move(last) },
                                  { { "out", PortKind::eValue, "value", false, "What it was given the frame before. The first frame, what it is given now.", nullptr },
                                    { "valid", PortKind::eValue, "bool", false, "False the first frame, when there is no frame before.", nullptr } },
                                  "A value as it was the frame before: last frame's viewProjection, for what moved since.", nullptr });

                std::vector<std::string> features;
                for (const char* feature : { "eMeshShader", "eRayTracing", "eShaderFloat64", "eShaderInt64", "eShaderInt16", "eShaderFloat16", "eInt64Atomics",
                                             "eAtomicFloat32", "eAtomicFloat64", "eMultiDrawIndirect", "eDrawIndirectCount", "eGeometryShader", "eTessellationShader",
                                             "eFillModeNonSolid", "eWideLines", "eSamplerAnisotropy", "eAsyncCompute" })
                    features.emplace_back(feature);
                types.push_back({ "DeviceSupports", "Values", { chosen({ "feature", PortKind::eValue, "string", false, "What is asked of the device.", nullptr }, std::move(features)) },
                                  { { "out", PortKind::eValue, "bool", false, "Whether it has it.", nullptr } },
                                  "Whether the device can do something — mesh shaders, ray tracing — as a value: what a Switch takes, to go another way where it cannot.", nullptr });
                types.back().inputs.front().initial = Value("eMeshShader");
            }
            // ---- what is read of the entities as a whole, written into them, and drawn to see them ---------------------
            {
                Port of { "entities", PortKind::eValue, "entities", false, "The entities: an Entities node's, a Filter's.", nullptr };
                types.push_back({ "SceneBounds", "Scene", { of },
                                  { { "min", PortKind::eValue, "vector", false, "The least corner of the box round all of them: each one's mesh where it is, or where it is when it has none.", nullptr },
                                    { "max", PortKind::eValue, "vector", false, "Its greatest corner.", nullptr },
                                    { "center", PortKind::eValue, "vector", false, "Its middle.", nullptr },
                                    { "radius", PortKind::eValue, "number", false, "Half its diagonal: the sphere round it.", nullptr },
                                    { "count", PortKind::eValue, "number", false, "How many there are.", nullptr } },
                                  "The box round entities, as values: what a ShadowCascades' bounds are, and how far away a camera is to see all of them.", nullptr });
                Port component = hinted({ "component", PortKind::eValue, "string", false, "The component written.", nullptr }, "component");
                Port field { "field", PortKind::eValue, "string", false, "Its field, by name: a number, a vector, a matrix, true or false.", nullptr };
                Port entity = hinted({ "entity", PortKind::eValue, "string", false, "Which entity, by its name: the first that has the component, when not given.", nullptr }, "entity");
                entity.optional = true;
                types.push_back({ "WriteComponent", "Scene", { std::move(component), std::move(field), std::move(entity), Any("value", "What it is made to hold.") },
                                  { { "written", PortKind::eValue, "bool", false, "Whether there was such an entity to write.", nullptr } },
                                  "Writes a value into a component's field of an entity, when it is not what it holds already: what the graph worked out, handed "
                                  "to a script or shown in the inspector — an exposure, a count read back.", nullptr });

                Port kind = chosen({ "kind", PortKind::eValue, "string", false, "What is drawn: eLine and eArrow from `from` to `to`, eBox from `min` to `max`, eSphere round "
                                     "`center` of `radius`, eFrustum what `matrix` (a viewProjection) sees, eAxes of `matrix` (a transform) `size` long, eTransformedBox the unit cube "
                                     "(-1 to 1) turned by `matrix`.", nullptr }, { "eLine", "eArrow", "eBox", "eSphere", "eFrustum", "eAxes", "eTransformedBox" });
                kind.initial = Value("eFrustum");
                const auto given = [&](std::string name, std::string type, std::string doc) {
                    Port port { std::move(name), PortKind::eValue, std::move(type), false, std::move(doc), nullptr };
                    port.optional = true;
                    return port;
                };
                Port color = hinted(given("color", "vector", "Its colour: red, green, blue, alpha."), "color");
                color.initial = Value(Value::List { 1, 1, 0, 1 });
                types.push_back({ "DebugShapes", "Flow",
                                  { std::move(kind), given("from", "vector", "Where a line starts: three numbers for each, as many as there are lines."),
                                    given("to", "vector", "Where it ends."), given("min", "vector", "A box's least corner: three numbers for each."), given("max", "vector", "Its greatest."),
                                    given("center", "vector", "A sphere's middle: three numbers for each."), given("radius", "value", "How far it reaches: one for all, or one for each."),
                                    given("matrix", "matrix", "A matrix: sixteen numbers for each — ShadowCascades' four matrices are four frusta."),
                                    given("size", "number", "How long axes are: 1 when not given."), std::move(color) },
                                  {},
                                  "Lines in the scene where values say, every frame: a cascade's frustum, a light's reach, a box of bounds. A DebugDraw pass draws them, "
                                  "with the rest of the scene's debug shapes.", nullptr });
            }
            // What is worked out of values, by what kind of working out it is: a list of them all is too long to look down.
            static const std::set<std::string_view> vectors { "Length", "Normalize", "Dot", "Cross", "Distance", "Reflect", "Refract", "Project", "ColorConvert" };
            static const std::set<std::string_view> logic { "Less", "LessOrEqual", "Greater", "GreaterOrEqual", "Equal", "NotEqual", "Not", "And", "Or", "Xor", "Select", "Any", "All" };
            static const std::set<std::string_view> plain { "Bool", "String", "Color", "Constant", "DeviceSupports", "Split", "ImageExtent", "VertexLayout", "Append", "Slice",
                                                            "Float", "Double", "Int", "UInt", "Vec2", "Vec3", "Vec4", "IVec2", "IVec3", "IVec4", "UVec2", "UVec3", "UVec4" };
            static const std::set<std::string_view> matrices { "MatrixMultiply", "MatrixInverse", "MatrixTranspose", "MatrixDeterminant", "MatrixTranslate", "MatrixRotate",
                                                               "MatrixAxisAngle", "MatrixScale", "MatrixCompose", "TransformPoint", "TransformDirection", "TransformPlane",
                                                               "QuatFromEuler", "QuatFromAxisAngle", "QuatToEuler", "QuatToMatrix", "QuatMultiply", "QuatInverse", "QuatSlerp", "QuatRotate" };
            for (auto& type : types) {
                if (type.category == "Values" && !plain.contains(type.name))
                    type.category = vectors.contains(type.name) || matrices.contains(type.name) ? "Vectors" : logic.contains(type.name) ? "Logic" : "Math";
                if (type.keywords.empty()) type.keywords = Keywords(type.name);
                type.icon = IconOf(type.name, type.category);
                type.level = LevelOf(type);
                type.composite = type.name == "Function";
                for (auto& port : type.inputs) Limit(type.name, port);
                Shaped(type);
                Group(type);
                if (type.category == "Passes") {
                    Port enabled { "enabled", PortKind::eValue, "bool", false, "False to leave the pass out: it is not run, and nothing waits for it.", nullptr };
                    enabled.optional = true;
                    enabled.initial = Value(true);
                    type.inputs.push_back(std::move(enabled));
                    Port views { "views", PortKind::eValue, "string", false, "Which views of the scene run it: every one, the game's only (what is run, and an "
                                 "editor's Game panel) — a vignette, a screen's words — or an editor's scene view only: what is drawn there to look at the scene by.", nullptr };
                    views.optional = true;
                    views.choices = { "all", "game", "editor" };
                    views.initial = Value("all");
                    type.inputs.push_back(std::move(views));
                    // What records compute dispatches and copies only may run on the async compute queue, beside the
                    // graphics passes it does not depend on (kor::PassBuilder::AsyncCompute).
                    if (RunsAsync(type.name)) {
                        Port async { "async", PortKind::eValue, "bool", false,
                                     "True to run it on the async compute queue, beside the graphics passes it does not depend on. On a device with "
                                     "no second queue it runs on the graphics queue, in order, which is always correct.", nullptr };
                        async.optional = true;
                        async.initial = Value(false);
                        type.inputs.push_back(std::move(async));
                    }
                }
                if (!type.builder)
                    for (auto& port : type.inputs)
                        if (port.initial.IsNull()) port.initial = Initial(type.name, port.name);
                // Where each input is set. A number, a truth, a vector or a colour of what works values out is set on the
                // node, where it is read; so is a pass's being on, a loop's count. What only a node can make —
                // a struct, a matrix, a vertex layout — is wired, from the node that sets it up. The rest is the inspector's.
                static const std::set<std::string_view> worked { "Values", "Math", "Vectors", "Logic", "Cameras", "Random" };
                static const std::set<std::string_view> lenses { "ShadowCascades", "PointShadows" };
                for (auto& port : type.inputs) {
                    if (port.kind != PortKind::eValue) continue;
                    const bool small = port.type == "number" || port.type == "bool" || port.type == "vector" || port.type == "value" || port.editor == "color";
                    if (port.type == "struct KoralVertexLayout") { port.place = Placement::eWire; port.companion = "VertexLayout"; }
                    else if (port.type == "struct" || port.type == "matrix" || port.type == "entities" || port.type == "bundle") port.place = Placement::eWire;
                    else if (lenses.contains(type.name)) port.place = Placement::eInspector;
                    else if (worked.contains(type.category) && type.name != "VertexLayout" && (small || !port.choices.empty() || type.name == "String")) port.place = Placement::eNode;
                    else if ((type.name == "Repeat" && port.name == "count") || (type.name == "Switch" && port.name == "index")
                             || (type.name == "Clear" && port.name == "color") || (type.name == "DebugView" && port.name == "name")) port.place = Placement::eNode;
                }
                if (type.name == "Collect")
                    for (auto& port : type.inputs) port.place = Placement::eWire;
                if (matrices.contains(type.name))       // a matrix is wired: none is typed in
                    for (auto& port : type.inputs)
                        if (port.name == "a" || port.name == "b" || port.name == "matrix") port.place = Placement::eWire;
                // What a node gives besides what it makes: each of its values, the parts of one thing as one, and what
                // the object it made says of itself. There to be asked for: none is shown until it is.
                std::vector<Port> more;
                const auto offer = [&](std::string name, std::string kind, std::string doc) {
                    const auto taken = [&](const std::vector<Port>& ports) { return std::ranges::find(ports, name, &Port::name) != ports.end(); };
                    if (taken(type.outputs) || taken(more)) return;
                    Port port { std::move(name), PortKind::eValue, std::move(kind), false, std::move(doc), nullptr };
                    port.optional = true;
                    more.push_back(std::move(port));
                };
                const std::string made = type.outputs.empty() ? std::string() : type.outputs.front().type;
                if (made == "Image" || made == "ImageView") {
                    offer("size", "vector", "Its width and height.");
                    offer("extent", "vector", "Its width, height and depth.");
                    offer("width", "number", "How many pixels across.");
                    offer("height", "number", "How many pixels down.");
                    offer("depth", "number", "How many pixels deep.");
                    offer("mipLevels", "number", "How many mip levels it has.");
                    offer("arrayLayers", "number", "How many layers it has.");
                } else if (made == "Buffer") {
                    offer("bytes", "number", "Its size, in bytes.");
                } else if (made == "Framebuffer") {
                    offer("size", "vector", "The width and height of what it draws into.");
                    offer("width", "number", "How many pixels across.");
                    offer("height", "number", "How many pixels down.");
                } else if (made == "ComputePipeline") {
                    offer("numThreads", "vector", "Its shader's work group, [numthreads(x, y, z)]: how much one dispatched group works on. An image's extent divided by it is how many groups cover the image.");
                } else if (made == "Mesh") {
                    offer("vertexCount", "number", "How many vertices it has.");
                    offer("indexCount", "number", "How many indices it has; 0 when it has none.");
                }
                for (const auto& port : type.inputs) {
                    if (port.kind != PortKind::eValue || port.repeatable) continue;
                    if (port.name.size() > 2 && port.name[port.name.size() - 2] == '.' && std::string_view("xyzw").find(port.name.back()) != std::string_view::npos)
                        offer(port.name.substr(0, port.name.size() - 2), "vector", "Its parts, as one vector.");
                    offer(port.name, port.type, "What its input of this name is.");
                }
                type.outputs.insert(type.outputs.end(), more.begin(), more.end());
                // An input in parts can be given whole: `groups` for `groups.x`, `.y` and `.z`, so a vector worked
                // out elsewhere goes in on one wire. There to be asked for, like any other that is not needed.
                for (std::size_t i = 0; i < type.inputs.size(); ++i) {
                    const Port& part = type.inputs[i];
                    if (part.kind != PortKind::eValue || part.repeatable || part.type != "number" || !part.name.ends_with(".x")) continue;
                    const std::string name = part.name.substr(0, part.name.size() - 2);
                    if (name.find('[') != std::string::npos || std::ranges::find(type.inputs, name, &Port::name) != type.inputs.end()) continue;
                    Port whole { name, PortKind::eValue, "vector", false, std::format("Its x, y and z as one vector, instead of each on its own.{}{}", part.doc.empty() ? "" : " ", part.doc), nullptr };
                    whole.optional = true;
                    whole.mode = part.mode;
                    whole.editor = "vector";
                    whole.min = part.min;       // each of it as its parts are
                    whole.max = part.max;
                    whole.logarithmic = part.logarithmic;
                    whole.integer = part.integer;
                    whole.group = part.group;
                    whole.when = part.when;
                    type.inputs.insert(type.inputs.begin() + static_cast<std::ptrdiff_t>(i), std::move(whole));
                    ++i;
                }
            }
            return types;
        }
    }

    bool RunsAsync(const std::string_view type)
    {
        return type == "Dispatch" || type == "CopyBuffer" || type == "ClearBuffer" || type == "Copy" || type == "CopyBufferToImage" || type == "CopyImageToBuffer";
    }

    Shape ShapeNamed(const std::string_view name)
    {
        if (name == "Float") return { Scalar::eFloat, 1 };
        if (name == "Double") return { Scalar::eDouble, 1 };
        if (name == "Int") return { Scalar::eInt, 1 };
        if (name == "UInt") return { Scalar::eUInt, 1 };
        if (name == "Bool") return { Scalar::eBool, 1 };
        const auto vector = [&](const std::string_view prefix, const Scalar scalar) -> Shape {
            if (name.size() != prefix.size() + 1 || !name.starts_with(prefix) || name.back() < '2' || name.back() > '4') return {};
            return { scalar, static_cast<std::uint8_t>(name.back() - '0') };
        };
        for (const auto& [prefix, scalar] : { std::pair { "Vec", Scalar::eFloat }, { "IVec", Scalar::eInt }, { "UVec", Scalar::eUInt } })
            if (const Shape shape = vector(prefix, scalar); shape.Known()) return shape;
        return {};
    }

    std::string ShapeName(const Shape shape)
    {
        const bool one = shape.components == 1;
        switch (shape.scalar) {
            case Scalar::eFloat: return one ? "Float" : std::format("Vec{}", shape.components);
            case Scalar::eDouble: return one ? "Double" : std::format("Double{}", shape.components);
            case Scalar::eInt: return one ? "Int" : std::format("IVec{}", shape.components);
            case Scalar::eUInt: return one ? "UInt" : std::format("UVec{}", shape.components);
            case Scalar::eBool: return one ? "Bool" : std::format("Bool{}", shape.components);
            case Scalar::eNone: break;
        }
        return {};
    }

    Value Conform(const Value& value, const Shape shape)
    {
        if (!shape.Known()) return value;
        const auto number = [&](const Value& each) -> Value {
            double n;
            if (const double* given = each.Number()) n = *given;
            else if (const bool* flag = each.Bool()) n = *flag ? 1.0 : 0.0;
            else return each;
            switch (shape.scalar) {
                case Scalar::eFloat: return static_cast<double>(static_cast<float>(n));
                case Scalar::eInt: return std::clamp(std::round(n), -2147483648.0, 2147483647.0);
                case Scalar::eUInt: return std::clamp(std::round(n), 0.0, 4294967295.0);
                case Scalar::eBool: return n != 0.0;
                default: return n;
            }
        };
        const auto* items = value.Items();
        if (shape.components == 1) return items ? (items->empty() ? value : number(items->front())) : number(value);
        if (!items && !value.Number() && !value.Bool()) return value;
        Value::List made;
        for (std::size_t i = 0; i < shape.components; ++i)
            made.push_back(items ? (i < items->size() ? number((*items)[i]) : number(Value(0.0))) : number(value));
        return Value(std::move(made));
    }

    bool Applies(const Port& port, const Node& node)
    {
        if (port.when.input.empty()) return true;
        Value is;
        if (const auto given = node.inputs.find(port.when.input); given != node.inputs.end()) {
            if (given->second.link) return true;        // what is wired in is known only as it runs
            is = given->second.value;
        } else if (const NodeType* type = FindType(node.type)) {
            if (const auto starts = std::ranges::find(type->inputs, port.when.input, &Port::name); starts != type->inputs.end()) is = starts->initial;
        }
        if (is.IsNull()) return true;
        return std::ranges::contains(port.when.values, is);
    }

    NumberLimits LimitsOf(const std::string_view structName, const std::string_view field)
    {
        constexpr float any = std::numeric_limits<float>::infinity();
        static const std::map<std::pair<std::string_view, std::string_view>, NumberLimits> known {
            // Where a vertex's numbers are: a binding of the few there are, a byte of a stride, one to four numbers.
            { { "KoralVertexBinding", "binding" }, { 0.f, 31.f } }, { { "KoralVertexBinding", "stride" }, { 0.f, 2048.f } },
            { { "KoralVertexAttribute", "binding" }, { 0.f, 31.f } }, { { "KoralVertexAttribute", "offset" }, { 0.f, 2047.f } },
            { { "KoralVertexAttribute", "channelCount" }, { 1.f, 4.f } }, { { "KoralVertexAttribute", "location" }, { -1.f, 31.f } },
            { { "KoralVertexAttribute", "locations" }, { 0.f, 16.f } }, { { "KoralVertexAttribute", "locationStride" }, { 0.f, 2048.f } },
            { { "KoralVertexLayout", "positionAttribute" }, { -1.f, 31.f } },
            // How it is drawn: lines some pixels wide, a share of the samples shaded, a depth between near and far.
            { { "KoralRasterizationState", "lineWidth" }, { 0.f, 64.f } },
            { { "KoralMultisampleState", "minSampleShading" }, { 0.f, 1.f } },
            { { "KoralDepthStencilState", "minDepth" }, { 0.f, 1.f } }, { { "KoralDepthStencilState", "maxDepth" }, { 0.f, 1.f } },
            { { "KoralStencilOpState", "compareMask" }, { 0.f, 255.f } }, { { "KoralStencilOpState", "writeMask" }, { 0.f, 255.f } },
            { { "KoralStencilOpState", "reference" }, { 0.f, 255.f } },
            { { "KoralClearColor", "components" }, { 1.f, 4.f } },
        };
        const auto it = known.find({ structName, field });
        return it == known.end() ? NumberLimits { -any, any, false } : it->second;
    }

    const std::vector<NodeType>& Catalog()
    {
        static const std::vector<NodeType> types = Build();
        return types;
    }

    std::string_view LayoutTypeOf(const std::string_view fieldType)
    {
        static const std::map<std::string_view, std::string_view> layouts {
            { "f32", "eFloat" }, { "f64", "eFloat" }, { "i32", "eInt" }, { "u32", "eUInt" }, { "bool", "eBool" }, { "bool32", "eBool" },
            { "vec2", "eVec2" }, { "vec3", "eVec3" }, { "vec4", "eVec4" }, { "quat", "eVec4" }, { "ivec2", "eIVec2" }, { "ivec3", "eIVec3" }, { "ivec4", "eIVec4" },
            { "uvec2", "eUVec2" }, { "uvec3", "eUVec3" }, { "uvec4", "eUVec4" }, { "mat4", "eMat4" } };
        const auto it = layouts.find(fieldType);
        return it == layouts.end() ? std::string_view {} : it->second;
    }

    const std::vector<std::pair<std::string_view, std::string_view>>& PlaceFields()
    {
        static const std::vector<std::pair<std::string_view, std::string_view>> places {
            { "worldPosition", "eVec3" }, { "forward", "eVec3" }, { "up", "eVec3" }, { "right", "eVec3" }, { "position", "eVec3" }, { "rotation", "eVec3" },
            { "scale", "eVec3" }, { "orientation", "eVec4" }, { "matrix", "eMat4" }, { "normalMatrix", "eMat4" }, { "previousMatrix", "eMat4" }, { "entityId", "eUInt" } };
        return places;
    }

    bool PlacedByDefault(const std::string_view name) { return name == "worldPosition" || name == "forward" || name == "matrix"; }

    std::string_view LevelName(const Level level)
    {
        switch (level) {
        case Level::eValue: return "value";
        case Level::eResource: return "resource";
        case Level::eCommand: return "command";
        case Level::eScene: return "scene";
        case Level::eTechnique: return "technique";
        }
        return "value";
    }

    const NodeType* FindType(const std::string_view name)
    {
        const auto& types = Catalog();
        const auto it = std::ranges::find(types, name, &NodeType::name);
        return it == types.end() ? nullptr : &*it;
    }

    Placement PlacementOf(const Port& port, const Node& node)
    {
        if (port.place == Placement::eWire) return Placement::eWire;
        const auto it = node.placed.find(port.name);
        if (it == node.placed.end()) return port.place;
        return it->second == "node" ? Placement::eNode : Placement::eInspector;
    }

    std::string StagesOf(const Node& node)
    {
        const auto it = node.inputs.find("stages");
        const std::string* stages = it == node.inputs.end() ? nullptr : it->second.value.String();
        return stages && *stages == "eMesh" ? "eMesh" : "eVertex";
    }

    std::vector<std::string> PartsOf(const NodeType& type, const std::string_view whole)
    {
        std::vector<std::string> parts;
        if (const auto port = std::ranges::find(type.inputs, whole, &Port::name); port == type.inputs.end() || port->editor != "vector") return parts;
        for (const char* axis : { ".x", ".y", ".z", ".w" }) {
            std::string name = std::string(whole) + axis;
            if (std::ranges::find(type.inputs, name, &Port::name) != type.inputs.end()) parts.push_back(std::move(name));
        }
        return parts;
    }

    std::vector<Port> PortsFor(const NodeType& type, const Node& node)
    {
        std::vector<Port> ports;
        const std::string stages = type.name == "GraphicsPipeline" ? StagesOf(node) : std::string();
        for (const auto& port : type.inputs)
            if (port.mode.empty() || port.mode == stages) ports.push_back(port);
        // A Bundle node has an input for each of its members' names.
        if (type.name == "Bundle")
            if (const auto names = node.inputs.find("names"); names != node.inputs.end() && names->second.value.Items())
                for (const auto& name : *names->second.value.Items())
                    if (name.String() && !name.String()->empty()) {
                        Port member { "field." + *name.String(), PortKind::eValue, "value", false, std::format("The member '{}': a value, or a wire.", *name.String()), nullptr };
                        member.optional = true;
                        member.place = Placement::eNode;
                        ports.push_back(std::move(member));
                    }
        return ports;
    }
}
