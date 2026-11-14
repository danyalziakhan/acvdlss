// Blurs each pixel along its own motion from the game's motion vectors.
//
// Turn the game's motion blur off and keep this at the top of the list.
// The blur reads the HUD-less image so the HUD stays sharp.

#include "ACVDLSS.fxh"

uniform float Amount <
    ui_type = "slider";
    ui_label = "Amount";
    ui_tooltip = "How much of each frame's motion is blurred.";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.5;

uniform float MaxBlur <
    ui_type = "slider";
    ui_label = "Maximum blur";
    ui_tooltip = "The longest blur, in percent of the screen height.";
    ui_min = 1.0; ui_max = 15.0; ui_step = 0.5;
> = 4.0;

uniform int Samples <
    ui_type = "slider";
    ui_label = "Samples";
    ui_tooltip = "Samples along the blur. More is smoother and slower.";
    ui_min = 4; ui_max = 32;
> = 16;

uniform uint FrameCount < source = "framecount"; >;

float3 streak(float4 pixel : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float3 current = tex2D(ReShade::BackBuffer, uv).rgb;
    float2 travel = ACVDLSS::motion(uv) * Amount;

    // In screen heights so the cap is the same in both directions.
    float reach = length(travel * float2(BUFFER_ASPECT_RATIO, 1.0));
    const float cap = MaxBlur * 0.01;
    if (reach > cap)
    {
        travel *= cap / reach;
        reach = cap;
    }
    if (reach * BUFFER_HEIGHT < 1.0)
        return current;

    // Evenly spaced from half the streak behind to half ahead. Noise shifts them.
    const float share = 1.0 / float(Samples);
    const float shift = ACVDLSS::noise(pixel.xy, FrameCount) - 0.5;
    const float2 first = uv + travel * ((0.5 + shift) * share - 0.5);
    const float2 stride = travel * share;
    float3 total = 0.0;
    [loop]
    for (int k = 0; k < Samples; ++k)
        total += ACVDLSS::scene(first + stride * float(k));
    return ACVDLSS::with_hud(total * share, uv, current);
}

technique ACVDLSS_MotionBlur <
    ui_label = "ACV DLSS Motion Blur (simple)";
    ui_tooltip = "Simple motion blur along the game's own motion vectors.";
>
{
    pass { VertexShader = PostProcessVS; PixelShader = streak; }
}
