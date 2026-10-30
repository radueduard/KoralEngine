#pragma once

/**
 * @file artifact.h
 * @brief What an importer makes of a file, as it is kept in the cache and read back: a model's meshes as numbers and
 *        what else it holds. Read back, it is as it was made, without the file or its importer.
 */

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "mesh.h"
#include "model.h"

namespace kasset
{
    /**
     * @brief A model file as it was read. What in it names a file is named as the file had it, not as a project does:
     *        a node's part's mesh as `#<index>` among the file's meshes, and a material's pictures by their paths
     *        from the model's folder (or whole, as the file gave them). Whoever reads it names them as it names files.
     */
    struct ModelArtifact {
        Model model;
        struct Mesh {
            int index = -1;         ///< Among the file's meshes: one with no triangles is not here, and the rest keep theirs.
            std::string name;       ///< What the file calls it.
            VertexData data;
            bool operator==(const Mesh&) const = default;
        };
        std::vector<Mesh> meshes;
    };

    [[nodiscard]] std::vector<std::byte> Serialize(const ModelArtifact& artifact);
    /** @brief One mesh, as a generator made it: kept so that what has it has it without running the generator. */
    [[nodiscard]] std::vector<std::byte> Serialize(const VertexData& mesh);
    [[nodiscard]] std::expected<VertexData, std::string> DeserializeMesh(std::span<const std::byte> bytes);
    /** @brief The artifact @p bytes keep, or why they are none: not one, of another version, cut short. */
    [[nodiscard]] std::expected<ModelArtifact, std::string> DeserializeModel(std::span<const std::byte> bytes);
}
