// The game's radial blur while flying as the raven. Redone on DLSS's output.
// Same math as the game's pixel shader. Reads the image through before_radial().

float4 radial_blur(float2 uv, float4 c0, float4 c1, float4 c2, float4 c3)
{
    float2 v = uv - c0.xy;
    float d = length(v);
    float2 dir = d > 0.0 ? v / d : float2(0.0, 0.0);
    float w = max(d - c1.x, 0.0);
    float2 base = dir * min(d, c1.x) + c0.xy;

    float4 sum = 0.0;
    float t = 1.0;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        sum += before_radial(base + dir * w * t);
        t += c2.x * 0.142857;
    }
    float weight = clamp(1.0 - (d * w * c3.x + c3.y), c3.z, c3.w);
    return sum * 0.125 * weight;
}
