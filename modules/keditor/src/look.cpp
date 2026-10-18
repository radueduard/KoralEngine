//
// keditor: what a node holds, made something to look at. @see look.h
//

#include <keditor/look.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace keditor
{
    namespace {
        /** A float of sixteen bits, as one of thirty-two. */
        float Half(const std::uint16_t h)
        {
            const std::uint32_t sign = (h >> 15) & 1u, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
            float value;
            if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
            else if (exponent == 31) value = mantissa ? std::nanf("") : INFINITY;
            else value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
            return sign ? -value : value;
        }

        /** One channel of one texel, as a number. */
        double Channel(const std::byte* at, const kgraph::TexelLayout& texel)
        {
            if (texel.bits == 8) {
                const auto v = static_cast<std::uint8_t>(*at);
                return texel.kind == 'u' ? v / 255.0 : texel.kind == 's' ? static_cast<std::int8_t>(v) / 127.0 : texel.kind == 'I' ? static_cast<std::int8_t>(v) : v;
            }
            if (texel.bits == 16) {
                std::uint16_t v;
                std::memcpy(&v, at, 2);
                return texel.kind == 'f' ? Half(v) : texel.kind == 'u' ? v / 65535.0 : texel.kind == 's' ? static_cast<std::int16_t>(v) / 32767.0
                     : texel.kind == 'I' ? static_cast<std::int16_t>(v) : v;
            }
            std::uint32_t v;
            std::memcpy(&v, at, 4);
            if (texel.kind == 'f') { float f; std::memcpy(&f, at, 4); return f; }
            return texel.kind == 'I' ? static_cast<double>(static_cast<std::int32_t>(v)) : static_cast<double>(v);
        }
    }

    const std::vector<std::string>& LookModeNames()
    {
        static const std::vector<std::string> names { "As suits it", "As it is", "Stretched to its range", "Red", "Green", "Blue", "Alpha" };
        return names;
    }

    Picture ToPicture(const kgraph::Peek& peek, const LookMode mode, const std::uint32_t slice, const std::uint32_t most)
    {
        LookOptions options;
        options.mode = mode;
        options.slice = slice;
        options.most = most;
        return ToPicture(peek, options);
    }

    Picture ToPicture(const kgraph::Peek& peek, const LookOptions& options)
    {
        Picture picture;
        const kgraph::TexelLayout texel = kgraph::TexelLayoutOf(peek.format);
        const std::size_t bytes = texel.Bytes();
        if (!peek.image || texel.channels == 0 || peek.extent.x == 0 || peek.extent.y == 0) return picture;
        const std::size_t layer = static_cast<std::size_t>(peek.extent.x) * peek.extent.y * bytes;
        const std::size_t first = layer * std::min(options.slice, std::max(peek.extent.z, 1u) - 1);
        if (peek.bytes.size() < first + layer) return picture;

        const bool whole = texel.kind == 'U' || texel.kind == 'I';
        LookMode mode = options.mode;
        if (mode == LookMode::eAuto) mode = texel.depth || texel.channels == 1 || whole ? LookMode::eStretched : LookMode::eAsItIs;
        const int only = mode == LookMode::eRed ? 0 : mode == LookMode::eGreen ? 1 : mode == LookMode::eBlue ? 2 : mode == LookMode::eAlpha ? 3 : -1;
        if (only >= texel.channels) return picture;
        // Which channels are read, and how they are shown: one alone is grey; alpha with colours lays them over a checkerboard.
        std::vector<int> colours;
        bool overChecks = false;
        if (only >= 0) {
            colours.push_back(only);
        } else {
            for (int c = 0; c < std::min(texel.channels, 3); ++c) if (options.channels[c]) colours.push_back(c);
            const bool alpha = options.channels[3] && texel.channels == 4;
            if (colours.empty() && alpha) colours.push_back(3);
            else overChecks = alpha;
            if (colours.empty()) return picture;
        }
        const bool grey = colours.size() == 1;
        const bool ranged = options.black && options.white;
        const bool stretched = mode == LookMode::eStretched || only >= 0 || ranged || (grey && colours.front() == 3);

        const std::uint32_t step = std::max<std::uint32_t>((std::max(peek.extent.x, peek.extent.y) + options.most - 1) / std::max(options.most, 1u), 1);
        picture.size = { (peek.extent.x + step - 1) / step, (peek.extent.y + step - 1) / step };
        const std::size_t count = static_cast<std::size_t>(picture.size.x) * picture.size.y;
        std::vector<double> values(count * 3, 0.0), alphas(overChecks ? count : 0, 1.0);
        double least = INFINITY, greatest = -INFINITY;
        const std::size_t channelBytes = static_cast<std::size_t>(texel.bits / 8);
        for (std::uint32_t y = 0; y < picture.size.y; ++y)
            for (std::uint32_t x = 0; x < picture.size.x; ++x) {
                const std::byte* at = peek.bytes.data() + first + (static_cast<std::size_t>(y) * step * peek.extent.x + static_cast<std::size_t>(x) * step) * bytes;
                const std::size_t i = static_cast<std::size_t>(y) * picture.size.x + x;
                double* out = values.data() + i * 3;
                for (const int c : colours) {
                    const double value = Channel(at + static_cast<std::size_t>(c) * channelBytes, texel);
                    out[grey ? 0 : std::min(c, 2)] = value;
                    if (std::isfinite(value)) { least = std::min(least, value); greatest = std::max(greatest, value); }
                }
                if (grey) out[1] = out[2] = out[0];
                if (overChecks) alphas[i] = std::clamp(Channel(at + 3 * channelBytes, texel), 0.0, 1.0);
            }
        if (!(least <= greatest)) least = greatest = 0.0;
        picture.least = least;
        picture.greatest = greatest;

        const double black = ranged ? *options.black : least, white = ranged ? *options.white : greatest;
        const double range = white != black ? white - black : 1.0;
        picture.pixels.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            kor::U8Vec4 pixel { 0, 0, 0, 255 };
            const std::size_t x = i % picture.size.x, y = i / picture.size.x;
            const double check = ((x / 8 + y / 8) & 1) ? 0.42 : 0.62;
            for (int c = 0; c < 3; ++c) {
                double value = values[i * 3 + static_cast<std::size_t>(c)];
                if (!std::isfinite(value)) value = 0.0;
                if (stretched) value = (value - black) / range;
                else if (texel.kind == 'f') value = std::pow(std::clamp(value, 0.0, 1.0), 1.0 / 2.2);     // light, as a display shows it
                value = std::clamp(value, 0.0, 1.0);
                if (overChecks) value = value * alphas[i] + check * (1.0 - alphas[i]);
                pixel[c] = static_cast<kor::u8>(std::lround(value * 255.0));
            }
            picture.pixels[i] = pixel;
        }
        return picture;
    }

    Histogram HistogramOf(const kgraph::Peek& peek, const std::uint32_t slice, const std::size_t bins)
    {
        Histogram histogram;
        const kgraph::TexelLayout texel = kgraph::TexelLayoutOf(peek.format);
        const std::size_t bytes = texel.Bytes();
        if (!peek.image || texel.channels == 0 || peek.extent.x == 0 || peek.extent.y == 0 || bins == 0) return histogram;
        const std::size_t texels = static_cast<std::size_t>(peek.extent.x) * peek.extent.y;
        const std::size_t first = texels * bytes * std::min(slice, std::max(peek.extent.z, 1u) - 1);
        if (peek.bytes.size() < first + texels * bytes) return histogram;
        const int channels = texel.channels;
        const std::size_t channelBytes = static_cast<std::size_t>(texel.bits / 8);
        // A big image is counted a texel in every few: about a million of them.
        const std::size_t step = std::max<std::size_t>(texels / (1u << 20), 1);
        std::vector<double> values;
        values.reserve((texels / step + 1) * static_cast<std::size_t>(channels));
        double least = INFINITY, greatest = -INFINITY;
        std::array<double, 4> sum {};
        std::array<std::uint64_t, 4> finite {};
        histogram.min.fill(INFINITY);
        histogram.max.fill(-INFINITY);
        for (std::size_t t = 0; t < texels; t += step) {
            const std::byte* at = peek.bytes.data() + first + t * bytes;
            for (int c = 0; c < channels; ++c) {
                const double value = Channel(at + static_cast<std::size_t>(c) * channelBytes, texel);
                values.push_back(value);
                if (std::isnan(value)) { ++histogram.nans; continue; }
                if (std::isinf(value)) { ++histogram.infinities; continue; }
                least = std::min(least, value);
                greatest = std::max(greatest, value);
                histogram.min[c] = std::min(histogram.min[c], value);
                histogram.max[c] = std::max(histogram.max[c], value);
                sum[c] += value;
                ++finite[c];
            }
            ++histogram.texels;
        }
        if (!(least <= greatest)) least = greatest = 0.0;
        histogram.channels = channels;
        histogram.least = least;
        histogram.greatest = greatest;
        for (int c = 0; c < channels; ++c) {
            histogram.bins[c].assign(bins, 0);
            histogram.mean[c] = finite[c] ? sum[c] / static_cast<double>(finite[c]) : 0.0;
            if (!finite[c]) histogram.min[c] = histogram.max[c] = 0.0;
        }
        const double span = greatest > least ? greatest - least : 1.0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double value = values[i];
            if (!std::isfinite(value)) continue;
            const auto bin = std::min(static_cast<std::size_t>((value - least) / span * static_cast<double>(bins)), bins - 1);
            ++histogram.bins[i % static_cast<std::size_t>(channels)][bin];
        }
        return histogram;
    }

    std::optional<Texel> TexelAt(const kgraph::Peek& peek, const std::uint32_t x, const std::uint32_t y, const std::uint32_t slice)
    {
        const kgraph::TexelLayout texel = kgraph::TexelLayoutOf(peek.format);
        const std::size_t bytes = texel.Bytes();
        if (!peek.image || texel.channels == 0 || x >= peek.extent.x || y >= peek.extent.y) return std::nullopt;
        const std::size_t at = ((static_cast<std::size_t>(std::min(slice, std::max(peek.extent.z, 1u) - 1)) * peek.extent.y + y) * peek.extent.x + x) * bytes;
        if (at + bytes > peek.bytes.size()) return std::nullopt;
        Texel out;
        out.channels = texel.channels;
        static constexpr const char* names[] { "R", "G", "B", "A" };
        const std::size_t channelBytes = static_cast<std::size_t>(texel.bits / 8);
        for (int c = 0; c < texel.channels; ++c) {
            const std::byte* where = peek.bytes.data() + at + static_cast<std::size_t>(c) * channelBytes;
            out.values[c] = Channel(where, texel);
            std::string value = texel.kind == 'U' || texel.kind == 'I' ? std::format("{}", static_cast<long long>(out.values[c])) : std::format("{:.5g}", out.values[c]);
            // A normalised value with what is stored for it.
            if (texel.kind == 'u' || texel.kind == 's') {
                std::uint32_t stored = 0;
                std::memcpy(&stored, where, std::min<std::size_t>(channelBytes, 4));
                value += std::format(" ({})", stored);
            }
            out.text += std::format("{}{} {}", c == 0 ? "" : "   ", texel.depth ? "D" : names[c], value);
        }
        return out;
    }

    namespace {
        /** What a type of a layout's is: its scalar, and how many of them. */
        std::optional<std::pair<char, std::uint32_t>> TypeOf(const std::string_view type)
        {
            static const std::pair<std::string_view, char> scalars[] { { "float", 'f' }, { "int", 'i' }, { "uint", 'u' }, { "bool", 'b' }, { "dword", 'u' } };
            for (const auto& [name, scalar] : scalars) {
                if (!type.starts_with(name)) continue;
                const std::string_view rest = type.substr(name.size());
                if (rest.empty()) return std::pair { scalar, 1u };
                if (rest.size() == 1 && rest[0] >= '1' && rest[0] <= '4') return std::pair { scalar, static_cast<std::uint32_t>(rest[0] - '0') };
                if (rest.size() == 3 && rest[1] == 'x' && rest[0] >= '1' && rest[0] <= '4' && rest[2] >= '1' && rest[2] <= '4')
                    return std::pair { scalar, static_cast<std::uint32_t>((rest[0] - '0') * (rest[2] - '0')) };
            }
            // GLSL's names.
            static const std::pair<std::string_view, char> vectors[] { { "vec", 'f' }, { "ivec", 'i' }, { "uvec", 'u' }, { "bvec", 'b' } };
            for (const auto& [name, scalar] : vectors)
                if (type.size() == name.size() + 1 && type.starts_with(name) && type.back() >= '2' && type.back() <= '4')
                    return std::pair { scalar, static_cast<std::uint32_t>(type.back() - '0') };
            if (type.starts_with("mat")) {
                const std::string_view rest = type.substr(3);
                if (rest.size() == 1 && rest[0] >= '2' && rest[0] <= '4') return std::pair { 'f', static_cast<std::uint32_t>((rest[0] - '0') * (rest[0] - '0')) };
                if (rest.size() == 3 && rest[1] == 'x' && rest[0] >= '2' && rest[0] <= '4' && rest[2] >= '2' && rest[2] <= '4')
                    return std::pair { 'f', static_cast<std::uint32_t>((rest[0] - '0') * (rest[2] - '0')) };
            }
            return std::nullopt;
        }

        std::string Number(const kgraph::Peek& peek, const std::size_t at, const char scalar, const bool hex)
        {
            if (at + 4 > peek.bytes.size()) return "";
            std::uint32_t word;
            std::memcpy(&word, peek.bytes.data() + at, 4);
            if (scalar == 'f') { float v; std::memcpy(&v, &word, 4); return std::format("{:.6g}", v); }
            if (hex) return std::format("0x{:08X}", word);
            if (scalar == 'i') return std::to_string(static_cast<std::int32_t>(word));
            if (scalar == 'b') return word ? "true" : "false";
            return std::to_string(word);
        }
    }

    std::optional<BufferLayout> ParseBufferLayout(const std::string_view text, std::string* error)
    {
        const auto fail = [&](std::string why) -> std::optional<BufferLayout> { if (error) *error = std::move(why); return std::nullopt; };
        BufferLayout layout;
        // Declarations, by ';', ',' or a line's end: a type, then perhaps a name, then perhaps [count].
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t end = text.find_first_of(";,\n", at);
            if (end == std::string_view::npos) end = text.size();
            std::string_view part = text.substr(at, end - at);
            at = end + 1;
            if (const auto comment = part.find("//"); comment != std::string_view::npos) part = part.substr(0, comment);
            std::vector<std::string_view> words;
            for (std::size_t i = 0; i < part.size();) {
                while (i < part.size() && std::isspace(static_cast<unsigned char>(part[i]))) ++i;
                const std::size_t from = i;
                while (i < part.size() && !std::isspace(static_cast<unsigned char>(part[i]))) ++i;
                if (i > from) words.push_back(part.substr(from, i - from));
            }
            if (words.empty()) continue;
            if (words.size() > 2) return fail(std::format("'{}' is more than a type and a name", std::string(part)));
            const auto type = TypeOf(words[0]);
            if (!type) return fail(std::format("'{}' is no type: float, int, uint or bool, a vector of them (float3, uvec2) or a matrix (float4x4, mat4)", std::string(words[0])));
            std::string name = words.size() == 2 ? std::string(words[1]) : std::format("field{}", layout.fields.size());
            std::uint32_t count = 1;
            if (const auto open = name.find('['); open != std::string::npos) {
                const auto close = name.find(']', open);
                if (close == std::string::npos) return fail(std::format("'{}' opens a [ it does not close", name));
                const std::string inside = name.substr(open + 1, close - open - 1);
                const auto [end, ec] = std::from_chars(inside.data(), inside.data() + inside.size(), count);
                if (ec != std::errc() || end != inside.data() + inside.size() || count == 0 || count > 4096) return fail(std::format("'{}' is no count of an array", inside));
                name = name.substr(0, open);
            }
            if (name.empty()) name = std::format("field{}", layout.fields.size());
            layout.fields.push_back({ name, layout.stride, type->first, type->second * count });
            layout.stride += type->second * count * 4;
        }
        if (layout.fields.empty()) return fail("there are no fields: `float3 position; uint id;`");
        return layout;
    }

    Table ToTable(const kgraph::Peek& peek, const BufferLayout* layout, const std::size_t first, const std::size_t count, const bool hex)
    {
        Table table;
        if (peek.image) return table;
        BufferLayout own;
        const bool shaders = !layout && peek.stride > 0 && !peek.fields.empty();
        if (shaders) { own.fields = peek.fields; own.stride = peek.stride; layout = &own; }
        const bool words = !layout;
        if (words) {
            for (std::uint32_t i = 0; i < 8; ++i) own.fields.push_back({ std::format("+{}", i * 4), i * 4, 'u', 1 });
            own.stride = 32;
            layout = &own;
        }
        // An element of more numbers than fit across — a matrix, a struct of many — is a line a field instead, four
        // numbers across: a vector on one line, a matrix as it reads, a row of it a line.
        std::size_t across = 0;
        for (const auto& field : layout->fields) across += field.numbers;
        const std::size_t used = peek.stride > 0 && peek.count > 0 ? std::min<std::size_t>(static_cast<std::size_t>(peek.stride) * peek.count, peek.bytes.size()) : peek.bytes.size();
        if (!words && across > 8) {
            table.byField = true;
            table.columns = { "#", "field", "x", "y", "z", "w" };
            table.total = layout->stride == 0 ? 0 : used / layout->stride;
            table.first = std::min(first, table.total);
            for (std::size_t row = table.first; row < std::min(table.total, table.first + count); ++row)
                for (const auto& field : layout->fields) {
                    const std::uint32_t lines = (field.numbers + 3) / 4;
                    const bool matrix = field.numbers == 16;
                    for (std::uint32_t line = 0; line < lines; ++line) {
                        std::vector<std::string> cells { line == 0 ? std::to_string(row) : std::string(),
                                                         line == 0 ? field.name : lines > 1 && !matrix ? std::format("[{}]", line * 4) : std::string() };
                        for (std::uint32_t n = 0; n < 4; ++n) {
                            // A matrix is stored a column after another: its row `line` is the line-th of each column.
                            const std::uint32_t at = matrix ? n * 4 + line : line * 4 + n;
                            cells.push_back(at < field.numbers ? Number(peek, row * layout->stride + field.offset + at * 4u, field.scalar, hex) : std::string());
                        }
                        table.rows.push_back(std::move(cells));
                    }
                }
            return table;
        }
        table.columns.push_back(words ? "byte" : "#");
        for (const auto& field : layout->fields) {
            static constexpr const char* parts[] { "x", "y", "z", "w" };
            for (std::uint32_t n = 0; n < field.numbers; ++n)
                table.columns.push_back(field.numbers == 1 ? field.name : field.numbers <= 4 ? std::format("{}.{}", field.name, parts[n]) : std::format("{}[{}]", field.name, n));
        }
        // As many rows as there are elements: of a buffer a shader lays out, those it holds, whatever room it has besides.
        table.total = layout->stride == 0 ? 0 : (used + (words ? layout->stride - 1 : 0)) / layout->stride;
        table.first = std::min(first, table.total);
        for (std::size_t row = table.first; row < std::min(table.total, table.first + count); ++row) {
            std::vector<std::string> cells { words ? std::to_string(row * layout->stride) : std::to_string(row) };
            for (const auto& field : layout->fields)
                for (std::uint32_t n = 0; n < field.numbers; ++n) cells.push_back(Number(peek, row * layout->stride + field.offset + n * 4u, words && !hex ? 'u' : field.scalar, hex));
            table.rows.push_back(std::move(cells));
        }
        return table;
    }

    std::vector<std::string> ToLines(const kgraph::Peek& peek, const std::size_t most)
    {
        std::vector<std::string> lines;
        if (peek.image) return lines;
        const auto number = [&](const std::size_t at, const char scalar) -> std::string {
            if (at + 4 > peek.bytes.size()) return "?";
            if (scalar == 'f') { float v; std::memcpy(&v, peek.bytes.data() + at, 4); return std::format("{:.4g}", v); }
            if (scalar == 'i') { std::int32_t v; std::memcpy(&v, peek.bytes.data() + at, 4); return std::to_string(v); }
            std::uint32_t v;
            std::memcpy(&v, peek.bytes.data() + at, 4);
            return std::to_string(v);
        };
        if (peek.stride > 0 && !peek.fields.empty()) {
            // By field: what the shader calls each value, and what it is in each element.
            for (std::uint32_t element = 0; element < std::max(peek.count, 1u) && lines.size() < most; ++element) {
                std::string line = std::format("[{}]", element);
                for (const auto& field : peek.fields) {
                    line += std::format("  {}", field.name);
                    for (std::uint32_t n = 0; n < std::min(field.numbers, 16u); ++n)
                        line += std::format("{}{}", n == 0 ? " " : ", ", number(static_cast<std::size_t>(element) * peek.stride + field.offset + n * 4, field.scalar));
                    if (field.numbers > 16) line += ", ...";
                }
                lines.push_back(std::move(line));
            }
            if (peek.count > most) lines.push_back(std::format("... and {} more", peek.count - most));
            return lines;
        }
        for (std::size_t at = 0; at + 4 <= peek.bytes.size() && lines.size() < most; at += 32) {
            std::string line = std::format("{:>6}:", at);
            for (std::size_t word = 0; word < 8 && at + word * 4 + 4 <= peek.bytes.size(); ++word) line += " " + number(at + word * 4, 'u');
            lines.push_back(std::move(line));
        }
        if (peek.bytes.size() > most * 32) lines.push_back(std::format("... of {} bytes", peek.bytes.size()));
        return lines;
    }
}
