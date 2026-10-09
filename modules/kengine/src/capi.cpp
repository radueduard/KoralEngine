// The engine's C interface: each function a call on the C++ object the handle is.

#include <koral_engine.h>

#include <format>
#include <string>

#include <frameGraph.h>

#include <kecs/json.h>
#include <keditor/document.h>
#include <keditor/play.h>

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

bool kengine_playing(KengineDocument* document)
{
    return Guarded([&] { const auto* scene = keditor::PlayScene::Of(Doc(document)); return scene && scene->Playing(); }, false);
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
    template <typename To, typename From> To Same(From function) { return reinterpret_cast<To>(function); }
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
