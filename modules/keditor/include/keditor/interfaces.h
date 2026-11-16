//
// keditor: what scripts show over the game — a HUD, a menu — drawn where the game is seen and given its input. In a
// player that is the scene's window; in an editor, the Game panel, not the editor's own view of the scene.
//

#pragma once

#include <utility>
#include <vector>

#include <scene.h>     // kor::UVec2, and what the frame graph is

namespace kor { class FrameGraph; class Input; class RenderPass; }
namespace kui { class Ui; }

namespace keditor
{
    class Document;

    /**
     * @brief Shows a document's script interfaces in one scene: a koral-ui pass for each, over the game's picture, and
     *        each frame each built again from what changed and given the scene's input — so that a script reads
     *        `interfaceWantsMouse` while the pointer is over one, and leaves the mouse to it.
     */
    class InterfaceHost {
    public:
        /**
         * @brief Each frame, after the scripts' updates and before the frame is drawn: the interfaces @p document has
         *        now, in @p graph, given @p input, over a picture @p size pixels big, @p seconds after the last frame.
         *        They are the last of the graph's passes, over all it draws: added again when another was added since.
         */
        void Show(kor::FrameGraph& graph, const Document& document, kor::Input& input, kor::UVec2 size, float seconds);
        /** @brief Shows none: what a scene does as its document goes, or it stops showing the game. */
        void Clear(kor::FrameGraph& graph);

    private:
        std::vector<std::pair<const kui::Ui*, kor::RenderPass*>> _passes;
    };
}
