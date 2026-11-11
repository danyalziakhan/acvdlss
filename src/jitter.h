#pragma once

#include <cstdint>

// The game jitters through 8 positions, but DLSS wants at least
// 8 x (output / render)^2. The game also has 16 and 32 position sequences
// behind the same switch, and jitter.cpp redirects the game's case to one.

// Scans for the game's code on a separate thread so loading isn't held up.
void jitter_start();
// Once a frame while DLSS runs. Picks the fewest of 8, 16 or 32 positions
// that meets NVIDIA's minimum (8 at DLAA). Cheap when nothing changes.
void jitter_apply(uint32_t render_width, uint32_t output_width);
// Restores the game's 8 positions (DLSS off, or the add-on unloading).
void jitter_restore();
// 8, 16 or 32, or 0 if the game's code wasn't found.
uint32_t jitter_positions();

