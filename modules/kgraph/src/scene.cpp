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
        std::memcpy(&out, matrix.m.data(), 64);
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
        kor::Mat4 projection = kor::Perspective(fieldOfView, aspect, near, far);
        if (flipY) projection[1][1] = -projection[1][1];
        return projection;
    }

    void RegisterSceneComponents(kecs::World& world)
    {
        (void)world.Id<Camera>();
        (void)world.Id<DirectionalLight>();
        (void)world.Id<PointLight>();
        (void)world.Id<SpotLight>();
        (void)world.Id<Sky>();
    }

    View ViewOf(const kecs::World& world, const kecs::Entity entity)
    {
        View view;
        kor::Vec3 position, scale;
        kor::Quat rotation;
        if (kor::Decompose(ToKor(world.WorldMatrix(entity)), position, rotation, scale))
            view.view = kor::Inverse(kor::Compose(position, rotation, kor::Vec3(1.f)));
        if (const Camera* camera = world.Get<Camera>(entity)) {
            view.fieldOfView = camera->fieldOfView * Radians;
            view.near = camera->near;
            view.far = std::max(camera->far, camera->near + 0.001f);
        }
        return view;
    }

    kecs::Entity FirstCamera(kecs::World& world)
    {
        const kecs::ComponentId id = world.Id<Camera>();
        for (const kecs::Entity entity : world.All())
            if (world.Has(entity, id) && world.IsEnabled(entity, id)) return entity;
        return kecs::NullEntity;
    }
}
