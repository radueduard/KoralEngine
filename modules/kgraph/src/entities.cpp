//
// kgraph: what entities bring to a graph. Resources made in code as ones a component holds, a component's values by
// field name, and how the framework's types are saved in one.
//

#include <kgraph/entities.h>

#include <format>
#include <stdexcept>

#include <capiInterop.h>
#include <koral_c.h>

#include <kecs/json.h>

#include "names.h"

namespace kgraph
{
    namespace {
        using Held = std::shared_ptr<Resource>;

        Held Wrap(KoralResource* handle, const char* kind)
        {
            auto resource = std::make_shared<Resource>();
            resource->handle = handle;
            resource->kind = kind;
            return resource;
        }
    }

    Held MakeResource(kor::Resource<kor::Image> image) { return Wrap(kor::capi::Adopt(std::move(image)), "Image"); }
    Held MakeResource(kor::Resource<kor::Buffer> buffer) { return Wrap(kor::capi::Adopt(std::move(buffer)), "Buffer"); }
    Held MakeResource(kor::Resource<kor::ImageView> view) { return Wrap(kor::capi::Adopt(std::move(view)), "ImageView"); }
    Held MakeResource(kor::Resource<kor::Sampler> sampler) { return Wrap(kor::capi::Adopt(std::move(sampler)), "Sampler"); }
    Held MakeResource(kor::Resource<kor::BufferView> view) { return Wrap(kor::capi::Adopt(std::move(view)), "BufferView"); }
    Held MakeResource(kor::Resource<kor::Mesh> mesh) { return Wrap(kor::capi::Adopt(std::move(mesh)), "Mesh"); }
    Held MakeResource(kor::Resource<kor::AccelerationStructure> structure) { return Wrap(kor::capi::Adopt(std::move(structure)), "AccelerationStructure"); }

    // ---- a component's values, by field ---------------------------------------------------------------------------

    Parameters::Parameters(kecs::World& world, const kecs::Entity entity, const kecs::ComponentId id) : _world(world), _entity(entity), _id(id)
    {
        if (id != kecs::NoComponent && world.Alive(entity))
            _bytes = static_cast<std::byte*>(const_cast<void*>(std::as_const(world).Get(entity, id)));
    }

    bool Parameters::Has(const std::string_view field) const
    {
        return _id != kecs::NoComponent && _world.Describe(_id).FindField(field) != nullptr;
    }

    const kecs::FieldDescription& Parameters::Field(const std::string_view field, const bool resource) const
    {
        if (!_bytes) throw std::invalid_argument("kgraph: the entity does not have the component");
        const kecs::ComponentDescription& type = _world.Describe(_id);
        const kecs::FieldDescription* found = type.FindField(field);
        if (!found) {
            std::string known;
            for (const auto& f : type.fields) known += std::format("{}{}", known.empty() ? "" : ", ", f.name);
            throw std::invalid_argument(std::format("kgraph: '{}' has no field '{}'. It has: {}", type.name, field, known.empty() ? "none" : known));
        }
        if (IsResourceType(found->type) != resource)
            throw std::invalid_argument(std::format("kgraph: '{}.{}' holds {}", type.name, field, resource ? "a value, not a resource" : "a resource, not a value"));
        return *found;
    }

    Parameters& Parameters::Set(const std::string_view field, Held resource)
    {
        const kecs::FieldDescription& found = Field(field, true);
        *reinterpret_cast<Held*>(_bytes + found.offset) = std::move(resource);
        _world.MarkChanged(_entity, _id);
        return *this;
    }

    Held Parameters::ResourceAt(const std::string_view field) const
    {
        if (!_bytes) return nullptr;
        const kecs::FieldDescription* found = _world.Describe(_id).FindField(field);
        return found && IsResourceType(found->type) ? *reinterpret_cast<const Held*>(_bytes + found->offset) : nullptr;
    }

    void Parameters::Write(const std::string_view field, const void* value, const std::size_t size)
    {
        const kecs::FieldDescription& found = Field(field, false);
        if (found.size != size)
            throw std::invalid_argument(std::format("kgraph: '{}' is a {} of {} bytes, and was given {}", field, found.type, found.size, size));
        std::memcpy(_bytes + found.offset, value, size);
        _world.MarkChanged(_entity, _id);
    }

    void Parameters::Read(const std::string_view field, void* value, const std::size_t size) const
    {
        const kecs::FieldDescription& found = Field(field, false);
        if (found.size != size)
            throw std::invalid_argument(std::format("kgraph: '{}' is a {} of {} bytes, and {} were asked for", field, found.type, found.size, size));
        std::memcpy(value, _bytes + found.offset, size);
    }

    // ---- saving -----------------------------------------------------------------------------------------------

    namespace {
        /** A list of @p count numbers of type T, one after another. */
        template <typename T>
        kecs::Codec Numbers(const std::size_t count)
        {
            return {
                [count](const void* value, const kecs::SaveContext&) {
                    std::string out = "[";
                    for (std::size_t i = 0; i < count; ++i) {
                        T item;
                        std::memcpy(&item, static_cast<const std::byte*>(value) + i * sizeof(T), sizeof(T));
                        out += std::format("{}{}", i ? "," : "", item);
                    }
                    return out + "]";
                },
                [count](void* value, const std::string_view json, const kecs::LoadContext&) {
                    kecs::json::Value parsed;
                    std::string error;
                    if (!kecs::json::Parse(json, parsed, error) || parsed.kind != kecs::json::Value::Kind::eArray || parsed.items.size() != count) return false;
                    for (std::size_t i = 0; i < count; ++i) {
                        if (parsed.items[i].kind != kecs::json::Value::Kind::eNumber) return false;
                        const T item = static_cast<T>(parsed.items[i].number);
                        std::memcpy(static_cast<std::byte*>(value) + i * sizeof(T), &item, sizeof(T));
                    }
                    return true;
                } };
        }
    }

    void RegisterCodecs(kecs::Codecs& codecs, const Runtime* runtime)
    {
        for (const std::size_t n : { 2, 3, 4 }) {
            codecs.Register(std::format("vec{}", n), Numbers<float>(n));
            codecs.Register(std::format("ivec{}", n), Numbers<std::int32_t>(n));
            codecs.Register(std::format("uvec{}", n), Numbers<std::uint32_t>(n));
            codecs.Register(std::format("bvec{}", n), Numbers<std::uint32_t>(n));
        }
        codecs.Register("mat4", Numbers<float>(16));
        codecs.Register("quat", Numbers<float>(4));
        codecs.Register("bool32", Numbers<std::uint32_t>(1));

        // A resource is saved as the node that made it. One made in code has no name to be found again by: null.
        const kecs::Codec resource {
            [](const void* value, const kecs::SaveContext&) {
                const Held& held = *static_cast<const Held*>(value);
                return held && !held->node.empty() ? kecs::json::Quote(held->node) : std::string("null");
            },
            [runtime](void* value, const std::string_view json, const kecs::LoadContext&) {
                kecs::json::Value parsed;
                std::string error;
                if (!kecs::json::Parse(json, parsed, error)) return false;
                Held& held = *static_cast<Held*>(value);
                if (parsed.kind == kecs::json::Value::Kind::eNull) { held.reset(); return true; }
                if (parsed.kind != kecs::json::Value::Kind::eString) return false;
                held = runtime ? runtime->ResourceOf(parsed.string) : nullptr;
                return true;
            } };
        for (const char* type : { "resource", "Image", "Buffer", "Sampler", "BufferView", "AccelerationStructure", "Mesh" }) codecs.Register(type, resource);
        // A field of a kind of resource reads what was saved when it was any: a Mesh's mesh, saved as a "resource" before it said.
        for (const char* type : { "Image", "Buffer", "Sampler", "BufferView", "AccelerationStructure", "Mesh" }) codecs.Reads(type, "resource");
    }
}
