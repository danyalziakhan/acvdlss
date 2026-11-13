#pragma once

#include <cstdint>

// NVIDIA Reflex through NvAPI, called the way a game would. Tools that wrap
// Reflex see these as the game's own calls.

enum class reflex_mode : uint32_t
{
    off = 0,
    on = 1,
    boost = 2, // on, and the GPU keeps its clocks up while the CPU is the limit
};

void reflex_register();
void reflex_unregister();

// For the overlay. nullptr when Reflex can run.
const char *reflex_unavailable();
