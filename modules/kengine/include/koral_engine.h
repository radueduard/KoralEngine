/*
 * Koral Engine's C interface: a scene's entities and components, the nodes of its graph, and scripts. What the C#
 * and Kotlin layers are written on, and what any other language's would be.
 *
 * It goes with koral_c.h, the framework's: a scene is opened through that (the engine's scenes are "Play" and
 * "Editor", in this library), and its frame graph is what finds the engine's side of it here:
 *
 *     KengineDocument* doc = kengine_document(koral_scene_graph(scene));
 *     KecsWorld* world = kengine_document_world(doc);
 *
 * Conventions: a function that can fail returns false, 0, null or KECS_NO_COMPONENT, with why in
 * kengine_last_error(). A string returned is good until the next call on the same thread. Everything is called on the
 * application's thread.
 */

#ifndef KORAL_ENGINE_H
#define KORAL_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <koral_c.h>

#if defined(_WIN32)
#  if defined(KENGINE_BUILD)
#    define KENGINE_API __declspec(dllexport)
#  else
#    define KENGINE_API __declspec(dllimport)
#  endif
#else
#  define KENGINE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KengineDocument KengineDocument;   /* a scene: keditor::Document */
typedef struct KecsWorld KecsWorld;               /* its entities: kecs::World */

typedef struct KecsEntity {
    uint32_t index;
    uint32_t generation;
} KecsEntity;

#define KECS_NO_COMPONENT 0xFFFFFFFFu
#define KECS_NULL_INDEX 0xFFFFFFFFu     /* the index of no entity */

/** Why the last call on this thread that failed did; empty when none has. */
KENGINE_API const char* kengine_last_error(void);

/* ---- a scene ------------------------------------------------------------------------------------------------- */

/** The engine's side of the scene whose frame graph this is (koral_scene_graph); null when it is no engine scene. */
KENGINE_API KengineDocument* kengine_document(KoralFrameGraph* graph);
KENGINE_API KecsWorld* kengine_document_world(KengineDocument* document);
/** The scene as JSON, as it is saved. */
KENGINE_API const char* kengine_document_json(KengineDocument* document);
KENGINE_API bool kengine_document_save(KengineDocument* document, const char* path);

/* ---- the graph ----------------------------------------------------------------------------------------------- */

/**
 * Adds a node of @p type, as the editor does. Its id is @p id when that is given and free, and otherwise made from
 * the type; what it got is returned, or null when there is no such type.
 */
KENGINE_API const char* kengine_node_add(KengineDocument* document, const char* type, const char* id, double x, double y);
KENGINE_API bool kengine_node_remove(KengineDocument* document, const char* node);
/** Wires input @p input of @p node to output @p output ("out" when null) of node @p from. */
KENGINE_API bool kengine_node_connect(KengineDocument* document, const char* from, const char* output, const char* node, const char* input);
/** Takes an input off a node: its wire, or its value. */
KENGINE_API bool kengine_node_clear_input(KengineDocument* document, const char* node, const char* input);

/**
 * What a node made, as a handle koral_c.h's functions take: an image to read, a buffer to write. Borrowed: release
 * it with koral_resource_release. Null when there is no such node or output, or it is not a resource. A node built
 * again (an input changed, a shader was edited) makes another, so ask each time one is needed, not once.
 */
KENGINE_API KoralResource* kengine_node_resource(KengineDocument* document, const char* node, const char* output);
/** A node's value output, as JSON; null when there is none. */
KENGINE_API const char* kengine_node_value_json(KengineDocument* document, const char* node, const char* output);
/** Sets a node's input from JSON: a number, a string, a list. As an edit in the editor is. */
KENGINE_API bool kengine_node_set_input_json(KengineDocument* document, const char* node, const char* input, const char* json);
/** Whether a node is built, and when not, why (kengine_last_error). */
KENGINE_API bool kengine_node_ok(KengineDocument* document, const char* node);

/* ---- entities ------------------------------------------------------------------------------------------------ */

KENGINE_API KecsEntity kecs_create(KecsWorld* world);
KENGINE_API bool kecs_destroy(KecsWorld* world, KecsEntity entity);
KENGINE_API bool kecs_alive(KecsWorld* world, KecsEntity entity);
KENGINE_API size_t kecs_entity_count(KecsWorld* world);
KENGINE_API bool kecs_set_parent(KecsWorld* world, KecsEntity child, KecsEntity parent);
KENGINE_API KecsEntity kecs_parent(KecsWorld* world, KecsEntity entity);

/** An entity with a name, as the editor makes one. */
KENGINE_API KecsEntity kengine_entity_create(KengineDocument* document, const char* name);
KENGINE_API const char* kengine_entity_name(KengineDocument* document, KecsEntity entity);
KENGINE_API bool kengine_entity_set_name(KengineDocument* document, KecsEntity entity, const char* name);

/* ---- components ---------------------------------------------------------------------------------------------- */

typedef struct KecsField {
    const char* name;
    const char* type;       /* "f32", "i32", "bool", "vec3", "mat4", "entity", "Image", ... */
    uint32_t offset;
    uint32_t size;
} KecsField;

/** The component type so named, or KECS_NO_COMPONENT. */
KENGINE_API uint32_t kecs_component_find(KecsWorld* world, const char* name);
KENGINE_API uint32_t kecs_component_count(KecsWorld* world);
KENGINE_API const char* kecs_component_name(KecsWorld* world, uint32_t component);
KENGINE_API size_t kecs_component_size(KecsWorld* world, uint32_t component);
KENGINE_API uint32_t kecs_component_field_count(KecsWorld* world, uint32_t component);
KENGINE_API bool kecs_component_field(KecsWorld* world, uint32_t component, uint32_t index, KecsField* out);
/** A tag: a component with nothing in it. The one so named, when there is one. */
KENGINE_API uint32_t kecs_register_tag(KecsWorld* world, const char* name);

/** Gives an entity a component: a copy of @p value (as many bytes as the component is), or a default one when null. */
KENGINE_API bool kecs_add(KecsWorld* world, KecsEntity entity, uint32_t component, const void* value);
KENGINE_API bool kecs_remove(KecsWorld* world, KecsEntity entity, uint32_t component);
KENGINE_API bool kecs_has(KecsWorld* world, KecsEntity entity, uint32_t component);
/** Where an entity's component is now: good until the world's shape next changes. Null when it has none, or for a tag. */
KENGINE_API void* kecs_get(KecsWorld* world, KecsEntity entity, uint32_t component);
KENGINE_API bool kecs_set_enabled(KecsWorld* world, KecsEntity entity, uint32_t component, bool enabled);
KENGINE_API bool kecs_is_enabled(KecsWorld* world, KecsEntity entity, uint32_t component);
/** Says a component was written through kecs_get: what a query for changes goes by. */
KENGINE_API bool kecs_mark_changed(KecsWorld* world, KecsEntity entity, uint32_t component);

/**
 * The entities that have every component of @p all and none of @p none. Up to @p capacity are written to @p out;
 * the number there are is returned, so a call with no room says how much to make. Switched-off components count as
 * not had unless @p include_disabled.
 */
KENGINE_API size_t kecs_query(KecsWorld* world, const uint32_t* all, size_t all_count, const uint32_t* none, size_t none_count,
                              bool include_disabled, KecsEntity* out, size_t capacity);

/**
 * A field of a component as JSON, and set from it: a number, true or false, a list of numbers for a vector, and for a
 * field that holds a resource the id of the node that makes it (or null). How a resource is put in a component.
 */
KENGINE_API const char* kengine_field_json(KengineDocument* document, KecsEntity entity, const char* component, const char* field);
KENGINE_API bool kengine_field_set_json(KengineDocument* document, KecsEntity entity, const char* component, const char* field, const char* json);

/* ---- scripts ------------------------------------------------------------------------------------------------- */

/**
 * What is called on each component of a script type. Any may be null. @p data is the component's bytes, where the
 * world keeps them now: good for the call. Called on the application's thread, a frame at a time:
 * created, enabled and started as they come due, then fixed_update (0 to 8 times), update, late_update, prepare.
 */
typedef struct KengineScriptCallbacks {
    void* user;
    void (*created)(void* user, KecsEntity entity, void* data);
    void (*enabled)(void* user, KecsEntity entity, void* data);
    void (*started)(void* user, KecsEntity entity, void* data);
    void (*fixed_update)(void* user, KecsEntity entity, void* data, float step);
    void (*update)(void* user, KecsEntity entity, void* data, float delta);
    void (*late_update)(void* user, KecsEntity entity, void* data, float delta);
    void (*prepare)(void* user, KecsEntity entity, void* data);     /* a graphics script's: before anything is drawn */
    void (*disabled)(void* user, KecsEntity entity, void* data);
    void (*destroyed)(void* user, KecsEntity entity, void* data);
} KengineScriptCallbacks;

/**
 * Makes a script type: a component of @p size bytes with these fields, and these functions called on each one. With
 * one so named already, it takes its new shape — every entity keeping the fields whose name and type are the same —
 * and its new functions: a script edited and loaded again. Returns the component, or KECS_NO_COMPONENT.
 */
KENGINE_API uint32_t kengine_script_register(KengineDocument* document, const char* name, size_t size, size_t alignment,
                                             const KecsField* fields, size_t field_count, const KengineScriptCallbacks* callbacks);
/** Stops calling a script type's functions: its code is about to go. Its data stays, on every entity. */
KENGINE_API void kengine_script_unregister(KengineDocument* document, const char* name);

#ifdef __cplusplus
}
#endif

#endif
