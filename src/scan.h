#pragma once

#include <cstddef>
#include <cstdint>

// Finds a byte pattern in the game's executable code. -1 matches any byte.
// Returns the match only when there is exactly one. count gets the number found.
uint8_t *scan_game_code(const int16_t *pattern, size_t size, int &count);
