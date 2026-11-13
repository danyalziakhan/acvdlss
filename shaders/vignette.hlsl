// The game's vignette. Redone on DLSS's output. The game applies it before
// tonemapping, so here it darkens linear light.

float3 srgb_decode(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow(max((c + 0.055) / 1.055, 0.0), 2.4);
}

float3 srgb_encode(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
}

float4 vignette(float4 c, float2 uv, float strength, bool hdr)
{
    float2 q = uv - 0.5;
    float v = pow(max(1.0 - dot(q, q), 0.0), strength);
    c.rgb = hdr ? c.rgb * v : srgb_encode(srgb_decode(c.rgb) * v);
    return c;
}
