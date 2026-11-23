// The exact vignette's change detector, pass 2. Compares every texel of the game's table, bit for bit, with
// the copy the inverse was built from and raises the flag on any difference (the decide pass may reject the table;
// the commit pass keeps the new bits). The inverse
// depends on nothing else (the build passes read only this table), and the build is deterministic, so equal bits
// mean an equal inverse: skipping the rebuild changes nothing. Also counts the changed texels, the largest change
// in 10-bit steps (R10G10B10A2 table) since last frame and since the last full build (the warm start's anchor).

Texture3D<float4> lut : register(t0);

#include "vignette_inverse_state.hlsl"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= 32))
        return;
    const float3 now = lut.Load(int4(id, 0)).rgb;
    const uint3 v = asuint(now);
    bool changed = false;
    uint delta = 0, from_anchor = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const uint3 at = uint3(id.xy, id.z * 3 + c);
        const uint old = state[at];
        if (old != v[c])
        {
            changed = true;
            delta = max(delta, steps_between(now[c], old));
        }
        from_anchor = max(from_anchor, steps_between(now[c], state[at + uint3(0, 0, STATE_ANCHOR_SLICE)]));
    }
    if (changed)
    {
        InterlockedOr(state[STATE_FLAG], 1u);
        InterlockedAdd(state[STATE_CHANGED], 1u);
        InterlockedMax(state[STATE_MAX_DELTA], delta);
    }
    if (from_anchor != 0)
        InterlockedMax(state[STATE_ANCHOR_DELTA], from_anchor);
}
