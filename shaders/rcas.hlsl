// RCAS, the sharpening pass of AMD FidelityFX Super Resolution 1, MIT license
// (see the README). Sharpens by the largest amount that cannot overshoot the
// five-tap neighborhood, and backs off where luma looks like noise.

cbuffer constants : register(b0)
{
    float sharpness; // scales the lobe, 0 to 1
    float hdr;       // 1 when the image is scene referred and needs mapping into 0..1 first
    float2 unused;
};

Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);

// Highest negative lobe RCAS allows, 0.25 - 1/16.
static const float LOBE_LIMIT = 0.1875;

float3 fetch(int2 p, int2 last)
{
    float3 c = source[clamp(p, int2(0, 0), last)].rgb;
    // A reversible curve keeps highlights inside the range the limits assume.
    return hdr > 0.0 ? c / (1.0 + max(c.r, max(c.g, c.b))) : c;
}

float luma2(float3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;

    int2 p = int2(id.xy);
    int2 last = int2(width, height) - 1;
    float3 b = fetch(p + int2(0, -1), last);
    float3 d = fetch(p + int2(-1, 0), last);
    float3 e = fetch(p, last);
    float3 f = fetch(p + int2(1, 0), last);
    float3 h = fetch(p + int2(0, 1), last);

    float3 mn = min(min(b, d), min(f, h));
    float3 mx = max(max(b, d), max(f, h));
    float3 hit_min = min(mn, e) / (4.0 * mx);
    float3 hit_max = (1.0 - max(mx, e)) / (4.0 * mn - 4.0);
    float3 lobe3 = max(-hit_min, hit_max);
    float lobe = max(-LOBE_LIMIT, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * sharpness;

    float bl = luma2(b), dl = luma2(d), el = luma2(e), fl = luma2(f), hl = luma2(h);
    float range = max(max(max(bl, dl), max(el, fl)), hl) - min(min(min(bl, dl), min(el, fl)), hl);
    float noise = saturate(abs(0.25 * (bl + dl + fl + hl) - el) / max(range, 1e-5));
    lobe *= 1.0 - 0.5 * noise;

    float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    if (hdr > 0.0)
        c = c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4);
    target[p] = float4(c, source[p].a);
}
