//
// kgraph: a buffer, as a shader lays it out. What a ShaderBuffer node reads of a compiled shader to know the struct a
// `StructuredBuffer<Light>` or a `ConstantBuffer<FrameData>` is made of: each field's name, where in an element it
// is, and what it is. The framework's own reflection stops at a block's members, and a StructuredBuffer's one member
// is its array; this reads the SPIR-V down to the fields.
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kgraph
{
    /** One value of an element: a number, a vector, a matrix, or a fixed array of one of those. */
    struct BlockField {
        std::string name;               ///< From the element: `position`, `material.albedo`, `cascades[1].split`.
        std::uint32_t offset = 0;       ///< Where it is, in bytes from the start of the element.
        char scalar = 'f';              ///< 'f' a float, 'i' an int, 'u' a uint, 'b' a bool: four bytes each.
        std::uint32_t components = 1;   ///< How many numbers one after another: a vector's, or one line of a matrix.
        std::uint32_t lines = 1;        ///< A matrix's lines; 1 for a number or a vector.
        std::uint32_t lineStride = 0;   ///< Bytes from one line of a matrix to the next.
        std::uint32_t count = 1;        ///< Array elements, or 1.
        std::uint32_t arrayStride = 0;  ///< Bytes from one array element to the next.

        /** How many numbers it takes. */
        [[nodiscard]] std::uint32_t Numbers() const { return components * lines * count; }
    };

    /** A uniform or storage buffer a shader declares: one element's fields, and whether there are many of them. */
    struct BlockLayout {
        std::string name;               ///< What the shader calls the buffer: `lights`.
        std::string type;               ///< What it calls the struct: `Light`.
        bool storage = false;           ///< A storage buffer; a uniform block when not.
        bool array = false;             ///< As many elements as there is room for: a StructuredBuffer, a `Light lights[]`.
        std::uint32_t first = 0;        ///< Where the first element is, in bytes from the start of the buffer.
        std::uint32_t stride = 0;       ///< Bytes from one element to the next: one element's size, for a block of one.
        std::vector<BlockField> fields;

        [[nodiscard]] const BlockField* Find(std::string_view field) const;
    };

    /** @brief Every uniform and storage buffer the compiled shader @p spirv declares. Empty when it is no SPIR-V. */
    [[nodiscard]] std::vector<BlockLayout> BlocksOf(std::span<const std::uint32_t> spirv);
}
