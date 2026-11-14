// Converts the TAA inputs for DLSS.
// Color is copied because the game leaves its textures in a pixel-shader-only state.

cbuffer constants : register(b0)
{
    float4 size;           // width, height, 1 / width, 1 / height
    float4 depth_params;   // sky threshold in view depth, P22, P23, device depth of the sky
    float4 inv_proj[4];
    float4 cam_to_world[3];
    float4 prev_view_proj[3]; // rows x, y and w
    float4 flags;          // x: 1 when the game wrote motion vectors
};

Texture2D<float4> game_color : register(t0);
Texture2D<float4> game_motion : register(t1);
Texture2D<float> game_depth : register(t2);

RWTexture2D<float4> out_color : register(u0);
RWTexture2D<float2> out_motion : register(u1);
RWTexture2D<float> out_depth : register(u2);

// 12 bits per component on a quartic curve to keep precision for small motion.
// x is red plus the high nibble of green. y is the low nibble plus blue.
float unpack_motion(uint q)
{
    float v = clamp(q * 0.000489 - 1.0, -1.0, 1.0);
    float v2 = v * v;
    return sign(v) * 0.2 * v2 * v2;
}

// Camera motion only. The game's inverse projection is only valid at the far plane.
// Unproject there and scale along the ray to the pixel's depth.
float2 reproject(float2 uv, float view_depth, bool sky)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth_params.w, 1.0);
    float4 v = float4(dot(clip, inv_proj[0]), dot(clip, inv_proj[1]), dot(clip, inv_proj[2]), dot(clip, inv_proj[3]));
    float3 view = v.xyz / v.w;
    if (!sky)
        view *= view_depth / max(abs(view.z), 1e-6);

    float4 pos = float4(view, 1.0);
    float4 world = float4(dot(pos, cam_to_world[0]), dot(pos, cam_to_world[1]), dot(pos, cam_to_world[2]), 1.0);
    float2 prev = float2(dot(world, prev_view_proj[0]), dot(world, prev_view_proj[1])) / dot(world, prev_view_proj[2]);
    return prev * float2(0.5, -0.5) + 0.5;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(size.xy)))
        return;

    int2 p = int2(id.xy);
    float d = game_depth[p];
    out_color[p] = game_color[p];
    out_depth[p] = saturate(depth_params.z / max(d, 1e-6) - depth_params.y);

    float2 uv = (float2(p) + 0.5) * size.zw;
    bool sky = d >= depth_params.x;

    // Dialogue cutscenes bind a 4x4 placeholder instead of motion vectors.
    // No jitter term: +1, 0, -1 and +2 times the jitter delta were tried and only 0 kept the clouds still.
    float2 motion;
    if (sky || flags.x == 0.0)
    {
        motion = reproject(uv, d, sky) - uv;
    }
    else
    {
        uint3 m = uint3(round(game_motion[p].xyz * 255.0));
        motion = -float2(unpack_motion(m.x * 16 + (m.y >> 4)), unpack_motion((m.y & 15) * 256 + m.z));
    }
    out_motion[p] = motion;
}
