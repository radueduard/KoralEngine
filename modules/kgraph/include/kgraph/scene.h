#pragma once

/**
 * @file scene.h
 * @brief What the graph knows of a scene's cameras: nothing by name. A scene's Camera, its lights and its Sky are
 *        scripts of the engine's package (kscript::PackageDirectory), components like any script's; a ForEach node
 *        reads any of them, and a View node works out what one sees.
 *
 * Where a camera or a light is, and which way it looks or shines, is its entity's transform: forward is -Z.
 */

#include <kmath/matrix.h>
#include <kmath/vector.h>

#include <kecs/kecs.h>

#include "entities.h"
#include "view.h"

namespace kgraph
{
    /**
     * @brief What @p entity sees: from where it is, turned as it is — its scale is no part of a view — through the
     *        `fieldOfView` (degrees), `near` and `far` of the first of its components that has a `fieldOfView` — or, of one
     *        with a `height`, a `near` and a `far` and no field of view (an OrthographicCamera), in a box that tall.
     */
    [[nodiscard]] View ViewOf(const kecs::World& world, kecs::Entity entity);
    /** @brief A kecs matrix as the framework's, and back: the same sixteen floats. */
    [[nodiscard]] kor::Mat4 ToKor(const kecs::Mat4& matrix);
    [[nodiscard]] kecs::Mat4 ToKecs(const kor::Mat4& matrix);
}
