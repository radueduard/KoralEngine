//
// keditor: what a node holds, made something to look at. A runtime copies a watched node's image or buffer out as
// bytes (kgraph::Peek); here an image's bytes become a picture a display shows — whatever its format, a depth or a
// count as much as a colour — and a buffer's become lines of numbers, by field where its shader says what it is.
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <kmath/vector.h>

#include <kgraph/runtime.h>

namespace keditor
{
    /** @brief How an image's values are made a picture. */
    enum class LookMode : std::uint8_t {
        eAuto,          ///< As it is for a colour; stretched for a depth, a count, or anything of one channel.
        eAsItIs,        ///< 0 is black and 1 is white. Light (a float format) is shown as a display shows it.
        eStretched,     ///< Its least value is black and its greatest white.
        eRed, eGreen, eBlue, eAlpha,    ///< One channel, stretched.
    };
    [[nodiscard]] const std::vector<std::string>& LookModeNames();

    struct Picture {
        std::vector<kor::U8Vec4> pixels;
        kor::UVec2 size { 0, 0 };
        double least = 0, greatest = 0;     ///< Of the image's values, over the channels shown.
    };

    /** @brief How an image is made a picture: what ToPicture is told besides the image. */
    struct LookOptions {
        LookMode mode = LookMode::eAuto;
        std::uint32_t slice = 0;                            ///< Of an image with depth.
        /// Which of red, green, blue and alpha are shown. One alone is grey; alpha with colours is the colours over a
        /// checkerboard, as much as alpha says.
        std::array<bool, 4> channels { true, true, true, false };
        /// Set: the values that are black and white, in place of what the mode says — what is between, stretched.
        std::optional<double> black, white;
        std::uint32_t most = 1024;                          ///< One wider than this is shown a texel in every few.
    };

    /**
     * @brief An image's texels as a picture: slice @p slice of it, for one with depth. One wider than @p most is
     *        shown a texel in every few. Empty when @p peek is no image, or is of a format that cannot be read.
     */
    [[nodiscard]] Picture ToPicture(const kgraph::Peek& peek, LookMode mode = LookMode::eAuto, std::uint32_t slice = 0, std::uint32_t most = 1024);
    [[nodiscard]] Picture ToPicture(const kgraph::Peek& peek, const LookOptions& options);

    /** @brief How an image's values are spread: per channel, how many texels in each of so many bins between its least and greatest. */
    struct Histogram {
        int channels = 0;
        std::array<std::vector<std::uint32_t>, 4> bins;     ///< One list a channel, of the same bins.
        double least = 0, greatest = 0;                      ///< What the bins run between: every channel's finite values.
        std::array<double, 4> min {}, max {}, mean {};       ///< Each channel's, of its finite values.
        std::uint64_t texels = 0;                            ///< Counted: one in every few of a big image.
        std::uint64_t nans = 0, infinities = 0;              ///< Values of a float that are not numbers, or are endless.
    };
    /** @brief The histogram of slice @p slice of an image. No channels when @p peek is no image that can be read. */
    [[nodiscard]] Histogram HistogramOf(const kgraph::Peek& peek, std::uint32_t slice = 0, std::size_t bins = 256);

    /** @brief One texel's values, as they are stored and as numbers. */
    struct Texel {
        int channels = 0;
        std::array<double, 4> values {};
        std::string text;               ///< `R 0.5 (128)   G 0.25 (64)   ...`
    };
    /** @brief The texel at @p x, @p y of slice @p slice: none outside the image, or for what cannot be read. */
    [[nodiscard]] std::optional<Texel> TexelAt(const kgraph::Peek& peek, std::uint32_t x, std::uint32_t y, std::uint32_t slice = 0);

    /** @brief What a buffer's elements are made of, as a person says it: fields one after another, packed. */
    struct BufferLayout {
        std::vector<kgraph::Peek::Field> fields;
        std::uint32_t stride = 0;       ///< The bytes of one element.
    };
    /**
     * @brief A layout from text, as a shader declares a struct: `float3 position; uint id; float4x4 model; float
     *        weights[4];` — float, int, uint and bool, as scalars, vectors (`float3`, `vec3`, `uint2`, `uvec2`) and
     *        matrices (`float4x4`, `mat4`), each of four bytes a number, one after another with nothing between. A
     *        field's name may be left out. Nothing (and @p error says why) when it is not one.
     */
    [[nodiscard]] std::optional<BufferLayout> ParseBufferLayout(std::string_view text, std::string* error = nullptr);

    /** @brief A buffer's elements as a table: a row an element, a column a number of it. */
    struct Table {
        std::vector<std::string> columns;               ///< The first is the element's index (or, for words, its byte).
        std::vector<std::vector<std::string>> rows;
        std::size_t total = 0;                          ///< How many elements (or rows of words) there are in all, of which these are from @ref first.
        std::size_t first = 0;
        /// An element of more than eight numbers is a line a field — `#`, `field`, then four numbers across, a matrix a row
        /// of it a line — rather than a row of its own.
        bool byField = false;
    };
    /**
     * @brief Rows @p first to @p first + @p count of a buffer: by @p layout when given, by the fields its shader gives
     *        it otherwise (a ShaderBuffer's), and as eight words a row when neither says. @p hex writes whole numbers
     *        in hexadecimal.
     */
    [[nodiscard]] Table ToTable(const kgraph::Peek& peek, const BufferLayout* layout, std::size_t first, std::size_t count, bool hex = false);

    /**
     * @brief A buffer's bytes as lines: an element a line with each field by name, for a buffer whose shader says
     *        what it is made of; otherwise its words, eight a line, as whole numbers. At most @p most lines.
     */
    [[nodiscard]] std::vector<std::string> ToLines(const kgraph::Peek& peek, std::size_t most = 48);
}
