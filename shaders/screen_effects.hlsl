// The game's screen effects in one pass: vignette, radial blur, double image and lens distortion.
// Real branches: ?: would run every effect.

cbuffer constants : register(b0)
{
    float4 size; // width, height, 1 / width, 1 / height
    float4 flags; // vignette strength, hdr, radial blur on, lens distortion on
    float4 radial_c0, radial_c1, radial_c2, radial_c3;
    float4 lens_c1, lens_c2, lens_c3;
    float4 doubling; // xy: offset, z: amount, 0 for off
    float4 exact; // x 1 for the exact vignette, y 1 when the game's table is on (c0.z), z its c6.x (split),
                  // w 1 to check the residual through the hardware filter
    // x start (0 interpolated inverse, 1 best of the 8 nodes), y Newton steps per pixel, z largest accepted
    // residual, w fallback rule (0 residual, 1 residual + det + top as first built, 2 never)
    float4 exact_opts;
    // guard: x largest output amplification ||J(t') J(t)^-1|| accepted (0 off), y 1 counts rejected pixels in
    // u1, z 1 rejects folds (det J(t) <= 0), w 1 rejects a solution with a coordinate at 0 or 1 (clamped/clipped)
    float4 exact_guard;
};

Texture2D<float4> source : register(t0);
SamplerState linear_clamp : register(s0);
RWTexture2D<float4> target : register(u0);
// The exact vignette. The game's grading table and our inverse of it.
Texture3D<float4> lut : register(t1);
Texture3D<float4> inverse : register(t2);
// The inverse build's state texture (counters in slice 96), for the guard's reject count.
RWTexture3D<uint> exact_state : register(u1);

#include "vignette.hlsl"
#include "vignette_exact.hlsl"

// How far the vignetted output moves per unit change of the input around the solution t: the inf-norm of
// J(t') J(t)^-1, t' = saturate(t + shift), columns of a clamped coordinate of t' set to 0 (they don't move the output).
// Flat, clipped or folded parts of the table make J(t) near singular and this large. There a step of the input
// (DLSS output moves by a few 8-bit steps between frames) can throw the output anywhere.
float exact_amplification(float3 t, float3 tp, out float det_t)
{
    float3 dx, dy, dz, ex, ey, ez;
    lut_trilerp(t, dx, dy, dz);
    lut_trilerp(tp, ex, ey, ez);
    const float3 a = cross(dy, dz), b = cross(dz, dx), d = cross(dx, dy);
    const float det = dot(dx, a);
    det_t = det;
    const float inv_det = abs(det) > 1e-9 ? 1.0 / det : 0.0;
    const float3 live = float3(tp > 0.0 && tp < 1.0);
    ex *= live.x;
    ey *= live.y;
    ez *= live.z;
    // Row i of J' J^-1 = (ex[i] a + ey[i] b + ez[i] d) / det, with a, b, d the rows of det * J^-1.
    float worst = 0.0;
    [unroll] for (uint i = 0; i < 3; ++i)
    {
        const float3 row = (ex[i] * a + ey[i] * b + ez[i] * d) * inv_det;
        worst = max(worst, abs(row.x) + abs(row.y) + abs(row.z));
    }
    return inv_det != 0.0 ? worst : 1e9;
}

// Exact where the table can be inverted. False hands the pixel to the fitted model.
bool vignette_exact(inout float4 c, float2 uv, float strength)
{
    // The table check rejected this frame's table, or the scatter met a cell no grading table has: fitted model.
    // (exact_guard.y: the state texture is bound.)
    if (exact_guard.y > 0.0 && (exact_state[uint3(11, 0, 96)] != 0 || exact_state[uint3(13, 0, 96)] != 0))
        return false;
    const float2 q = uv - 0.5;
    const float lv = strength * log2(max(1.0 - dot(q, q), 1e-6));
    const float3 y = saturate(c.rgb);
    bool ok = false;
    [branch] if (exact.y == 0.0)
    {
        // The game's closed-form curve: sRGB(saturate(h / (0.977099 h + 1.465649))). Strictly rising below h = 64.
        if (all(y < 1.0))
        {
            const float v = exp2(lv);
            [unroll] for (uint i = 0; i < 3; ++i)
            {
                const float x = srgb_decode(y[i]);
                const float h = 1.465649 * x / (1.0 - 0.977099 * x) * v;
                c[i] = srgb_encode(saturate(h / (h * 0.977099 + 1.465649)));
            }
            ok = true;
        }
    }
    // The second chain on one side of the screen isn't inverted.
    else if (exact.z == 0.0 || (exact.z > 0.0 ? uv.x >= exact.z : uv.x <= -exact.z))
    {
        float3 start;
        [branch] if (exact_opts.x > 0.5)
        {
            // The node among the 8 around y whose table value is closest to y.
            const int3 n = min(int3(y * 32.0), int3(31, 31, 31));
            float best = 1e9;
            start = 0.5;
            [loop] for (int k = 0; k < 8; ++k)
            {
                const float3 t0 = inverse.Load(int4(n + int3(k >> 2, (k >> 1) & 1, k & 1), 0)).xyz;
                float3 dx, dy, dz;
                const float3 d = abs(lut_trilerp(t0, dx, dy, dz) - y);
                const float e = max(d.x, max(d.y, d.z));
                if (e < best)
                {
                    best = e;
                    start = t0;
                }
            }
        }
        else
            start = inverse.SampleLevel(linear_clamp, y * (32.0 / 33.0) + 0.5 / 33.0, 0.0).xyz;
        const float3 t = lut_steps(y, start, int(exact_opts.y));
        float3 residual;
        float det = 1.0;
        [branch] if (exact.w > 0.5 && exact_opts.w != 1.0)
        {
            // The residual through the hardware filter, as the game reads the table: 1 sample instead of 8 loads.
            residual = lut.SampleLevel(linear_clamp, t * LUT_SCALE + LUT_OFFSET, 0.0).rgb - y;
        }
        else
        {
            float3 dx, dy, dz;
            residual = lut_trilerp(t, dx, dy, dz) - y;
            det = dot(dx, cross(dy, dz));
        }
        const float3 e = abs(residual);
        const bool close = max(e.x, max(e.y, e.z)) <= exact_opts.z;
        const bool regular = abs(det) >= 1e-6 && all(t < 0.999);
        if (exact_opts.w > 1.5 || (close && (exact_opts.w < 0.5 || regular)))
        {
            const float3 tp = saturate(t + 0.05 * lv);
            // Guard: the output must not jump when the input moves by a quantisation step, the solution must not sit
            // on a fold or at the edge of the table (where the preimage is not unique).
            bool stable = true;
            [branch] if (exact_guard.x > 0.0 || exact_guard.z > 0.0)
            {
                float det_t;
                const float amp = exact_amplification(t, tp, det_t);
                stable = (exact_guard.x <= 0.0 || amp <= exact_guard.x) && (exact_guard.z <= 0.0 || det_t > 0.0);
            }
            if (exact_guard.w > 0.0)
                stable = stable && all(t > 0.001 && t < 0.999);
            if (stable)
            {
                c.rgb = lut.SampleLevel(linear_clamp, tp * LUT_SCALE + LUT_OFFSET, 0.0).rgb;
                ok = true;
            }
            else if (exact_guard.y > 0.0)
                InterlockedAdd(exact_state[uint3(9, 0, 96)], 1u);
        }
    }
    return ok;
}

float4 before_radial(float2 uv)
{
    float4 c = source.SampleLevel(linear_clamp, uv, 0.0);
    [branch] if (flags.x != 0.0)
    {
        bool done = false;
        [branch] if (exact.x > 0.0 && flags.y <= 0.0)
            done = vignette_exact(c, uv, flags.x);
        if (!done)
            c = vignette(c, uv, flags.x, flags.y > 0.0);
    }
    return c;
}

#include "radial.hlsl"

float4 before_double(float2 uv)
{
    float4 c;
    [branch] if (flags.z > 0.0)
        c = radial_blur(uv, radial_c0, radial_c1, radial_c2, radial_c3);
    else
        c = before_radial(uv);
    return c;
}

#include "double_image.hlsl"

float4 before_lens(float2 uv)
{
    float4 c;
    [branch] if (doubling.z != 0.0)
        c = double_image(uv, doubling);
    else
        c = before_double(uv);
    return c;
}

#include "lens.hlsl"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(size.x) || id.y >= uint(size.y))
        return;
    float2 uv = (float2(id.xy) + 0.5) * size.zw;
    [branch] if (flags.w > 0.0)
        target[id.xy] = lens_distortion(uv, lens_c1, lens_c2, lens_c3);
    else
        target[id.xy] = before_lens(uv);
}
