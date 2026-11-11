#pragma once

#include <cstdint>

// NVIDIA Reflex through NvAPI, called the way a game would: the per-frame wait
// after Present plus latency markers. Tools that wrap Reflex (frame generation,
// frame limiters) see these as the game's own calls.

enum class reflex_mode : uint32_t
{
    off = 0,
    on = 1,
    boost = 2, // on, and the GPU keeps its clocks up while the CPU is the limit
};

void reflex_register();
void reflex_unregister();

// Reason Reflex can't run, for the overlay, or nullptr.
const char *reflex_unavailable();
