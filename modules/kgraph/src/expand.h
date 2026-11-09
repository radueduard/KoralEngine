//
// kgraph: a graph with its loops written out. A Repeat node is a count; every node wired from one, straight or
// through others, is there once for each turn of it — `down [0]`, `down [1]` — and what it takes of the Repeat is
// that turn's number. A Previous is what a node of the loop made the turn before, and an Element what it made on
// one turn, for whatever is outside the loop. What comes of it is an ordinary graph, which is what is run.
//

#pragma once

#include <functional>
#include <map>
#include <optional>
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
            std::string called;                                             ///< The call's id: its own, or — of a Reference to it — the call's.
        };
        std::map<std::string, Call, std::less<>> calls;
        /// FunctionInput and FunctionOutput nodes of the graph itself — a function opened in place — and what each is.
        std::map<std::string, Input, std::less<>> aliases;
        /// What each Bundle there is holds, by `<node>.<output>` (a Bundle node's `out`, a ForEach's `element`, an Unbundle's `of`): each
        /// member's name and what it is — a wire, or a value given.
        std::map<std::string, std::vector<std::pair<std::string, Input>>, std::less<>> bundles;
        /// The inputs of each node a Bundle filled, by name: a node that has no such input lets them pass, and says so.
        std::map<std::string, std::set<std::string>, std::less<>> fromBundle;
        /// What a node that takes a Bundle says of it as it is: the names that matched nothing, on either side.
        std::map<std::string, std::string, std::less<>> notes;
        /// The Draws `batched` that were written out as one indirect draw of every element: `<draw>/batch`, a Filter of
        /// them grouped by mesh; `<draw>/pack`, their meshes as one; `<draw>/commands`, a command each mesh.
        std::set<std::string, std::less<>> batched;
        /// A Draw `batched` that could not be, and why: it draws one at a time.
        std::map<std::string, std::string, std::less<>> unbatched;
        /// A Repeat whose count is wired, by its id as written out (`inner [2]`, of a loop in a loop): what it is wired from.
        /// How many turns it has is what that gives once the graph has run — Expand's `counts` — and one until then.
        std::map<std::string, Link, std::less<>> wiredCounts;
    };

    /**
     * @brief What a ForEach's (an Entity's, an Entities node's, a Filter's) `element` holds: each member's name, and which
     *        of the node's outputs it is. The runtime's to say: it knows the components' fields.
     */
    using ElementOf = std::function<std::vector<std::pair<std::string, std::string>>(const Node&)>;

    /**
     * @brief The fields of a component a buffer holds, by its name, in the order it declares them: each field's name and its
     *        type as a BufferBuilder's layout has it (LayoutTypeOf). Null when there is no such component. The runtime's to say.
     */
    using FieldsOf = std::function<std::optional<std::vector<std::pair<std::string, std::string>>>(std::string_view component)>;

    /**
     * @brief Whether a ForEach's (an Entity's...) output @p name is of where the entity is — `matrix`, `worldPosition`,
     *        `forward`... — what its `transform` Bundle holds.
     */
    [[nodiscard]] bool IsPlacementOutput(std::string_view name);

    /**
     * @brief The Bundles a ForEach (an Entity, an Entities node, a Filter) gives besides its `element`: `transform`, where
     *        the entity is; and one of each component's fields, by the component's name — its own (`component`), and each
     *        of `with`'s. What each holds is its members: of `transform`, the placement outputs; of a component, its fields.
     */
    [[nodiscard]] std::vector<std::string> EachBundles(const Node& node);

    /** @brief Where a Bundle taken at input @p input of a node of type @p type goes: the prefix of the inputs it fills. Empty: none. */
    [[nodiscard]] std::string BundleTaken(std::string_view type, std::string_view input);

    /** @brief The port a FunctionInput or FunctionOutput of kind @p kind (eValue, eEntities, eMesh, eBuffer, eImage, eResource) is. */
    [[nodiscard]] Port FunctionPort(std::string name, std::string_view kind, std::string doc = {});

    /** @brief What a node of a loop is called on turn @p turn. */
    [[nodiscard]] std::string TurnId(std::string_view id, int turn);
    /// @p counts: how many turns each Repeat whose count is wired has, by its id as written out (Expansion::wiredCounts), as
    /// the graph last gave it: one, for any not in it.
    [[nodiscard]] Expansion Expand(const Graph& written, const ElementOf& elementOf = {}, const FieldsOf& fieldsOf = {},
                                   const std::map<std::string, int, std::less<>>& counts = {});
    /**
     * @brief Why the Draw @p draw of @p graph (a graph as it runs: its calls and Bundles written out) cannot be written out
     *        as one indirect draw of every element — `batched` — or nothing when it can. @p each, when given, is the
     *        inputs it is wired a value of each element at, besides its mesh: what its shader would read from its
     *        [entity("PER_INSTANCE")] buffer instead.
     */
    [[nodiscard]] std::string WhyNotBatched(const Graph& graph, const Node& draw, std::vector<std::string>* each = nullptr);
}
