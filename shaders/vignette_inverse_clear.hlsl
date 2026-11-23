// The exact vignette's cell-scatter inverse, pass 1 of 3 (full builds only). Empties the per-node keys and
// makes the current table the warm start's anchor.

Texture3D<float4> lut : register(t0);
RWTexture3D<uint> keys : register(u1);

#include "vignette_inverse_state.hlsl"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= 33) || skip_build() || warm_build())
        return;
    keys[id] = 0xffffffff;
    if (all(id < 32))
    {
        const uint3 v = asuint(lut.Load(int4(id, 0)).rgb);
        [unroll] for (uint c = 0; c < 3; ++c)
            state[uint3(id.xy, STATE_ANCHOR_SLICE + id.z * 3 + c)] = v[c];
    }
}
