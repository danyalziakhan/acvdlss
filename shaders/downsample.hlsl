// When DLSS upscales, the game still keeps its TAA output at render size: the
// next frame's reflections and history read it. This fills that texture from
// the DLSS image, so they see the same picture the screen does.

Texture2D<float4> source : register(t0);
SamplerState linear_clamp : register(s0);
RWTexture2D<float4> target : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    target[id.xy] = source.SampleLevel(linear_clamp, uv, 0.0);
}
