// The exact vignette's change detector, pass 1. One thread: the flag starts at the force bit, the counters
// of this frame at 0.

#include "vignette_inverse_state.hlsl"

[numthreads(1, 1, 1)]
void main()
{
    state[STATE_FLAG] = build.x;
    state[STATE_BUILDS] = state[STATE_BUILDS] + 1;
    state[STATE_CHANGED] = 0;
    state[STATE_MAX_DELTA] = 0;
    state[STATE_FULL_NODES] = 0;
    state[STATE_SCATTER_NODES] = 0;
    state[STATE_ANCHOR_DELTA] = 0;
    state[STATE_REJECTS_LAST] = state[STATE_REJECTS];
    state[STATE_REJECTS] = 0;
    state[STATE_TABLE_REJECTED] = 0;
    state[STATE_WILD] = 0;
}
