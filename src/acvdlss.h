#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace reshade::log
{
    enum class level;
}

struct ID3D12Device;

namespace reshade::api
{
    struct command_queue;
}

// Logs to acvdlss.log next to the game's exe; warnings and errors also to
// ReShade's log, where they get an "ACV DLSS: " in front.
void acv_log(reshade::log::level level, const char *message);
// The folder of the game's exe.
std::wstring game_folder();
// "310.9.16" from a file's version resource, or empty.
std::string file_version(const std::wstring &path);
// The first lines of the log: versions and the game's graphics settings. Also
// starts writing access violations to the log.
void log_start();
// At device creation: the GPU and driver, and the modules a report needs.
void log_adapter(ID3D12Device *device);
void log_modules();

struct settings
{
    std::atomic<bool> enabled{true};
    std::atomic<uint32_t> preset{0}; // NVIDIA's value: 0 lets DLSS choose, 1..13 are A..M
    std::atomic<float> sharpness{0.0f}; // 0..1, 0 skips RCAS
    std::atomic<uint32_t> reflex{0};    // reflex_mode
    // Only in acvdlss.ini: 0 picks from the upscaling ratio, else 8, 16 or 32.
    std::atomic<uint32_t> jitter_positions{0};
    // Only in ReShade.ini: detailed logging, every frame if need be.
    std::atomic<bool> debug{false};
    // Only in ReShade.ini: give frame generation tools the image without the
    // HUD. Turn off if an effect drawn after the TAA looks wrong in generated
    // frames.
    std::atomic<bool> hudless{true};
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
// Sets a texture LOD bias on the game's samplers as they are created. Runs at
// add-on load, which ReShade does inside D3D12CreateDevice, before the game
// can build pipelines. May clear the pipeline cache.
void lod_register();
// Once a frame. Marks a pipeline cache cleared this session as rebuilt after
// enough frames with DLSS replacing the TAA, or enough steady frames while
// DLSS is off or can't run.
void lod_frame(bool gameplay);
// Bias for FSR rendering below the resolution scale's size. fsr_ratio is 1
// with FSR off.
float lod_bias_for(float fsr_ratio);
// The bias the game's current settings file asks for, applied on restart.
float lod_bias_saved();
float lod_bias();             // 0 when none was applied
uint32_t lod_biased_samplers();
// With Debug on: the game's own biases on its tiling texture samplers.
std::string lod_game_biases();
