#pragma once

/**
 * @file runtime.h
 * @brief A graph, running: its nodes built into GPU objects, and its passes in a frame graph.
 *
 * @code
 * kgraph::Runtime runtime(scene.Graph());       // the scene's frame graph
 * runtime.Set(graph);                           // builds what is new or changed, and nothing else
 * // each frame, before the frame graph runs:
 * runtime.Update();                             // picks up shaders edited since
 * @endcode
 *
 * Set works out every node in the order its wires need, and builds a node again only when what goes into it has
 * changed — its inputs' values, or an object it is wired to having been rebuilt. A node that cannot be built says
 * why (Status), and so does everything that needed it; the rest of the graph runs.
 *
 * Passes run in the order their resources need, not the order they are in: one that writes an image runs before
 * one that reads it. Each resource a pass uses is imported into the frame graph under its node's id.
 */

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <image.h>
#include <resource.h>

#include <kecs/kecs.h>

#include "catalog.h"
#include "graph.h"

struct KoralResource;
namespace kor { class FrameGraph; }

namespace kgraph
{
    /**
     * @brief A GPU object a node made. Shared by whoever uses it — the nodes wired to it, the passes that run with
     *        it — and freed when none does.
     */
    struct Resource {
        KoralResource* handle = nullptr;    ///< Owned. Null for the screen.
        std::string kind;                   ///< "Image", "Shader", "ComputePipeline"...
        std::string node;                   ///< The node that made it.
        bool screen = false;                ///< The scene's window, which is the frame graph's to give.
        std::shared_ptr<Resource> source;   ///< For a view: what it is a view of.
        /// What its node made in its place, when the node was built again: whoever holds this one is after that one.
        std::shared_ptr<Resource> replacedBy;

        Resource() = default;
        Resource(const Resource&) = delete;
        Resource& operator=(const Resource&) = delete;
        ~Resource();
    };

    /** @brief How a node fared when the graph was last set. */
    struct NodeStatus {
        bool ok = false;
        std::string error;          ///< Why not, when not ok.
        std::uint64_t builds = 0;   ///< How many times it has been built: unchanged by a Set that changed nothing of it.
    };

    /** @brief What a pass that runs for each entity did, the last frame it was readied for. */
    struct PassReport {
        std::size_t ran = 0;        ///< Entities it runs for.
        std::size_t skipped = 0;    ///< Entities that have what it asks for, and that it cannot run for.
        std::string why;            ///< Why the first of those: a parameter with nothing in it, a mesh that does not fit.
    };

    class Runtime {
    public:
        /**
         * @brief Runs its passes in @p frameGraph, which must outlive it.
         * @param world Where the entities are that pipelines run for, and where each pipeline's component is
         *              registered. It must outlive the runtime too. With none, a pass that runs per entity is an error.
         */
        explicit Runtime(kor::FrameGraph& frameGraph, kecs::World* world = nullptr);
        ~Runtime();
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        /** @brief Makes @p graph what runs. On the frame graph's thread, between frames. */
        void Set(Graph graph);
        [[nodiscard]] const Graph& Current() const { return _graph; }
        /**
         * @brief Each frame, before the frame graph runs. Builds again what depends on a shader that was edited,
         *        and readies the passes that run per entity: which entities, and each one's descriptor set.
         */
        void Update();

        /**
         * @brief Keeps @p resource until the frame that may be drawing with it is done: for whoever takes one from
         *        where a pass could find it — an entity's field — in the middle of a frame. What a graph's own edits
         *        replace is kept so already.
         */
        void Retire(std::shared_ptr<Resource> resource);

        /** @brief How node @p id fared, or null when there is none. */
        [[nodiscard]] const NodeStatus* Status(std::string_view id) const;
        /** @brief Whether every node is ok. */
        [[nodiscard]] bool Ok() const;
        /** @brief Every node that is not ok, with why: "id: error". */
        [[nodiscard]] std::vector<std::string> Problems() const;

        /** @brief What node @p id 's @p output is, when it is a resource; null otherwise. */
        [[nodiscard]] std::shared_ptr<Resource> ResourceOf(std::string_view id, std::string_view output = "out") const;
        /** @brief The same, as the image it is; empty when it is not one. */
        [[nodiscard]] kor::ResourceRef<const kor::Image> ImageOf(std::string_view id, std::string_view output = "out") const;
        /** @brief What node @p id 's @p output is, when it is a value; null otherwise. */
        [[nodiscard]] Value ValueOf(std::string_view id, std::string_view output = "out") const;

        /**
         * @brief Every input node @p id has: its type's, and for a pipeline those its shaders give it — `param.<name>`
         *        and `constant.<name>` — once they are built.
         */
        [[nodiscard]] std::vector<Port> PortsOf(std::string_view id) const;
        /**
         * @brief A pipeline node's parameters nobody wired: the ones each entity that runs it will supply.
         *        Empty when everything is set, and the pipeline runs once.
         */
        [[nodiscard]] std::vector<std::string> PerEntityParameters(std::string_view id) const;

        /**
         * @brief The component generated for pipeline node @p id: what an entity has, to be run by it. It is named
         *        after the node, with a field for each parameter left unwired and each push constant. NoComponent
         *        when the pipeline has neither, and so runs once.
         */
        [[nodiscard]] kecs::ComponentId ComponentOf(std::string_view id) const;
        /** @brief What became of that component's fields when the pipeline last changed its shape; null when it never did. */
        [[nodiscard]] const kecs::Migration* MigrationOf(std::string_view id) const;
        /** @brief How pass node @p id, which runs per entity, fared this frame; null for another kind of node. */
        [[nodiscard]] const PassReport* ReportOf(std::string_view id) const;
        /**
         * @brief The inputs taken off pipeline nodes because an edited shader no longer has them, or has them as
         *        another kind: "node.input", in the order it happened. Current() no longer has them. Emptied by the call.
         */
        [[nodiscard]] std::vector<std::string> TakeReset();

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
        Graph _graph;
    };
}
