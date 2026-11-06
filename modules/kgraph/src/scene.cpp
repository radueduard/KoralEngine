//
// kgraph: the scene's own components, and what little they do.
//

#include <kgraph/scene.h>

#include <cstring>

#include <kmath/quaternion.h>
#include <kmath/transform.h>

namespace kgraph
{
    namespace {
        constexpr float Radians = 3.14159265358979323846f / 180.f;
    }

    kor::Mat4 ToKor(const kecs::Mat4& matrix)
    {
        kor::Mat4 out(1.f);
        std::memcpy(static_cast<void*>(&out), matrix.m.data(), 64);     // sixteen floats, a column after another, as both hold them
        return out;
    }

    kecs::Mat4 ToKecs(const kor::Mat4& matrix)
    {
        kecs::Mat4 out;
        std::memcpy(out.m.data(), &matrix, 64);
        return out;
    }

    kor::Mat4 View::Projection(const float aspect, const bool flipY) const
    {
        const float high = orthographic * 0.5f, wide = high * aspect;
        kor::Mat4 projection = orthographic > 0.f ? kor::Orthographic(-wide, wide, -high, high, near, far) : kor::Perspective(fieldOfView, aspect, near, far);
        if (flipY) projection[1][1] = -projection[1][1];
        return projection;
    }

    View ViewOf(const kecs::World& world, const kecs::Entity entity)
    {
        View view;
        kor::Vec3 position, scale;
        kor::Quat rotation;
        if (kor::Decompose(ToKor(world.WorldMatrix(entity)), position, rotation, scale))
            view.view = kor::Inverse(kor::Compose(position, rotation, kor::Vec3(1.f)));
        // Its lens: whichever of its components says how wide it sees — through a field of view, or, with a near and a far,
        // in a box of a height.
        for (const kecs::ComponentId id : world.ComponentsOf(entity)) {
            const kecs::ComponentDescription& type = world.Describe(id);
            const auto* bytes = static_cast<const std::byte*>(world.Get(entity, id));
            const auto number = [&](const char* name, const float otherwise) {
                const kecs::FieldDescription* field = type.FindField(name);
                if (!bytes || !field || field->type != "f32") return otherwise;
                float v = otherwise;
                std::memcpy(&v, bytes + field->offset, 4);
                return v;
            };
            const bool lens = type.FindField("fieldOfView") != nullptr;
            const bool box = !lens && type.FindField("height") && type.FindField("near") && type.FindField("far");
            if (!lens && !box) continue;
            if (lens) view.fieldOfView = number("fieldOfView", 60.f) * Radians;
            else view.orthographic = std::max(number("height", 10.f), 0.001f);
            view.near = std::max(number("near", 0.1f), 0.0001f);
            view.far = std::max(number("far", 100.f), view.near + 0.001f);
            break;
        }
        return view;
    }
}
