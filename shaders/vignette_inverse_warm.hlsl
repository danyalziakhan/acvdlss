// The exact vignette's warm-start inverse. One thread per inverse node. When
// the game's table changed, each node with a solution is solved again in the cell that held its solution last frame,
// with the full build's own cell solve (vignette_cell.hlsl: 8 Newton steps from the cell centre, clamped to the cell),
// so the result is what the full build computes for that cell. Interior nodes (last residual <= 0.5/255) keep it
// when the new residual is <= 0.5/255. Otherwise the 3x3x3 cells around are solved and the best kept when it is
// <= 0.5/255. Only then is the key cleared, and the scatter and resolve solve the node over all cells, exactly as
// the full build. Boundary nodes (no exact preimage, last residual > 0.5/255: starting points
// outside the table's image) keep the re-solve in their cell. Nodes without a solution (w 1e9) are kept.

Texture3D<float4> lut : register(t0);
RWTexture3D<float4> inverse : register(u0);
RWTexture3D<uint> keys : register(u1);

#include "vignette_exact.hlsl"
#include "vignette_cell.hlsl"
#include "vignette_inverse_state.hlsl"

static const float EXACT_RESIDUAL = 0.5 / 255.0;

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= 33) || skip_build() || !warm_build())
        return;
    const float4 last = inverse[id];
    if (last.w >= 1e8)
    {
        keys[id] = KEY_WARM;
        return;
    }
    const float3 y = float3(id) / 32.0;
    const int3 i = min(int3(last.xyz * 31.0), int3(30, 30, 30));
    float3 c[8];
    cell_load(i, c);
    float e;
    const float3 f = cell_solve(c, y, e);
    if (last.w > EXACT_RESIDUAL || e <= EXACT_RESIDUAL)
    {
        inverse[id] = float4((float3(i) + f) / 31.0, e);
        keys[id] = KEY_WARM;
        return;
    }
    // An interior node whose exact solution left its cell: the 3x3x3 cells around, smallest residual (ties: the lowest
    // cell index, as the full build's key). Only when none is exact does the node go to the full scatter.
    InterlockedAdd(state[STATE_FULL_NODES], 1u);
    uint best_key = 0xffffffff;
    float3 best_t = 0.0;
    float best_e = 1e9;
    [loop] for (uint k = 0; k < 27; ++k)
    {
        const int3 j = clamp(i + int3(uint3(k / 9, (k / 3) % 3, k % 3)) - 1, 0, 30);
        cell_load(j, c);
        float ek;
        const float3 fk = cell_solve(c, y, ek);
        const uint key = (min(uint(ek * 65536.0), 131071u) << 15) | uint(j.x * 961 + j.y * 31 + j.z);
        if (key < best_key)
        {
            best_key = key;
            best_t = (float3(j) + fk) / 31.0;
            best_e = ek;
        }
    }
    if (best_e <= EXACT_RESIDUAL || build.w != 0)
    {
        inverse[id] = float4(best_t, best_e);
        keys[id] = KEY_WARM;
    }
    else
    {
        keys[id] = 0xffffffff;
        InterlockedAdd(state[STATE_SCATTER_NODES], 1u);
    }
}
