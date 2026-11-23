// The exact vignette's change detector and warm start, shared by the inverse build passes.
// state (u2, R32_UINT, 32 x 32 x 193): slices 0..95 a bit copy of the table the inverse was built from (texel
// (x, y, z) channel c at (x, y, 3z + c)). Slices 97..192 the same for the table of the last full build (the anchor
// of the warm start). Slice 96, x =
//   0 rebuild flag of this frame, 1 rebuilds so far, 2 builds asked for so far,
//   3 texels that changed this frame, 4 largest change this frame in 10-bit steps (any channel),
//   5 nodes of the warm start that lost the solution in their own cell (searched the 27 around) this frame,
//   6 full rebuilds so far, 7 nodes the warm start handed to the full scatter this frame,
//   8 largest change of the table since the last full build (10-bit steps).
// build.w bit 1: the table was noted at this frame's tonemapping draw (else at an earlier frame's).
// build.x: 1 forces a full rebuild (every-frame mode, new textures, a changed build mode). build.y: 1 when the state
// texture exists; 0 rebuilds every frame (the detector can't run). build.z: 1 warm start, 0 off. build.w: bit 0
// keeps the best of the 27 cells even when it is not exact (the warm start never runs the scatter). Bits 8..: the
// largest change since the last full build that still allows a warm start (more: full build, new anchor).

cbuffer build_constants : register(b0)
{
    uint4 build;
};
RWTexture3D<uint> state : register(u2);

static const uint3 STATE_FLAG = uint3(0, 0, 96);
static const uint3 STATE_REBUILDS = uint3(1, 0, 96);
static const uint3 STATE_BUILDS = uint3(2, 0, 96);
static const uint3 STATE_CHANGED = uint3(3, 0, 96);
static const uint3 STATE_MAX_DELTA = uint3(4, 0, 96);
static const uint3 STATE_FULL_NODES = uint3(5, 0, 96);
static const uint3 STATE_FULL_BUILDS = uint3(6, 0, 96);
static const uint3 STATE_SCATTER_NODES = uint3(7, 0, 96);
static const uint3 STATE_ANCHOR_DELTA = uint3(8, 0, 96);
// The screen pass's guard counts its rejected pixels in 9. The next frame's begin pass moves the count to 10.
static const uint3 STATE_REJECTS = uint3(9, 0, 96);
static const uint3 STATE_REJECTS_LAST = uint3(10, 0, 96);
// Table check: 11 this frame's table was rejected (1), 12 rejected tables so far, 13 scatter cells over the cap.
static const uint3 STATE_TABLE_REJECTED = uint3(11, 0, 96);
static const uint3 STATE_TABLES_REJECTED = uint3(12, 0, 96);
static const uint3 STATE_WILD = uint3(13, 0, 96);
static const uint STATE_ANCHOR_SLICE = 97;
// A table noted at an earlier frame's tonemapping draw that moved more than this (10-bit steps) from the last accepted
// one is the stale pointer of a table the game already replaced: its memory holds something else (measured: up to
// 1023 steps on 36-75% of texels in the frame before each swap. Real frames move by at most 1 step).
static const uint TABLE_GARBAGE_LIMIT = 32;
// Scatter cells whose output box spans more inverse nodes than this are skipped (a normal table: at most 966).
static const uint SCATTER_CELL_CAP = 2048;

// Change between two stored channel values in 10-bit steps; NaN or garbage (a first copy) counts as the largest.
uint steps_between(float now, uint old_bits)
{
    const float d = abs(round(saturate(now) * 1023.0) - round(saturate(asfloat(old_bits)) * 1023.0));
    return d == d ? uint(min(d, 1023.0)) : 1023u;
}
// Key of a node the warm start settled: the scatter skips it, the resolve keeps it.
static const uint KEY_WARM = 0xfffffffe;

// True when the table is the one the inverse already holds.
bool skip_build()
{
    return build.y != 0 && state[STATE_FLAG] == 0;
}

// This build starts from last frame's inverse: warm start on, not forced, and the table is still within the limit
// of the one the last full build used (valid after the detect pass).
bool warm_build()
{
    return build.z != 0 && build.x == 0 && state[STATE_ANCHOR_DELTA] <= (build.w >> 8);
}

// build.w bit 1: the table pointer was noted at this frame's tonemapping draw.
bool table_this_frame()
{
    return (build.w & 2u) != 0;
}

// Nothing for the scatter: no change, or the warm start settled every node.
bool skip_scatter()
{
    return skip_build() || (warm_build() && state[STATE_SCATTER_NODES] == 0);
}
