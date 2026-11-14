// Shows the depth or motion vectors ACV DLSS gives effects.

#include "ACVDLSS.fxh"

uniform int Show <
    ui_type = "combo";
    ui_label = "Show";
    ui_items = "Depth\0Motion\0";
> = 0;

uniform float DepthRange <
    ui_type = "slider";
    ui_category = "Depth view";
    ui_label = "Range";
    ui_tooltip = "How far the view reaches before it turns black.";
    ui_min = 4.0; ui_max = 20.0; ui_step = 0.5;
> = 14.0;

uniform float MotionScale <
    ui_type = "slider";
    ui_category = "Motion view";
    ui_label = "Strength";
    ui_tooltip = "How little motion it takes to reach full color.";
    ui_min = 1.0; ui_max = 100.0;
> = 20.0;

// White near the camera. Black far away and at the sky. Log scale.
float3 depth_view(float2 uv)
{
    const float d = tex2Dlod(ACVDLSS::Depth, float4(uv, 0.0, 0.0)).x;
    return d > 0.0 ? 1.0 - saturate(log2(1.0 / d) / DepthRange) : 0.0;
}

float3 hue(float h)
{
    return saturate(abs(frac(h + float3(0.0, 2.0, 1.0) / 3.0) * 6.0 - 3.0) - 1.0);
}

// Gray where nothing moves. Hue is the direction and saturation the speed.
float3 motion_view(float2 uv)
{
    const float2 pixels = ACVDLSS::motion(uv) * BUFFER_SCREEN_SIZE;
    const float speed = saturate(length(pixels) / (100.0 / MotionScale));
    const float direction = atan2(-pixels.y, pixels.x) / 6.2831853 + 0.5;
    return lerp(0.5, hue(direction), speed);
}

float3 view(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float3 screen = tex2D(ReShade::BackBuffer, uv).rgb;
    const float3 shown = Show == 0 ? depth_view(uv) : motion_view(uv);
    return lerp(shown, screen, ACVDLSS::hud(uv, screen));
}

technique ACVDLSS_View <
    ui_label = "ACV DLSS View";
    ui_tooltip = "Shows the depth or the motion vectors ACV DLSS gives effects.";
>
{
    pass { VertexShader = PostProcessVS; PixelShader = view; }
}
