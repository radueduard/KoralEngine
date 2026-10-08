//
// kgraph: the kinds of node. One a builder the framework describes, and the engine's own.
//

#include <kgraph/catalog.h>

#include <algorithm>
#include <format>

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

        std::vector<NodeType> Build()
        {
            std::vector<NodeType> types;

            // ---- one a builder ----------------------------------------------------------------------------------
            for (const auto& description : kor::builders::All()) {
                NodeType type;
                type.name = std::string(description.name);
                type.category = IsPipeline(description.name) ? "Pipelines" : "Resources";
                type.doc = std::string(description.doc);
                type.builder = &description;
                for (const auto& field : description.constructor) type.inputs.push_back(PortOf(std::string(field.name), field, false, field.doc));
                for (const auto& setting : description.settings) {
                    // A pipeline's bindings and constants are the engine's to give, from what is wired: its
                    // `param.` and `constant.` inputs stand for them.
                    if (IsPipeline(description.name) && HiddenPipelineSetting(setting.name)) continue;
                    for (const auto& argument : setting.arguments) {
                        if (argument.type->kind == Kind::eBytes) continue;   // initial data is not a graph's to hold
                        type.inputs.push_back(PortOf(InputName(setting, argument), argument, setting.repeatable, setting.doc));
                    }
                }
                type.outputs.push_back(ResourcePort("out", std::string(description.result)));
                types.push_back(std::move(type));
            }

            // ---- values, and what is worked out from them ---------------------------------------------------------
            const auto value = [&](std::string name, std::vector<Port> inputs, std::string out, std::string doc) {
                types.push_back({ std::move(name), "Values", std::move(inputs), { { "out", PortKind::eValue, std::move(out), false, {}, nullptr } }, std::move(doc), nullptr });
            };
            value("Number", { Number("value") }, "number", "A number.");
            value("Bool", { { "value", PortKind::eValue, "bool", false, {}, nullptr } }, "bool", "True or false.");
            value("String", { { "value", PortKind::eValue, "string", false, {}, nullptr } }, "string", "Text.");
            value("Vector", { Number("x"), Number("y"), Number("z"), Number("w") }, "vector", "As many numbers as are given, x first.");
            for (const char* name : { "Add", "Subtract", "Multiply", "Divide", "Min", "Max" })
                value(name, { Any("a"), Any("b") }, "value", "Of two numbers, or of two vectors a component at a time; a number goes with every component of a vector.");
            for (const char* name : { "Negate", "Abs", "Floor", "Ceil", "Round" })
                value(name, { Any("a") }, "value", "Of a number, or of each component of a vector.");
            value("Component", { { "vector", PortKind::eValue, "vector", false, {}, nullptr }, Number("index") }, "number", "One component of a vector: 0 the first.");
            value("ImageExtent", { ResourcePort("image", "Image") }, "vector", "An image's width, height and depth.");

            // ---- the screen, and a node by name -------------------------------------------------------------------
            types.push_back({ "Screen", "Resources", {}, { ResourcePort("out", "Image") }, "The scene's window: what a pass writes to show something.", nullptr });
            types.push_back({ "Reference", "Resources", { { "node", PortKind::eValue, "string", false, "The id of the node it stands for.", nullptr } },
                              { ResourcePort("out", "Resource") },
                              "Another node's output, by the node's id: a resource made elsewhere in the graph, without a wire across it.", nullptr });

            // ---- passes -------------------------------------------------------------------------------------------
            types.push_back({ "Clear", "Passes", { ResourcePort("image", "Image"), { "color", PortKind::eValue, "vector", false, "Red, green, blue, alpha.", nullptr } }, {},
                              "Fills an image with one colour.", nullptr });
            types.push_back({ "Blit", "Passes", { ResourcePort("source", "Image"), ResourcePort("destination", "Image") }, {},
                              "Copies an image onto another, scaled to fit: onto the screen, to show it.", nullptr });
            types.push_back({ "Dispatch", "Passes",
                              { ResourcePort("pipeline", "ComputePipeline"), Number("groups.x"), Number("groups.y"), Number("groups.z") }, {},
                              "Runs a compute pipeline: so many groups each way, 1 where not given.", nullptr });
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
}
