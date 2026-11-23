// The exact vignette. The game darkens the scene before its grading table:
// out = T(saturate(log2(h * v) * 0.05 + 0.623697) * 31/32 + 1/64). In table coordinates the darkening is a shift
// along the grey diagonal, so on the graded image: out = T(saturate(T^-1(y) + 0.05 * log2(v))).
// Needs `lut` (the game's 32^3 table) declared before the include.

static const float LUT_SCALE = 31.0 / 32.0;
static const float LUT_OFFSET = 1.0 / 64.0;

float3 lut_texel(int3 i)
{
    return lut.Load(int4(i, 0)).rgb;
}

// The trilinear interpolant at table coordinate t (0..1 per axis) and its Jacobian columns.
float3 lut_trilerp(float3 t, out float3 dx, out float3 dy, out float3 dz)
{
    const float3 u = saturate(t) * 31.0;
    const int3 i = min(int3(u), int3(30, 30, 30));
    const float3 f = u - float3(i);
    const float3 c000 = lut_texel(i);
    const float3 c100 = lut_texel(i + int3(1, 0, 0));
    const float3 c010 = lut_texel(i + int3(0, 1, 0));
    const float3 c110 = lut_texel(i + int3(1, 1, 0));
    const float3 c001 = lut_texel(i + int3(0, 0, 1));
    const float3 c101 = lut_texel(i + int3(1, 0, 1));
    const float3 c011 = lut_texel(i + int3(0, 1, 1));
    const float3 c111 = lut_texel(i + int3(1, 1, 1));
    const float3 x00 = lerp(c000, c100, f.x), x10 = lerp(c010, c110, f.x);
    const float3 x01 = lerp(c001, c101, f.x), x11 = lerp(c011, c111, f.x);
    const float3 y0 = lerp(x00, x10, f.y), y1 = lerp(x01, x11, f.y);
    dx = 31.0 * lerp(lerp(c100 - c000, c110 - c010, f.y), lerp(c101 - c001, c111 - c011, f.y), f.z);
    dy = 31.0 * lerp(x10 - x00, x11 - x01, f.z);
    dz = 31.0 * (y1 - y0);
    return lerp(y0, y1, f.z);
}

// Newton steps toward lut_trilerp(t) = y. det is the Jacobian's determinant at the end.
float3 lut_newton(float3 y, float3 t, int steps, out float3 residual, out float det)
{
    float3 dx, dy, dz;
    det = 0.0;
    [loop] for (int k = 0; k < steps; ++k)
    {
        const float3 r = lut_trilerp(t, dx, dy, dz) - y;
        const float3 a = cross(dy, dz), b = cross(dz, dx), c = cross(dx, dy);
        det = dot(dx, a);
        if (abs(det) < 1e-8)
            break;
        t = saturate(t - float3(dot(a, r), dot(b, r), dot(c, r)) / det);
    }
    residual = lut_trilerp(t, dx, dy, dz) - y;
    det = dot(dx, cross(dy, dz));
    return t;
}

// Newton steps only, no final evaluation (the caller checks the result its own way).
float3 lut_steps(float3 y, float3 t, int steps)
{
    float3 dx, dy, dz;
    [loop] for (int k = 0; k < steps; ++k)
    {
        const float3 r = lut_trilerp(t, dx, dy, dz) - y;
        const float3 a = cross(dy, dz), b = cross(dz, dx), c = cross(dx, dy);
        const float det = dot(dx, a);
        if (abs(det) < 1e-8)
            break;
        t = saturate(t - float3(dot(a, r), dot(b, r), dot(c, r)) / det);
    }
    return t;
}

float srgb_decode(float y)
{
    return y <= 0.04045 ? y / 12.92 : pow((y + 0.055) / 1.055, 2.4);
}

float srgb_encode(float x)
{
    return x <= 0.003131 ? x * 12.92 : 1.055 * pow(x, 0.416667) - 0.055;
}
