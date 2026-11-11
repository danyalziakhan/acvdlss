// When DLSS upscales, the game's TAA output is still render size, and the next
// frame's reflections and history read it. Fill it from the DLSS image so they
// match what's on screen.

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
