//
// kgraph: the kinds of node. One a builder the framework describes, and the engine's own.
//

#include <kgraph/catalog.h>
#include <kgraph/graph.h>

#include <algorithm>
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

        /** The inputs a node of a type cannot do without: the rest are there to be asked for. */
        bool Essential(const std::string_view type, const std::string_view input)
        {
            static const std::map<std::string_view, std::set<std::string_view>> essential {
                { "Buffer", { "instanceCount", "usage" } },
                { "Image", { "format", "extent.x", "extent.y", "usage" } },
                { "ImageView", { "image" } },
                { "BufferView", { "buffer" } },
                { "Sampler", {} },
                { "Shader", { "entryPoint.module", "entryPoint.entry" } },
                { "Framebuffer", { "color[].view" } },
                { "Mesh", { "vertexBuffer.buffer", "vertexLayout" } },
                { "AccelerationStructure", { "mesh[]" } },
                { "ComputePipeline", { "compute.module", "compute.entry" } },
                { "GraphicsPipeline", { "stages", "vertex.module", "vertex.entry", "vertex.layout", "mesh.module", "mesh.entry",
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
                    { "usage", L { "eSampled", "eTransferSrc", "eTransferDst" } }, { "mipLevels", 1 }, { "arrayLayers", 1 }, { "sampleCount", "e1" },
                    { "isPerFrame", false }, { "sharedAcrossQueues", false } } },
                { "Buffer", {
                    { "instanceCount", 256 }, { "usage", L { "eStorage" } }, { "type", "eDeviceLocal" }, { "isPerFrame", false }, { "sharedAcrossQueues", false } } },
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
                { "Number", { { "value", 0 } } }, { "Bool", { { "value", false } } }, { "String", { { "value", "" } } },
                { "Vector", { { "x", 0 }, { "y", 0 }, { "z", 0 }, { "w", 1 } } },
                { "Color", { { "color", L { 1, 1, 1, 1 } } } },
                { "Component", { { "index", 0 } } },
                { "Clamp", { { "min", 0 }, { "max", 1 } } }, { "Lerp", { { "t", 0.5 } } }, { "Smoothstep", { { "edge0", 0 }, { "edge1", 1 } } },
                { "Step", { { "edge", 0.5 } } }, { "Pow", { { "b", 2 } } },
                { "Primitive", { { "shape", "eQuad" } } },
                { "VertexLayout", { { "attributes", L { M { { "semantic", "POSITION" }, { "type", "eFloat" }, { "count", 3 } } } } } },
                { "Clear", { { "color", L { 0, 0, 0, 1 } } } },
                { "Dispatch", { { "groups.x", 1 }, { "groups.y", 1 }, { "groups.z", 1 }, { "groupSize.x", 8 }, { "groupSize.y", 8 }, { "groupSize.z", 1 },
                                { "with", L {} }, { "without", L {} } } },
                { "Draw", { { "command", "eAuto" }, { "instances", 1 }, { "load", false }, { "with", L {} }, { "without", L {} } } },
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
                { "Mesh", "geometry vertices indices model" }, { "Primitive", "quad cube triangle square box mesh geometry shape" },
                { "AccelerationStructure", "ray tracing bvh blas tlas" },
                { "ComputePipeline", "compute shader kernel gpu program" }, { "GraphicsPipeline", "vertex fragment mesh shader material render program" },
                { "RayTracingPipeline", "ray tracing shader raygen hit miss" },
                { "Screen", "window swapchain present output backbuffer" }, { "Reference", "alias link portal reroute" },
                { "Clear", "fill colour color wipe erase" }, { "Blit", "copy scale show present stretch resize" },
                { "Copy", "duplicate image transfer" }, { "CopyBuffer", "duplicate buffer transfer" },
                { "GenerateMipmaps", "mip mipmap levels downsample" },
                { "Dispatch", "run compute execute kernel" }, { "Draw", "render rasterize mesh entities" },
                { "Number", "float scalar constant value" }, { "Bool", "boolean flag true false constant" }, { "String", "text name constant" },
                { "Vector", "vec2 vec3 vec4 combine compose make" }, { "Color", "colour rgba picker constant" },
                { "Component", "element index get swizzle" }, { "Split", "break separate components xyz swizzle" },
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
            };
            const auto it = keywords.find(type);
            return it == keywords.end() ? std::string() : std::string(it->second);
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
            value("Number", { Number("value") }, "number", "A number.");
            value("Bool", { { "value", PortKind::eValue, "bool", false, {}, nullptr } }, "bool", "True or false.");
            value("String", { { "value", PortKind::eValue, "string", false, {}, nullptr } }, "string", "Text.");
            value("Vector", { Number("x"), Number("y"), optional(Number("z")), optional(Number("w")) }, "vector", "As many numbers as are given, x first.");
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
                value(name, { Number("a"), Number("b") }, "bool", "True or false: how the number a stands to the number b.");
            value("Equal", { Any("a"), Any("b") }, "bool", "Whether a and b are the same.");
            value("NotEqual", { Any("a"), Any("b") }, "bool", "Whether a and b differ.");
            const Port flag { "a", PortKind::eValue, "bool", false, {}, nullptr };
            Port other = flag;
            other.name = "b";
            value("Not", { flag }, "bool", "True when a is false.");
            value("And", { flag, other }, "bool", "True when both are.");
            value("Or", { flag, other }, "bool", "True when either is.");
            value("Xor", { flag, other }, "bool", "True when one is and the other is not.");
            value("Select", { { "condition", PortKind::eValue, "bool", false, {}, nullptr }, Any("a"), Any("b") }, "value", "a when the condition is true, b when it is not.");
            value("Split", { { "vector", PortKind::eValue, "vector", false, {}, nullptr } }, "vector", "A vector's components, each an output of its own: x, y, z, w.");
            for (const char* part : { "x", "y", "z", "w" }) types.back().outputs.push_back({ part, PortKind::eValue, "number", false, {}, nullptr });
            value("Component", { { "vector", PortKind::eValue, "vector", false, {}, nullptr }, Number("index") }, "number", "One component of a vector: 0 the first.");
            value("ImageExtent", { ResourcePort("image", "Image") }, "vector", "An image's width, height and depth.");
            value("VertexLayout", { hinted({ "attributes", PortKind::eValue, "list", false,
                                      "What a vertex has, in order: { \"semantic\": \"POSITION\", \"type\": \"eFloat\", \"count\": 3 }, and optionally \"binding\", \"instanced\", \"namespace\".", nullptr }, "attributes") },
                  "struct KoralVertexLayout", "How a mesh's vertices are laid out, packed in the order given: what a pipeline's vertex.layout and a Mesh's vertexLayout take.");

            // ---- the screen, and a node by name -------------------------------------------------------------------
            types.push_back({ "Screen", "Resources", {}, { ResourcePort("out", "Image") }, "The scene's window: what a pass writes to show something.", nullptr });
            types.push_back({ "Primitive", "Resources",
                              { chosen({ "shape", PortKind::eValue, "string", false, "eQuad (a unit square facing +z), eCube (a unit cube) or eTriangle. Each is centred on the origin.", nullptr },
                                       { "eQuad", "eCube", "eTriangle" }) },
                              { ResourcePort("out", "Mesh"), { "layout", PortKind::eValue, "struct KoralVertexLayout", false, "Its vertices: POSITION, NORMAL and UV. What a pipeline that draws it takes as vertex.layout.", nullptr } },
                              "A mesh of a simple shape, with positions, normals and texture coordinates.", nullptr });
            types.push_back({ "Reference", "Resources", { hinted({ "node", PortKind::eValue, "string", false, "The id of the node it stands for.", nullptr }, "node") },
                              { ResourcePort("out", "Resource") },
                              "Another node's output, by the node's id: a resource made elsewhere in the graph, without a wire across it.", nullptr });

            // ---- passes -------------------------------------------------------------------------------------------
            types.push_back({ "Clear", "Passes", { ResourcePort("image", "Image"), hinted({ "color", PortKind::eValue, "vector", false, "Red, green, blue, alpha.", nullptr }, "color") }, {},
                              "Fills an image with one colour.", nullptr });
            types.push_back({ "Blit", "Passes", { ResourcePort("source", "Image"), ResourcePort("destination", "Image") }, {},
                              "Copies an image onto another, scaled to fit: onto the screen, to show it.", nullptr });
            types.push_back({ "Copy", "Passes", { ResourcePort("source", "Image"), ResourcePort("destination", "Image") }, {},
                              "Copies an image onto another of the same size, as it is.", nullptr });
            types.push_back({ "CopyBuffer", "Passes", { ResourcePort("source", "Buffer"), ResourcePort("destination", "Buffer") }, {},
                              "Copies a buffer's bytes into another: as many as the smaller of the two holds.", nullptr });
            types.push_back({ "GenerateMipmaps", "Passes", { ResourcePort("image", "Image") }, {},
                              "Fills an image's smaller mip levels from its largest.", nullptr });
            const Port with = optional(hinted({ "with", PortKind::eValue, "list", false, "Components, by name, an entity must have to be run for: tags, mostly.", nullptr }, "components"));
            const Port without = optional(hinted({ "without", PortKind::eValue, "list", false, "Components, by name, an entity must not have.", nullptr }, "components"));
            types.push_back({ "Dispatch", "Passes",
                              { ResourcePort("pipeline", "ComputePipeline"), optional(Number("groups.x")), optional(Number("groups.y")), optional(Number("groups.z")),
                                optional(hinted({ "cover", PortKind::eValue, "string", false, "One of the pipeline's image parameters, by name: as many groups as cover it, for each entity's own image when it is per entity. Instead of groups.", nullptr }, "parameter")),
                                optional(Number("groupSize.x", "How much of the covered image one group works on: the shader's numthreads.")), optional(Number("groupSize.y")), optional(Number("groupSize.z")),
                                with, without }, {},
                              "Runs a compute pipeline: once when everything it needs is wired, and otherwise once for each entity that has its component.", nullptr });
            types.push_back({ "Draw", "Passes",
                              { ResourcePort("pipeline", "GraphicsPipeline"),
                                optional(ResourcePort("framebuffer", "Framebuffer", "What it draws into: the pipeline's own, when not wired.")),
                                optional(chosen({ "command", PortKind::eValue, "string", false, "eAuto (indexed when the mesh has indices), eDraw or eDrawIndexed.", nullptr }, { "eAuto", "eDraw", "eDrawIndexed" })),
                                optional(Number("instances", "How many of each mesh: 1 when not given.")),
                                optional({ "load", PortKind::eValue, "bool", false, "True to draw over what the framebuffer holds; false, or not given, clears it first.", nullptr }),
                                with, without }, {},
                              "Draws every entity that has a Mesh, and the pipeline's component when it has one, into a framebuffer.", nullptr });
            // What is worked out of values, by what kind of working out it is: a list of them all is too long to look down.
            static const std::set<std::string_view> vectors { "Length", "Normalize", "Dot", "Cross", "Distance" };
            static const std::set<std::string_view> logic { "Less", "LessOrEqual", "Greater", "GreaterOrEqual", "Equal", "NotEqual", "Not", "And", "Or", "Xor", "Select" };
            static const std::set<std::string_view> plain { "Number", "Bool", "String", "Vector", "Color", "Component", "Split", "ImageExtent", "VertexLayout" };
            for (auto& type : types) {
                if (type.category == "Values" && !plain.contains(type.name))
                    type.category = vectors.contains(type.name) ? "Vectors" : logic.contains(type.name) ? "Logic" : "Math";
                type.keywords = Keywords(type.name);
                if (!type.builder)
                    for (auto& port : type.inputs)
                        if (port.initial.IsNull()) port.initial = Initial(type.name, port.name);
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
            }
            return types;
        }
    }

    const std::vector<NodeType>& Catalog()
    {
        static const std::vector<NodeType> types = Build();
        return types;
    }

    const NodeType* FindType(const std::string_view name)
    {
        const auto& types = Catalog();
        const auto it = std::ranges::find(types, name, &NodeType::name);
        return it == types.end() ? nullptr : &*it;
    }

    std::string StagesOf(const Node& node)
    {
        const auto it = node.inputs.find("stages");
        const std::string* stages = it == node.inputs.end() ? nullptr : it->second.value.String();
        return stages && *stages == "eMesh" ? "eMesh" : "eVertex";
    }

    std::vector<Port> PortsFor(const NodeType& type, const Node& node)
    {
        std::vector<Port> ports;
        const std::string stages = type.name == "GraphicsPipeline" ? StagesOf(node) : std::string();
        for (const auto& port : type.inputs)
            if (port.mode.empty() || port.mode == stages) ports.push_back(port);
        return ports;
    }
}
