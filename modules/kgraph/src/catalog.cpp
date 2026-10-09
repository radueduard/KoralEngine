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
                { "Copy", "duplicate image transfer" }, { "CopyBuffer", "duplicate buffer transfer" },
                { "GenerateMipmaps", "mip mipmap levels downsample" },
                { "Dispatch", "run compute execute kernel indirect" },
                { "Draw", "render rasterize mesh entities triangle fullscreen indirect instanced viewport scissor mesh tasks" },
                { "TraceRays", "ray tracing raygen rtx path trace" },
                { "SceneView", "camera view projection lights sky sun frame uniform scene" }, { "Pick", "select click id entity mouse read back" },
                { "DebugDraw", "gizmo lines helpers debug overlay" }, { "Resolve", "multisample msaa antialias samples" },
                { "ClearBuffer", "zero fill buffer reset" }, { "CopyBufferToImage", "upload texels buffer image transfer" },
                { "CopyImageToBuffer", "download read back texels image buffer transfer" },
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
            {
                const auto given = [](std::string name, std::string type, std::string doc) {
                    Port port { std::move(name), PortKind::eValue, std::move(type), false, std::move(doc), nullptr };
                    port.optional = true;
                    return port;
                };
                types.push_back({ "Screen", "Resources", {},
                                  { ResourcePort("out", "Image"), given("extent", "vector", "How big the window is: its width, its height, and 1. An image sized by it follows the window."),
                                    given("size", "vector", "The window's width and height."), given("width", "number", "How many pixels across the window is."),
                                    given("height", "number", "How many pixels down.") },
                                  "The scene's window: what a pass writes to show something.", nullptr });
                Port extent { "extent", PortKind::eValue, "vector", false, "How big the picture it is for is, its width and height: what the camera's aspect is worked out from. Square, when not given.", nullptr };
                extent.optional = true;
                types.push_back({ "SceneView", "Resources", { std::move(extent) },
                                  { ResourcePort("frame", "Buffer", "What the frame is seen from and lit by: a uniform block, laid out as the scene shaders' FrameData."),
                                    ResourcePort("lights", "Buffer", "Every light in the scene, up to 64: an array, laid out as the scene shaders' Light.") },
                                  "The scene as shaders see it, written each frame from the world: the camera (the first entity with a Camera, or the editor's view), the sky and its sun, and every light.", nullptr });
            }
            types.push_back({ "Primitive", "Resources",
                              { chosen({ "shape", PortKind::eValue, "string", false,
                                         "eCube, eSphere, ePlane (facing up), eCylinder, eCone, eCapsule and eTorus, each about a unit across; eQuad (a unit square facing +z) and eTriangle. All are centred on the origin.", nullptr },
                                       { "eCube", "eSphere", "ePlane", "eCylinder", "eCone", "eCapsule", "eTorus", "eQuad", "eTriangle" }) },
                              { ResourcePort("out", "Mesh"), { "layout", PortKind::eValue, "struct KoralVertexLayout", false, "Its vertices: POSITION, NORMAL and UV. What a pipeline that draws it takes as vertex.layout.", nullptr } },
                              "A mesh of a simple shape, with positions, normals and texture coordinates. The same shapes are there for any entity's Mesh without a node, as the default meshes.", nullptr });
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
            types.push_back({ "Clear", "Passes", { ResourcePort("image", "Image"), hinted({ "color", PortKind::eValue, "vector", false, "Red, green, blue, alpha.", nullptr }, "color") }, {},
                              "Fills an image with one colour.", nullptr });
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
                                optional(Number("sourceOffset", "The byte of the source it starts at.")), optional(Number("destinationOffset", "The byte of the destination it starts at.")) }, {},
                              "Copies a buffer's bytes into another: as many as the smaller of the two holds.", nullptr });
            types.push_back({ "ClearBuffer", "Passes",
                              { ResourcePort("buffer", "Buffer"), optional(Number("offset", "The byte it starts at.")), optional(Number("bytes", "How many bytes: to the buffer's end, when not given.")) }, {},
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
            types.push_back({ "Pick", "Passes", { ResourcePort("ids", "Image", "An image of which entity is where: one a pipeline draws its `entityId` push constant into, as a number of 32 bits.") }, {},
                              "Lets a click on the picture pick an entity: reads back the pixel asked about. It costs nothing in a frame nobody asks.", nullptr });
            types.push_back({ "DebugDraw", "Passes",
                              { ResourcePort("image", "Image", "The picture they are drawn over."),
                                optional(ResourcePort("depth", "Image", "A depth image to test them against: on top of everything, when not wired.")) }, {},
                              "Draws the scene's debug shapes over a picture, seen as a SceneView sees it: an editor's gizmo, a camera's reach, a light's.", nullptr });
            const Port with = optional(hinted({ "with", PortKind::eValue, "list", false, "Components, by name, an entity must have to be run for: tags, mostly.", nullptr }, "components"));
            const Port without = optional(hinted({ "without", PortKind::eValue, "list", false, "Components, by name, an entity must not have.", nullptr }, "components"));
            types.push_back({ "Dispatch", "Passes",
                              { ResourcePort("pipeline", "ComputePipeline"),
                                optional(Number("groups.x", "How many groups to run, each way: rounded up, so an image's extent divided by the pipeline's numThreads covers the image.")), optional(Number("groups.y")), optional(Number("groups.z")),
                                optional(hinted({ "cover", PortKind::eValue, "string", false, "One of the pipeline's image parameters, by name: as many groups as cover it, for each entity's own image when it is per entity. Instead of groups.", nullptr }, "parameter")),
                                optional(Number("groupSize.x", "How much of the covered image one group works on: the pipeline's numThreads, when not given.")), optional(Number("groupSize.y")), optional(Number("groupSize.z")),
                                optional(ResourcePort("indirect", "Buffer", "A buffer that says how many groups, three numbers of 32 bits, written by a pass before this one: instead of groups. It needs eIndirect among its usage.")),
                                optional(Number("indirectOffset", "The byte of the indirect buffer the three numbers are at.")),
                                with, without }, {},
                              "Runs a compute pipeline: once when everything it needs is wired, and otherwise once for each entity that has its component.", nullptr });
            types.push_back({ "TraceRays", "Passes",
                              { ResourcePort("pipeline", "RayTracingPipeline"),
                                optional(Number("size.x", "How many rays, each way: one for each pixel of the image they are for.")), optional(Number("size.y")), optional(Number("size.z")),
                                optional(hinted({ "cover", PortKind::eValue, "string", false, "One of the pipeline's image parameters, by name: a ray for each of its pixels. Instead of size.", nullptr }, "parameter")),
                                with, without }, {},
                              "Runs a ray tracing pipeline: once when everything it needs is wired, and otherwise once for each entity that has its component.", nullptr });
            {
                const std::vector<std::string> loads { "eClear", "eLoad", "eDontCare" }, stores { "eStore", "eDontCare" };
                const std::vector<std::string> compares { "eNever", "eLess", "eEqual", "eLessOrEqual", "eGreater", "eNotEqual", "eGreaterOrEqual", "eAlways" };
                std::vector<Port> ports {
                    ResourcePort("pipeline", "GraphicsPipeline"),
                    optional(ResourcePort("framebuffer", "Framebuffer", "What it draws into: the pipeline's own, when not wired.")),
                    optional(ResourcePort("mesh", "Mesh", "One mesh to draw, instead of each entity's own.")),
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
                    with, without };
                types.push_back({ "Draw", "Passes", std::move(ports), {},
                                  "Draws with a graphics pipeline into a framebuffer. Each entity that has a Mesh, and the pipeline's component when it has one; or one mesh wired in; or no mesh at all, when the vertex shader makes its own vertices.", nullptr });
            }
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
                    type.inputs.insert(type.inputs.begin() + static_cast<std::ptrdiff_t>(i), std::move(whole));
                    ++i;
                }
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
        return ports;
    }
}
