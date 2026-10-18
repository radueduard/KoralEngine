//
// kgraph: a buffer, as a shader lays it out — read from the shader's SPIR-V. Only what says how a block is laid out
// is read: the types, their names, and the offsets and strides they are decorated with.
//

#include "blocks.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>

namespace kgraph
{
    namespace {
        // The instructions, decorations and storage classes read here, by the numbers the SPIR-V specification gives them.
        enum : std::uint32_t {
            OpName = 5, OpMemberName = 6, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
            OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59,
            OpDecorate = 71, OpMemberDecorate = 72,
            BufferBlock = 3, RowMajor = 4, ArrayStride = 6, MatrixStride = 7, Offset = 35,
            Uniform = 2, StorageBuffer = 12,
        };
        constexpr std::uint32_t Magic = 0x07230203;
        constexpr std::uint32_t MostElements = 64;      // of an array of structs inside an element: each is fields of its own

        struct Type {
            std::uint32_t op = 0;
            std::uint32_t width = 32;               // a float's or an int's, in bits
            bool isSigned = false;
            std::uint32_t of = 0;                   // what a vector, a matrix, an array or a pointer is of
            std::uint32_t count = 0;                // a vector's components, a matrix's columns, an array's length (a constant's id)
            std::uint32_t storage = 0;              // a pointer's storage class
            std::vector<std::uint32_t> members;     // a struct's
        };

        using Member = std::pair<std::uint32_t, std::uint32_t>;     // a struct, and which of its members

        struct Module {
            std::map<std::uint32_t, Type> types;
            std::map<std::uint32_t, std::string> names;
            std::map<Member, std::string> memberNames;
            std::map<std::uint32_t, std::uint32_t> constants, arrayStrides;
            std::map<Member, std::uint32_t> offsets, matrixStrides;
            std::set<Member> rowMajor;
            std::set<std::uint32_t> bufferBlocks;
            std::vector<std::array<std::uint32_t, 3>> variables;    // its pointer type, itself, its storage class

            [[nodiscard]] const Type* Find(const std::uint32_t id) const
            {
                const auto it = types.find(id);
                return it == types.end() ? nullptr : &it->second;
            }
            template <typename Key>
            [[nodiscard]] static std::uint32_t At(const std::map<Key, std::uint32_t>& map, const Key& key)
            {
                const auto it = map.find(key);
                return it == map.end() ? 0 : it->second;
            }
        };

        /** A string as an instruction holds it: its bytes in words, low byte first, up to a zero. */
        std::string Text(const std::span<const std::uint32_t> words)
        {
            std::string text;
            for (const std::uint32_t word : words)
                for (int byte = 0; byte < 4; ++byte) {
                    const char c = static_cast<char>((word >> (byte * 8)) & 0xFF);
                    if (c == 0) return text;
                    text += c;
                }
            return text;
        }

        Module Read(const std::span<const std::uint32_t> spirv)
        {
            Module module;
            if (spirv.size() < 5 || spirv[0] != Magic) return module;
            for (std::size_t at = 5; at < spirv.size();) {
                const std::uint32_t length = spirv[at] >> 16, op = spirv[at] & 0xFFFF;
                if (length == 0 || at + length > spirv.size()) break;
                const auto w = spirv.subspan(at + 1, length - 1);     // what follows the instruction's first word
                at += length;
                switch (op) {
                case OpName: if (w.size() >= 2) module.names[w[0]] = Text(w.subspan(1)); break;
                case OpMemberName: if (w.size() >= 3) module.memberNames[{ w[0], w[1] }] = Text(w.subspan(2)); break;
                case OpTypeBool: if (w.size() >= 1) module.types[w[0]] = { .op = op }; break;
                case OpTypeInt: if (w.size() >= 3) module.types[w[0]] = { .op = op, .width = w[1], .isSigned = w[2] != 0 }; break;
                case OpTypeFloat: if (w.size() >= 2) module.types[w[0]] = { .op = op, .width = w[1] }; break;
                case OpTypeVector: case OpTypeMatrix: case OpTypeArray:
                    if (w.size() >= 3) module.types[w[0]] = { .op = op, .of = w[1], .count = w[2] };
                    break;
                case OpTypeRuntimeArray: if (w.size() >= 2) module.types[w[0]] = { .op = op, .of = w[1] }; break;
                case OpTypeStruct:
                    if (w.size() >= 1) module.types[w[0]] = { .op = op, .members = { w.begin() + 1, w.end() } };
                    break;
                case OpTypePointer: if (w.size() >= 3) module.types[w[0]] = { .op = op, .of = w[2], .storage = w[1] }; break;
                case OpConstant: if (w.size() >= 3) module.constants[w[1]] = w[2]; break;
                case OpVariable: if (w.size() >= 3) module.variables.push_back({ w[0], w[1], w[2] }); break;
                case OpDecorate:
                    if (w.size() >= 3 && w[1] == ArrayStride) module.arrayStrides[w[0]] = w[2];
                    else if (w.size() >= 2 && w[1] == BufferBlock) module.bufferBlocks.insert(w[0]);
                    break;
                case OpMemberDecorate:
                    if (w.size() >= 4 && w[2] == Offset) module.offsets[{ w[0], w[1] }] = w[3];
                    else if (w.size() >= 4 && w[2] == MatrixStride) module.matrixStrides[{ w[0], w[1] }] = w[3];
                    else if (w.size() >= 3 && w[2] == RowMajor) module.rowMajor.insert({ w[0], w[1] });
                    break;
                default: break;
                }
            }
            return module;
        }

        /** A number, a vector or a matrix as a field; nothing for anything else, or for numbers that are not of 32 bits. */
        std::optional<BlockField> Leaf(const Module& module, const std::uint32_t id, const std::uint32_t matrixStride, const bool rowMajor)
        {
            const Type* type = module.Find(id);
            BlockField field;
            std::uint32_t columns = 1;
            if (type && type->op == OpTypeMatrix) { columns = type->count; type = module.Find(type->of); }
            if (type && type->op == OpTypeVector) { field.components = type->count; type = module.Find(type->of); }
            if (!type) return std::nullopt;
            if (type->op == OpTypeBool) field.scalar = 'b';
            else if (type->op == OpTypeFloat && type->width == 32) field.scalar = 'f';
            else if (type->op == OpTypeInt && type->width == 32) field.scalar = type->isSigned ? 'i' : 'u';
            else return std::nullopt;
            if (columns > 1) {
                // In memory a matrix is its columns one after another, or — decorated so — its rows.
                field.lines = columns;
                if (rowMajor) std::swap(field.lines, field.components);
                field.lineStride = matrixStride != 0 ? matrixStride : field.components * 4;
            }
            return field;
        }

        /** Every value in struct @p id, and in the structs in it: each by its path, where it is from @p base. */
        void Flatten(const Module& module, const std::uint32_t id, const std::string& prefix, const std::uint32_t base, std::vector<BlockField>& out, const int depth = 0)
        {
            const Type* type = module.Find(id);
            if (!type || type->op != OpTypeStruct || depth > 8) return;
            for (std::uint32_t i = 0; i < type->members.size(); ++i) {
                const Member member { id, i };
                const auto named = module.memberNames.find(member);
                const std::string name = prefix + (named == module.memberNames.end() || named->second.empty() ? std::format("member{}", i) : named->second);
                const std::uint32_t offset = base + Module::At(module.offsets, member);
                std::uint32_t of = type->members[i], count = 1, stride = 0;
                const Type* each = module.Find(of);
                if (each && each->op == OpTypeArray) {
                    count = Module::At(module.constants, each->count);
                    stride = Module::At(module.arrayStrides, of);
                    of = each->of;
                    each = module.Find(of);
                }
                if (!each || count == 0) continue;
                if (each->op == OpTypeStruct) {
                    if (count == 1 && stride == 0) Flatten(module, of, name + ".", offset, out, depth + 1);
                    else
                        for (std::uint32_t element = 0; element < std::min(count, MostElements); ++element)
                            Flatten(module, of, std::format("{}[{}].", name, element), offset + element * stride, out, depth + 1);
                    continue;
                }
                auto field = Leaf(module, of, Module::At(module.matrixStrides, member), module.rowMajor.contains(member));
                if (!field) continue;   // an array of arrays, an array with no end inside an element, a double
                field->name = name;
                field->offset = offset;
                field->count = count;
                field->arrayStride = stride;
                out.push_back(std::move(*field));
            }
        }

        /** `Light_std430` is `Light`: the compiler names a struct again for each way it is laid out. */
        std::string Plain(std::string name)
        {
            for (const std::string_view suffix : { "_std140", "_std430", "_natural", "_scalar" })
                if (name.ends_with(suffix)) name.erase(name.size() - suffix.size());
            return name;
        }
    }

    const BlockField* BlockLayout::Find(const std::string_view field) const
    {
        const auto it = std::ranges::find(fields, field, &BlockField::name);
        return it == fields.end() ? nullptr : &*it;
    }

    std::vector<BlockLayout> BlocksOf(const std::span<const std::uint32_t> spirv)
    {
        std::vector<BlockLayout> blocks;
        const Module module = Read(spirv);
        for (const auto& [pointer, variable, storage] : module.variables) {
            if (storage != Uniform && storage != StorageBuffer) continue;
            const Type* pointed = module.Find(pointer);
            std::uint32_t id = pointed ? pointed->of : 0;
            const Type* block = module.Find(id);
            // An array of blocks is laid out as one of them is.
            while (block && (block->op == OpTypeArray || block->op == OpTypeRuntimeArray)) { id = block->of; block = module.Find(id); }
            if (!block || block->op != OpTypeStruct || block->members.empty()) continue;

            BlockLayout layout;
            const auto named = module.names.find(variable);
            const auto typeName = module.names.find(id);
            layout.type = Plain(typeName == module.names.end() ? std::string() : typeName->second);
            layout.name = named == module.names.end() || named->second.empty() ? layout.type : named->second;
            layout.storage = storage == StorageBuffer || module.bufferBlocks.contains(id);

            const auto last = static_cast<std::uint32_t>(block->members.size() - 1);
            const Type* tail = module.Find(block->members[last]);
            if (tail && tail->op == OpTypeRuntimeArray) {
                // As many of one thing as there is room for: the thing is what is laid out, wherever in the block they start.
                layout.array = true;
                layout.first = Module::At(module.offsets, Member { id, last });
                layout.stride = Module::At(module.arrayStrides, block->members[last]);
                const Type* element = module.Find(tail->of);
                if (element && element->op == OpTypeStruct) {
                    if (const auto it = module.names.find(tail->of); it != module.names.end()) layout.type = Plain(it->second);
                    Flatten(module, tail->of, {}, 0, layout.fields);
                } else if (auto field = Leaf(module, tail->of, 0, false)) {
                    field->name = "value";      // an array of numbers or vectors: each is the element
                    layout.fields.push_back(std::move(*field));
                }
            } else {
                Flatten(module, id, {}, 0, layout.fields);
            }
            // An array in a uniform block is compiled as a struct of one member, `data`, to give it the block's
            // strides: the field is the array, by the name the shader gave it.
            for (auto& field : layout.fields)
                if (field.name.ends_with(".data")) field.name.erase(field.name.size() - 5);
            std::uint32_t end = 0;
            for (const auto& field : layout.fields)
                end = std::max(end, field.offset + (field.count - 1) * field.arrayStride + (field.lines - 1) * field.lineStride + field.components * 4);
            if (layout.stride == 0) layout.stride = (end + 15) / 16 * 16;
            if (layout.fields.empty() || layout.stride < end) continue;
            blocks.push_back(std::move(layout));
        }
        return blocks;
    }
}
