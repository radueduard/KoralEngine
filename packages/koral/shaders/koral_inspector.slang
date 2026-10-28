// What a shader says of its inputs for the editor to show them by: `import koral_inspector;`, then
//
//     void computeMain(uint3 id : SV_DispatchThreadID,
//                      [Range(0, 50)] uniform float windSpeed = 10.0,
//                      [Color] uniform float4 tint = float4(1, 0.5, 0, 1))
//
// A Dispatch's or a Draw's `push.windSpeed` is then dragged from 0 to 50, starting at 10, and `push.tint` is a colour.
// The default is the shader's own (`= 10.0`): what the input is until it is given, and what a new entity's component
// starts as. A vector's is read when it is written as numbers.
//
// Slang lets an attribute be of a parameter or of a field, not both: on a struct's field the same are FieldRange,
// FieldMin, FieldMax, FieldLogarithmic and FieldColor.

/** Kept from min to max as it is dragged and typed. */
[__AttributeUsage(_AttributeTargets.Param)] struct RangeAttribute { float min; float max; };
/** Kept at min or more. */
[__AttributeUsage(_AttributeTargets.Param)] struct MinAttribute { float min; };
/** Kept at max or less. */
[__AttributeUsage(_AttributeTargets.Param)] struct MaxAttribute { float max; };
/** Dragged by so much of what it is: a size, a distance, an intensity. */
[__AttributeUsage(_AttributeTargets.Param)] struct LogarithmicAttribute {};
/** A float3 or float4 picked as a colour. */
[__AttributeUsage(_AttributeTargets.Param)] struct ColorAttribute {};

[__AttributeUsage(_AttributeTargets.Var)] struct FieldRangeAttribute { float min; float max; };
[__AttributeUsage(_AttributeTargets.Var)] struct FieldMinAttribute { float min; };
[__AttributeUsage(_AttributeTargets.Var)] struct FieldMaxAttribute { float max; };
[__AttributeUsage(_AttributeTargets.Var)] struct FieldLogarithmicAttribute {};
[__AttributeUsage(_AttributeTargets.Var)] struct FieldColorAttribute {};
