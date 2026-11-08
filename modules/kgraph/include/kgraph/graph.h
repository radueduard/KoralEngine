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
#include "textures.h"
#include "value.h"

namespace kgraph
{
    /**
     * @brief How often a pass runs. Setup once, when it is built (and when the scene starts or stops playing); fixed on
     *        the fixed step, whatever the frame rate; every frame. They run in that order: setup, fixed, every frame.
     */
    enum class PassRate : std::uint8_t { eFrame, eSetup, eFixed };
    /** @brief What a rate is called: `frame`, `setup`, `fixed`. */
    [[nodiscard]] std::string_view PassRateName(PassRate rate);

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
         * Which of the passes are setup: what is made once rather than every frame — a noise texture, a table worked
         * out on the device, a buffer cleared to begin with. Each is one of `passes`, and they come first in it, in the
         * order they run. A pass node on a setup page records on the frame after it is built, and again only when it is
         * built again: when what it is wired to is — an image resized, a shader edited, a picture arrived from its
         * file. Saved apart from the rest (`"setup": [...]`).
         */
        std::vector<std::string> setup;
        /**
         * Which of the passes run on the fixed step: what must not go faster or slower with the frame rate — a
         * simulation. Each is one of `passes`, after the setup pages and before those of every frame, in the order they
         * run. A pass node on a fixed page records on a frame that has a fixed step in it (Runtime::FixedStep) and rests
         * on one that has none. Saved apart from the rest (`"fixed": [...]`).
         */
        std::vector<std::string> fixed;
        /**
         * Which page runs after which, where what they use does not say: each `{ page, before }` — `page` runs after
         * `before`. What the pages read and write orders the rest (a page that reads what another writes runs after it,
         * whatever is said here); two that both write something need one of these between them, or they are a tie
         * (an editor says so). Only pages of the same rate are ordered by them: setup, the fixed step and every frame
         * run in that order whatever. `passes` is put in an order that keeps them (OrderByRate). Saved as `"after"`.
         */
        std::vector<std::pair<std::string, std::string>> after;
        /**
         * Whether the order is `after`'s: true for a graph read with an `"after"` (even an empty one), or settled by an
         * editor. False for one made in code or saved before there were links: its pages are in the order `passes` lists
         * them, which an editor makes links of where it matters (each pair that writes the same thing), once it knows.
         */
        bool orderSettled = false;
        /**
         * How many fixed steps a frame runs at most: its fixed pages are in the frame that many times over, each time
         * after the last, and those beyond the steps it has rest. A frame with more steps than this loses the rest —
         * the simulation slows rather than falling further behind. Saved when it is not 4 (`"fixedStepsAtMost"`).
         */
        int fixedStepsAtMost = 4;
        /**
         * How each model file the scene has meshes of is read, by the file as its meshes' names have it (`sponza.gltf`
         * of `@sponza.gltf#3`): which attributes are left out, and how the rest are put in buffers. A file not here is
         * read whole, in one buffer. Changed, the file is read again, and what held its meshes holds the new ones.
         */
        std::map<std::string, MeshImport> meshImports;
        /**
         * How each picture file the scene reads is kept, by its asset id (`@wall.png`): block-compressed, cooked into the
         * project's `.koral/cache/textures`, or — not here — as it is read. Changed, it is read again, cooked or not.
         */
        std::map<std::string, TextureImport> textureImports;
        /**
         * Pictures made of the channels of others, by their own ids (`@wall_ORM.pack`). Each file one is made of reads,
         * to whoever holds it, as the pack's channel it went into: the scene names the files, and the device has one
         * picture of them.
         */
        std::map<std::string, TexturePack> texturePacks;
        /**
         * What the default meshes (`$cube`, a Shape node's) have of their surface's directions: a TANGENT of four
         * numbers, a TANGENT and a BITANGENT, or neither. Changed, they are made again, and what held them holds the new.
         */
        Tangents defaultMeshTangents = Tangents::eFour;
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
        /** @brief How often @p pass runs: once (setup), on the fixed step, or every frame — which a pass is when nothing says. */
        [[nodiscard]] PassRate RateOf(std::string_view pass) const;
        /** @brief Whether @p pass is a setup page. @see setup */
        [[nodiscard]] bool IsSetup(std::string_view pass) const;
        /** @brief Whether @p pass is a page of the fixed step. @see fixed */
        [[nodiscard]] bool IsFixed(std::string_view pass) const;
        /** @brief Makes @p pass one of @p rate — the last of them — and puts the passes in order. False for no such pass. */
        bool SetRate(std::string_view pass, PassRate rate);
        /**
         * @brief Puts the passes in the order of their rates — setup, then fixed, then every frame — and in each the
         *        order `after` says, those it says nothing of keeping their places; forgets in `setup` and `fixed` what
         *        is no pass (or, in `fixed`, what is setup already), and in `after` what names no pass. A ring of links
         *        is broken where it closes, the earliest first.
         */
        void OrderByRate();
        /** @brief Whether @p page runs after @p before by `after`'s links, one or several in a row. */
        [[nodiscard]] bool RunsAfter(std::string_view page, std::string_view before) const;
        /** @brief Adds a node of @p type, and returns it to be filled in. */
        Node& Add(std::string id, std::string type);

        /** @brief As JSON: what a saved graph is. */
        [[nodiscard]] std::string ToJson() const;
        /** @brief Reads a saved graph. False, with @p error saying why, when @p json is not one. */
        [[nodiscard]] static bool FromJson(std::string_view json, Graph& out, std::string& error);
    };
}
