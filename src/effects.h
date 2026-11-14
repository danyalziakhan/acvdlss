#pragma once

// ReShade effects get what DLSS gets from the game:
//
//   texture motion : ACVDLSS_MOTION; // RG16F, render size: UV offset to where the pixel was a frame ago
//   texture depth : ACVDLSS_DEPTH;   // R32F, render size: reversed device depth, 1 near, 0 far
//   texture hudless : ACVDLSS_HUDLESS; // output size: the DLSS image without the HUD
//
// Motion and depth read zeros while DLSS isn't running. The HUD-less texture is 1x1 then.

void effects_register();
void effects_unregister();

// True when an effect reads the HUD-less image and it's worth a copy.
bool effects_hudless_wanted();
