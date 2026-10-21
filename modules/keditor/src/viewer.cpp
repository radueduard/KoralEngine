//
// keditor: the viewer. @see viewer.h
//

#include <keditor/viewer.h>

#include <algorithm>
#include <cmath>
#include <format>

#include <capiInterop.h>
#include <kgui/layout.h>
#include <kui/icons.h>
#include <mesh.h>

#include <keditor/editor.h>

namespace keditor
{
    namespace {
        constexpr float InfoWidth = 280.f;     // the column beside a picture: what it is, its histogram, its texel
        constexpr float Label = 96.f;
        constexpr std::size_t RowsAPage = 64;

        kui::Widget Heading(std::string text)
        {
            kui::TextStyle style;
            style.size = 15.f;
            style.weight = 700;
            return kui::Text(std::move(text), style);
        }
        /** A small button in a bar of the viewer's controls: as the editor's chips are, its words in the middle. */
        kui::Widget Chip(std::string label, std::function<void()> onPressed, const bool enabled = true)
        {
            const kui::Theme& t = kui::Theme::Current();
            kui::TextStyle style = t.textStyle;
            style.size = 12.f;
            style.color = t.text;
            return kui::Button(kui::Text(std::move(label), style, kui::TextAlign::eCenter, false), std::move(onPressed),
                               kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(10.f, 4.f)).SetEnabled(enabled));
        }
        /** A small button that is a picture of what it does, as the editor's are: said by what is shown when the pointer rests on it. */
        kui::Widget IconChip(const std::string_view icon, std::string what, std::function<void()> onPressed, const bool enabled = true)
        {
            const kui::Theme& t = kui::Theme::Current();
            return kui::Tooltip(std::move(what), kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(icon, kui::IconStyle::eFilled, enabled ? t.textMuted : t.textMuted.WithAlpha(0.4f))),
                                                             std::move(onPressed), kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)).SetEnabled(enabled)));
        }
        kui::Widget Rows(std::vector<kui::Widget> children, const float gap = 6.f)
        {
            return kui::Column(std::move(children), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(gap));
        }
        kui::Widget Bar(std::vector<kui::Widget> children)
        {
            return kui::Row(std::move(children), kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
        }
        kui::Widget Fact(std::string label, std::string value) { return kgui::Labeled(std::move(label), kgui::Line(std::move(value)), Label); }
        kui::Widget Red(std::string text) { return kui::Text(std::move(text), kui::TextStyle {}.SetSize(13.f).SetColor(kui::Color::Hex(0xFF6B6B))); }

        std::string Bytes(const std::uint64_t bytes)
        {
            if (bytes >= 1024ull * 1024ull) return std::format("{:.2f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            if (bytes >= 1024ull) return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
            return std::format("{} bytes", bytes);
        }

        /**
         * What is looked at, and what is said of it: side by side where there is room, the second as wide as InfoWidth;
         * otherwise one over the other, the second taking at most two fifths of the height.
         */
        kui::Widget Split(kui::Widget main, kui::Widget side)
        {
            return kui::CustomLayout([](kui::LayoutContext& context, const kui::BoxConstraints& given) {
                const kor::Vec2 size { given.HasBoundedWidth() ? given.maxWidth : 800.f, given.HasBoundedHeight() ? given.maxHeight : 600.f };
                constexpr float Gap = 8.f;
                if (size.x >= InfoWidth * 2.3f) {
                    const float wide = size.x - InfoWidth - Gap;
                    context.measure(0, kui::BoxConstraints { wide, wide, size.y, size.y });
                    context.measure(1, kui::BoxConstraints { InfoWidth, InfoWidth, size.y, size.y });
                    context.place(0, { 0.f, 0.f });
                    context.place(1, { wide + Gap, 0.f });
                } else {
                    const float low = std::min(std::round(size.y * 0.4f), 360.f), high = std::max(size.y - low - Gap, 0.f);
                    context.measure(0, kui::BoxConstraints { size.x, size.x, high, high });
                    context.measure(1, kui::BoxConstraints { size.x, size.x, low, low });
                    context.place(0, { 0.f, 0.f });
                    context.place(1, { 0.f, high + Gap });
                }
                return size;
            }, { std::move(main), std::move(side) });
        }

        const kui::Color ChannelColor[4] { kui::Color::Hex(0xFF5C5C), kui::Color::Hex(0x5CDB6E), kui::Color::Hex(0x5C9DFF), kui::Color::Hex(0xD8D8D8) };
        constexpr const char* ChannelName[4] { "R", "G", "B", "A" };

        // ---- an image ------------------------------------------------------------------------------------------

        /**
         * The picture, as big as there is room for or zoomed: the wheel zooms about the pointer, a drag moves it, and
         * the texel under the pointer is said under it. Where it looks is its own, kept from build to build.
         */
        struct TextureCanvas final : kui::StatefulWidget {
            std::shared_ptr<Session> session;
            float scale = 0.f;                  // view units a texel; 0: as big as fits
            kor::Vec2 centre { 0.5f, 0.5f };    // the point of the image in the middle, from 0 to 1 across it
            std::optional<kor::Vec2> pointer;   // where the pointer is over it, in its own units
            kor::Vec2 size { 1.f, 1.f };        // as last painted

            explicit TextureCanvas(std::shared_ptr<Session> s) : session(std::move(s)) {}
            void DidUpdateWidget(const kui::StatefulWidget& newer) override { session = static_cast<const TextureCanvas&>(newer).session; }

            [[nodiscard]] kor::Vec2 Texels() const
            {
                const auto& peek = session->ViewedPeek();
                return peek ? kor::Vec2(static_cast<float>(peek->extent.x), static_cast<float>(peek->extent.y)) : kor::Vec2(1.f);
            }
            [[nodiscard]] float Fit() const { const kor::Vec2 t = Texels(); return std::max(std::min(size.x / t.x, size.y / t.y) * 0.95f, 1e-4f); }
            [[nodiscard]] float Scale() const { return scale > 0.f ? scale : Fit(); }
            /** Where a point of the canvas is on the image, from 0 to 1 across and down it. */
            [[nodiscard]] kor::Vec2 ToImage(const kor::Vec2 at) const { return centre + (at - size * 0.5f) / (Texels() * Scale()); }

            kui::Widget Build() override
            {
                const auto picture = session->ViewedPicture();
                kui::Widget canvas = kui::CustomPaint([this, picture](kui::Canvas& c, const kor::Vec2 given) {
                    size = kor::Max(given, kor::Vec2(1.f));
                    const kui::Rect bounds = kui::Rect::FromSize(given);
                    c.Save();
                    c.ClipRect(bounds);
                    c.DrawRect(bounds, kui::Paint::Fill(kui::Color::Hex(0x141418)));
                    if (picture.Get()) {
                        const kor::Vec2 texels = Texels(), shown = texels * Scale();
                        const kor::Vec2 corner = size * 0.5f - centre * shown;
                        const kui::Rect where = kui::Rect::XYWH(corner.x, corner.y, shown.x, shown.y);
                        c.DrawRect(where.Inflate(1.f), kui::Paint::Stroked(kui::Color::Hex(0x3A3A44), 1.f));
                        c.DrawImage(picture, where, {}, kui::colors::White, session->imageView.filter);
                        // Near enough to tell texels apart: the lines between them, and the one under the pointer.
                        const float texel = Scale();
                        if (texel >= 12.f) {
                            const kui::Color line = kui::Color::Hex(0x000000).WithAlpha(0.35f);
                            const float x0 = std::max(where.left, 0.f), x1 = std::min(where.right, size.x), y0 = std::max(where.top, 0.f), y1 = std::min(where.bottom, size.y);
                            for (float x = where.left + std::ceil((x0 - where.left) / texel) * texel; x <= x1; x += texel) c.DrawLine({ x, y0 }, { x, y1 }, kui::Paint::Stroked(line, 1.f));
                            for (float y = where.top + std::ceil((y0 - where.top) / texel) * texel; y <= y1; y += texel) c.DrawLine({ x0, y }, { x1, y }, kui::Paint::Stroked(line, 1.f));
                        }
                        if (pointer) {
                            const kor::Vec2 at = ToImage(*pointer) * texels;
                            if (at.x >= 0.f && at.y >= 0.f && at.x < texels.x && at.y < texels.y) {
                                const kor::Vec2 cell { std::floor(at.x), std::floor(at.y) };
                                c.DrawRect(kui::Rect::XYWH(corner.x + cell.x * texel, corner.y + cell.y * texel, std::max(texel, 1.f), std::max(texel, 1.f)),
                                           kui::Paint::Stroked(kui::Color::Hex(0xFFD54A), 1.f));
                            }
                        }
                    }
                    c.Restore();
                }, { -1.f, -1.f }, kui::Container(kui::ContainerOptions {}.SetAlignment(kui::Alignment::Center())));     // as big as it is given
                kui::GestureOptions gestures;
                gestures.onHover = [this](const kor::Vec2 local) { SetState([&] { pointer = local; }); };
                gestures.onExit = [this] { SetState([&] { pointer.reset(); }); };
                gestures.onPanUpdate = [this](const kor::Vec2 delta, const kor::Vec2 local) {
                    SetState([&] {
                        if (scale <= 0.f) scale = Fit();
                        centre -= delta / (Texels() * scale);
                        pointer = local;
                    });
                };
                gestures.onScroll = [this](const kor::Vec2 delta) {
                    SetState([&] {
                        // About the pointer: what is under it stays under it.
                        const kor::Vec2 at = pointer.value_or(size * 0.5f);
                        const kor::Vec2 under = ToImage(at);
                        scale = std::clamp(Scale() * std::pow(1.2f, delta.y), 1e-3f, 256.f);
                        centre = under - (at - size * 0.5f) / (Texels() * scale);
                    });
                    return true;
                };
                std::vector<kui::Widget> layers;
                layers.push_back(kui::GestureDetector(std::move(gestures), std::move(canvas)));
                // Over it: how near, and back to fitting it or to a texel a unit.
                const std::string zoom = std::format("{:.0f}%", Scale() * 100.f);
                layers.push_back(kui::Positioned(kui::PositionedOptions { .top = 6.f, .right = 6.f },
                    Bar({ kgui::Muted(zoom, 12.f), Chip("Fit", [this] { SetState([&] { scale = 0.f; centre = { 0.5f, 0.5f }; }); }),
                          Chip("1:1", [this] { SetState([&] { scale = 1.f; }); }) })));
                // Under it: the texel under the pointer, as it is stored.
                std::string said = "Over the picture: what a texel holds. The wheel zooms, a drag moves it.";
                if (const auto& peek = session->ViewedPeek(); peek && pointer) {
                    const kor::Vec2 at = ToImage(*pointer) * Texels();
                    if (at.x >= 0.f && at.y >= 0.f && at.x < Texels().x && at.y < Texels().y) {
                        const auto x = static_cast<std::uint32_t>(at.x), y = static_cast<std::uint32_t>(at.y);
                        const auto texel = TexelAt(*peek, x, y, session->imageView.look.slice);
                        said = std::format("{}, {}     {}", x, y, texel ? texel->text : std::string("?"));
                    }
                }
                layers.push_back(kui::Positioned(kui::PositionedOptions { .left = 6.f, .right = 6.f, .bottom = 6.f },
                    kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(6.f, 3.f))
                                       .SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x101012).WithAlpha(0.8f)).SetRadius(4.f)),
                                   kgui::Line(said, kui::TextStyle {}.SetSize(12.f).SetColor(kui::Color::Hex(0xE6E6E6))))));
                return kui::Stack(std::move(layers));
            }
        };

        /** How each channel's values are spread: a curve each, between the least and the greatest; and where black and white are. */
        kui::Widget HistogramView(const Histogram& histogram, const bool log, const std::array<bool, 4>& shown, const std::optional<double> black, const std::optional<double> white)
        {
            return kui::CustomPaint([histogram, log, shown, black, white](kui::Canvas& c, const kor::Vec2 size) {
                const kui::Rect bounds = kui::Rect::FromSize(size);
                c.DrawRect(bounds, kui::Paint::Fill(kui::Color::Hex(0x141418)));
                c.DrawRect(bounds, kui::Paint::Stroked(kui::Color::Hex(0x3A3A44), 1.f));
                if (histogram.channels == 0) return;
                // As high as the highest bin of what is shown: a channel switched off — an alpha all of one value, its one bin
                // every texel — flattens nothing.
                const auto drawn = [&](const int ch) { return shown[ch] || histogram.channels == 1; };
                std::uint32_t most = 1;
                for (int ch = 0; ch < histogram.channels; ++ch)
                    if (drawn(ch)) for (const auto n : histogram.bins[ch]) most = std::max(most, n);
                const auto height = [&](const std::uint32_t n) {
                    const double share = log ? std::log1p(static_cast<double>(n)) / std::log1p(static_cast<double>(most)) : static_cast<double>(n) / most;
                    return static_cast<float>(share) * (size.y - 4.f);
                };
                for (int ch = 0; ch < histogram.channels; ++ch) {
                    if (!drawn(ch)) continue;
                    const auto& bins = histogram.bins[ch];
                    const float wide = size.x / static_cast<float>(bins.size());
                    const kui::Color color = histogram.channels == 1 ? ChannelColor[3] : ChannelColor[ch];
                    for (std::size_t i = 0; i < bins.size(); ++i) {
                        if (bins[i] == 0) continue;
                        const float h = std::max(height(bins[i]), 1.f);
                        c.DrawRect(kui::Rect::XYWH(static_cast<float>(i) * wide, size.y - 2.f - h, std::max(wide, 1.f), h), kui::Paint::Fill(color.WithAlpha(0.45f)));
                    }
                }
                // Where the picture is black and white, when that is said rather than its range.
                const double span = histogram.greatest > histogram.least ? histogram.greatest - histogram.least : 1.0;
                for (const auto& mark : { black, white }) {
                    if (!mark) continue;
                    const float x = static_cast<float>((*mark - histogram.least) / span) * size.x;
                    if (x >= 0.f && x <= size.x) c.DrawLine({ x, 0.f }, { x, size.y }, kui::Paint::Stroked(kui::Color::Hex(0xFFD54A), 1.f));
                }
            }, { -1.f, 120.f });
        }

        kui::Widget ImageView(const std::shared_ptr<Session>& session)
        {
            auto& view = session->imageView;
            const auto& peek = session->ViewedPeek();
            std::vector<kui::Widget> controls;
            std::vector<kui::Widget> facts;
            if (peek) {
                const kgraph::TexelLayout texel = kgraph::TexelLayoutOf(peek->format);
                // Which level, which layer, which slice.
                std::vector<std::string> levels;
                for (std::uint32_t m = 0; m < peek->mips; ++m)
                    levels.push_back(std::format("Level {}  ·  {} × {}", m, std::max(peek->fullExtent.x >> m, 1u), std::max(peek->fullExtent.y >> m, 1u)));
                controls.push_back(kui::SizedBox(190.f, -1.f, kui::Dropdown(levels, static_cast<int>(peek->mip), [session](const int m) {
                    session->imageView.mip = static_cast<std::uint32_t>(m);
                    session->ViewChanged();
                })));
                if (peek->layers > 1) {
                    std::vector<std::string> layers;
                    static constexpr const char* faces[] { "+X", "−X", "+Y", "−Y", "+Z", "−Z" };
                    for (std::uint32_t l = 0; l < std::min(peek->layers, 256u); ++l)
                        layers.push_back(peek->layers == 6 ? std::format("Layer {}  ({} as a cube)", l, faces[l]) : std::format("Layer {}", l));
                    controls.push_back(kui::SizedBox(170.f, -1.f, kui::Dropdown(layers, static_cast<int>(std::min(peek->layer, 255u)), [session](const int l) {
                        session->imageView.layer = static_cast<std::uint32_t>(l);
                        session->ViewChanged();
                    })));
                }
                if (peek->extent.z > 1)
                    controls.push_back(kgui::Labeled("Slice", kui::SizedBox(80.f, -1.f, kgui::DragFloats({ static_cast<float>(view.look.slice) }, [session, depth = peek->extent.z](const std::vector<float>& to) {
                        session->imageView.look.slice = static_cast<std::uint32_t>(std::clamp(std::round(to[0]), 0.f, static_cast<float>(depth - 1)));
                        session->ViewChanged();
                    }, 0.2f, 0, 0.f, static_cast<float>(peek->extent.z - 1))), 40.f));
                controls.push_back(kui::SizedBox(110.f, -1.f, kui::Dropdown({ "Linear", "Nearest" }, view.filter == kui::ImageFilter::eNearest ? 1 : 0, [session](const int f) {
                    session->imageView.filter = f == 1 ? kui::ImageFilter::eNearest : kui::ImageFilter::eLinear;
                    session->ViewChanged();
                })));
                controls.push_back(kui::SizedBox(170.f, -1.f, kui::Dropdown(LookModeNames(), static_cast<int>(view.look.mode), [session](const int m) {
                    session->imageView.look.mode = static_cast<LookMode>(m);
                    session->ViewChanged();
                })));
                // Which channels: each on and off.
                for (int ch = 0; ch < std::min(texel.channels, 4); ++ch)
                    controls.push_back(kui::Checkbox(view.look.channels[ch], [session, ch](const bool on) {
                        session->imageView.look.channels[ch] = on;
                        session->ViewChanged();
                    }, texel.depth ? "Depth" : ChannelName[ch]));

                facts.push_back(Heading("The image"));
                facts.push_back(Fact("Format", peek->format));
                facts.push_back(Fact("Type", peek->extent.z > 1 ? "3D" : peek->layers == 6 ? "2D, six layers (a cube)" : peek->layers > 1 ? "2D array" : peek->type));
                facts.push_back(Fact("Size", peek->fullExtent.z > 1 ? std::format("{} × {} × {}", peek->fullExtent.x, peek->fullExtent.y, peek->fullExtent.z)
                                                                    : std::format("{} × {}", peek->fullExtent.x, peek->fullExtent.y)));
                facts.push_back(Fact("Levels", std::to_string(peek->mips)));
                facts.push_back(Fact("Layers", std::to_string(peek->layers)));
                facts.push_back(Fact("Samples", std::to_string(peek->samples)));
                std::string usage;
                for (const auto& flag : peek->usage) usage += (usage.empty() ? "" : ", ") + flag.substr(1);
                facts.push_back(kgui::Labeled("Usage", kui::Text(usage, kui::TextStyle {}.SetSize(13.f)), Label));
                facts.push_back(Fact("Memory", Bytes(peek->byteSize)));
                facts.push_back(Fact("Shown", std::format("{} × {}{}", peek->extent.x, peek->extent.y, peek->extent.z > 1 ? std::format(" × {}", peek->extent.z) : std::string())));

                // Its values: how they are spread, and what they run between.
                const Histogram& histogram = session->ViewedHistogram();
                facts.push_back(kui::Separator());
                facts.push_back(Bar({ kui::Expanded(Heading("Histogram")), kui::Checkbox(view.logHistogram, [session](const bool on) { session->imageView.logHistogram = on; session->ViewChanged(); }, "Log") }));
                facts.push_back(HistogramView(histogram, view.logHistogram, view.look.channels, view.look.black, view.look.white));
                facts.push_back(Bar({ kgui::Muted(std::format("{:.4g}", histogram.least), 11.f), kui::Expanded(kui::Text("")), kgui::Muted(std::format("{:.4g}", histogram.greatest), 11.f) }));
                std::vector<kui::TableColumn> columns { { "", 30.f }, { "least" }, { "greatest" }, { "mean" } };
                std::vector<std::vector<kui::Widget>> stats;
                for (int ch = 0; ch < histogram.channels; ++ch)
                    stats.push_back({ kui::Text(texel.depth ? "D" : ChannelName[ch], kui::TextStyle {}.SetSize(12.f).SetColor(ChannelColor[histogram.channels == 1 ? 3 : ch])),
                                      kgui::Line(std::format("{:.5g}", histogram.min[ch])), kgui::Line(std::format("{:.5g}", histogram.max[ch])), kgui::Line(std::format("{:.5g}", histogram.mean[ch])) });
                if (!stats.empty()) facts.push_back(kui::Table(std::move(columns), std::move(stats), kui::TableOptions {}.SetRowHeight(22.f)));
                if (histogram.nans || histogram.infinities)
                    facts.push_back(Red(std::format("{} values are not numbers, and {} are endless.", histogram.nans, histogram.infinities)));
                facts.push_back(kgui::Muted(std::format("Counted over {} texels{}.", histogram.texels,
                                                        histogram.texels < static_cast<std::uint64_t>(peek->extent.x) * peek->extent.y ? " (one in every few)" : ""), 11.f));

                // What is black and what is white: the image's range, or two values said.
                facts.push_back(kui::Separator());
                facts.push_back(Heading("Range"));
                const Picture& made = session->ViewedPictureMade();
                const float black = static_cast<float>(view.look.black.value_or(made.least)), white = static_cast<float>(view.look.white.value_or(made.greatest));
                facts.push_back(kgui::Labeled(view.look.black ? "Black, white" : "Its range", kgui::DragFloats({ black, white }, [session](const std::vector<float>& to) {
                    session->imageView.look.black = to[0];
                    session->imageView.look.white = to[1];
                    session->ViewChanged();
                }, std::max(std::abs(white - black), 1e-3f) * 0.004f, 4), Label));
                facts.push_back(Bar({
                    Chip("Fit to its values", [session, histogram] { session->imageView.look.black = histogram.least; session->imageView.look.white = histogram.greatest; session->ViewChanged(); }),
                    Chip("0 to 1", [session] { session->imageView.look.black = 0.0; session->imageView.look.white = 1.0; session->ViewChanged(); }),
                    Chip("As the mode has it", [session] { session->imageView.look.black.reset(); session->imageView.look.white.reset(); session->ViewChanged(); }, view.look.black.has_value()),
                }));
            }
            std::vector<kui::Widget> rows;
            if (!controls.empty()) rows.push_back(kui::Wrap(std::move(controls), 8.f, 6.f));
            if (!session->ViewSays().empty()) rows.push_back(kgui::Muted(session->ViewSays()));
            rows.push_back(kui::Expanded(Split(kui::Make<TextureCanvas>(session), kui::ScrollView(kui::Padding(kui::EdgeInsets::Symmetric(6.f, 0.f), Rows(std::move(facts), 6.f))))));
            return Rows(std::move(rows), 8.f);
        }

        // ---- a buffer ------------------------------------------------------------------------------------------

        kui::Widget BufferView(const std::shared_ptr<Session>& session)
        {
            const auto& peek = session->ViewedPeek();
            std::vector<kui::Widget> rows;
            if (!session->ViewSays().empty()) rows.push_back(kgui::Muted(session->ViewSays()));
            if (!peek) return Rows(std::move(rows));
            const bool fromShader = peek->stride > 0 && !peek->fields.empty();
            const auto& layout = session->ViewedBufferLayout();
            // What it is.
            std::vector<kui::Widget> facts { Fact("Size", Bytes(peek->byteSize)) };
            if (layout) facts.push_back(Fact("Read as", std::format("{} fields, {} bytes an element, as typed", layout->fields.size(), layout->stride)));
            else if (fromShader) facts.push_back(Fact("Read as", std::format("its shader's struct: {} of {} bytes", peek->count, peek->stride)));
            else facts.push_back(Fact("Read as", "words, eight a row: type a layout to read it by"));
            rows.push_back(Rows(std::move(facts), 2.f));
            // How to read it: as typed, or as its shader lays it out.
            std::string described;
            for (const auto& field : peek->fields) {
                const char* scalar = field.scalar == 'f' ? "float" : field.scalar == 'i' ? "int" : field.scalar == 'b' ? "bool" : "uint";
                described += std::format("{}{}", described.empty() ? "" : "; ",
                                         field.numbers == 1 ? std::format("{} {}", scalar, field.name)
                                         : field.numbers <= 4 ? std::format("{}{} {}", scalar, field.numbers, field.name)
                                         : field.numbers == 16 ? std::format("{}4x4 {}", scalar, field.name)
                                                               : std::format("{} {}[{}]", scalar, field.name, field.numbers));
            }
            kui::TextFieldOptions typed;
            typed.text = session->BufferLayoutText();
            typed.placeholder = fromShader ? std::format("as its shader has it: {}", described) : std::string("float3 position; uint id; float4x4 model;");
            typed.onSubmitted = [session](const std::string& text) { session->SetBufferLayout(text); };
            typed.onFocusLost = [session](const std::string& text) { if (text != session->BufferLayoutText()) session->SetBufferLayout(text); };
            rows.push_back(kgui::Labeled("Layout", kui::TextField(std::move(typed)), Label));
            if (!session->BufferLayoutError().empty()) rows.push_back(Red(session->BufferLayoutError()));
            rows.push_back(kgui::Muted("Fields one after another, packed, four bytes a number: put in a field for what the buffer leaves between (float pad;). Empty, it is read by its shader's struct, or as words.", 11.f));

            const Table table = ToTable(*peek, layout ? &*layout : nullptr, session->bufferFirst, RowsAPage, session->bufferHex);
            const std::size_t last = table.total == 0 ? 0 : (table.total - 1) / RowsAPage * RowsAPage;
            rows.push_back(Bar({
                IconChip("chevron_left", "The page before", [session] { session->bufferFirst = session->bufferFirst >= RowsAPage ? session->bufferFirst - RowsAPage : 0; session->ViewChanged(); }, table.first > 0),
                kgui::Line(table.total == 0 ? std::string("No rows")
                           : std::format("{} {} to {} of {}", table.byField ? "Elements" : "Rows", table.first,
                                         table.first + (table.byField ? std::min(RowsAPage, table.total - table.first) : table.rows.size()) - 1, table.total)),
                IconChip("chevron_right", "The page after", [session, last] { session->bufferFirst = std::min(session->bufferFirst + RowsAPage, last); session->ViewChanged(); }, table.first < last),
                kgui::Labeled("Go to", kui::SizedBox(90.f, -1.f, kgui::DragFloats({ static_cast<float>(table.first) }, [session, total = table.total](const std::vector<float>& to) {
                    session->bufferFirst = static_cast<std::size_t>(std::clamp(std::round(to[0]), 0.f, static_cast<float>(total == 0 ? 0 : total - 1)));
                    session->ViewChanged();
                }, 1.f, 0, 0.f, static_cast<float>(table.total == 0 ? 0 : table.total - 1))), 44.f),
                kui::Checkbox(session->bufferHex, [session](const bool on) { session->bufferHex = on; session->ViewChanged(); }, "Hexadecimal"),
            }));
            // The table: a column a number, each as wide as a number needs — or its name, when that is wider — moved across
            // when there are many. A field's name, in a table of a line a field, has room for itself.
            std::vector<kui::TableColumn> columns;
            float wide = 0.f;
            for (std::size_t i = 0; i < table.columns.size(); ++i) {
                float width = i == 0 ? (table.byField ? 40.f : 64.f) : table.byField ? (i == 1 ? 130.f : 76.f) : 96.f;
                width = std::max(width, 7.2f * static_cast<float>(table.columns[i].size()) + 20.f);
                if (table.byField && i == 1)
                    for (const auto& row : table.rows) width = std::max(width, 7.2f * static_cast<float>(row[1].size()) + 20.f);
                columns.push_back({ table.columns[i], width });
                wide += width;
            }
            std::vector<std::vector<kui::Widget>> cells;
            for (const auto& row : table.rows) {
                std::vector<kui::Widget> line;
                for (std::size_t i = 0; i < row.size(); ++i)
                    line.push_back(kgui::Line(row[i], kui::TextStyle {}.SetSize(12.f).SetColor(i == 0 ? kui::Theme::Current().textMuted : kui::Theme::Current().text)));
                cells.push_back(std::move(line));
            }
            rows.push_back(kui::Expanded(kui::ScrollView(kui::ScrollView(kui::SizedBox(wide, -1.f, kui::Table(std::move(columns), std::move(cells), kui::TableOptions {}.SetRowHeight(22.f))),
                                                                         kui::Axis::eHorizontal))));
            return Rows(std::move(rows), 8.f);
        }

        // ---- a mesh ----------------------------------------------------------------------------------------------

        /** The mesh, drawn as big as it is shown: a drag turns it about, the wheel brings it near. */
        struct MeshCanvas final : kui::StatefulWidget {
            std::shared_ptr<Session> session;
            explicit MeshCanvas(std::shared_ptr<Session> s) : session(std::move(s)) {}
            void DidUpdateWidget(const kui::StatefulWidget& newer) override { session = static_cast<const MeshCanvas&>(newer).session; }
            kui::Widget Build() override
            {
                MeshViewer* viewer = session->ViewedMesh();
                const auto picture = session->ViewedPicture();
                kui::Widget shown = picture.Get() ? kui::Image(picture, kui::ImageFit::eFill) : kui::Center(kgui::Muted(session->ViewSays().empty() ? "Drawing it..." : session->ViewSays()));
                kui::GestureOptions gestures;
                gestures.onPanUpdate = [this](const kor::Vec2 delta, kor::Vec2) { if (MeshViewer* v = session->ViewedMesh()) v->Turn(-delta.x * 0.01f, delta.y * 0.01f); };
                gestures.onScroll = [this](const kor::Vec2 delta) { if (MeshViewer* v = session->ViewedMesh()) v->Zoom(delta.y); SetState(); return true; };
                return kui::SizeObserver([viewer](kor::Vec2, const kor::Vec2 pixels) {
                    if (viewer) viewer->SetExtent({ static_cast<std::uint32_t>(std::max(pixels.x, 16.f)), static_cast<std::uint32_t>(std::max(pixels.y, 16.f)) });
                }, kui::GestureDetector(std::move(gestures), kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x131417))), std::move(shown))));
            }
        };

        std::string ChannelTypeName(const kor::ChannelType type)
        {
            switch (type) {
            case kor::ChannelType::eFloat: return "float";
            case kor::ChannelType::eInt: return "int";
            case kor::ChannelType::eUInt: return "uint";
            case kor::ChannelType::eShort: return "short";
            case kor::ChannelType::eUShort: return "ushort";
            case kor::ChannelType::eByte: return "byte";
            case kor::ChannelType::eUByte: return "ubyte";
            case kor::ChannelType::eDouble: return "double";
            }
            return "?";
        }

        kui::Widget MeshView(const std::shared_ptr<Session>& session)
        {
            MeshViewer* viewer = session->ViewedMesh();
            const auto held = session->ViewedMeshHeld();
            std::vector<kui::Widget> facts { Heading("The mesh") };
            if (held && !held->pending) {
                const KoralResource* handle = held->part && held->source ? held->source->handle : held->handle;
                const auto mesh = handle ? kor::capi::MeshOf(const_cast<KoralResource*>(handle)) : kor::ResourceRef<const kor::Mesh>();
                if (mesh.Get()) {
                    const std::uint64_t indices = held->part ? held->indexCount : mesh->IndexCount().value_or(0);
                    facts.push_back(Fact("Vertices", held->part ? std::format("{} (of the whole model's mesh)", mesh->VertexCount()) : std::to_string(mesh->VertexCount())));
                    facts.push_back(Fact("Triangles", std::to_string((indices > 0 ? indices : mesh->VertexCount()) / 3)));
                    facts.push_back(Fact("Indices", indices == 0 ? std::string("none: drawn in vertex order")
                                                                 : std::format("{}, of {}", indices, mesh->IndexType() ? ChannelTypeName(*mesh->IndexType()) : std::string("?"))));
                    if (held->part) facts.push_back(Fact("Its part", std::format("from index {}, vertices from {}", held->firstIndex, held->vertexOffset)));
                    // What a vertex is.
                    facts.push_back(kui::Separator());
                    facts.push_back(Heading("A vertex"));
                    const auto& layout = mesh->Layout();
                    std::vector<std::vector<kui::Widget>> attributes;
                    for (const auto& attribute : layout.attributes)
                        attributes.push_back({ kgui::Line(attribute.semantic.empty() ? std::format("location {}", attribute.location.value_or(0)) : attribute.semantic),
                                               kgui::Line(std::format("{} × {}", ChannelTypeName(attribute.channelType), attribute.channelCount * attribute.locations)),
                                               kgui::Line(std::to_string(attribute.binding)), kgui::Line(std::to_string(attribute.offset)) });
                    if (!attributes.empty())
                        facts.push_back(kui::Table({ { "what" }, { "as" }, { "buffer", 52.f }, { "byte", 44.f } }, std::move(attributes), kui::TableOptions {}.SetRowHeight(22.f)));
                    for (std::size_t b = 0; b < layout.bindings.size(); ++b)
                        facts.push_back(Fact(std::format("Buffer {}", b), std::format("{} bytes a vertex", layout.bindings[b].stride)));
                }
                if (held->bounded) {
                    facts.push_back(kui::Separator());
                    facts.push_back(Heading("Where it reaches"));
                    facts.push_back(Fact("From", std::format("{:.4g}, {:.4g}, {:.4g}", held->bounds[0], held->bounds[1], held->bounds[2])));
                    facts.push_back(Fact("To", std::format("{:.4g}, {:.4g}, {:.4g}", held->bounds[3], held->bounds[4], held->bounds[5])));
                    facts.push_back(Fact("Size", std::format("{:.4g} × {:.4g} × {:.4g}", held->bounds[3] - held->bounds[0], held->bounds[4] - held->bounds[1], held->bounds[5] - held->bounds[2])));
                }
            } else {
                facts.push_back(kgui::Muted(held ? "Still being read." : "It is not there any more."));
            }

            std::vector<kui::Widget> controls;
            if (viewer) {
                // What colours it: light, nothing, or any of its attributes.
                std::vector<std::string> shown { "Lit", "Plain" };
                const auto attributes = viewer->Attributes();
                int chosen = viewer->CurrentShading() == MeshViewer::Shading::ePlain ? 1 : 0;
                for (std::size_t i = 0; i < attributes.size(); ++i) {
                    shown.push_back(std::format("{}   ·   {}{}", attributes[i].semantic, attributes[i].type, attributes[i].count == 1 ? std::string() : std::to_string(attributes[i].count)));
                    if (viewer->CurrentShading() == MeshViewer::Shading::eAttribute && attributes[i].semantic == viewer->ShownAttribute()) chosen = static_cast<int>(i) + 2;
                }
                controls.push_back(kui::SizedBox(190.f, -1.f, kui::Dropdown(shown, chosen, [session, attributes](const int m) {
                    if (MeshViewer* v = session->ViewedMesh()) {
                        if (m < 2) v->SetShading(m == 0 ? MeshViewer::Shading::eLit : MeshViewer::Shading::ePlain);
                        else v->ShowAttribute(attributes[static_cast<std::size_t>(m - 2)].semantic);
                    }
                    session->ViewChanged();
                })));
                if (viewer->CurrentShading() == MeshViewer::Shading::eAttribute)
                    controls.push_back(kui::SizedBox(150.f, -1.f, kui::Dropdown({ "As 0 to 1", "As −1 to 1", "As a UV, checkered" }, static_cast<int>(viewer->CurrentMapping()), [session](const int m) {
                        if (MeshViewer* v = session->ViewedMesh()) v->SetMapping(static_cast<MeshViewer::Mapping>(m));
                        session->ViewChanged();
                    })));
                controls.push_back(kui::Checkbox(viewer->Wireframe(), [session](const bool on) { if (MeshViewer* v = session->ViewedMesh()) v->SetWireframe(on); session->ViewChanged(); }, "Its edges"));
                if (viewer->Wireframe())
                    controls.push_back(kgui::Labeled("Width", kui::SizedBox(64.f, -1.f, kgui::DragFloats({ viewer->EdgeWidth() }, [session](const std::vector<float>& to) {
                        if (MeshViewer* v = session->ViewedMesh()) v->SetEdgeWidth(to[0]);
                        session->ViewChanged();
                    }, 0.05f, 1, 0.5f, 8.f)), 40.f));
                controls.push_back(Chip("Seen whole", [session] { if (MeshViewer* v = session->ViewedMesh()) v->Frame(); session->ViewChanged(); }));
                controls.push_back(kgui::Muted(std::format("{:.0f}% near   ·   drag to turn it, the wheel to bring it near", viewer->Nearness() * 100.f), 12.f));
            }
            std::vector<kui::Widget> rows;
            rows.push_back(kui::Wrap(std::move(controls), 8.f, 6.f));
            if (!session->ViewSays().empty() && session->ViewedPicture().Get()) rows.push_back(Red(session->ViewSays()));
            rows.push_back(kui::Expanded(Split(kui::Make<MeshCanvas>(session), kui::ScrollView(kui::Padding(kui::EdgeInsets::Symmetric(6.f, 0.f), Rows(std::move(facts), 6.f))))));
            return Rows(std::move(rows), 8.f);
        }
    }

    kui::Widget ViewerPanel(std::shared_ptr<Session> session)
    {
        const auto kind = session->Viewed();
        if (kind == Session::ViewKind::eNone)
            return kui::Padding(kui::EdgeInsets::All(16.f), Rows({
                Heading("Nothing is viewed"),
                kgui::Muted("Open a texture, a buffer or a mesh here to look at all of it: a node's, from its inspector or its menu (Open in the viewer), "
                            "or a file's, pressed twice in the Assets panel."),
            }));
        const std::string& id = session->Viewing();
        const char* what = kind == Session::ViewKind::eImage ? "Image" : kind == Session::ViewKind::eBuffer ? "Buffer" : "Mesh";
        kui::Widget body = kind == Session::ViewKind::eImage ? ImageView(session) : kind == Session::ViewKind::eBuffer ? BufferView(session) : MeshView(session);
        return kui::Padding(kui::EdgeInsets::All(10.f), Rows({
            Bar({ kui::Expanded(Heading(std::format("{}   ·   {}", id.starts_with('@') ? session->AssetLabel(id) : id, what))),
                  Chip("Stop viewing", [session] { session->StopViewing(); }) }),
            kui::Expanded(std::move(body)),
        }, 8.f));
    }
}
