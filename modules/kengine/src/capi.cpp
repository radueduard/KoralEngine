// The engine's C interface: each function a call on the C++ object the handle is.

#include <koral_engine.h>

#include <cstring>
#include <format>
#include <optional>
#include <set>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include <frameGraph.h>

#include <kecs/json.h>
#include <keditor/document.h>
#include <keditor/play.h>
#include <kgraph/meshes.h>
#include <kscript/declarations.h>

namespace {
    thread_local std::string t_error;
    thread_local std::string t_text;

    const char* Keep(std::string text)
    {
        t_text = std::move(text);
        return t_text.c_str();
    }
    void Fail(std::string why) { t_error = std::move(why); }

    /** Runs @p body, turning anything thrown into the error and @p failed. */
    template <typename F, typename R>
    R Guarded(F&& body, R failed)
    {
        try {
            return body();
        } catch (const std::exception& e) {
            Fail(e.what());
            return failed;
        }
    }

    keditor::Document& Doc(KengineDocument* document)
    {
        if (!document) throw std::invalid_argument("the document is null");
        return *reinterpret_cast<keditor::Document*>(document);
    }
    kecs::World& World(KecsWorld* world)
    {
        if (!world) throw std::invalid_argument("the world is null");
        return *reinterpret_cast<kecs::World*>(world);
    }
    kecs::Entity Of(const KecsEntity entity) { return { entity.index, entity.generation }; }
    KecsEntity Of(const kecs::Entity entity) { return { entity.index, entity.generation }; }
    const char* Text(const char* text) { return text ? text : ""; }

    bool Parse(const char* json, kgraph::Value& out)
    {
        kecs::json::Value parsed;
        std::string error;
        if (!kecs::json::Parse(Text(json), parsed, error)) { Fail(std::format("not JSON: {}", error)); return false; }
        out = kgraph::Value::FromJson(parsed);
        return true;
    }
}

// Each is declared extern "C" in the header, and so is one here.

const char* kengine_last_error(void) { return t_error.c_str(); }

const char* kengine_package_directory(void)
{
    static thread_local std::string text;
    text = kscript::PackageDirectory().generic_string();
    return text.c_str();
}

// ---- a scene -----------------------------------------------------------------------------------------------------

KengineDocument* kengine_document(KoralFrameGraph* graph)
{
    keditor::Document* document = keditor::Document::Of(reinterpret_cast<const kor::FrameGraph*>(graph));
    if (!document) Fail("that is not the frame graph of a scene the engine runs");
    return reinterpret_cast<KengineDocument*>(document);
}

KecsWorld* kengine_document_world(KengineDocument* document)
{
    return Guarded([&] { return reinterpret_cast<KecsWorld*>(&Doc(document).World()); }, static_cast<KecsWorld*>(nullptr));
}

const char* kengine_document_json(KengineDocument* document)
{
    return Guarded([&] { return Keep(Doc(document).ToJson()); }, static_cast<const char*>(nullptr));
}

const char* kengine_project_directory(KengineDocument* document)
{
    return Guarded([&] { return Keep(Doc(document).ProjectDirectory().string()); }, static_cast<const char*>(nullptr));
}

const char* kengine_module_scripts(KengineDocument* document)
{
    return Guarded([&] {
        std::string folders;
        for (const auto& module : Doc(document).Modules()) folders += (folders.empty() ? "" : "\n") + (module / "scripts").string();
        return Keep(std::move(folders));
    }, static_cast<const char*>(nullptr));
}

bool kengine_playing(KengineDocument* document)
{
    return Guarded([&] { const auto* scene = keditor::PlayScene::Of(Doc(document)); return scene && scene->Playing(); }, false);
}
KoralInput* kengine_script_input(KengineDocument* document)
{
    return Guarded([&] {
        auto* scene = keditor::PlayScene::Of(Doc(document));
        // (A KoralInput is a kor::Input, as Koral's own C interface hands one over.)
        return scene ? reinterpret_cast<KoralInput*>(const_cast<kor::Input*>(&scene->ScriptInput())) : nullptr;
    }, static_cast<KoralInput*>(nullptr));
}
bool kengine_script_viewport(KengineDocument* document, float* width, float* height)
{
    return Guarded([&] {
        const auto* scene = keditor::PlayScene::Of(Doc(document));
        if (!scene) return false;
        const kor::UVec2 size = scene->ScriptViewport();
        if (width) *width = static_cast<float>(size.x);
        if (height) *height = static_cast<float>(size.y);
        return true;
    }, false);
}

KoralDebugDraw* kengine_debug(KengineDocument* document)
{
    return Guarded([&] {
        auto* scene = keditor::PlayScene::Of(Doc(document));
        // (A KoralDebugDraw is a kor::DebugDraw, as Koral's own C interface hands one over.)
        return scene ? reinterpret_cast<KoralDebugDraw*>(&scene->SceneDebug()) : nullptr;
    }, static_cast<KoralDebugDraw*>(nullptr));
}

bool kengine_set_playing(KengineDocument* document, const bool playing)
{
    return Guarded([&] {
        auto* scene = keditor::PlayScene::Of(Doc(document));
        if (!scene) return false;
        scene->SetPlaying(playing);
        return true;
    }, false);
}
bool kengine_document_save(KengineDocument* document, const char* path)
{
    return Guarded([&] {
        std::string error;
        if (Doc(document).Save(Text(path), error)) return true;
        Fail(error);
        return false;
    }, false);
}

// ---- the graph ---------------------------------------------------------------------------------------------------

const char* kengine_node_add(KengineDocument* document, const char* type, const char* id, const double x, const double y)
{
    return Guarded([&]() -> const char* {
        std::string made = Doc(document).AddNode(Text(type), x, y, Text(id));
        if (made.empty()) { Fail(std::format("there is no node type '{}'", Text(type))); return nullptr; }
        return Keep(std::move(made));
    }, static_cast<const char*>(nullptr));
}

bool kengine_node_remove(KengineDocument* document, const char* node)
{
    return Guarded([&] {
        if (!Doc(document).Graph().Find(Text(node))) { Fail(std::format("there is no node '{}'", Text(node))); return false; }
        Doc(document).RemoveNodes({ Text(node) });
        return true;
    }, false);
}

bool kengine_node_connect(KengineDocument* document, const char* from, const char* output, const char* node, const char* input)
{
    return Guarded([&] {
        if (Doc(document).Connect(Text(from), output && *output ? output : "out", Text(node), Text(input))) return true;
        Fail(std::format("there is no node '{}', or none '{}'", Text(from), Text(node)));
        return false;
    }, false);
}

bool kengine_node_clear_input(KengineDocument* document, const char* node, const char* input)
{
    return Guarded([&] { return Doc(document).ClearInput(Text(node), Text(input)); }, false);
}

KoralResource* kengine_node_resource(KengineDocument* document, const char* node, const char* output)
{
    return Guarded([&]() -> KoralResource* {
        const auto resource = Doc(document).Runtime().ResourceOf(Text(node), output && *output ? output : "out");
        if (!resource || !resource->handle) { Fail(std::format("node '{}' has made no resource", Text(node))); return nullptr; }
        Doc(document).ScriptUses(Text(node));   // what it writes there reaches every view of the graph
        return koral_resource_borrow(resource->handle);
    }, static_cast<KoralResource*>(nullptr));
}

const char* kengine_node_value_json(KengineDocument* document, const char* node, const char* output)
{
    return Guarded([&]() -> const char* {
        const kgraph::Value value = Doc(document).Runtime().ValueOf(Text(node), output && *output ? output : "out");
        if (value.IsNull()) { Fail(std::format("node '{}' has no value", Text(node))); return nullptr; }
        return Keep(value.ToJson());
    }, static_cast<const char*>(nullptr));
}

bool kengine_node_set_input_json(KengineDocument* document, const char* node, const char* input, const char* json)
{
    return Guarded([&] {
        kgraph::Value value;
        if (!Parse(json, value)) return false;
        if (Doc(document).SetInput(Text(node), Text(input), std::move(value))) return true;
        Fail(std::format("there is no node '{}'", Text(node)));
        return false;
    }, false);
}

bool kengine_node_ok(KengineDocument* document, const char* node)
{
    return Guarded([&] {
        const auto* status = Doc(document).Runtime().Status(Text(node));
        if (!status) { Fail(std::format("there is no node '{}'", Text(node))); return false; }
        if (!status->ok) Fail(status->error);
        return status->ok;
    }, false);
}

// ---- entities ----------------------------------------------------------------------------------------------------

KecsEntity kecs_create(KecsWorld* world) { return Guarded([&] { return Of(World(world).Create()); }, KecsEntity { KECS_NULL_INDEX, 0 }); }
bool kecs_destroy(KecsWorld* world, const KecsEntity entity) { return Guarded([&] { return World(world).Destroy(Of(entity)); }, false); }
bool kecs_alive(KecsWorld* world, const KecsEntity entity) { return Guarded([&] { return World(world).Alive(Of(entity)); }, false); }
size_t kecs_entity_count(KecsWorld* world) { return Guarded([&] { return World(world).EntityCount(); }, size_t { 0 }); }
bool kecs_set_parent(KecsWorld* world, const KecsEntity child, const KecsEntity parent)
{
    return Guarded([&] { return World(world).SetParent(Of(child), Of(parent)); }, false);
}
KecsEntity kecs_parent(KecsWorld* world, const KecsEntity entity)
{
    return Guarded([&] { return Of(World(world).ParentOf(Of(entity))); }, KecsEntity { KECS_NULL_INDEX, 0 });
}

bool kecs_set_parent_at(KecsWorld* world, const KecsEntity child, const KecsEntity parent, const int32_t index, const bool keep_world_transform)
{
    return Guarded([&] { return World(world).SetParent(Of(child), Of(parent), index, keep_world_transform); }, false);
}
size_t kecs_child_count(KecsWorld* world, const KecsEntity entity)
{
    return Guarded([&] { const kecs::Entity e = Of(entity); return e.IsNull() ? World(world).Roots().size() : World(world).ChildrenOf(e).size(); }, size_t { 0 });
}
KecsEntity kecs_child(KecsWorld* world, const KecsEntity entity, const size_t index)
{
    return Guarded([&] {
        const kecs::Entity e = Of(entity);
        const auto& under = e.IsNull() ? World(world).Roots() : World(world).ChildrenOf(e);
        return index < under.size() ? Of(under[index]) : KecsEntity { KECS_NULL_INDEX, 0 };
    }, KecsEntity { KECS_NULL_INDEX, 0 });
}
KecsEntity kecs_find(KecsWorld* world, const char* name)
{
    return Guarded([&] { return Of(World(world).FindEntity(Text(name))); }, KecsEntity { KECS_NULL_INDEX, 0 });
}

namespace {
    bool Three(KecsWorld* world, const KecsEntity entity, float out[3], kecs::Vec3 (kecs::World::*read)(kecs::Entity) const)
    {
        return Guarded([&] {
            if (!out || !World(world).Alive(Of(entity))) return false;
            const kecs::Vec3 value = (World(world).*read)(Of(entity));
            out[0] = value.x; out[1] = value.y; out[2] = value.z;
            return true;
        }, false);
    }
    bool SetThree(KecsWorld* world, const KecsEntity entity, const float value[3], bool (kecs::World::*write)(kecs::Entity, const kecs::Vec3&))
    {
        return Guarded([&] { return value && (World(world).*write)(Of(entity), kecs::Vec3 { value[0], value[1], value[2] }); }, false);
    }
    std::size_t Threes(KecsWorld* world, const KecsEntity* entities, const std::size_t count, float* out,
                       kecs::Vec3 (kecs::World::*read)(kecs::Entity) const, const kecs::Vec3 otherwise)
    {
        return Guarded([&] {
            if (count && (!entities || !out)) return std::size_t { 0 };
            kecs::World& w = World(world);
            std::size_t alive = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const kecs::Entity entity = Of(entities[i]);
                const kecs::Vec3 value = w.Alive(entity) ? (++alive, (w.*read)(entity)) : otherwise;
                out[i * 3] = value.x; out[i * 3 + 1] = value.y; out[i * 3 + 2] = value.z;
            }
            return alive;
        }, std::size_t { 0 });
    }
    std::size_t SetThrees(KecsWorld* world, const KecsEntity* entities, const std::size_t count, const float* values,
                          bool (kecs::World::*write)(kecs::Entity, const kecs::Vec3&))
    {
        return Guarded([&] {
            if (count && (!entities || !values)) return std::size_t { 0 };
            kecs::World& w = World(world);
            std::size_t alive = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const kecs::Entity entity = Of(entities[i]);
                if (!w.Alive(entity)) continue;
                (w.*write)(entity, kecs::Vec3 { values[i * 3], values[i * 3 + 1], values[i * 3 + 2] });
                ++alive;
            }
            return alive;
        }, std::size_t { 0 });
    }
    bool Sixteen(KecsWorld* world, const KecsEntity entity, float out[16], kecs::Mat4 (kecs::World::*read)(kecs::Entity) const)
    {
        return Guarded([&] {
            if (!out || !World(world).Alive(Of(entity))) return false;
            const kecs::Mat4 value = (World(world).*read)(Of(entity));
            std::copy(value.m.begin(), value.m.end(), out);
            return true;
        }, false);
    }
}
bool kecs_position(KecsWorld* world, const KecsEntity entity, float out[3]) { return Three(world, entity, out, &kecs::World::PositionOf); }
bool kecs_rotation(KecsWorld* world, const KecsEntity entity, float out[3]) { return Three(world, entity, out, &kecs::World::RotationOf); }
bool kecs_scale(KecsWorld* world, const KecsEntity entity, float out[3]) { return Three(world, entity, out, &kecs::World::ScaleOf); }
bool kecs_set_position(KecsWorld* world, const KecsEntity entity, const float value[3]) { return SetThree(world, entity, value, &kecs::World::SetPosition); }
bool kecs_set_rotation(KecsWorld* world, const KecsEntity entity, const float value[3]) { return SetThree(world, entity, value, &kecs::World::SetRotation); }
bool kecs_set_scale(KecsWorld* world, const KecsEntity entity, const float value[3]) { return SetThree(world, entity, value, &kecs::World::SetScale); }
size_t kecs_positions(KecsWorld* world, const KecsEntity* entities, const size_t count, float* out) { return Threes(world, entities, count, out, &kecs::World::PositionOf, {}); }
size_t kecs_rotations(KecsWorld* world, const KecsEntity* entities, const size_t count, float* out) { return Threes(world, entities, count, out, &kecs::World::RotationOf, {}); }
size_t kecs_scales(KecsWorld* world, const KecsEntity* entities, const size_t count, float* out) { return Threes(world, entities, count, out, &kecs::World::ScaleOf, { 1.f, 1.f, 1.f }); }
size_t kecs_set_positions(KecsWorld* world, const KecsEntity* entities, const size_t count, const float* values) { return SetThrees(world, entities, count, values, &kecs::World::SetPosition); }
size_t kecs_set_rotations(KecsWorld* world, const KecsEntity* entities, const size_t count, const float* values) { return SetThrees(world, entities, count, values, &kecs::World::SetRotation); }
size_t kecs_set_scales(KecsWorld* world, const KecsEntity* entities, const size_t count, const float* values) { return SetThrees(world, entities, count, values, &kecs::World::SetScale); }
bool kecs_orientation(KecsWorld* world, const KecsEntity entity, float out[4])
{
    return Guarded([&] {
        if (!out || !World(world).Alive(Of(entity))) return false;
        const kecs::Quat q = World(world).OrientationOf(Of(entity));
        out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        return true;
    }, false);
}
bool kecs_set_orientation(KecsWorld* world, const KecsEntity entity, const float value[4])
{
    return Guarded([&] { return value && World(world).SetOrientation(Of(entity), kecs::Quat { value[0], value[1], value[2], value[3] }); }, false);
}
size_t kecs_orientations(KecsWorld* world, const KecsEntity* entities, const size_t count, float* out)
{
    return Guarded([&] {
        if (count && (!entities || !out)) return std::size_t { 0 };
        kecs::World& w = World(world);
        std::size_t alive = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const kecs::Entity entity = Of(entities[i]);
            const kecs::Quat q = w.Alive(entity) ? (++alive, w.OrientationOf(entity)) : kecs::Quat {};
            out[i * 4] = q.x; out[i * 4 + 1] = q.y; out[i * 4 + 2] = q.z; out[i * 4 + 3] = q.w;
        }
        return alive;
    }, std::size_t { 0 });
}
size_t kecs_set_orientations(KecsWorld* world, const KecsEntity* entities, const size_t count, const float* values)
{
    return Guarded([&] {
        if (count && (!entities || !values)) return std::size_t { 0 };
        kecs::World& w = World(world);
        std::size_t alive = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const kecs::Entity entity = Of(entities[i]);
            if (!w.Alive(entity)) continue;
            w.SetOrientation(entity, kecs::Quat { values[i * 4], values[i * 4 + 1], values[i * 4 + 2], values[i * 4 + 3] });
            ++alive;
        }
        return alive;
    }, std::size_t { 0 });
}
bool kecs_local_matrix(KecsWorld* world, const KecsEntity entity, float out[16]) { return Sixteen(world, entity, out, &kecs::World::LocalMatrix); }
bool kecs_world_matrix(KecsWorld* world, const KecsEntity entity, float out[16]) { return Sixteen(world, entity, out, &kecs::World::WorldMatrix); }
bool kecs_set_world_matrix(KecsWorld* world, const KecsEntity entity, const float matrix[16])
{
    return Guarded([&] {
        if (!matrix) return false;
        kecs::Mat4 value;
        std::copy(matrix, matrix + 16, value.m.begin());
        return World(world).SetWorldMatrix(Of(entity), value);
    }, false);
}

KecsEntity kengine_entity_create(KengineDocument* document, const char* name)
{
    return Guarded([&] { return Of(Doc(document).CreateEntity(name && *name ? name : "Entity")); }, KecsEntity { KECS_NULL_INDEX, 0 });
}
const char* kengine_entity_name(KengineDocument* document, const KecsEntity entity)
{
    return Guarded([&] { return Keep(Doc(document).NameOf(Of(entity))); }, static_cast<const char*>(nullptr));
}
bool kengine_entity_set_name(KengineDocument* document, const KecsEntity entity, const char* name)
{
    return Guarded([&] { return Doc(document).Rename(Of(entity), Text(name)); }, false);
}

// ---- components --------------------------------------------------------------------------------------------------

namespace {
    const kecs::ComponentDescription* Describe(KecsWorld* world, const uint32_t component)
    {
        kecs::World& w = World(world);
        if (component >= w.ComponentCount()) { Fail(std::format("there is no component {}", component)); return nullptr; }
        return &w.Describe(component);
    }
}

uint32_t kecs_component_find(KecsWorld* world, const char* name) { return Guarded([&] { return World(world).Find(Text(name)); }, KECS_NO_COMPONENT); }
uint32_t kecs_component_count(KecsWorld* world) { return Guarded([&] { return static_cast<uint32_t>(World(world).ComponentCount()); }, 0u); }
const char* kecs_component_name(KecsWorld* world, const uint32_t component)
{
    return Guarded([&]() -> const char* { const auto* type = Describe(world, component); return type ? type->name.c_str() : nullptr; }, static_cast<const char*>(nullptr));
}
size_t kecs_component_size(KecsWorld* world, const uint32_t component)
{
    return Guarded([&] { const auto* type = Describe(world, component); return type ? type->size : size_t { 0 }; }, size_t { 0 });
}
uint32_t kecs_component_field_count(KecsWorld* world, const uint32_t component)
{
    return Guarded([&] { const auto* type = Describe(world, component); return type ? static_cast<uint32_t>(type->fields.size()) : 0u; }, 0u);
}
bool kecs_component_field(KecsWorld* world, const uint32_t component, const uint32_t index, KecsField* out)
{
    return Guarded([&] {
        const auto* type = Describe(world, component);
        if (!type || !out) return false;
        if (index >= type->fields.size()) { Fail(std::format("'{}' has {} fields", type->name, type->fields.size())); return false; }
        const auto& field = type->fields[index];
        *out = { field.name.c_str(), field.type.c_str(), static_cast<uint32_t>(field.offset), static_cast<uint32_t>(field.size) };
        return true;
    }, false);
}
uint32_t kecs_register_tag(KecsWorld* world, const char* name)
{
    return Guarded([&] {
        kecs::World& w = World(world);
        if (const kecs::ComponentId known = w.Find(Text(name)); known != kecs::NoComponent) return known;
        if (!name || !*name) { Fail("a tag needs a name"); return KECS_NO_COMPONENT; }
        kecs::ComponentDescription tag;
        tag.name = name;
        return w.Register(std::move(tag));
    }, KECS_NO_COMPONENT);
}

bool kecs_add(KecsWorld* world, const KecsEntity entity, const uint32_t component, const void* value)
{
    return Guarded([&] {
        kecs::World& w = World(world);
        if (!w.Alive(Of(entity))) { Fail("the entity is not alive"); return false; }
        if (!Describe(world, component)) return false;
        return w.Add(Of(entity), component, value);
    }, false);
}
bool kecs_remove(KecsWorld* world, const KecsEntity entity, const uint32_t component)
{
    return Guarded([&] { return Describe(world, component) && World(world).Remove(Of(entity), component); }, false);
}
bool kecs_has(KecsWorld* world, const KecsEntity entity, const uint32_t component)
{
    return Guarded([&] { return component < World(world).ComponentCount() && World(world).Has(Of(entity), component); }, false);
}
void* kecs_get(KecsWorld* world, const KecsEntity entity, const uint32_t component)
{
    return Guarded([&]() -> void* {
        kecs::World& w = World(world);
        if (component >= w.ComponentCount() || !w.Alive(Of(entity))) return nullptr;
        return const_cast<void*>(std::as_const(w).Get(Of(entity), component));
    }, static_cast<void*>(nullptr));
}
bool kecs_set_enabled(KecsWorld* world, const KecsEntity entity, const uint32_t component, const bool enabled)
{
    return Guarded([&] { return Describe(world, component) && World(world).SetEnabled(Of(entity), component, enabled); }, false);
}
bool kecs_is_enabled(KecsWorld* world, const KecsEntity entity, const uint32_t component)
{
    return Guarded([&] { return component < World(world).ComponentCount() && World(world).IsEnabled(Of(entity), component); }, false);
}
namespace {
    /** A field of numbers, as many entities' of it are moved at once: which, how many numbers it is, and of what kind. */
    struct NumberField {
        const kecs::FieldDescription* field = nullptr;
        std::uint32_t width = 0;
        bool floats = false, oneByte = false;       // a "bool" is a byte; every other kind is four a number
    };
    NumberField NumbersOfField(kecs::World& world, const std::uint32_t component, const char* name)
    {
        NumberField found;
        if (!name || component >= world.ComponentCount()) return found;
        const kecs::FieldDescription* field = world.Describe(component).FindField(name);
        if (!field) return found;
        static const std::set<std::string, std::less<>> floats { "f32", "vec2", "vec3", "vec4", "quat", "mat4" };
        static const std::set<std::string, std::less<>> ints { "i32", "u32", "bool32", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4" };
        if (field->type == "bool") return { field, 1, false, true };
        if (floats.contains(field->type)) return { field, static_cast<std::uint32_t>(field->size / 4), true, false };
        if (ints.contains(field->type)) return { field, static_cast<std::uint32_t>(field->size / 4), false, false };
        return found;
    }

    template <typename T>
    std::size_t Fields(KecsWorld* world, const KecsEntity* entities, const std::size_t count, const std::uint32_t component, const char* name,
                       T* numbers, const bool write)
    {
        return Guarded([&] {
            kecs::World& w = World(world);
            const NumberField found = NumbersOfField(w, component, name);
            if (!found.field || found.floats != std::is_same_v<T, float> || (count && (!entities || !numbers))) return std::size_t { 0 };
            std::size_t had = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const kecs::Entity entity = Of(entities[i]);
                T* at = numbers + i * found.width;
                auto* bytes = w.Alive(entity) ? static_cast<std::byte*>(const_cast<void*>(std::as_const(w).Get(entity, component))) : nullptr;
                if (!bytes) {
                    if (!write) std::fill(at, at + found.width, T {});
                    continue;
                }
                std::byte* field = bytes + found.field->offset;
                if (found.oneByte) {
                    if (write) *reinterpret_cast<bool*>(field) = *at != 0;
                    else *at = *reinterpret_cast<const bool*>(field) ? 1 : 0;
                } else if (write) {
                    std::memcpy(field, at, found.width * sizeof(T));
                } else {
                    std::memcpy(at, field, found.width * sizeof(T));
                }
                if (write) w.MarkChanged(entity, component);
                ++had;
            }
            return had;
        }, std::size_t { 0 });
    }
}

uint32_t kecs_field_width(KecsWorld* world, const uint32_t component, const char* field)
{
    return Guarded([&] { return NumbersOfField(World(world), component, field).width; }, 0u);
}
size_t kecs_field_floats(KecsWorld* world, const KecsEntity* entities, const size_t count, const uint32_t component, const char* field, float* out)
{
    return Fields(world, entities, count, component, field, out, false);
}
size_t kecs_set_field_floats(KecsWorld* world, const KecsEntity* entities, const size_t count, const uint32_t component, const char* field, const float* values)
{
    return Fields(world, entities, count, component, field, const_cast<float*>(values), true);
}
size_t kecs_field_ints(KecsWorld* world, const KecsEntity* entities, const size_t count, const uint32_t component, const char* field, int32_t* out)
{
    return Fields(world, entities, count, component, field, out, false);
}
size_t kecs_set_field_ints(KecsWorld* world, const KecsEntity* entities, const size_t count, const uint32_t component, const char* field, const int32_t* values)
{
    return Fields(world, entities, count, component, field, const_cast<int32_t*>(values), true);
}

bool kecs_mark_changed(KecsWorld* world, const KecsEntity entity, const uint32_t component)
{
    return Guarded([&] { return component < World(world).ComponentCount() && World(world).MarkChanged(Of(entity), component); }, false);
}

size_t kecs_query(KecsWorld* world, const uint32_t* all, const size_t all_count, const uint32_t* none, const size_t none_count,
                  const bool include_disabled, KecsEntity* out, const size_t capacity)
{
    return Guarded([&] {
        kecs::World& w = World(world);
        kecs::QueryDescription query;
        for (size_t i = 0; i < all_count; ++i) {
            if (all[i] >= w.ComponentCount()) return size_t { 0 };      // a component nothing is: nothing has it
            query.all.push_back(all[i]);
        }
        for (size_t i = 0; i < none_count; ++i)
            if (none[i] < w.ComponentCount()) query.none.push_back(none[i]);
        query.includeDisabled = include_disabled;
        size_t count = 0;
        w.Each(query, [&](const kecs::ChunkView& view) {
            for (std::size_t row = 0; row < view.Count(); ++row) {
                if (!view.AllMatch() && !view.Matches(row)) continue;
                if (out && count < capacity) out[count] = Of(view.EntityAt(row));
                ++count;
            }
        });
        return count;
    }, size_t { 0 });
}

const char* kengine_field_json(KengineDocument* document, const KecsEntity entity, const char* component, const char* field)
{
    return Guarded([&] { return Keep(Doc(document).Field(Of(entity), Text(component), Text(field)).ToJson()); }, static_cast<const char*>(nullptr));
}

bool kengine_field_set_json(KengineDocument* document, const KecsEntity entity, const char* component, const char* field, const char* json)
{
    return Guarded([&] {
        kgraph::Value value;
        if (!Parse(json, value)) return false;
        std::string error;
        if (Doc(document).SetField(Of(entity), Text(component), Text(field), value, &error)) return true;
        Fail(error);
        return false;
    }, false);
}

// ---- scripts -----------------------------------------------------------------------------------------------------

namespace {
    // The C entity and kecs's are the same two numbers, and are passed the same way: the functions are handed over as they are.
    static_assert(sizeof(KecsEntity) == sizeof(kecs::Entity) && alignof(KecsEntity) == alignof(kecs::Entity));
    template <typename To, typename From> To Same(From function) { return reinterpret_cast<To>(reinterpret_cast<void (*)()>(function)); }
}

uint32_t kengine_script_register(KengineDocument* document, const char* name, const size_t size, const size_t alignment,
                                 const KecsField* fields, const size_t field_count, const KengineScriptCallbacks* callbacks)
{
    return Guarded([&] {
        if (!name || !*name) { Fail("a script needs a name"); return KECS_NO_COMPONENT; }
        kscript::ScriptType type;
        type.name = name;
        type.size = size;
        type.alignment = alignment ? alignment : 8;
        for (size_t i = 0; i < field_count; ++i) {
            if (fields[i].offset + fields[i].size > size) { Fail(std::format("field '{}' is past the end of the script's data", Text(fields[i].name))); return KECS_NO_COMPONENT; }
            type.fields.push_back({ Text(fields[i].name), Text(fields[i].type), fields[i].offset, fields[i].size, nullptr });
        }
        if (callbacks) {
            using Plain = void (*)(void*, kecs::Entity, void*);
            using Timed = void (*)(void*, kecs::Entity, void*, float);
            auto& to = type.callbacks;
            to.user = callbacks->user;
            to.created = Same<Plain>(callbacks->created);
            to.enabled = Same<Plain>(callbacks->enabled);
            to.started = Same<Plain>(callbacks->started);
            to.fixedUpdate = Same<Timed>(callbacks->fixed_update);
            to.update = Same<Timed>(callbacks->update);
            to.lateUpdate = Same<Timed>(callbacks->late_update);
            to.prepare = Same<Plain>(callbacks->prepare);
            to.disabled = Same<Plain>(callbacks->disabled);
            to.destroyed = Same<Plain>(callbacks->destroyed);
        }
        std::string error;
        const kecs::ComponentId id = Doc(document).Scripts().Register(std::move(type), &error);
        if (id == kecs::NoComponent) Fail(error);
        return id;
    }, KECS_NO_COMPONENT);
}

void kengine_script_unregister(KengineDocument* document, const char* name)
{
    Guarded([&] { Doc(document).Scripts().Unregister(Text(name)); return 0; }, 0);
}

void kengine_text_layout(size_t* size, size_t* alignment)
{
    if (size) *size = sizeof(std::string);
    if (alignment) *alignment = alignof(std::string);
}

const char* kengine_text_get(const void* at, size_t* length)
{
    const auto* text = at ? std::launder(static_cast<const std::string*>(at)) : nullptr;
    if (length) *length = text ? text->size() : 0;
    return text ? text->data() : "";
}

void kengine_text_set(void* at, const char* utf8, const size_t length)
{
    if (!at) return;
    *std::launder(static_cast<std::string*>(at)) = utf8 ? std::string(utf8, length) : std::string();
}

bool kengine_resource_script_register(KengineDocument* document, const char* name, const size_t size, const KecsField* fields, const size_t field_count,
                                      const KengineScriptResource* resources, const size_t resource_count, const KengineResourceScriptCallbacks* callbacks)
{
    return Guarded([&] {
        if (!name || !*name) { Fail("a script needs a name"); return false; }
        kscript::ScriptType type;
        type.name = name;
        type.makesResources = true;
        type.size = size;
        for (size_t i = 0; i < field_count; ++i) {
            if (fields[i].offset + fields[i].size > size) { Fail(std::format("field '{}' is past the end of the script's data", Text(fields[i].name))); return false; }
            type.fields.push_back({ Text(fields[i].name), Text(fields[i].type), fields[i].offset, fields[i].size, nullptr });
        }
        for (size_t i = 0; i < resource_count; ++i) {
            const KengineScriptResource& given = resources[i];
            const std::string kind = Text(given.kind);
            if (!given.name || !*given.name) { Fail(std::format("a resource of '{}' needs a name", name)); return false; }
            if (kind != "Image" && kind != "Buffer" && kind != "ImageView" && kind != "BufferView" && kind != "Sampler") {
                Fail(std::format("{}.{} is a '{}': a script makes an Image, a Buffer, an ImageView, a BufferView or a Sampler", name, given.name, kind));
                return false;
            }
            kscript::DeclaredResource resource { given.name, kind, {} };
            if (given.settings && *given.settings) {
                kgraph::Value settings;
                if (!Parse(given.settings, settings)) return false;
                if (!settings.Named()) { Fail(std::format("{}.{}'s settings are an object of its inputs: {{\"format\": \"eR32_SFLOAT\"}}", name, given.name)); return false; }
                for (const auto& [input, value] : *settings.Named()) resource.settings.emplace_back(input, value.ToJson());
            }
            type.resources.push_back(std::move(resource));
        }
        std::function<std::string(std::span<kgraph::Contents>, std::span<const std::byte>)> fill;
        if (callbacks && callbacks->fill)
            fill = [user = callbacks->user, call = callbacks->fill](const std::span<kgraph::Contents> contents, const std::span<const std::byte> settings) -> std::string {
                std::vector<KengineContents> given;
                given.reserve(contents.size());
                for (auto& each : contents)
                    given.push_back({ each.name.c_str(), each.node.c_str(), each.kind.c_str(), each.format.c_str(), { each.extent.x, each.extent.y, each.extent.z },
                                      each.layers, each.texelBytes, each.bytes.data(), each.bytes.size() });
                const char* why = call(user, given.data(), given.size(), settings.data(), settings.size());
                return why ? std::string(why) : std::string();
            };
        Doc(document).RegisterResourceScript(std::move(type), std::move(fill));
        return true;
    }, false);
}

void kengine_resource_script_unregister(KengineDocument* document, const char* name)
{
    Guarded([&] { Doc(document).UnregisterResourceScript(Text(name)); return 0; }, 0);
}

// ---- conditions -------------------------------------------------------------------------------------------------------

bool kengine_condition_register(KengineDocument* document, const char* name, const char* const* components, const size_t component_count,
                                const KengineConditionInput* inputs, const size_t input_count, const KengineConditionCallbacks* callbacks)
{
    return Guarded([&] {
        if (!name || !*name) { Fail("a condition needs a name"); return false; }
        if (!callbacks || !callbacks->test) { Fail(std::format("condition '{}' needs a test", name)); return false; }
        kgraph::Condition condition;
        condition.name = name;
        for (size_t i = 0; i < component_count; ++i) {
            if (!components[i] || !*components[i]) { Fail(std::format("condition '{}': its component {} has no name", name, i)); return false; }
            condition.components.emplace_back(components[i]);
        }
        for (size_t i = 0; i < input_count; ++i) {
            if (!inputs[i].name || !*inputs[i].name) { Fail(std::format("condition '{}': its input {} has no name", name, i)); return false; }
            kgraph::Port port { inputs[i].name, kgraph::PortKind::eValue, inputs[i].type && *inputs[i].type ? inputs[i].type : "value", false, {}, nullptr };
            if (inputs[i].initial && *inputs[i].initial && !Parse(inputs[i].initial, port.initial)) return false;
            condition.parameters.push_back(std::move(port));
        }
        condition.test = [user = callbacks->user, test = callbacks->test, world = &Doc(document).World(), names = condition.components](
                             const std::span<const void* const> given, const std::span<const kgraph::Value> values) {
            std::string components = "[";
            for (std::size_t i = 0; i < given.size() && i < names.size(); ++i)
                components += (i ? "," : "") + kgraph::Runtime::ComponentJson(*world, names[i], given[i]);
            components += "]";
            const std::string inputs = kgraph::Value(kgraph::Value::List(values.begin(), values.end())).ToJson();
            return test(user, components.c_str(), inputs.c_str()) != 0;
        };
        Doc(document).Runtime().SetCondition(std::move(condition));
        return true;
    }, false);
}

void kengine_condition_unregister(KengineDocument* document, const char* name)
{
    Guarded([&] { Doc(document).Runtime().RemoveCondition(Text(name)); return 0; }, 0);
}


// ---- meshes ---------------------------------------------------------------------------------------------------------

namespace {
    /** "POSITION UV; NORMAL TANGENT": a buffer to each ';', semantics apart by spaces or commas. */
    std::vector<std::vector<std::string>> Buffers(const char* text)
    {
        std::vector<std::vector<std::string>> buffers(1);
        std::string word;
        const auto end = [&] { if (!word.empty()) buffers.back().push_back(std::exchange(word, {})); };
        for (const char* c = text ? text : ""; *c; ++c) {
            if (*c == ';') { end(); buffers.emplace_back(); }
            else if (*c == ' ' || *c == ',' || *c == '\t' || *c == '\n') end();
            else word += *c;
        }
        end();
        std::erase_if(buffers, [](const auto& buffer) { return buffer.empty(); });
        return buffers;
    }

    kgraph::VertexAttribute AttributeOf(const KengineAttribute& given, const std::uint32_t vertices)
    {
        if (!given.semantic || !*given.semantic) throw std::invalid_argument("an attribute needs its semantic: what it is, as a shader asks for it");
        if (given.type > KENGINE_ATTRIBUTE_UINT) throw std::invalid_argument(std::format("'{}' has no type {}: 0 float, 1 int, 2 uint", given.semantic, given.type));
        if (given.count < 1 || given.count > 4) throw std::invalid_argument(std::format("'{}' has {} numbers a vertex: 1 to 4", given.semantic, given.count));
        if (!given.data && vertices > 0) throw std::invalid_argument(std::format("'{}' has no numbers", given.semantic));
        kgraph::VertexAttribute attribute { given.semantic, static_cast<kgraph::AttributeType>(given.type), given.count, {} };
        attribute.words.resize(static_cast<std::size_t>(given.count) * vertices);
        if (!attribute.words.empty()) std::memcpy(attribute.words.data(), given.data, attribute.words.size() * 4);
        return attribute;
    }

    const char* Made(KengineDocument* document, const char* name, const kgraph::VertexData& data)
    {
        if (!name || !*name) throw std::invalid_argument("a mesh needs a name");
        const std::string id = std::format("$script/{}", name);
        std::string error;
        if (!Doc(document).Runtime().MakeMesh(id, data, &error)) { Fail(error); return nullptr; }
        return Keep(id);
    }

    /**
     * The mesh last read back on this thread, while it is the mesh the id is: what each question about it is answered
     * from. Which mesh it was is kept weakly: a thread's own outlives the device, and must hold nothing of it.
     */
    thread_local std::string t_read;
    thread_local std::weak_ptr<kgraph::Resource> t_readHeld;
    thread_local std::optional<kgraph::VertexData> t_readData;

    const kgraph::VertexData* Read(KengineDocument* document, const char* mesh)
    {
        auto& runtime = Doc(document).Runtime();
        auto held = runtime.ResourceOf(Text(mesh));
        while (held && held->replacedBy) held = held->replacedBy;
        if (!held) { Fail(std::format("'{}' is no mesh", Text(mesh))); return nullptr; }
        if (t_read != Text(mesh) || t_readHeld.lock() != held || !t_readData) {
            std::string error;
            t_readData = runtime.ReadMesh(Text(mesh), &error);
            t_read = Text(mesh);
            t_readHeld = held;
            if (!t_readData) { Fail(error); return nullptr; }
        }
        return &*t_readData;
    }
}

const char* kengine_image_create(KengineDocument* document, const char* name, const uint32_t width, const uint32_t height, const uint8_t* rgba)
{
    return Guarded([&]() -> const char* {
        if (!name || !*name) throw std::invalid_argument("a picture needs a name");
        if (!rgba) throw std::invalid_argument("a picture needs its texels");
        const std::string id = std::format("$script/{}", name);
        std::string error;
        const auto bytes = std::span(reinterpret_cast<const std::byte*>(rgba), static_cast<std::size_t>(width) * height * 4);
        if (!Doc(document).Runtime().MakeImage(id, width, height, bytes, &error)) { Fail(error); return nullptr; }
        return Keep(id);
    }, static_cast<const char*>(nullptr));
}

bool kengine_entity_give_picture(KengineDocument* document, const KecsEntity entity, const char* field, const char* picture)
{
    return Guarded([&] { return field && picture && Doc(document).GivePicture(Of(entity), field, picture); }, false);
}

const char* kengine_mesh_create(KengineDocument* document, const char* name, const uint32_t vertex_count, const KengineAttribute* attributes,
                                const uint32_t attribute_count, const uint32_t* indices, const uint32_t index_count, const char* buffers)
{
    return Guarded([&]() -> const char* {
        kgraph::VertexData data;
        data.vertexCount = vertex_count;
        for (std::uint32_t i = 0; i < attribute_count; ++i) data.attributes.push_back(AttributeOf(attributes[i], vertex_count));
        if (indices) data.indices.assign(indices, indices + index_count);
        data.buffers = Buffers(buffers);
        return Made(document, name, data);
    }, static_cast<const char*>(nullptr));
}

const char* kengine_mesh_with_attributes(KengineDocument* document, const char* mesh, const char* name, const KengineAttribute* attributes,
                                         const uint32_t attribute_count, const char* buffers)
{
    return Guarded([&]() -> const char* {
        const kgraph::VertexData* read = Read(document, mesh);
        if (!read) return nullptr;
        kgraph::VertexData data = *read;
        for (std::uint32_t i = 0; i < attribute_count; ++i) {
            kgraph::VertexAttribute attribute = AttributeOf(attributes[i], data.vertexCount);
            if (auto* own = data.Find(attribute.semantic)) *own = std::move(attribute);
            else data.attributes.push_back(std::move(attribute));
        }
        if (buffers) data.buffers = Buffers(buffers);
        return Made(document, name, data);
    }, static_cast<const char*>(nullptr));
}

const char* kengine_mesh_without_attributes(KengineDocument* document, const char* mesh, const char* name, const char* semantics)
{
    return Guarded([&]() -> const char* {
        const kgraph::VertexData* read = Read(document, mesh);
        if (!read) return nullptr;
        kgraph::VertexData data = *read;
        for (const auto& buffer : Buffers(semantics))
            for (const auto& semantic : buffer) {
                std::erase_if(data.attributes, [&](const kgraph::VertexAttribute& a) { return a.semantic == semantic; });
                for (auto& group : data.buffers) std::erase(group, semantic);
            }
        std::erase_if(data.buffers, [](const auto& group) { return group.empty(); });
        return Made(document, name, data);
    }, static_cast<const char*>(nullptr));
}

uint32_t kengine_mesh_vertex_count(KengineDocument* document, const char* mesh)
{
    return Guarded([&]() -> uint32_t { const auto* read = Read(document, mesh); return read ? read->vertexCount : 0u; }, 0u);
}

uint32_t kengine_mesh_attribute_count(KengineDocument* document, const char* mesh)
{
    return Guarded([&]() -> uint32_t { const auto* read = Read(document, mesh); return read ? static_cast<uint32_t>(read->attributes.size()) : 0u; }, 0u);
}

bool kengine_mesh_attribute(KengineDocument* document, const char* mesh, const uint32_t index, KengineAttribute* out)
{
    return Guarded([&] {
        const auto* read = Read(document, mesh);
        if (!read || !out) return false;
        if (index >= read->attributes.size()) { Fail(std::format("'{}' has {} attributes", Text(mesh), read->attributes.size())); return false; }
        const auto& attribute = read->attributes[index];
        *out = { attribute.semantic.c_str(), static_cast<uint32_t>(attribute.type), attribute.count, attribute.words.data() };
        return true;
    }, false);
}

size_t kengine_mesh_indices(KengineDocument* document, const char* mesh, uint32_t* out, const size_t capacity)
{
    return Guarded([&]() -> size_t {
        const auto* read = Read(document, mesh);
        if (!read) return 0;
        if (out) std::memcpy(out, read->indices.data(), std::min(capacity, read->indices.size()) * 4);
        return read->indices.size();
    }, static_cast<size_t>(0));
}
