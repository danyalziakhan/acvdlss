// The exact vignette's table check, last pass. An accepted table's bits become the copy the next frame compares
// with.

Texture3D<float4> lut : register(t0);

#include "vignette_inverse_state.hlsl"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= 32) || state[STATE_TABLE_REJECTED] != 0)
        return;
    const uint3 v = asuint(lut.Load(int4(id, 0)).rgb);
    [unroll] for (uint c = 0; c < 3; ++c)
        state[uint3(id.xy, id.z * 3 + c)] = v[c];
}
