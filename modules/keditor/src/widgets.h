#pragma once

//
// keditor: the editor's small widgets, shared by its panels — buttons, headings, problems, rows of numbers — and the
// picture of a mesh that waits for it to be drawn.
//

#include <keditor/editor.h>

#include <kgraph/catalog.h>
#include <kgui/layout.h>
#include <koralUI.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace keditor::widgets
{
    using Value = kgraph::Value;

    /** How wide the names before an inspector's controls are. */
    inline constexpr float LabelWidth = 132.f;

    /** A button that does something: as tall as the fields and lists beside it, in the style of the panel's other buttons. */
    kui::Widget Small(std::string label, std::function<void()> onPressed);
    /** A small button in a line of words or a heading — a pass that runs for an entity, Reset — where a whole control's height would push the line apart. */
    kui::Widget Chip(std::string label, std::function<void()> onPressed);
    /** A small button that is a picture of what it does: said by what is shown when the pointer rests on it. */
    kui::Widget SmallIcon(std::string_view icon, std::function<void()> onPressed);
    kui::Widget Heading(std::string text);
    /** What to do about a problem, when what it says is one of those there is something plain to do about: said after it. */
    std::string HintFor(const std::string& problem);
    /** A problem, red, with what to do about it under it when there is something plain to do. */
    kui::Widget Problem(std::string text);
    kui::Widget Rows(std::vector<kui::Widget> children, float gap = 6.f);
    kui::Widget Panel(std::vector<kui::Widget> children);
    float NumberOf(const Value& value, float fallback = 0.f);
    /** A value typed as JSON: for what has no editor of its own. Nothing typed is no value. */
    kui::Widget JsonField(const Value& value, const std::function<void(Value)>& set, const std::string& key);
    /** How a number is dragged, wherever it is (DragNumbers). */
    kui::DragValueOptions Dragging(const kgraph::NumberLimits& limits, kui::NumberType type);
    /** A few controls that are one thing's — a vector's numbers — side by side where they fit, a line each where they do not. */
    kui::Widget Fitted(std::vector<kui::Widget> parts);
    /** The same for a few numbers: each of a vector, a box, labelled X, Y, Z, W. */
    kui::Widget DragNumbers(std::vector<float> numbers, std::function<void(std::vector<float>)> set, const kgraph::NumberLimits& limits, kui::NumberType type);
    /**
     * The picture a picture file's resource is of: itself, or — for one read through a view (a compressed one whose
     * channels are spread, a file of a pack) — the image the view is of. Empty for anything else.
     */
    kor::ResourceRef<const kor::Image> PictureOf(const kgraph::Resource* held);
    /** A tool of a panel's title bar: a picture of what it does, lit while it is on. */
    kui::Widget Tool(std::string_view icon, const std::string& what, bool active, std::function<void()> pressed, bool enabled = true);
    /** A heading over a part of a list, and what it says, small and quiet; with something to do at its end. */
    kui::Widget ListHeading(std::string title, kui::Widget trailing = {});

    /**
     * A mesh's small picture where it is shown — an asset's tile, the inspector, the drawer — built again on its own
     * when a new one is drawn: one every few frames while the Assets panel's meshes are, and each was the whole
     * interface built again (some 45 ms with a scene of hundreds of entities).
     */
    class MeshThumbnail final : public kui::StatefulWidget {
    public:
        using Find = std::function<kor::ResourceRef<const kor::Image>()>;
        using Show = std::function<kui::Widget(kor::ResourceRef<const kor::Image>)>;
        /// @p find gives the picture, empty until it is drawn; @p show what is shown of it, given it or nothing.
        MeshThumbnail(std::shared_ptr<Session> session, Find find, Show show) : _session(std::move(session)), _find(std::move(find)), _show(std::move(show)) {}

        void InitState() override
        {
            _seen = _session->MeshPicturesMade();
            Animate([this](float) {
                if (const auto made = _session->MeshPicturesMade(); made != _seen) {
                    _seen = made;
                    if (_find().Get() != _shown) SetState();
                }
                return true;
            });
        }
        void DidUpdateWidget(const kui::StatefulWidget& newer) override
        {
            const auto& given = static_cast<const MeshThumbnail&>(newer);
            _find = given._find;
            _show = given._show;
            _seen = _session->MeshPicturesMade();
        }
        kui::Widget Build() override
        {
            const auto picture = _find();
            _shown = picture.Get();
            return _show(picture);
        }

    private:
        std::shared_ptr<Session> _session;
        Find _find;
        Show _show;
        std::uint64_t _seen = 0;
        const void* _shown = nullptr;
    };
}
