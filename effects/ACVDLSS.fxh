// What ACV DLSS gives ReShade effects.
//
// Motion: UV offset from each pixel to where it was a frame ago. Render size.
// Depth: reversed device depth. 1 at the near plane and 0 at the sky. Render size.
// HUD-less: the DLSS image before the HUD. It's 1x1 while DLSS isn't running.

#pragma once

#include "ReShade.fxh"

namespace ACVDLSS
{
    texture MotionTex : ACVDLSS_MOTION;
    sampler Motion { Texture = MotionTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

    texture DepthTex : ACVDLSS_DEPTH;
    sampler Depth { Texture = DepthTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

    texture HudlessTex : ACVDLSS_HUDLESS;
    sampler Hudless { Texture = HudlessTex; };

    float2 motion(float2 uv)
    {
        return tex2Dlod(Motion, float4(uv, 0.0, 0.0)).xy;
    }

    // Distance from the camera in near plane units. Only ratios mean anything. The sky is very far.
    float view_distance(float2 uv)
    {
        return 1.0 / max(tex2Dlod(Depth, float4(uv, 0.0, 0.0)).x, 1e-6);
    }

    bool has_hudless()
    {
        return tex2Dsize(Hudless).x > 1;
    }

    // The picture without the HUD, or the screen when there is none.
    // A real branch: ?: would read both textures.
    float3 scene(float2 uv)
    {
        [branch] if (has_hudless())
            return tex2Dlod(Hudless, float4(uv, 0.0, 0.0)).rgb;
        return tex2Dlod(ReShade::BackBuffer, float4(uv, 0.0, 0.0)).rgb;
    }

    // How much HUD covers this pixel, 0 to 1.
    // Effects above this one in the list change the screen too and count as HUD.
    float hud(float2 uv, float3 screen)
    {
        const float3 diff = abs(screen - tex2Dlod(Hudless, float4(uv, 0.0, 0.0)).rgb);
        return has_hudless() ? smoothstep(0.01, 0.05, max(diff.r, max(diff.g, diff.b))) : 0.0;
    }

    // The blurred picture with the HUD put back. HUD that only darkens the scene, like a translucent band,
    // darkens the blur by the same amount. Any other HUD is kept as it is on screen.
    float3 with_hud(float3 blurred, float2 uv, float3 screen)
    {
        if (!has_hudless())
            return blurred;
        const float3 clean = tex2Dlod(Hudless, float4(uv, 0.0, 0.0)).rgb;
        const float3 diff = screen - clean;
        const float spread = max(abs(diff.r), max(abs(diff.g), abs(diff.b)));
        if (spread < 0.01)
            return blurred;
        if (max(diff.r, max(diff.g, diff.b)) <= 0.004)
            return blurred * saturate(screen / max(clean, 1e-4));
        return lerp(blurred, screen, smoothstep(0.01, 0.05, spread));
    }

    // Interleaved gradient noise, moving each frame.
    float noise(float2 pixel, uint frame)
    {
        pixel += float(frame % 64) * 5.588238;
        return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    }
}
