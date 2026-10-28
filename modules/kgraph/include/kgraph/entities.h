//
// kgraph: what entities bring to a graph. A pipeline whose node leaves parameters unwired runs once for each entity
// that supplies them, in a component generated for it; this is how code reaches that component, and how resources
// made in code become ones a component can hold.
//
// @code
// const kecs::ComponentId blur = runtime.ComponentOf("blur");        // the pipeline node's id
// const kecs::Entity entity = world.Create();
// world.Add(entity, blur);
// kgraph::Parameters(world, entity, blur)
//     .Set("target", kgraph::MakeResource(std::move(image)))        // a parameter the node left unwired
//     .Set("radius", 4.f);                                           // one of the shader's push constants
// world.Add<kgraph::Mesh>(entity, { kgraph::MakeResource(std::move(mesh)) });   // what a Draw node draws it with
// @endcode
//

#pragma once

#include <cstring>
#include <memory>
#include <string_view>

#include <accelerationStructure.h>
#include <buffer.h>
#include <bufferView.h>
#include <image.h>
#include <imageView.h>
#include <kmath.h>
#include <mesh.h>
#include <resource.h>
#include <sampler.h>

#include <kecs/kecs.h>
#include <kecs/serialize.h>

#include "runtime.h"

namespace kgraph
{
    /** @brief A resource made in code, as one a component or a graph can hold. It lives as long as something holds it. */
    [[nodiscard]] std::shared_ptr<Resource> MakeResource(kor::Resource<kor::Image> image);
    [[nodiscard]] std::shared_ptr<Resource> MakeResource(kor::Resource<kor::Buffer> buffer);
    [[nodiscard]] std::shared_ptr<Resource> MakeResource(kor::Resource<kor::ImageView> view);
    [[nodiscard]] std::shared_ptr<Resource> MakeResource(kor::Resource<kor::Sampler> sampler);
    [[nodiscard]] std::shared_ptr<Resource> MakeResource(kor::Resource<kor::BufferView> view);
    [[nodiscard]] std::shared_ptr<Resource> MakeResource(kor::Resource<kor::Mesh> mesh);
    [[nodiscard]] std::shared_ptr<Resource> MakeResource(kor::Resource<kor::AccelerationStructure> structure);

    /** @brief The mesh an entity is drawn with: what a Draw node asks of every entity it draws. */
    struct Mesh {
        std::shared_ptr<Resource> mesh;
    };

    /** @brief One entity's values of a component generated for a pipeline, by field name. */
    class Parameters {
    public:
        /** @brief Of @p entity 's component @p id. Not Valid() when the entity does not have it. */
        Parameters(kecs::World& world, kecs::Entity entity, kecs::ComponentId id);

        [[nodiscard]] bool Valid() const { return _bytes != nullptr; }
        /** @brief Whether it has a field so named. */
        [[nodiscard]] bool Has(std::string_view field) const;

        /** @brief Gives resource field @p field what to hold. Throws (std::invalid_argument) when there is no such field, or it holds values. */
        Parameters& Set(std::string_view field, std::shared_ptr<Resource> resource);
        Parameters& Set(const std::string_view field, std::nullptr_t) { return Set(field, std::shared_ptr<Resource>()); }
        /** @brief Sets value field @p field. Throws (std::invalid_argument) when there is no such field, or @p T is not its size. */
        template <typename T> requires (std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_null_pointer_v<T>)
        Parameters& Set(const std::string_view field, const T& value) { Write(field, &value, sizeof(T)); return *this; }

        /** @brief What resource field @p field holds; null when nothing, or when it is no resource field. */
        [[nodiscard]] std::shared_ptr<Resource> ResourceAt(std::string_view field) const;
        /** @brief Value field @p field. Throws (std::invalid_argument) when there is no such field, or @p T is not its size. */
        template <typename T> requires std::is_trivially_copyable_v<T>
        [[nodiscard]] T Get(const std::string_view field) const { T value; Read(field, &value, sizeof(T)); return value; }

    private:
        void Write(std::string_view field, const void* value, std::size_t size);
        void Read(std::string_view field, void* value, std::size_t size) const;
        [[nodiscard]] const kecs::FieldDescription& Field(std::string_view field, bool resource) const;

        kecs::World& _world;
        kecs::Entity _entity;
        kecs::ComponentId _id;
        std::byte* _bytes = nullptr;
    };

    /**
     * @brief Teaches @p codecs to save and load what the framework's types are in a component: its vectors and
     *        matrices as lists of numbers, and a resource as the id of the node that made it.
     * @param runtime Where a loaded resource is looked up, by node id. A resource made in code is saved as null,
     *                and so is every resource when this is null.
     */
    void RegisterCodecs(kecs::Codecs& codecs, const Runtime* runtime);
}

// What the framework's types are called in a component's fields: what they are saved and matched by.
template <> struct kecs::Name<kgraph::Mesh> { static constexpr std::string_view value = "Mesh"; };
template <> struct kecs::Name<std::shared_ptr<kgraph::Resource>> { static constexpr std::string_view value = "resource"; };
template <> struct kecs::Name<kor::Vec2> { static constexpr std::string_view value = "vec2"; };
template <> struct kecs::Name<kor::Vec3> { static constexpr std::string_view value = "vec3"; };
template <> struct kecs::Name<kor::Vec4> { static constexpr std::string_view value = "vec4"; };
template <> struct kecs::Name<kor::IVec2> { static constexpr std::string_view value = "ivec2"; };
template <> struct kecs::Name<kor::IVec3> { static constexpr std::string_view value = "ivec3"; };
template <> struct kecs::Name<kor::IVec4> { static constexpr std::string_view value = "ivec4"; };
template <> struct kecs::Name<kor::UVec2> { static constexpr std::string_view value = "uvec2"; };
template <> struct kecs::Name<kor::UVec3> { static constexpr std::string_view value = "uvec3"; };
template <> struct kecs::Name<kor::UVec4> { static constexpr std::string_view value = "uvec4"; };
template <> struct kecs::Name<kor::Mat4> { static constexpr std::string_view value = "mat4"; };
template <> struct kecs::Name<kor::Quat> { static constexpr std::string_view value = "quat"; };

template <> struct kecs::Fields<kgraph::Mesh> {
    // A mesh, not any resource: what an editor offers it is meshes.
    static void Describe(kecs::FieldList<kgraph::Mesh>& list) { list.Field("mesh", &kgraph::Mesh::mesh, "Mesh"); }
};
