#pragma once

// How a builder's settings are spelled as a node's inputs, in one place for the catalog and the runtime.

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
    /** Whether a component field of this type holds a resource: a shared reference to one, whatever its kind. */
    inline bool IsResourceType(const std::string_view type)
    {
        return type == "resource" || type == "Image" || type == "Buffer" || type == "Sampler" || type == "BufferView" || type == "AccelerationStructure"
            || type == "Mesh";
    }
}
