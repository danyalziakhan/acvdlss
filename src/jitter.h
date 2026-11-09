#pragma once

#include <cstdint>

// The game jitters the camera through 8 positions, while DLSS asks for at
// least 8 x (output / render)^2 of them. The game already has 16 and 32
// position sequences behind the same switch; jitter.cpp points the case the
// game uses at one of those.

// Starts looking for the game's code on a thread, so loading is not held up.
void jitter_start();
// Once a frame while DLSS runs: picks the fewest of 8, 16 and 32 positions that
// meet NVIDIA's 8 x (output / render)^2, so 8 at DLAA. Cheap when nothing
// changes.
void jitter_apply(uint32_t render_width, uint32_t output_width);
// Gives the game its own 8 back (DLSS off, or the add-on unloading).
void jitter_restore();
// How many positions the game cycles through now: 8, 16 or 32, or 0 when its
// code was not found.
uint32_t jitter_positions();

