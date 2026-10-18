// The engine's annotation: what of each entity a shader's buffer holds, for the engine to fill.
//
//     import entity;
//
//     struct Instance {
//         [entity("MATRIX")] float4x4 model;   // where the entity is in the world
//         float3 color;                        // not annotated: the entity's own, a field of the pipeline's component
//     };
//     [entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;
//
// A buffer marked PER_INSTANCE has an element for each entity an indirect Draw draws — the entities of the Filter
// whose meshes its MeshPack packs, in the order its commands draw them — so a vertex shader finds its own at
// SV_StartInstanceLocation + SV_InstanceID. Its fields:
//
//     MATRIX          float4x4   where the entity is in the world
//     NORMAL_MATRIX   float4x4   what turns its normals: the inverse of that, transposed
//     POSITION        float3     where it is in the world
//     ID              uint       which entity it is: one more than its index (0 is none)
//
// Any field not annotated is the entity's own: a field of the component named after the pipeline's node, shown in
// the inspector and set from a script (`world.set(entity, "<pipeline>", "color", ...)`). It is zero on an entity
// without that component.
//
// The attribute is named after the module, as Koral's are (`[camera("VIEW_MATRIX")]`): Slang takes its name from
// the struct below, minus the trailing "Attribute".

module entity;

[__AttributeUsage(_AttributeTargets.Var)]
public struct entityAttribute
{
    string semantic;
};
