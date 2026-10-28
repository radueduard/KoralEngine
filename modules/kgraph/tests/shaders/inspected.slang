// Push constants as the editor shows them: defaults and koral_inspector's attributes. Writes them into `target`, so a test
// sees what a Dispatch given nothing for them ran with.

import koral_inspector;

struct Wave
{
    [FieldRange(1, 4)] float length = 2.5;
};

RWStructuredBuffer<float> target;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID, uniform Wave wave, uniform float plain,
                 [Range(0, 50)] uniform float speed = 10.0,
                 [Color] uniform float4 tint = float4(1, 0.5, 0, 1),
                 [Min(0)] uniform int steps = 3)
{
    target[0] = speed;
    target[1] = tint.x; target[2] = tint.y; target[3] = tint.z; target[4] = tint.w;
    target[5] = steps;
    target[6] = wave.length;
    target[7] = plain;
}
