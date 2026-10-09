#pragma once

/** @file view.h  Where a scene is seen from: what a SceneView node works its camera out of. */

#include <kmath/matrix.h>

namespace kgraph
{
    /** @brief Where something is seen from, and the lens it is seen through. */
    struct View {
        kor::Mat4 view { 1.f };
        float fieldOfView = 1.0471976f;     ///< Top to bottom, in radians.
        float near = 0.1f;
        float far = 100.f;

        /** @brief The projection for an image @p aspect wide to its height, Y pointing down as the device has it. */
        [[nodiscard]] kor::Mat4 Projection(float aspect, bool flipY = true) const;
    };
}
