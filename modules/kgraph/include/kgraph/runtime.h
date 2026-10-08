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

    class Runtime {
    public:
        /** @brief Runs its passes in @p frameGraph, which must outlive it. */
        explicit Runtime(kor::FrameGraph& frameGraph);
        ~Runtime();
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        /** @brief Makes @p graph what runs. On the frame graph's thread, between frames. */
        void Set(Graph graph);
        [[nodiscard]] const Graph& Current() const { return _graph; }
        /** @brief Each frame, before the frame graph runs: builds again what depends on a shader that was edited. */
        void Update();

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

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
        Graph _graph;
    };
}
