// The exact vignette's cell-scatter inverse, pass 3 of 3. Each node solves again in the cell that won it and
// stores the table coordinate (w: its largest residual). A node no cell reached keeps itself as the coordinate, w 1e9.

Texture3D<float4> lut : register(t0);
RWTexture3D<float4> inverse : register(u0);
RWTexture3D<uint> keys : register(u1);

#include "vignette_exact.hlsl"
#include "vignette_cell.hlsl"
#include "vignette_inverse_state.hlsl"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= 33) || skip_build())
        return;
    if (all(id == 0))
    {
        state[STATE_REBUILDS] = state[STATE_REBUILDS] + 1;
        if (!warm_build())
            state[STATE_FULL_BUILDS] = state[STATE_FULL_BUILDS] + 1;
    }
    const float3 y = float3(id) / 32.0;
    const uint key = keys[id];
    if (key == KEY_WARM)
        return;
    if (key == 0xffffffff)
    {
        inverse[id] = float4(y, 1e9);
        return;
    }
    const uint cell = key & 32767;
    const int3 i = int3(cell / 961, (cell / 31) % 31, cell % 31);
    float3 c[8];
    cell_load(i, c);
    float e;
    const float3 f = cell_solve(c, y, e);
    inverse[id] = float4((float3(i) + f) / 31.0, e);
}
