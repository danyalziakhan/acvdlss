// One cell of the game's 32^3 table as a trilinear map, for the cell-scatter inverse build
// (vignette_inverse_clear/scatter/resolve.hlsl).
// Needs lut_texel() (vignette_exact.hlsl) before the include. Corner k = dx * 4 + dy * 2 + dz.

void cell_load(int3 i, out float3 c[8])
{
    [unroll] for (int k = 0; k < 8; ++k)
        c[k] = lut_texel(i + int3(k >> 2, (k >> 1) & 1, k & 1));
}

// Value at local f (0..1 per axis) and the Jacobian columns.
float3 cell_value(float3 c[8], float3 f, out float3 jx, out float3 jy, out float3 jz)
{
    float3 v = 0.0;
    jx = 0.0;
    jy = 0.0;
    jz = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const float3 d = float3(k >> 2, (k >> 1) & 1, k & 1);
        const float3 w = d > 0.5 ? f : 1.0 - f;
        const float3 s = d > 0.5 ? 1.0 : -1.0;
        v += w.x * w.y * w.z * c[k];
        jx += s.x * w.y * w.z * c[k];
        jy += w.x * s.y * w.z * c[k];
        jz += w.x * w.y * s.z * c[k];
    }
    return v;
}

// 8 Newton steps toward value y from the cell centre, clamped to the cell. e: largest residual at the end.
float3 cell_solve(float3 c[8], float3 y, out float e)
{
    float3 f = 0.5;
    float3 jx, jy, jz;
    [loop] for (int k = 0; k < 8; ++k)
    {
        const float3 r = cell_value(c, f, jx, jy, jz) - y;
        const float3 a = cross(jy, jz), b = cross(jz, jx), d = cross(jx, jy);
        const float det = dot(jx, a);
        if (abs(det) < 1e-12)
            break;
        f = saturate(f - float3(dot(a, r), dot(b, r), dot(d, r)) / det);
    }
    const float3 r = abs(cell_value(c, f, jx, jy, jz) - y);
    e = max(r.x, max(r.y, r.z));
    return f;
}
