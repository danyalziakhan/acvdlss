// The game's lens distortion in the raven view. Redone on DLSS's output.
// Same math as the game's pixel shader. Reads the image through before_lens().

float4 lens_distortion(float2 uv, float4 c1, float4 c2, float4 c3)
{
    float2 p = float2(uv.x, uv.y * c1.x + c1.y) - 0.5;
    float r = exp(log(abs(length(p) * c1.z)) * c2.z);
    float2 at = uv + (uv - 0.5) * r * c2.w;
    return float4(before_lens(at).rgb + (c3.rgb - r - c2.y), 1.0);
}
