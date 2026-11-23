// The exact vignette's table check, after the detect pass. One thread. A table pointer noted at an earlier
// frame's tonemapping draw whose contents moved more than TABLE_GARBAGE_LIMIT from the last accepted table is
// rejected: no build this frame, the copy stays, and the screen pass uses the fitted model (vignette_inverse_state.hlsl).

#include "vignette_inverse_state.hlsl"

[numthreads(1, 1, 1)]
void main()
{
    if (build.x == 0 && !table_this_frame() && state[STATE_MAX_DELTA] > TABLE_GARBAGE_LIMIT)
    {
        state[STATE_TABLE_REJECTED] = 1;
        state[STATE_TABLES_REJECTED] = state[STATE_TABLES_REJECTED] + 1;
        state[STATE_FLAG] = 0;
    }
}
