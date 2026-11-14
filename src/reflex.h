#pragma once

#include <cstdint>

// NVIDIA Reflex through NvAPI, called the way a game would. Wrapping tools take them for the game's own.

enum class reflex_mode : uint32_t
{
    off = 0,
    on = 1,
    boost = 2, // on plus GPU clocks kept up while the CPU is the limit
};

// Where the wait and the frame start marker go. acvdlss.ini only.
enum class reflex_placement : uint32_t
{
    after_present = 0,     // right after Present as in 0.5
    frame_start = 1,       // in the game's BeginFrame before it reads input
    frame_start_paced = 2, // and the driver paces frames by the markers
};

void reflex_register();
void reflex_unregister();

// From the game's BeginFrame (frame_start.cpp), on the game's thread.
void reflex_frame_start();

// For the overlay. nullptr when Reflex can run.
const char *reflex_unavailable();
