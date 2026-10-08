#pragma once

/**
 * @file catalog.h
 * @brief Every kind of node there is, and the inputs and outputs each has: what an editor offers and draws ports for.
 *
 * Most come from the framework's builder descriptions, one node type a builder — Image, Sampler, Shader, the
 * pipelines — whose inputs are the builder's settings. The rest are the engine's own: values and what is worked
 * out from them, the screen, a reference to another node, and the passes that run.
 *
 * A pipeline node has more inputs than are listed here, which depend on its shaders: `param.<name>` for each
 * descriptor they declare and `constant.<name>` for each specialization constant. Runtime::PortsOf gives those.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "value.h"

namespace kor::builders { struct Description; struct Type; }

namespace kgraph
{
    enum class PortKind : std::uint8_t {
        eValue,     ///< A Value: given on the node, or wired from a node that works one out.
        eResource,  ///< A GPU object: always wired, from the node that makes it.
    };

    struct Port {
        std::string name;
        PortKind kind = PortKind::eValue;
        /// What it takes or gives. A value: "bool", "number", "string", "vector", "enum kor::Image::Format",
        /// "flags kor::Image::Usage", "struct KoralRasterizationState". A resource: "Image", "Shader", or
        /// "Resource" for any.
        std::string type;
        bool repeatable = false;    ///< Given any number of times: `color[0].view`, `color[1].view`.
        std::string doc;
        const kor::builders::Type* builder = nullptr;   ///< The builder argument it is, for a builder node's input.
        /// Not one of the few a node of this type cannot do without: an editor shows it once it is asked for, or given.
        bool optional = false;
        std::vector<std::string> choices;   ///< When it is one of a few names: what they are.
        /// What kind of thing a value is, beyond its type, for an editor to offer the right control: "color",
        /// "module" (a shader module), "entry" (an entry point of the module beside it), "node" (a node's id),
        /// "components" (component names), "parameter" (one of a pipeline's image parameters), "attributes".
        std::string editor;
        /// The setting of the node's `stages` input it belongs to ("eVertex", "eMesh"); empty when it is there whatever that is.
        std::string mode;
        /// What a node starts with for it, when it is one a node cannot do without; what it is when it is not given
        /// otherwise; and, for a struct, what the fields that are not given are. Null when there is nothing to say.
        Value initial;
    };

    struct NodeType {
        std::string name;
        std::string category;       ///< "Resources", "Pipelines", "Values", "Passes".
        std::vector<Port> inputs;
        std::vector<Port> outputs;
        std::string doc;
        const kor::builders::Description* builder = nullptr;    ///< For a builder node.
        /// Whether an editor offers it. A Shader is not: a pipeline names its shaders itself. One still loads and runs.
        bool listed = true;
        /// Other words someone looking for it might type: "texture" for an Image, "mix" for a Lerp.
        std::string keywords;
    };

    struct Node;
    /**
     * @brief The inputs of @p type that a node set up as @p node is has: all of them, but for those that belong to
     *        another choice of its `stages` (a graphics pipeline drawn with mesh shaders has no vertex shader).
     */
    [[nodiscard]] std::vector<Port> PortsFor(const NodeType& type, const Node& node);
    /**
     * @brief The inputs @p whole stands for when it is one given in parts (`groups` → `groups.x`, `.y`, `.z`); none
     *        when it is not such an input. A node gives the whole or its parts, not both.
     */
    [[nodiscard]] std::vector<std::string> PartsOf(const NodeType& type, std::string_view whole);
    /** @brief What a graphics pipeline node's `stages` is: "eVertex" unless it says "eMesh". */
    [[nodiscard]] std::string StagesOf(const Node& node);

    /** @brief Every node type. */
    [[nodiscard]] const std::vector<NodeType>& Catalog();
    /** @brief The node type so named, or null. */
    [[nodiscard]] const NodeType* FindType(std::string_view name);
}
