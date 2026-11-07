#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace reshade::api
{
    struct command_queue;
}

struct settings
{
    std::atomic<bool> enabled{true};
    std::atomic<uint32_t> preset{0}; // NVIDIA's value: 0 lets DLSS choose, 1..13 are A..M
    std::atomic<float> sharpness{0.0f}; // 0..1, 0 skips RCAS
    // Only in ReShade.ini: logs how the frame leaves the TAA pass.
    std::atomic<bool> debug{false};
};
extern settings g_settings;

enum class taa_state
{
    no_device,   // the game has not made a D3D12 device yet, or it is not D3D12
    unavailable, // DLSS or our own passes could not start; see message
    off,
    waiting,     // no TAA pass seen recently
    running,
    failed,      // DLSS stopped working this session; see message
};

struct taa_status
{
    taa_state state = taa_state::no_device;
    uint32_t width = 0, height = 0;               // what the game renders
    uint32_t output_width = 0, output_height = 0; // what DLSS makes of it
    uint32_t display_width = 0;
    bool hdr = false;
    bool camera_motion_only = false;
    std::string message;
};

void taa_register();
void taa_unregister();
// Once a frame, from present, with the size of the back buffer.
void taa_present(reshade::api::command_queue *queue, uint32_t display_width, uint32_t display_height);
// Throws DLSS's history away, as after it was switched back on: the game's
// own TAA ran in between.
void taa_restart();
taa_status taa_get_status();

// Texture LOD bias, set on the game's samplers as they are created.
// Called as the add-on loads, which ReShade does inside D3D12CreateDevice,
// before the game has a device to build pipelines with. It may clear the
// pipeline cache.
void lod_register();
float lod_bias_for(float resolution_scale);
float lod_bias();             // 0 when none was applied
uint32_t lod_biased_samplers();
// With Debug on: the game's own biases on its tiling texture samplers.
std::string lod_game_biases();
