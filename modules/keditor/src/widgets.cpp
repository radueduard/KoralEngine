//
// keditor: the editor's small widgets, shared by its panels — buttons, headings, problems, rows of numbers — and the
// picture of a mesh that waits for it to be drawn.
//

#include "widgets.h"

#include <capiInterop.h>
#include <kecs/json.h>

#include <algorithm>
#include <limits>

namespace keditor::widgets
{
    
    /** A label in the middle of a button: a line high, and as wide as the button when it is stretched, as its words when not. */
    static kui::Widget ButtonLabel(std::string label, const float size, const bool fill = true)
    {
        const kui::Theme& t = kui::Theme::Current();
        kui::TextStyle style = t.textStyle;
        style.size = size;
        style.color = t.text;
        kui::Widget text = kui::Text(std::move(label), style, kui::TextAlign::eCenter, false);
        return fill ? kui::SizedBox(-1.f, size * style.lineHeight, kui::Center(std::move(text))) : text;
    }
    /** A button that does something: as tall as the fields and lists beside it, in the style of the panel's other buttons. */
    kui::Widget Small(std::string label, std::function<void()> onPressed)
    {
        const kui::Theme& t = kui::Theme::Current();
        const float size = 14.f;
        const float vertical = std::max(0.f, (t.controlHeight - size * t.textStyle.lineHeight) * 0.5f);
        return kui::Button(ButtonLabel(std::move(label), size), std::move(onPressed),
                           kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(14.f, vertical)));
    }
    /** A small button in a line of words or a heading — a pass that runs for an entity, Reset — where a whole control's height would push the line apart. */
    kui::Widget Chip(std::string label, std::function<void()> onPressed)
    {
        return kui::Button(ButtonLabel(std::move(label), 12.f, false), std::move(onPressed),     // as wide as its words, wherever it is
                           kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(10.f, 4.f)));
    }
    /** A small button that is a picture of what it does: said by what is shown when the pointer rests on it. */
    kui::Widget SmallIcon(const std::string_view icon, std::function<void()> onPressed)
    {
        return kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(icon, kui::IconStyle::eFilled, kui::Theme::Current().textMuted)), std::move(onPressed),
                           kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)));
    }
    kui::Widget Heading(std::string text)
    {
        kui::TextStyle style;
        style.size = 15.f;
        style.weight = 700;
        return kui::Text(std::move(text), style);
    }
    /** What to do about a problem, when what it says is one of those there is something plain to do about: said after it. */
    std::string HintFor(const std::string& problem)
    {
        static const std::pair<const char*, const char*> hints[] {
            { "is still being read", "Wait: its file is being read, and it is drawn when it has arrived." },
            { "has nothing in it", "Give the field something: drag a mesh or a texture onto it from the Assets panel, or choose $white for a picture it has no use for." },
            { "has no mesh in it", "Give its Mesh a mesh: a default one from the list, or one dragged from the Assets panel." },
            { ".slang among the shaders", "Make the shader (Assets, New shader) or put its file in the project's shaders folder; or name another module on the node." },
            { "Failed to load Slang module", "The shader does not compile. Open it from the Assets panel and mend what the message names: it is tried again when the file is saved." },
            { "which failed", "Mend the node it names first: this one only waits for it." },
            { "which is not in the graph", "Wire the input to a node that is there, or take the input off." },
            { "wired, through other nodes, to itself", "Take out one wire of the circle: a node cannot wait for itself." },
            { "among its usage", "Add the usage it names to the image's or the buffer's 'usage' input." },
            { "has no input", "Take the input off the node: its kind, or its shader, no longer has it." },
            { "has no field", "Name one of the fields listed, or take the input off: the shader's struct no longer has it." },
            { "uses no buffer", "Set 'parameter' to one of the buffers listed." },
            { "has no output", "Wire from an output the node has: its inspector lists every one, under What it gives." },
            { "needs an image wired in", "Wire an Image or an ImageView to it." },
            { "there is no component", "Add the component to an entity, or build the pipeline or the script that makes it." },
            { "more than one list of entities", "Wire every field from the same Entities node: an element of the buffer is one entity's." },
            { "there is no turn", "Give an index the loop has: its turns count from 0." },
            { "'first'", "Name, in 'first', the node it is on the first turn." },
            { "takes a ", "Wire it to a node that makes that kind of thing." },
        };
        for (const auto& [has, hint] : hints)
            if (problem.find(has) != std::string::npos) return hint;
        return {};
    }

    kui::Widget Problem(std::string text)
    {
        const std::string hint = HintFor(text);
        kui::Widget said = kui::Text(std::move(text), kui::TextStyle {}.SetSize(13.f).SetColor(kui::Color::Hex(0xFF6B6B)));
        if (hint.empty()) return said;
        // What is wrong, and under it what to do about it.
        return kui::Column({ std::move(said), kui::Text("→ " + hint, kui::TextStyle {}.SetSize(12.f).SetColor(kui::Theme::Current().textMuted)) },
                           kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(2.f));
    }
    kui::Widget Rows(std::vector<kui::Widget> children, const float gap)
    {
        return kui::Column(std::move(children), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(gap));
    }
    kui::Widget Panel(std::vector<kui::Widget> children)
    {
        return kui::ScrollView(kui::Padding(kui::EdgeInsets::All(10.f), Rows(std::move(children), 8.f)));
    }

    float NumberOf(const Value& value, const float fallback)
    {
        if (const double* number = value.Number()) return static_cast<float>(*number);
        if (const bool* flag = value.Bool()) return *flag ? 1.f : 0.f;
        return fallback;
    }

    /** A value typed as JSON: for what has no editor of its own. Nothing typed is no value. */
    kui::Widget JsonField(const Value& value, const std::function<void(Value)>& set, const std::string& key)
    {
        const std::string text = value.IsNull() ? std::string() : value.ToJson();
        return kui::TextField(kui::TextFieldOptions { .text = text, .placeholder = "as JSON",
            .onSubmitted = [set](const std::string& typed) {
                if (typed.empty()) { set(Value {}); return; }
                kecs::json::Value parsed;
                std::string error;
                if (kecs::json::Parse(typed, parsed, error)) set(Value::FromJson(parsed));
            } }).Key(key + "=" + text);
    }

    /**
     * How a number is dragged, wherever it is: within @p limits; a unit every few pixels when it is @p whole, and shown
     * so; by so much of what it is when it is logarithmic, about twice as much every seventy pixels.
     */
    kui::DragValueOptions Dragging(const kgraph::NumberLimits& limits, const kui::NumberType type)
    {
        const bool whole = type == kui::NumberType::eInt || type == kui::NumberType::eUInt;
        return kui::DragValueOptions {}.SetRange(limits.min, limits.max).SetLogarithmic(limits.logarithmic).SetType(type)
            .SetSpeed(limits.logarithmic || !whole ? 0.01f : 0.1f).SetDecimals(whole ? 0 : limits.logarithmic ? 3 : 2);
    }
    /**
     * A few controls that are one thing's — a vector's numbers — side by side where they fit, and where they do not, a
     * line each, or for four two a line. As narrow as the widest of them: an inspector with a float4 in it can be narrow.
     */
    kui::Widget Fitted(std::vector<kui::Widget> parts)
    {
        if (parts.size() == 1) return std::move(parts.front());
        const std::size_t count = parts.size();
        return kui::CustomLayout([count](kui::LayoutContext& context, const kui::BoxConstraints& given) {
            constexpr float Gap = 4.f, Unbounded = std::numeric_limits<float>::infinity();
            std::vector<float> natural(count);
            for (std::size_t i = 0; i < count; ++i) natural[i] = context.measure(i, kui::BoxConstraints { 0.f, Unbounded, 0.f, Unbounded }).x;
            const auto across = [&](const std::size_t each) {     // the width that many a line needs
                float widest = 0.f;
                for (std::size_t first = 0; first < count; first += each) {
                    float line = Gap * static_cast<float>(std::min(each, count - first) - 1);
                    for (std::size_t i = first; i < std::min(first + each, count); ++i) line += natural[i];
                    widest = std::max(widest, line);
                }
                return widest;
            };
            const float width = given.HasBoundedWidth() ? given.maxWidth : across(count);
            const std::size_t each = width >= across(count) ? count : count == 4 && width >= across(2) ? 2 : 1;
            const float cell = std::max((width - Gap * static_cast<float>(each - 1)) / static_cast<float>(each), 0.f);
            float y = 0.f;
            for (std::size_t first = 0; first < count; first += each) {
                float tallest = 0.f;
                for (std::size_t i = first; i < std::min(first + each, count); ++i) {
                    tallest = std::max(tallest, context.measure(i, kui::BoxConstraints { cell, cell, 0.f, Unbounded }).y);
                    context.place(i, { static_cast<float>(i - first) * (cell + Gap), y });
                }
                y += tallest + (first + each < count ? Gap : 0.f);
            }
            return kor::Vec2 { width, y };
        }, std::move(parts));
    }

    /** The same for a few numbers: each of a vector, a box, labelled X, Y, Z, W — side by side where they fit (Fitted). */
    kui::Widget DragNumbers(std::vector<float> numbers, std::function<void(std::vector<float>)> set, const kgraph::NumberLimits& limits, const kui::NumberType type)
    {
        static constexpr const char* axes[] { "X", "Y", "Z", "W" };
        std::vector<kui::Widget> parts;
        for (std::size_t i = 0; i < numbers.size(); ++i) {
            auto options = Dragging(limits, type).SetWidth(48.f);
            if (numbers.size() > 1 && i < 4) options.SetLabel(axes[i]);
            parts.push_back(kui::DragValue(numbers[i], [numbers, set, i](const double v) {
                auto changed = numbers;
                changed[i] = static_cast<float>(v);
                if (set) set(changed);
            }, options));
        }
        return Fitted(std::move(parts));
    }

    /**
     * The picture a picture file's resource is of: itself, or — for one read through a view (a compressed one whose
     * channels are spread, a file of a pack) — the image the view is of. Empty for anything else.
     */
    kor::ResourceRef<const kor::Image> PictureOf(const kgraph::Resource* held)
    {
        if (!held || !held->handle) return {};
        if (held->kind == "Image") return kor::capi::ImageOf(held->handle);
        if (held->kind == "ImageView") return kor::capi::ImageViewOf(held->handle)->SourceImage();
        return {};
    }

    /** A square button with an icon: lit while what it stands for is on, dim while it cannot be pressed. */
    kui::Widget Tool(const std::string_view icon, const std::string& what, const bool active, std::function<void()> pressed, const bool enabled)
    {
        const kui::Color tint = enabled ? kui::Color::Hex(0xE0E0E0) : kui::Color::Hex(0x707070);
        kui::Widget face = kui::Container(kui::ContainerOptions {}.SetSize(24.f, 24.f).SetAlignment(kui::Alignment::Center())
                                              .SetDecoration(kui::Decoration {}.SetColor(active ? kui::Theme::Current().primary.WithAlpha(0.35f) : kui::colors::Transparent).SetRadius(3.f)),
                                          kui::SizedBox(16.f, 16.f, kui::Icon(icon, kui::IconStyle::eFilled, tint)));
        return kui::Tooltip(what, kui::Button(std::move(face), std::move(pressed), kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain).SetEnabled(enabled)));
    }

    /** A heading over a part of a list, and what it says, small and quiet; with something to do at its end. */
    kui::Widget ListHeading(std::string title, kui::Widget trailing)
    {
        const kui::Theme& t = kui::Theme::Current();
        std::vector<kui::Widget> line { kui::Expanded(kui::Text(std::move(title), kui::TextStyle {}.SetSize(11.f).SetColor(t.textMuted).SetLetterSpacing(0.6f), kui::TextAlign::eStart, false, 1, true)) };
        if (trailing) line.push_back(std::move(trailing));
        return kui::Padding(kui::EdgeInsets::Only(4.f, 10.f, 0.f, 2.f), kui::SizedBox(-1.f, 22.f, kui::Row(std::move(line), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));
    }
}
