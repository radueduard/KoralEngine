#pragma once

// How a builder's settings are spelled as a node's inputs, in one place for the catalog and the runtime.

#include <cctype>
#include <format>
#include <string>
#include <string_view>

#include <builderDescriptions.h>

namespace kgraph
{
    inline bool IsPipeline(const std::string_view builder) { return builder.ends_with("Pipeline"); }

    /** A pipeline's settings that are worked out from what is wired, and so are no inputs of their own. */
    inline bool HiddenPipelineSetting(const std::string_view setting)
    {
        return setting == "binding" || setting == "specializationConstant" || setting == "specializationConstantNamed";
    }

    /** `format` for a setting of one argument, `extent.x` for one of several. One given more than once has `[i]` after its name. */
    inline std::string InputName(const kor::builders::Setting& setting, const kor::builders::Field& argument, const int index = -1)
    {
        std::string name(setting.name);
        if (setting.repeatable) name += index < 0 ? "[]" : std::format("[{}]", index);
        if (setting.arguments.size() > 1) name += std::format(".{}", argument.name);
        return name;
    }
}

namespace kgraph
{
    inline bool IsShader(const kor::builders::Field& argument)
    {
        return argument.type->kind == kor::builders::Kind::eResource && argument.type->name == "Shader";
    }

    /**
     * What a pipeline's setting is called as a node's input. One that takes a shader is named for the stage it is —
     * `vertex`, `fragment`, `tessellationControl`, `miss[0]`, `hitGroup[0].closestHit` — and the node has
     * `<stage>.module` and `<stage>.entry` for it. What goes with a shader goes by the stage too (`vertex.layout`),
     * or by its own name (`patchControlPoints`). Anything else is as InputName has it.
     */
    inline std::string PipelineInputName(const kor::builders::Setting& setting, const kor::builders::Field& argument, const int index = -1)
    {
        bool shaders = false;
        for (const auto& each : setting.arguments) shaders = shaders || IsShader(each);
        if (!shaders) return InputName(setting, argument, index);
        const std::string_view name = setting.name;
        const std::string at = !setting.repeatable ? std::string() : index < 0 ? std::string("[]") : std::format("[{}]", index);
        if (name.ends_with("Shader")) {
            const std::string stage = std::string(name.substr(0, name.size() - 6)) + at;
            return argument.name == "shader" ? stage : std::format("{}.{}", stage, argument.name);
        }
        if (name.ends_with("State")) {
            if (!IsShader(argument)) return std::string(argument.name);
            std::string part(argument.name);
            if (!part.empty()) part[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
            return std::string(name.substr(0, name.size() - 5)) + at + part;
        }
        return std::format("{}{}.{}", name, at, argument.name);
    }

    /** Which choice of a graphics pipeline's `stages` a setting belongs to; empty when it is there for either. */
    inline std::string_view StageMode(const std::string_view setting)
    {
        if (setting == "vertexShader" || setting == "tessellationState" || setting == "geometryShader" || setting == "inputAssemblyState") return "eVertex";
        if (setting == "taskShader" || setting == "meshShader") return "eMesh";
        return {};
    }

    /** Whether a component field of this type holds a resource: a shared reference to one, whatever its kind. */
    inline bool IsResourceType(const std::string_view type)
    {
        return type == "resource" || type == "Image" || type == "Buffer" || type == "Sampler" || type == "BufferView" || type == "AccelerationStructure"
            || type == "Mesh";
    }
}
