// The game's underwater double image: the image mixed with a shifted copy of itself. Redone on DLSS's output.
// Same math as the game's pixel shader. Reads the image through before_double().

float4 double_image(float2 uv, float4 d)
{
    const float4 a = before_double(uv);
    return lerp(a, before_double(uv + d.xy), d.z);
}
