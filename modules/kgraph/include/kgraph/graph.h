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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "meshes.h"
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
        /// The pass it is part of: a page of the graph, by name. Empty for one that is in none. @see Graph::passes
        std::string pass;
        /// Where an input is set, where this node says otherwise than its type: "node" (a control on the node) or
        /// "inspector", by the input's name. One its type has wired only is wired only, whatever this says. @see Placement
        std::map<std::string, std::string> placed;
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
        /**
         * The passes a frame is made in, in the order they are drawn: each a page of the graph, with the nodes whose
         * `pass` names it — what the pass makes for itself, what it runs, and References to what earlier passes made.
         * A pass is a way of laying a graph out and of ordering what it does where what is used does not say: a wire
         * goes from any node to any other, whichever passes they are in.
         */
        std::vector<std::string> passes;
        /**
         * How each model file the scene has meshes of is read, by the file as its meshes' names have it (`sponza.gltf`
         * of `@sponza.gltf#3`): which attributes are left out, and how the rest are put in buffers. A file not here is
         * read whole, in one buffer. Changed, the file is read again, and what held its meshes holds the new ones.
         */
        std::map<std::string, MeshImport> meshImports;
        /**
         * The functions its Function nodes call, by name: each a graph of its own, with FunctionInput and FunctionOutput
         * nodes at its edges. Not saved with it: whoever has the graph says where they come from — a project's
         * `functions` folder. Before it runs, each call is written out into the nodes of what it calls.
         */
        std::map<std::string, std::shared_ptr<const Graph>> functions;
        /**
         * Where each node is when every node is seen at once, by id: a layout of its own, apart from where it is on its
         * pass's page (Node::x, y). A node not here is where its page has it, beside the other passes' (an editor's to say).
         */
        std::map<std::string, std::pair<double, double>> overview;
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
