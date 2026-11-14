#pragma once

// Reflex's wait belongs where the game starts a frame and before it reads input. The game has no Reflex.
// frame_start.cpp makes its BeginFrame call reflex_frame_start() first.

// Scans for BeginFrame on a separate thread and hooks it when found.
void frame_start_hook();
// True once BeginFrame calls reflex_frame_start().
bool frame_start_hooked();
