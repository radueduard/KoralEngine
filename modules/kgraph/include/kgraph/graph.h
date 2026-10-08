#pragma once

/**
 * @file graph.h
 * @brief A scene's pass graph, as data: nodes, what each input holds, and which outputs they are wired to.
 *
 * This is what is edited and saved. It builds nothing and knows no GPU: a Runtime turns it into objects and
 * passes. An input holds a value, or is wired to another node's output; a wire is kept on the input it goes into,
 * since an input takes one.
 *
 * Input names follow the node's type. A builder node's are its builder's settings: `format` for a setting of one
 * argument, `extent.x` for one of several, `color[0].view` for one that is given more than once. A pipeline node
 * also has `param.<name>` for each of its shaders' parameters and `constant.<name>` for each specialization
 * constant. @see catalog.h
 */

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "value.h"

namespace kgraph
{
    /** @brief A wire's far end: an output of another node. Most nodes have one output, "out". */
    struct Link {
        std::string node;
        std::string output = "out";
        bool operator==(const Link&) const = default;
    };

    /** @brief What an input holds: a value, or a wire to an output (which wins). */
    struct Input {
        Value value;
        std::optional<Link> link;
        bool operator==(const Input&) const = default;
    };

    struct Node {
        std::string id;                         ///< Unique in the graph: what wires and saved files name it by.
        std::string type;                       ///< "Image", "ComputePipeline", "Dispatch", "Add"... @see Catalog
        std::string title;                      ///< What a person calls it; its id when empty.
        std::map<std::string, Input> inputs;
        double x = 0, y = 0;                    ///< Where it is in the editor.
        bool operator==(const Node&) const = default;

        Node& Set(std::string input, Value value) { inputs[std::move(input)] = Input { std::move(value), std::nullopt }; return *this; }
        Node& Wire(std::string input, std::string node, std::string output = "out")
        {
            inputs[std::move(input)] = Input { {}, Link { std::move(node), std::move(output) } };
            return *this;
        }
    };

    struct Graph {
        std::vector<Node> nodes;
        bool operator==(const Graph&) const = default;

        [[nodiscard]] const Node* Find(std::string_view id) const;
        [[nodiscard]] Node* Find(std::string_view id);
        /** @brief Adds a node of @p type, and returns it to be filled in. */
        Node& Add(std::string id, std::string type);

        /** @brief As JSON: what a saved graph is. */
        [[nodiscard]] std::string ToJson() const;
        /** @brief Reads a saved graph. False, with @p error saying why, when @p json is not one. */
        [[nodiscard]] static bool FromJson(std::string_view json, Graph& out, std::string& error);
    };
}
