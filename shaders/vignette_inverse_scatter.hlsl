// The exact vignette's cell-scatter inverse, pass 2 of 3. One thread per cell of the game's 32^3 table:
// every inverse node (33^3 grid of graded colours) inside the cell's output box, widened by one node spacing, is
// solved in the cell. The node keeps the cell with the smallest residual (key: residual in 1/65536, 17 bits, then
// the cell, 15 bits).

Texture3D<float4> lut : register(t0);
RWTexture3D<uint> keys : register(u1);

#include "vignette_exact.hlsl"
#include "vignette_cell.hlsl"
#include "vignette_inverse_state.hlsl"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= 31) || skip_scatter())
        return;
    float3 c[8];
    cell_load(int3(id), c);
    float3 lo = c[0], hi = c[0];
    [unroll] for (int k = 1; k < 8; ++k)
    {
        lo = min(lo, c[k]);
        hi = max(hi, c[k]);
    }
    const int3 a = int3(clamp(ceil((lo - 1.0 / 32.0) * 32.0), 0.0, 32.0));
    const int3 b = int3(clamp(floor((hi + 1.0 / 32.0) * 32.0), 0.0, 32.0));
    const uint cell = id.x * 961 + id.y * 31 + id.z;
    // A cell spanning far more nodes than any real table cell: the table is not a grading table. Skip, and the frame
    // uses the fitted model.
    const int3 extent = max(b - a + 1, 0);
    if (uint(extent.x * extent.y * extent.z) > SCATTER_CELL_CAP)
    {
        InterlockedAdd(state[STATE_WILD], 1u);
        return;
    }
    [loop] for (int z = a.z; z <= b.z; ++z)
        [loop] for (int y = a.y; y <= b.y; ++y)
            [loop] for (int x = a.x; x <= b.x; ++x)
            {
                if (keys[uint3(x, y, z)] == KEY_WARM)
                    continue;
                float e;
                cell_solve(c, float3(x, y, z) / 32.0, e);
                const uint key = (min(uint(e * 65536.0), 131071u) << 15) | cell;
                InterlockedMin(keys[uint3(x, y, z)], key);
            }
}
