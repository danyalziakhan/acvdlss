// The game's screen effects in one pass, in the game's order: vignette, radial
// blur, lens distortion.

cbuffer constants : register(b0)
{
    float4 size; // width, height, 1 / width, 1 / height
    float4 flags; // vignette strength, hdr, radial blur on, lens distortion on
    float4 radial_c0, radial_c1, radial_c2, radial_c3;
    float4 lens_c1, lens_c2, lens_c3;
};

Texture2D<float4> source : register(t0);
SamplerState linear_clamp : register(s0);
RWTexture2D<float4> target : register(u0);

#include "vignette.hlsl"

float4 before_radial(float2 uv)
{
    float4 c = source.SampleLevel(linear_clamp, uv, 0.0);
    return flags.x > 0.0 ? vignette(c, uv, flags.x, flags.y > 0.0) : c;
}

#include "radial.hlsl"

float4 before_lens(float2 uv)
{
    return flags.z > 0.0 ? radial_blur(uv, radial_c0, radial_c1, radial_c2, radial_c3) : before_radial(uv);
}

#include "lens.hlsl"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(size.x) || id.y >= uint(size.y))
        return;
    float2 uv = (float2(id.xy) + 0.5) * size.zw;
    target[id.xy] = flags.w > 0.0 ? lens_distortion(uv, lens_c1, lens_c2, lens_c3) : before_lens(uv);
}
