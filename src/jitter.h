#pragma once

#include <cstdint>

// The game jitters through 8 positions. DLSS wants at least
// 8 x (output / render)^2.

// Scans on a separate thread so loading isn't held up.
void jitter_start();
// Fewest positions that meet NVIDIA's minimum. Cheap when nothing changes.
void jitter_apply(uint32_t render_width, uint32_t output_width);
void jitter_restore();
// 0 if the game's code wasn't found.
uint32_t jitter_positions();

