//
// keditor: the viewer — one texture, buffer or mesh, looked at closely, as a graphics debugger shows a resource. An
// image is shown at any level and layer, zoomed and moved about, with what each texel holds under the pointer, its
// histogram and all there is to say of it; a buffer is a table of its elements, by the layout its shader gives it or
// one typed; a mesh is drawn by itself, to be turned about and brought near. What it shows is Session::View's.
//

#pragma once

#include <memory>

#include <kui/widgets.h>

namespace keditor
{
    class Session;

    /** @brief The Viewer panel's content: what @p session views, or how to view something. */
    [[nodiscard]] kui::Widget ViewerPanel(std::shared_ptr<Session> session);
}
