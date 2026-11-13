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

// Warnings and errors also go to ReShade's log.
void acv_log(reshade::log::level level, const char *message);
std::wstring game_folder();
// "310.9.16", or empty.
std::string file_version(const std::wstring &path);
// Also starts logging access violations.
void log_start();
void log_adapter(ID3D12Device *device);
void log_modules();

struct settings
{
    std::atomic<bool> enabled{true};
    std::atomic<uint32_t> preset{0}; // 0 lets DLSS choose, 1..13 are A..M
    std::atomic<float> sharpness{0.0f}; // 0..1, 0 skips RCAS
    std::atomic<uint32_t> reflex{1};    // reflex_mode
    // The rest are only in acvdlss.ini.
    // 0 picks from the upscaling ratio.
    std::atomic<uint32_t> jitter_positions{0};
    std::atomic<bool> debug{false};
    // HUD-less image for frame generation tools.
    std::atomic<bool> hudless{true};
};
extern settings g_settings;

enum class taa_state
{
    no_device,   // no D3D12 device yet
    unavailable, // see message
    off,
    waiting,     // no TAA pass seen recently
    running,
    failed,      // for this session, see message
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
void taa_present(reshade::api::command_queue *queue, uint32_t display_width, uint32_t display_height);
// Drops DLSS history. The game's own TAA ran in between.
void taa_restart();
taa_status taa_get_status();
// ReShade loads add-ons inside D3D12CreateDevice, before the game can build
// pipelines. May clear the pipeline cache.
void lod_register();
void lod_frame(bool gameplay);
// fsr_ratio is 1 with FSR off.
float lod_bias_for(float fsr_ratio);
// What the next start will apply.
float lod_bias_saved();
float lod_bias();
uint32_t lod_biased_samplers();
// Debug only.
std::string lod_game_biases();
