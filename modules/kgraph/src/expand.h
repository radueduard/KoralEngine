//
// kgraph: a graph with its loops written out. A Repeat node is a count; every node wired from one, straight or
// through others, is there once for each turn of it — `down [0]`, `down [1]` — and what it takes of the Repeat is
// that turn's number. A Previous is what a node of the loop made the turn before, and an Element what it made on
// one turn, for whatever is outside the loop. What comes of it is an ordinary graph, which is what is run.
//

#pragma once

#include <map>
#include <string>
#include <vector>

#include <set>

#include <kgraph/catalog.h>
#include <kgraph/graph.h>

namespace kgraph
{
    struct Expansion {
        Graph graph;                                                ///< What runs: no Repeat, Previous or Element in it.
        std::map<std::string, std::string, std::less<>> errors;     ///< A node of the graph as written, and why it could not be written out.
        /// A node of a loop, and what it is on each turn: `down` is `down [0]`, `down [1]`...
        std::map<std::string, std::vector<std::string>, std::less<>> turns;
        /** A Function node, as it was written out: what it calls, its ports, and what each output is. */
        struct Call {
            std::string function;
            std::vector<Port> inputs;                                       ///< A port for each FunctionInput, top to bottom.
            std::vector<std::string> outputs;                               ///< A name for each FunctionOutput, top to bottom.
            std::map<std::string, Link, std::less<>> gives;                 ///< What each output is, written out: an inner node's.
            std::vector<std::string> nodes;                                 ///< The nodes it was written out into: `<call>/<node>`.
        };
        std::map<std::string, Call, std::less<>> calls;
        /// FunctionInput and FunctionOutput nodes of the graph itself — a function opened in place — and what each is.
        std::map<std::string, Input, std::less<>> aliases;
    };

    /** @brief The port a FunctionInput or FunctionOutput of kind @p kind (eValue, eEntities, eMesh, eBuffer, eImage, eResource) is. */
    [[nodiscard]] Port FunctionPort(std::string name, std::string_view kind, std::string doc = {});

    /** @brief What a node of a loop is called on turn @p turn. */
    [[nodiscard]] std::string TurnId(std::string_view id, int turn);
    [[nodiscard]] Expansion Expand(const Graph& written);
}
