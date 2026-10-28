#pragma once

/** @file view.h  Where a scene is seen from: what an editor puts in place of the main View and its Perspective. */

#include <kmath/matrix.h>

namespace kgraph
{
    /** @brief Where something is seen from, and the lens it is seen through. */
    struct View {
        kor::Mat4 view { 1.f };
        float fieldOfView = 1.0471976f;     ///< Top to bottom, in radians.
        float near = 0.1f;
        float far = 100.f;
        /// More than 0: seen straight on, no nearer thing bigger — an orthographic view this many units tall, from `near`
        /// (which may be behind where it is seen from) to `far`. A graph's main Perspective is then this box.
        float orthographic = 0.f;

        /** @brief The projection for an image @p aspect wide to its height, Y pointing down as the device has it. */
        [[nodiscard]] kor::Mat4 Projection(float aspect, bool flipY = true) const;
    };
}
