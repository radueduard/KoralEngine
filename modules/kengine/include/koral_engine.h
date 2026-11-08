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
/**
 * Where the engine's own package is: the folder whose `scripts` are every project's — the scene's Camera, its lights,
 * its Sky — as C# and Kotlin sources. A host compiles them with a project's own scripts.
 */
KENGINE_API const char* kengine_package_directory(void);

/* ---- a scene ------------------------------------------------------------------------------------------------- */

/** The engine's side of the scene whose frame graph this is (koral_scene_graph); null when it is no engine scene. */
KENGINE_API KengineDocument* kengine_document(KoralFrameGraph* graph);
KENGINE_API KecsWorld* kengine_document_world(KengineDocument* document);
/** The scene as JSON, as it is saved. */
KENGINE_API const char* kengine_document_json(KengineDocument* document);
/** The folder of the project the scene is of; "" for a scene that is in none. A runner of a project's scripts runs them in its own only. */
KENGINE_API const char* kengine_project_directory(KengineDocument* document);
/**
 * The `scripts` folders of the modules the scene's project imports, nearest first, one a line; "" for none. A runner of a
 * project's scripts compiles them with its own: what a module has is the project's too.
 */
KENGINE_API const char* kengine_module_scripts(KengineDocument* document);
KENGINE_API bool kengine_document_save(KengineDocument* document, const char* path);
/** Whether the scene plays: its scripts are updated. A scene run on its own does; one in an editor, when told to. */
KENGINE_API bool kengine_playing(KengineDocument* document);
KENGINE_API bool kengine_set_playing(KengineDocument* document, bool playing);
/**
 * The keys, the mouse and the gamepads the document's scripts read: its scene's window's — or, in an editor, the Game
 * panel's, fed only while the pointer is over it or it was the last clicked. Null for a document no scene runs.
 */
KENGINE_API KoralInput* kengine_script_input(KengineDocument* document);
/**
 * How big, in pixels, the window is whose mouse the scripts read (kengine_script_input): what its positions are from the
 * top-left of — to make a ray of the cursor through the camera. False, and nothing written, for a document no scene runs.
 */
KENGINE_API bool kengine_script_viewport(KengineDocument* document, float* width, float* height);
/**
 * The debug lines the document's scripts draw with (koral_debug_line and the rest): its scene's, shown over the
 * editor's view of it, and by a DebugDraw node of its graph. Drawn in an update, they last that frame; a fixed update
 * runs 0 to 8 times a frame, so what is to be seen every frame is drawn in update, or given a duration. Null for a
 * document no scene runs.
 */
KENGINE_API KoralDebugDraw* kengine_debug(KengineDocument* document);

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
/** Puts child under parent (the null entity: at the top), last there, staying where it is in the world. */
KENGINE_API bool kecs_set_parent(KecsWorld* world, KecsEntity child, KecsEntity parent);
/** The same, at index among what is there (last, when below 0), and staying where it is in the world or not. */
KENGINE_API bool kecs_set_parent_at(KecsWorld* world, KecsEntity child, KecsEntity parent, int32_t index, bool keep_world_transform);
KENGINE_API KecsEntity kecs_parent(KecsWorld* world, KecsEntity entity);
/** How many entities are directly under entity; for the null entity, how many are at the top. */
KENGINE_API size_t kecs_child_count(KecsWorld* world, KecsEntity entity);
/** The index th of them, in order; the null entity past the last. */
KENGINE_API KecsEntity kecs_child(KecsWorld* world, KecsEntity entity, size_t index);
/** The first entity so named, parents before their children; the null entity when there is none. */
KENGINE_API KecsEntity kecs_find(KecsWorld* world, const char* name);

/* An entity's transform, relative to what it is under: a position, a rotation (Euler angles, in degrees: about X,
   about Y, about Z) and a scale, three floats each. The matrices are sixteen, column after column. */
KENGINE_API bool kecs_position(KecsWorld* world, KecsEntity entity, float out[3]);
KENGINE_API bool kecs_rotation(KecsWorld* world, KecsEntity entity, float out[3]);
KENGINE_API bool kecs_scale(KecsWorld* world, KecsEntity entity, float out[3]);
KENGINE_API bool kecs_set_position(KecsWorld* world, KecsEntity entity, const float value[3]);
KENGINE_API bool kecs_set_rotation(KecsWorld* world, KecsEntity entity, const float value[3]);
KENGINE_API bool kecs_set_scale(KecsWorld* world, KecsEntity entity, const float value[3]);
KENGINE_API bool kecs_local_matrix(KecsWorld* world, KecsEntity entity, float out[16]);
/** Where it is in the world: what it is under, and its own. */
KENGINE_API bool kecs_world_matrix(KecsWorld* world, KecsEntity entity, float out[16]);
/** Places it in the world, by changing its own transform. */
KENGINE_API bool kecs_set_world_matrix(KecsWorld* world, KecsEntity entity, const float matrix[16]);

/* The same, of many entities in one call: `count` of them, three floats each in `out` or `values`, one after another in
   the order of `entities`. What one script does to thousands — a call each would cost more than the work does. An
   entity that is not alive is left out: its three are written as zeros (a scale as ones), and set from nothing. Each
   says how many were alive. */
KENGINE_API size_t kecs_positions(KecsWorld* world, const KecsEntity* entities, size_t count, float* out);
KENGINE_API size_t kecs_rotations(KecsWorld* world, const KecsEntity* entities, size_t count, float* out);
KENGINE_API size_t kecs_scales(KecsWorld* world, const KecsEntity* entities, size_t count, float* out);
KENGINE_API size_t kecs_set_positions(KecsWorld* world, const KecsEntity* entities, size_t count, const float* values);
KENGINE_API size_t kecs_set_rotations(KecsWorld* world, const KecsEntity* entities, size_t count, const float* values);
KENGINE_API size_t kecs_set_scales(KecsWorld* world, const KecsEntity* entities, size_t count, const float* values);

/* How an entity is turned as a quaternion, four floats x, y, z, w: what its matrix is made with. Set as one, it is
   kept as given (made length 1), and the Euler angles are worked out of it — so a rotation that is integrated a step
   at a time is not rounded off through the angles. Of many, as above, four floats each; one not alive reads as the
   identity. */
KENGINE_API bool kecs_orientation(KecsWorld* world, KecsEntity entity, float out[4]);
KENGINE_API bool kecs_set_orientation(KecsWorld* world, KecsEntity entity, const float value[4]);
KENGINE_API size_t kecs_orientations(KecsWorld* world, const KecsEntity* entities, size_t count, float* out);
KENGINE_API size_t kecs_set_orientations(KecsWorld* world, const KecsEntity* entities, size_t count, const float* values);

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

/* A field of a component, of many entities at once: its numbers, `width` of them an entity, one entity's after
   another in the order of `entities`. Floats for a field of them ("f32", "vec2" to "vec4", "quat", "mat4"), whole
   numbers for one of those ("i32", "u32", "bool", "bool32", "ivec" and "uvec"). An entity without the component reads
   as zeros and is not set; what is set is marked changed. Each says how many had it — none, for a field there is not
   or that is not of numbers of that kind. */
/** How many numbers the field is an entity: 1 for an "f32", 3 for a "vec3", 16 for a "mat4"; 0 for a field there is not, or not of numbers. */
KENGINE_API uint32_t kecs_field_width(KecsWorld* world, uint32_t component, const char* field);
KENGINE_API size_t kecs_field_floats(KecsWorld* world, const KecsEntity* entities, size_t count, uint32_t component, const char* field, float* out);
KENGINE_API size_t kecs_set_field_floats(KecsWorld* world, const KecsEntity* entities, size_t count, uint32_t component, const char* field, const float* values);
KENGINE_API size_t kecs_field_ints(KecsWorld* world, const KecsEntity* entities, size_t count, uint32_t component, const char* field, int32_t* out);
KENGINE_API size_t kecs_set_field_ints(KecsWorld* world, const KecsEntity* entities, size_t count, uint32_t component, const char* field, const int32_t* values);

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

/* ---- pictures ----------------------------------------------------------------------------------------------- */

/**
 * Makes a picture of @p width × @p height texels, @p rgba four bytes each (red, green, blue, alpha), its smaller levels
 * made of it, called `$script/<name>`, which is returned: what a material's picture field holds to have it. One so
 * named already is replaced: what held it holds this one. Null, with why, when the bytes are not so many.
 */
KENGINE_API const char* kengine_image_create(KengineDocument* document, const char* name, uint32_t width, uint32_t height, const uint8_t* rgba);
/**
 * Gives @p entity picture @p picture in its picture field @p field, of whichever of its components has one so named (its
 * material's `baseColorMap`). False when none has.
 */
KENGINE_API bool kengine_entity_give_picture(KengineDocument* document, KecsEntity entity, const char* field, const char* picture);

/* ---- meshes ------------------------------------------------------------------------------------------------- */

/** What an attribute's numbers are. Each is four bytes. */
enum { KENGINE_ATTRIBUTE_FLOAT = 0, KENGINE_ATTRIBUTE_INT = 1, KENGINE_ATTRIBUTE_UINT = 2 };

/**
 * One attribute of every vertex of a mesh: what it is, as a shader asks for it (POSITION, NORMAL, UV, TANGENT, or any
 * name of a script's own: "WIND"), the type and count of its numbers, and the numbers: @p count of them a vertex, one
 * vertex after another. Written by kengine_mesh_attribute, @p data is borrowed: good until the next mesh call on the
 * same thread.
 */
typedef struct KengineAttribute {
    const char* semantic;
    uint32_t type;          /* KENGINE_ATTRIBUTE_* */
    uint32_t count;         /* 1 to 4 */
    const void* data;
} KengineAttribute;

/**
 * Makes a mesh of @p vertex_count vertices, with these attributes (one of them POSITION, of floats) and these
 * indices (none: drawn in the order of its vertices). @p buffers says which vertex buffer each attribute is in:
 * "POSITION UV; NORMAL TANGENT" — a buffer to each ';' — with what none names in one more after them; null or empty,
 * all in one. It is called `$script/<name>`, which is returned, and what an entity's Mesh holds to have it
 * (kengine_field_set_json). One so named already is replaced: every entity that held it holds this one. Not saved
 * with the scene: a script makes its meshes when it runs. Null when the data is no mesh, with why.
 */
KENGINE_API const char* kengine_mesh_create(KengineDocument* document, const char* name, uint32_t vertex_count, const KengineAttribute* attributes,
                                            uint32_t attribute_count, const uint32_t* indices, uint32_t index_count, const char* buffers);
/**
 * A copy of @p mesh — any: `$cube`, `@model.gltf#3`, a script's — with these attributes added, or put in place of its
 * own of the same semantic, called `$script/<name>`. @p buffers as for kengine_mesh_create; null keeps the mesh's,
 * with what it did not have in one more. The mesh is read back from the device: the call waits for it.
 */
KENGINE_API const char* kengine_mesh_with_attributes(KengineDocument* document, const char* mesh, const char* name, const KengineAttribute* attributes,
                                                     uint32_t attribute_count, const char* buffers);
/** A copy of @p mesh without the attributes @p semantics names ("TANGENT BITANGENT"), called `$script/<name>`. */
KENGINE_API const char* kengine_mesh_without_attributes(KengineDocument* document, const char* mesh, const char* name, const char* semantics);
/** How many vertices a mesh has: 0, with why, for what is no mesh or is still being read. Reads it back from the device. */
KENGINE_API uint32_t kengine_mesh_vertex_count(KengineDocument* document, const char* mesh);
/** How many attributes it has; and the @p index th, its numbers read back (@see KengineAttribute). */
KENGINE_API uint32_t kengine_mesh_attribute_count(KengineDocument* document, const char* mesh);
KENGINE_API bool kengine_mesh_attribute(KengineDocument* document, const char* mesh, uint32_t index, KengineAttribute* out);
/** Its indices: up to @p capacity written to @p out; how many there are returned. */
KENGINE_API size_t kengine_mesh_indices(KengineDocument* document, const char* mesh, uint32_t* out, size_t capacity);

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

/* ---- scripts that make resources --------------------------------------------------------------------------------- */

/** One of the resources a resource script makes: an output of the Script node that runs it, by its name. */
typedef struct KengineScriptResource {
    const char* name;           /* the field it is */
    const char* kind;           /* the node type it is made as: "Image", "Buffer", "ImageView", "BufferView", "Sampler" */
    /* What it is made with, as a JSON object of its node type's inputs — {"format": "eR32_SFLOAT", "extent.x": 256},
     * {"image": "height"} for one made of another of the script's — or null for nothing: as its node type starts. */
    const char* settings;
} KengineScriptResource;

/** What a resource script fills: one of its images or buffers as it was made, and its contents, to be written. */
typedef struct KengineContents {
    const char* name;           /* the resource, as the script declares it */
    const char* node;           /* the Script node it is of */
    const char* kind;           /* "Image" or "Buffer" */
    const char* format;         /* an image's: "eRGBA8_UNORM" */
    uint32_t extent[3];         /* an image's, of its first level */
    uint32_t layers;            /* an image's array layers */
    uint32_t texel_bytes;       /* an image's: the bytes of one texel */
    void* bytes;                /* to fill, zeros to begin with: an image's first level, a layer, a row, a texel after another; a buffer's every byte */
    size_t size;
} KengineContents;

typedef struct KengineResourceScriptCallbacks {
    void* user;
    /* Fills each of @p contents' bytes — its images and buffers, @p count of them — given @p settings: the script's
     * fields, as it registered them. Returns null when it did, or why it could not — the caller's, good until it is
     * called again. */
    const char* (*fill)(void* user, const KengineContents* contents, size_t count, const void* settings, size_t settings_size);
} KengineResourceScriptCallbacks;

/**
 * Makes a script that makes resources — @p resources, each an output of a Script node that names it — one the graph's
 * Script nodes can name, with these fields as the node's inputs, and runs it: what it makes is filled by it as it is
 * made. Given no resources, it makes what its source declares. With one so named already, it takes its place, and
 * whatever it made is made and filled again. False when it cannot be.
 */
KENGINE_API bool kengine_resource_script_register(KengineDocument* document, const char* name, size_t size, const KecsField* fields, size_t field_count,
                                                  const KengineScriptResource* resources, size_t resource_count, const KengineResourceScriptCallbacks* callbacks);
/** Stops running a resource script: its code is about to go. A Script node still names it, and what it made is made empty. */
KENGINE_API void kengine_resource_script_unregister(KengineDocument* document, const char* name);

#ifdef __cplusplus
}
#endif

#endif
