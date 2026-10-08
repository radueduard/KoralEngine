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
    };

    struct NodeType {
        std::string name;
        std::string category;       ///< "Resources", "Pipelines", "Values", "Passes".
        std::vector<Port> inputs;
        std::vector<Port> outputs;
        std::string doc;
        const kor::builders::Description* builder = nullptr;    ///< For a builder node.
    };

    /** @brief Every node type. */
    [[nodiscard]] const std::vector<NodeType>& Catalog();
    /** @brief The node type so named, or null. */
    [[nodiscard]] const NodeType* FindType(std::string_view name);
}
