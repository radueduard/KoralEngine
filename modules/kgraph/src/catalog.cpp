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
            for (const char* name : { "Add", "Subtract", "Multiply", "Divide", "Min", "Max" })
                value(name, { Any("a"), Any("b") }, "value", "Of two numbers, or of two vectors a component at a time; a number goes with every component of a vector.");
            for (const char* name : { "Negate", "Abs", "Floor", "Ceil", "Round" })
                value(name, { Any("a") }, "value", "Of a number, or of each component of a vector.");
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
