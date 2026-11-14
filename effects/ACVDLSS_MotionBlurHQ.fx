// Depth-aware motion blur from the game's motion vectors and depth.
// Based on McGuire et al. 2012, Guertin et al. 2014 and Jimenez 2014.
//
// Turn the game's motion blur off and keep this at the top of the list.

#include "ACVDLSS.fxh"

// A tile covers the longest blur. A 3x3 block of tiles then sees all motion that can reach a pixel.
#define ACVDLSS_TILE (BUFFER_HEIGHT / 30)
#define ACVDLSS_TILES_X ((BUFFER_WIDTH + ACVDLSS_TILE - 1) / ACVDLSS_TILE)
#define ACVDLSS_TILES_Y ((BUFFER_HEIGHT + ACVDLSS_TILE - 1) / ACVDLSS_TILE)

uniform float Amount <
    ui_type = "slider";
    ui_label = "Amount";
    ui_tooltip = "How much of each frame's motion is blurred. Works like a camera's shutter.";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.5;

uniform float MaxBlur <
    ui_type = "slider";
    ui_label = "Maximum blur";
    ui_tooltip = "The longest blur, in percent of the screen height.";
    ui_min = 1.0; ui_max = 6.5; ui_step = 0.1;
> = 5.0;

uniform int Samples <
    ui_type = "slider";
    ui_label = "Samples";
    ui_tooltip = "Samples per pixel. More is smoother and slower.";
    ui_min = 8; ui_max = 32;
> = 16;

uniform uint FrameCount < source = "framecount"; >;

texture ACVDLSS_HQ_TileMaxTex { Width = ACVDLSS_TILES_X; Height = ACVDLSS_TILES_Y; Format = RG16F; };
sampler TileMax { Texture = ACVDLSS_HQ_TileMaxTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
texture ACVDLSS_HQ_NeighborMaxTex { Width = ACVDLSS_TILES_X; Height = ACVDLSS_TILES_Y; Format = RG16F; };
sampler NeighborMax { Texture = ACVDLSS_HQ_NeighborMaxTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

// Half the blur in pixels. Capped at a tile.
float2 velocity(float2 uv)
{
    const float2 v = ACVDLSS::motion(uv) * BUFFER_SCREEN_SIZE * (Amount * 0.5);
    const float limit = min(MaxBlur * 0.01 * BUFFER_HEIGHT * 0.5, float(ACVDLSS_TILE));
    const float len = length(v);
    return len > limit ? v * (limit / len) : v;
}

float2 longer(float2 a, float2 b)
{
    return dot(a, a) >= dot(b, b) ? a : b;
}

// Fastest motion in the tile. Reads every second pixel.
float2 tile_max(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float2 origin = floor(position.xy) * ACVDLSS_TILE;
    const uint side = ACVDLSS_TILE / 2;
    // Adds 0. A constant count makes the compiler try a huge unroll and fail.
    const uint count = side * side + uint(min(Samples, 0));
    float2 fastest = 0.0;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const float2 pixel = float2(i % side, i / side) * 2.0 + 1.0;
        fastest = longer(fastest, velocity((origin + pixel) * BUFFER_PIXEL_SIZE));
    }
    return fastest;
}

// Fastest motion around the tile. Diagonal tiles count only when moving toward this one (Jimenez).
float2 neighbor_max(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const int2 tile = int2(position.xy);
    const int2 last = int2(ACVDLSS_TILES_X - 1, ACVDLSS_TILES_Y - 1);
    float2 fastest = 0.0;
    [unroll]
    for (int i = 0; i < 9; ++i)
    {
        const int2 offset = int2(i % 3, i / 3) - 1;
        const float2 v = tex2Dfetch(TileMax, clamp(tile + offset, int2(0, 0), last)).xy;
        const bool diagonal = offset.x != 0 && offset.y != 0;
        const bool toward = dot(v, -float2(offset) * 0.7071) >= 0.7071 * length(v);
        if (!diagonal || toward)
            fastest = longer(fastest, v);
    }
    return fastest;
}

// 1 when a is in front of b. Fades to 0 within 5% of depth.
float in_front(float a, float b)
{
    return saturate(1.0 - (a - b) / (0.05 * min(a, b)));
}

// How much a blur of this speed reaches dist pixels away.
float cone(float dist, float speed)
{
    return saturate(1.0 - dist / speed);
}

float cylinder(float dist, float speed)
{
    return 1.0 - smoothstep(0.95 * speed, 1.05 * speed, dist);
}

float2 unit(float2 v)
{
    const float len = length(v);
    return len > 1e-4 ? v / len : 0.0;
}

float3 gather(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float3 screen = tex2D(ReShade::BackBuffer, uv).rgb;
    const float j = ACVDLSS::noise(position.xy, FrameCount);

    // Jitter the tile lookup so the grid doesn't show (Guertin).
    const float2 shift = (float2(j, ACVDLSS::noise(position.yx + 17.0, FrameCount)) - 0.5) * (ACVDLSS_TILE * 0.5);
    const int2 tile = clamp(int2((position.xy + shift) / ACVDLSS_TILE), int2(0, 0),
                            int2(ACVDLSS_TILES_X - 1, ACVDLSS_TILES_Y - 1));
    const float2 vn = tex2Dfetch(NeighborMax, tile).xy;
    if (dot(vn, vn) < 0.25)
        return screen;

    const float2 vc = velocity(uv);
    const float speed_c = length(vc);
    const float zc = ACVDLSS::view_distance(uv);

    // Own direction if moving, else across the dominant motion (Guertin).
    const float2 wn = unit(vn);
    float2 wp = float2(-wn.y, wn.x);
    if (dot(wp, vc) < 0.0)
        wp = -wp;
    const float2 wc = unit(lerp(wp, unit(vc), saturate((speed_c - 0.5) / 1.5)));

    // Faster pixels weigh themselves less.
    float weight = float(Samples) / (40.0 * max(speed_c, 0.5));
    float3 sum = ACVDLSS::scene(uv) * weight;

    [loop]
    for (int i = 0; i < Samples; ++i)
    {
        // Odd samples follow this pixel's own motion to keep overlaps apart.
        const float2 d = (i % 2 == 1 && speed_c > 0.5) ? vc : vn;
        const float t = (float(i) + 0.5 + (j - 0.5) * 0.95) / float(Samples) * 2.0 - 1.0;
        const float2 offset = d * t;
        const float2 suv = uv + offset * BUFFER_PIXEL_SIZE;
        const float dist = length(offset);

        const float zs = ACVDLSS::view_distance(suv);
        const float2 vs = velocity(suv);
        const float speed_s = length(vs);

        const float2 dir = unit(d);
        const float wa = abs(dot(wc, dir));
        const float wb = abs(dot(unit(vs), dir));

        // Front samples blur over this pixel. Ones behind show through its blur.
        const float w = in_front(zs, zc) * cone(dist, max(speed_s, 0.5)) * wb +
                        in_front(zc, zs) * cone(dist, max(speed_c, 0.5)) * wa +
                        cylinder(dist, max(min(speed_s, speed_c), 0.5)) * max(wa, wb) * 2.0;
        weight += w;
        sum += ACVDLSS::scene(suv) * w;
    }
    return ACVDLSS::with_hud(sum / weight, uv, screen);
}

technique ACVDLSS_MotionBlurHQ <
    ui_label = "ACV DLSS Motion Blur (depth-aware)";
    ui_tooltip = "Motion blur from the game's own motion vectors that also spreads past the edges of moving\n"
                 "objects. Depth keeps a blur from smearing over things in front of it.";
>
{
    pass { VertexShader = PostProcessVS; PixelShader = tile_max; RenderTarget = ACVDLSS_HQ_TileMaxTex; }
    pass { VertexShader = PostProcessVS; PixelShader = neighbor_max; RenderTarget = ACVDLSS_HQ_NeighborMaxTex; }
    pass { VertexShader = PostProcessVS; PixelShader = gather; }
}
