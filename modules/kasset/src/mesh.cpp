#include <kasset/mesh.h>

#include <algorithm>
#include <format>

namespace kasset
{
    const VertexAttribute* VertexData::Find(const std::string_view semantic) const
    {
        const auto it = std::ranges::find(attributes, semantic, &VertexAttribute::semantic);
        return it == attributes.end() ? nullptr : &*it;
    }
    VertexAttribute* VertexData::Find(const std::string_view semantic)
    {
        const auto it = std::ranges::find(attributes, semantic, &VertexAttribute::semantic);
        return it == attributes.end() ? nullptr : &*it;
    }

    std::string VertexData::Problem() const
    {
        if (vertexCount == 0) return "it has no vertices";
        for (std::size_t i = 0; i < attributes.size(); ++i) {
            const VertexAttribute& attribute = attributes[i];
            if (attribute.semantic.empty()) return std::format("attribute {} has no semantic: what it is, as a shader asks for it", i);
            if (attribute.count < 1 || attribute.count > 4) return std::format("'{}' has {} numbers a vertex, and an attribute has 1 to 4", attribute.semantic, attribute.count);
            if (attribute.words.size() != static_cast<std::size_t>(attribute.count) * vertexCount)
                return std::format("'{}' has {} numbers, and {} vertices of {} each are {}", attribute.semantic, attribute.words.size(), vertexCount, attribute.count,
                                   static_cast<std::size_t>(attribute.count) * vertexCount);
            for (std::size_t j = 0; j < i; ++j)
                if (attributes[j].semantic == attribute.semantic) return std::format("'{}' is there twice", attribute.semantic);
        }
        const VertexAttribute* position = Find("POSITION");
        if (!position || position->type != AttributeType::eFloat || position->count < 2) return "it needs a POSITION of floats, two to four of them a vertex";
        for (const std::uint32_t index : indices)
            if (index >= vertexCount) return std::format("an index is {}, and there are {} vertices", index, vertexCount);
        for (const auto& buffer : buffers)
            for (const auto& semantic : buffer)
                if (!Find(semantic)) return std::format("a buffer names '{}', which it has not", semantic);
        return {};
    }
}
