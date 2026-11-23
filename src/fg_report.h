#pragma once

// A frame generation report in the log, for support: OptiScaler's FGInput and FGOutput, the loader DLLs in the
// game folder, and every loaded frame generation library with its version and signer (NVIDIA's own DLSS frame
// generation or a replacement). Written once DLSS has run 60 s in a row (or after 5 min) and again 4 min later if anything
// changed. Runs on its own thread. Nothing on the game's frames.
void fg_report_start();
