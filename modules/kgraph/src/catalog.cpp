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
                { "Linear", { { "a", L { 1, 1, 1 } } } }, { "Append", { { "a", L { 0, 0, 0 } }, { "b", 1 } } },
                { "Less", { { "a", 0 }, { "b", 0 } } }, { "LessOrEqual", { { "a", 0 }, { "b", 0 } } }, { "Greater", { { "a", 0 }, { "b", 0 } } },
                { "GreaterOrEqual", { { "a", 0 }, { "b", 0 } } }, { "Equal", { { "a", 0 }, { "b", 0 } } }, { "NotEqual", { { "a", 0 }, { "b", 0 } } },
                { "Not", { { "a", false } } }, { "And", { { "a", false }, { "b", false } } }, { "Or", { { "a", false }, { "b", false } } }, { "Xor", { { "a", false }, { "b", false } } },
                { "Select", { { "condition", true }, { "a", 0 }, { "b", 0 } } }, { "Split", { { "vector", L { 0, 0, 0, 0 } } } },
                { "Component", { { "vector", L { 0, 0, 0, 0 } }, { "index", 0 } } },
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
                                { "ComponentValue", "field script entity read get value" },
                { "EntityTransform", "position rotation scale matrix world entity" }, { "DataBuffer", "uniform block struct pack values constant buffer frame" },
                { "Entities", "all every world entity list each scene query" }, { "Entity", "one first single named camera sky sun component get find" }, { "Filter", "with without components tags keep query select entities meshes group groupby sort batch instances" },
                { "Selected", "picked selection outline entity id editor" },
                { "ForEach", "each every all loop iterate list component lights ecs world query scene entities instances" },
                { "BufferBuilder", "ssbo struct storage structured uniform constant buffer layout fields instances array lights cameras pack" },
                { "MeshPack", "pack merge combine batch meshes one mesh geometry indirect gpu driven bindless vertices indices" },
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
                { "Linear", "colour color gamma srgb linear convert" }, { "Collect", "gather list array all elements foreach values" },
                { "ShadowCascades", "csm cascaded shadow map sun directional light matrices" },
                { "PointShadows", "cube map omnidirectional shadow point light lamp six faces matrices" },
                { "ImageFile", "texture picture png jpg hdr environment map load import asset" },
                { "Repeat", "loop for each times iterate count mip chain" }, { "Previous", "loop last turn before feedback chain" },
                { "Element", "loop index pick one turn result" }, { "Note", "comment text label remark annotate" }, { "Switch", "branch choose select if either option toggle" },
                { "ShaderBuffer", "ssbo struct storage structured uniform constant buffer map fields entities array lights" },
                { "Append", "join concatenate vec4 combine" }, { "MatrixMultiply", "mat4 transform compose" }, { "MatrixInverse", "mat4 invert" }, { "Pick", "select click id entity mouse read back" },
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
                { "Primitive", "view_in_ar" }, { "ImageFile", "insert_photo" }, { "Screen", "monitor" }, { "Reference", "link" },
                { "VertexLayout", "reorder" }, { "DataBuffer", "dns" }, { "ShaderBuffer", "table_chart" }, { "BufferBuilder", "view_module" }, { "MeshPack", "inventory_2" }, { "Function", "functions" }, { "FunctionInput", "login" }, { "FunctionOutput", "logout" }, { "IndirectDraws", "format_list_numbered" },
                { "ComputePipeline", "memory" }, { "GraphicsPipeline", "brush" }, { "RayTracingPipeline", "flare" },
                // What is run.
                { "Clear", "format_color_reset" }, { "Blit", "open_in_full" }, { "Resolve", "blur_on" }, { "Copy", "content_copy" },
                { "CopyBuffer", "file_copy" }, { "ClearBuffer", "clear_all" }, { "CopyBufferToImage", "upload" }, { "CopyImageToBuffer", "download" },
                { "GenerateMipmaps", "photo_size_select_small" }, { "Dispatch", "play_arrow" }, { "TraceRays", "flare" }, { "Draw", "draw" },
                { "Pick", "touch_app" }, { "DebugDraw", "bug_report" },
                // Values.
                { "Number", "pin" }, { "Bool", "toggle_on" }, { "String", "text_fields" }, { "Vector", "north_east" }, { "Color", "palette" },
                { "Constant", "tag" }, { "Component", "looks_one" }, { "Split", "call_split" }, { "Append", "playlist_add" },
                { "ImageExtent", "aspect_ratio" }, { "Linear", "gradient" }, { "Select", "alt_route" },
                // Cameras.
                { "View", "visibility" }, { "Perspective", "videocam" }, { "Orthographic", "crop_square" }, { "Frustum", "change_history" },
                { "Oblique", "flip" }, { "Paraboloid", "panorama_fish_eye" }, { "CubeFace", "view_in_ar" }, { "ShadowCascades", "wb_sunny" },
                { "PointShadows", "lightbulb" },
                // The world.
                { "Entities", "public" }, { "Filter", "filter_alt" }, { "ForEach", "loop" }, { "Entity", "place" }, { "ComponentValue", "input" },
                { "EntityTransform", "3d_rotation" }, { "Selected", "near_me" }, { "Collect", "collections" },
                // What is done more than once, and one of several ways.
                { "Repeat", "repeat" }, { "Previous", "skip_previous" }, { "Element", "format_list_numbered" }, { "Switch", "call_split" },
                { "Note", "sticky_note_2" },
            };
            if (const auto it = icons.find(type); it != icons.end()) return std::string(it->second);
            if (type.starts_with("Matrix") || type.starts_with("Transform")) return "grid_on";
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
            value("Append", { Any("a"), Any("b") }, "vector", "The numbers of a, then those of b: a colour and its alpha as one vector of four.");
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
                value("Linear", { Any("a") }, "vector", "A colour as it was picked — as the display shows it — in the linear terms light adds up in: each of red, green and blue to the power 2.2. Its alpha is as it was.");

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
                                    given("height", "number", "How many pixels down."), given("aspect", "number", "Its width over its height: a lens's `aspect`.") },
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
                const Port entity = in("entity", "string", "Which entity, by name: the first that has the component, when not given.", "entity");
                types.push_back({ "ComponentValue", "Scene",
                                  { hinted({ "component", PortKind::eValue, "string", false, "The component's name: a script's, a pipeline's, one of the scene's own.", nullptr }, "component"),
                                    { "field", PortKind::eValue, "string", false, "The name of one of its fields.", nullptr }, entity },
                                  { out("out", "value", "The field: a number, or a vector.", true), out("present", "number", "1 when there is such an entity, 0 when not.") },
                                  "One field of a component, read from an entity every frame: what a script counts, or a camera's far plane, as a value to wire.", nullptr });
                Port named = entity;
                named.optional = false;
                named.doc = "Which entity, by name.";
                types.push_back({ "EntityTransform", "Scene", { named },
                                  { out("matrix", "matrix", "Where it is in the world: its own transform under those of everything above it.", true),
                                    out("worldPosition", "vector", "Where it is in the world."), out("forward", "vector", "The way it faces."),
                                    out("up", "vector", "Its up: +y, turned as it is."), out("right", "vector", "Its right: +x, turned as it is."),
                                    out("position", "vector", "Where it is, relative to what it is under."), out("rotation", "vector", "How it is turned, in degrees."),
                                    out("scale", "vector", {}), out("present", "number", "1 when there is such an entity.") },
                                  "An entity's place in the world, as values.", nullptr });
                types.push_back({ "Selected", "Scene", {},
                                  { out("entityId", "number", "Which entity is picked, from 1: 0 for none. What a ForEach's `entityId` is compared with.", true),
                                    out("present", "number", "1 when one is picked."),
                                    out("picking", "number", "1 on a frame a pick is asked of (Runtime::Pick): when what draws the ids it reads has to draw them.") },
                                  "The entity picked in an editor, as a number: what a pass that draws a line round it takes, once for every draw.", nullptr });

                // Every entity that has a component, one after another, and a buffer a shader declares with an element
                // for each: the fields of the one wired to the fields of the other.
                {
                    Port component = hinted({ "component", PortKind::eValue, "string", false, "The component every one of them has: a light, a script's, a pipeline's.", nullptr }, "component");
                    Port with = hinted({ "with", PortKind::eValue, "list", false, "Components, by name, an entity must have as well. Their fields are given too, as `<Component>.<field>`.", nullptr }, "components");
                    Port without = hinted({ "without", PortKind::eValue, "list", false, "Components, by name, an entity must not have.", nullptr }, "components");
                    with.optional = without.optional = true;
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
                        out("worldPosition", "vector", "Where it is in the world."), out("forward", "vector", "The way it faces."),
                        out("up", "vector", "Its up: +y, turned as it is."), out("right", "vector", "Its right: +x, turned as it is."),
                        out("position", "vector", "Where it is, relative to what it is under."), out("rotation", "vector", "How it is turned, in degrees."),
                        out("scale", "vector", {}),
                        out("present", "number", "1 when there is such an element, 0 when there is none.") };
                    types.push_back({ "ForEach", "Scene", { component, with, without, repeat }, each,
                                      "Every entity that has a component, an element at a time: each field of it, its index, and where the entity is. "
                                      "Every node wired from it is worked out once an element, and a BufferBuilder or ShaderBuffer wired from those holds an element for each.", nullptr });
                    // One entity, not a loop: the first with a component, or the one named — its fields, and where it is.
                    std::vector<Port> one;
                    for (const auto& port : each)
                        if (port.name != "index" && port.name != "count" && port.name != "turn" && port.name != "entityIndex" && port.name != "entities") one.push_back(port);
                    one.back().doc = "1 when there is such an entity: whether it was found.";
                    Port first = component;
                    first.doc = "The component it has: a camera, a sky, a script's.";
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
                    std::vector<Port> kept = every;
                    kept.push_back(out("group", "number", "Grouped: which group this element is of, from 0. 0 when it is not grouped."));
                    kept.push_back(out("groups", "list", "Grouped: { first, count } for each group, in order — where its elements start, and how many. What an IndirectDraws takes."));
                    kept.push_back(out("groupCount", "number", "Grouped: how many groups there are."));
                    types.push_back({ "Filter", "Scene", { std::move(of), std::move(keep), std::move(drop), std::move(groupBy) }, kept,
                                      "The entities it is wired, those with every component of `with` and none of `without`: an element at a time, with the fields of "
                                      "`with`'s components and where each is. A Draw wired from it draws each. With `groupBy`, those of the same mesh are next to each other.", nullptr });

                    Port module { "module", PortKind::eValue, "string", false, "The shader module that declares the buffer: a .slang file among the project's shaders, by its name without the ending.", nullptr };
                    module.editor = "module";
                    Port entry { "entry", PortKind::eValue, "string", false, "An entry point of the module that uses the buffer.", nullptr };
                    entry.editor = "entry";
                    Port parameter { "parameter", PortKind::eValue, "string", false, "What the shader calls the buffer: the `lights` of `StructuredBuffer<Light> lights`.", nullptr };
                    Port after = ResourcePort("after", "Buffer", "Another ShaderBuffer of the same struct, whose elements come first in this one: several kinds of entity in one buffer.");
                    after.optional = true;
                    types.push_back({ "ShaderBuffer", "Resources", { std::move(module), std::move(entry), std::move(parameter), std::move(after) },
                                      { ResourcePort("out", "Buffer"), out("count", "number", "How many elements it holds: what the shader loops to."),
                                        out("stride", "number", "The size of one element, in bytes.") },
                                      "A buffer a shader declares, laid out as the shader lays it out: an input for each field of its struct, by name. With fields wired from "
                                      "a ForEach it holds an element for each entity; with none, one element.", nullptr });
                }

                Port usage { "usage", PortKind::eValue, "string", false, "eUniform: a block of values a shader declares as a ConstantBuffer. eStorage: an array it declares as a StructuredBuffer.", nullptr };
                usage.optional = true;
                usage.choices = { "eUniform", "eStorage" };
                usage.initial = Value("eUniform");
                Port fieldValue { "field[].value", PortKind::eValue, "value", true, "A value of the block, in the order the shader declares them: a number, a vector, a matrix, or a long list of numbers.", nullptr };
                Port fieldType { "field[].type", PortKind::eValue, "string", true, "What it is in the shader, when its size does not say: an int or a uint, a uint4 of which only the first is given. eAuto goes by how many numbers there are.", nullptr };
                fieldType.optional = true;
                fieldType.choices = { "eAuto", "eFloat", "eInt", "eUInt", "eVec2", "eVec3", "eVec4", "eIVec4", "eUVec4", "eMat4", "eFloats" };
                types.push_back({ "DataBuffer", "Resources", { std::move(usage), std::move(fieldValue), std::move(fieldType) },
                                  { ResourcePort("out", "Buffer") },
                                  "A buffer made of values: each field after the last, laid out as a shader's block is, and written again every frame a value wired to it changes.", nullptr });

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
                    types.push_back({ "BufferBuilder", "Resources", { std::move(layout), std::move(count), std::move(rules), std::move(after) },
                                      { ResourcePort("out", "Buffer"), out("count", "number", "How many elements it holds: what the shader loops to.", true),
                                        out("stride", "number", "The size of one element, in bytes."), out("bytes", "number", "The size of the whole buffer, in bytes.") },
                                      "A buffer of elements laid out on the node: an input for each field, `field.<name>`. "
                                      "Wired from a ForEach, an element for each of its elements; from values of no ForEach, one.", nullptr });
                }
                        }
            types.push_back({ "Primitive", "Resources",
                              { chosen({ "shape", PortKind::eValue, "string", false,
                                         "eCube, eSphere, ePlane (facing up), eCylinder, eCone, eCapsule and eTorus, each about a unit across; eQuad (a unit square facing +z) and eTriangle. All are centred on the origin.", nullptr },
                                       { "eCube", "eSphere", "ePlane", "eCylinder", "eCone", "eCapsule", "eTorus", "eQuad", "eTriangle" }) },
                              { ResourcePort("out", "Mesh"), { "layout", PortKind::eValue, "struct KoralVertexLayout", false, "Its vertices: POSITION, NORMAL and UV. What a pipeline that draws it takes as vertex.layout.", nullptr } },
                              "A mesh of a simple shape, with positions, normals and texture coordinates. The same shapes are there for any entity's Mesh without a node, as the default meshes.", nullptr });
            // ---- many meshes drawn as one: packed, and drawn by the commands a buffer holds ------------------------------
            {
                Port meshes = ResourcePort("meshes", "Mesh", "Each element's mesh: a Filter's `Mesh.mesh`. The distinct ones are packed, in the order the Filter has them first — grouped by Mesh, a group's.");
                types.push_back({ "MeshPack", "Resources", { std::move(meshes) },
                                  { ResourcePort("mesh", "Mesh", "All of them as one mesh: what an indirect Draw draws."),
                                    { "layout", PortKind::eValue, "struct KoralVertexLayout", false, "Its vertices, as the meshes have them: what a pipeline that draws it takes as vertex.layout.", nullptr },
                                    { "ranges", PortKind::eValue, "list", false, "Each mesh's part of it: { firstIndex, indexCount, vertexOffset, boundsMin, boundsMax }, in order. What an IndirectDraws takes.", nullptr },
                                    ResourcePort("rangeBuffer", "Buffer", "The same, for a shader: 48 bytes a mesh — uint firstIndex, indexCount; int vertexOffset; uint; float4 boundsMin; float4 boundsMax."),
                                    { "count", PortKind::eValue, "number", false, "How many meshes it holds.", nullptr } },
                                  "Many meshes as one: their vertices one after another in one buffer, and their indices in another, each mesh copied once on the device. "
                                  "Built again when the meshes are other ones. Their vertices must be laid out alike.", nullptr });
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
                Port given { "value", PortKind::eValue, "value", false, "What it is given, while the function is opened in place: what the call is given there.", nullptr };
                given.optional = true;
                types.push_back({ "FunctionInput", "Functions", { edge("The input's name: a port of every call, in the order the inputs are on the page, top to bottom."), kind, std::move(initial), given },
                                  { { "out", PortKind::eValue, "value", false, "What the call is given there.", nullptr } },
                                  "An edge of a function: what a call is given at one of its inputs, for the function's nodes to take.", nullptr });
                Port wired = given;
                wired.optional = false;
                wired.doc = "What it gives: wired from a node of the function.";
                types.push_back({ "FunctionOutput", "Functions", { edge("The output's name: a port of every call, in the order the outputs are on the page, top to bottom."), kind, wired },
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
                types.push_back({ "Note", "Flow", { std::move(text) }, {}, "Words on the graph, beside what they are about: why a pass is there, what a number means. It makes nothing.", nullptr });
                Port index { "index", PortKind::eValue, "number", false, "Which of the options, from 0. True is 1 and false 0.", nullptr };
                index.initial = Value(0);
                Port option { "option[]", PortKind::eResource, "Resource", true, "One of the things it can be: a resource, or a value.", nullptr };
                types.push_back({ "Switch", "Flow", { std::move(index), std::move(option) }, { ResourcePort("out", "Resource") },
                                  "One of several things wired to it, by number: a picture or another to show, a pass's way of working or another.", nullptr });
            }
            types.push_back({ "ImageFile", "Resources",
                              { { "file", PortKind::eValue, "string", false, "The picture's file — png, jpg, hdr and the like: under the project's assets folder, or by its whole path.", nullptr } },
                              { ResourcePort("out", "Image") },
                              "A picture from a file, with its smaller levels made: a sky to look up, a pattern to draw with. An entity's own pictures need no node: its fields name their files.", nullptr });
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
                              "Draws the scene's debug shapes over a picture, seen as the Camera node sees it: an editor's gizmo, a camera's reach, a light's.", nullptr });
            types.push_back({ "Dispatch", "Passes",
                              { ResourcePort("pipeline", "ComputePipeline"),
                                optional(Number("groups.x", "How many groups to run, each way: rounded up, so an image's extent divided by the pipeline's numThreads covers the image.")), optional(Number("groups.y")), optional(Number("groups.z")),
                                optional(hinted({ "cover", PortKind::eValue, "string", false, "One of the pipeline's image parameters, by name: as many groups as cover it — each element's own image, when it is given here from a ForEach. Instead of groups.", nullptr }, "parameter")),
                                optional(Number("groupSize.x", "How much of the covered image one group works on: the pipeline's numThreads, when not given.")), optional(Number("groupSize.y")), optional(Number("groupSize.z")),
                                optional(ResourcePort("indirect", "Buffer", "A buffer that says how many groups, three numbers of 32 bits, written by a pass before this one: instead of groups. It needs eIndirect among its usage.")),
                                optional(Number("indirectOffset", "The byte of the indirect buffer the three numbers are at.")) }, {},
                              "Runs a compute pipeline. What its pipeline leaves unwired is given here, `param.<name>` and `push.<name>`: wired from a ForEach, it "
                              "runs once for each of its elements, and otherwise once.", nullptr });
            types.push_back({ "TraceRays", "Passes",
                              { ResourcePort("pipeline", "RayTracingPipeline"),
                                optional(Number("size.x", "How many rays, each way: one for each pixel of the image they are for.")), optional(Number("size.y")), optional(Number("size.z")),
                                optional(hinted({ "cover", PortKind::eValue, "string", false, "One of the pipeline's image parameters, by name: a ray for each of its pixels. Instead of size.", nullptr }, "parameter")) }, {},
                              "Runs a ray tracing pipeline. What its pipeline leaves unwired is given here, as a Dispatch's is: once, or once for each element of a ForEach.", nullptr });
            {
                const std::vector<std::string> loads { "eClear", "eLoad", "eDontCare" }, stores { "eStore", "eDontCare" };
                const std::vector<std::string> compares { "eNever", "eLess", "eEqual", "eLessOrEqual", "eGreater", "eNotEqual", "eGreaterOrEqual", "eAlways" };
                std::vector<Port> ports {
                    ResourcePort("pipeline", "GraphicsPipeline"),
                    optional(ResourcePort("framebuffer", "Framebuffer", "What it draws into: the pipeline's own, when not wired.")),
                    optional(ResourcePort("mesh", "Mesh", "What is drawn: a ForEach of Mesh's `field.mesh`, a mesh for each element — or one mesh, a Primitive's. None, for a vertex shader that makes its own vertices.")),
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
                    optional(hinted({ "idle", PortKind::eValue, "vector", false, "Wired from numbers, one for each of what 'cullIndex' counts — a PointShadows' idle: while this pass's is not zero it draws nothing, and what it draws into stays as it was. For what need not be drawn again every frame.", nullptr }, "matrix")) };
                types.push_back({ "Draw", "Passes", std::move(ports), {},
                                  "Draws with a graphics pipeline into a framebuffer. What a draw has of its own is given here — its `mesh`, and what the pipeline leaves "
                                  "unwired as `param.<name>` and `push.<name>`, a model matrix among them: wired from a ForEach, a draw for each of its elements; "
                                  "otherwise, one draw.", nullptr });
            }
            // What is worked out of values, by what kind of working out it is: a list of them all is too long to look down.
            static const std::set<std::string_view> vectors { "Length", "Normalize", "Dot", "Cross", "Distance", "Linear", "Reflect", "Refract", "Project" };
            static const std::set<std::string_view> logic { "Less", "LessOrEqual", "Greater", "GreaterOrEqual", "Equal", "NotEqual", "Not", "And", "Or", "Xor", "Select" };
            static const std::set<std::string_view> plain { "Number", "Bool", "String", "Vector", "Color", "Constant", "Component", "Split", "ImageExtent", "VertexLayout", "Append" };
            static const std::set<std::string_view> matrices { "MatrixMultiply", "MatrixInverse", "MatrixTranspose", "MatrixDeterminant", "MatrixTranslate", "MatrixRotate",
                                                               "MatrixAxisAngle", "MatrixScale", "MatrixCompose", "TransformPoint", "TransformDirection", "TransformPlane" };
            for (auto& type : types) {
                if (type.category == "Values" && !plain.contains(type.name))
                    type.category = vectors.contains(type.name) || matrices.contains(type.name) ? "Vectors" : logic.contains(type.name) ? "Logic" : "Math";
                type.keywords = Keywords(type.name);
                type.icon = IconOf(type.name, type.category);
                if (type.category == "Passes") {
                    Port enabled { "enabled", PortKind::eValue, "bool", false, "False to leave the pass out: it is not run, and nothing waits for it.", nullptr };
                    enabled.optional = true;
                    enabled.initial = Value(true);
                    type.inputs.push_back(std::move(enabled));
                }
                if (!type.builder)
                    for (auto& port : type.inputs)
                        if (port.initial.IsNull()) port.initial = Initial(type.name, port.name);
                // Where each input is set. A number, a truth, a vector or a colour of what works values out is set on the
                // node, where it is read; so is a pass's being on, a loop's count. What only a node can make —
                // a struct, a matrix, a vertex layout — is wired, from the node that sets it up. The rest is the inspector's.
                static const std::set<std::string_view> worked { "Values", "Math", "Vectors", "Logic", "Cameras" };
                static const std::set<std::string_view> lenses { "ShadowCascades", "PointShadows" };
                for (auto& port : type.inputs) {
                    if (port.kind != PortKind::eValue) continue;
                    const bool small = port.type == "number" || port.type == "bool" || port.type == "vector" || port.type == "value" || port.editor == "color";
                    if (port.type == "struct KoralVertexLayout") { port.place = Placement::eWire; port.companion = "VertexLayout"; }
                    else if (port.type == "struct" || port.type == "matrix" || port.type == "entities") port.place = Placement::eWire;
                    else if (lenses.contains(type.name)) port.place = Placement::eInspector;
                    else if (worked.contains(type.category) && type.name != "VertexLayout" && (small || !port.choices.empty() || type.name == "String")) port.place = Placement::eNode;
                    else if ((type.name == "Repeat" && port.name == "count") || (type.name == "Switch" && port.name == "index")
                             || (type.name == "Clear" && port.name == "color")) port.place = Placement::eNode;
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
        return ports;
    }
}
