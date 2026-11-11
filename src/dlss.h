#pragma once

#include <d3d12.h>
#include <cstdint>
#include <string>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

struct dlss_frame
{
    ID3D12Resource *color = nullptr;
    ID3D12Resource *motion = nullptr;
    ID3D12Resource *depth = nullptr;
    ID3D12Resource *output = nullptr;
    uint32_t width = 0, height = 0;               // what the game renders
    uint32_t output_width = 0, output_height = 0; // what DLSS makes of it
    float jitter_x = 0.0f, jitter_y = 0.0f;
    float frame_ms = 0.0f;
    // The game's camera, only passed along for frame generation tools.
    float camera_near = 0.1f, camera_far = 10000.0f, camera_fov = 1.0f; // fov: vertical, radians
    // The final image before the HUD is drawn on it, and its resource state at
    // Present. Also only for frame generation tools.
    ID3D12Resource *hudless = nullptr;
    uint32_t hudless_state = 0;
    bool hdr = false;
    bool reset = false;
    uint32_t preset = 0;
};

// One DLSS feature on one device (DLAA when the sizes match). Creation and
// evaluation both record into the game's command list.
class dlss
{
public:
    // With log on, NGX's own messages go to our log too.
    bool init(ID3D12Device *device, bool log);
    void shutdown();

    // Rebuilds the feature first if a size, HDR or the preset changed.
    bool evaluate(ID3D12GraphicsCommandList *list, const dlss_frame &frame);

    // True if evaluate() would release the current feature. The GPU may still
    // be using it, so the caller waits for idle first.
    bool will_rebuild(const dlss_frame &frame) const
    {
        return _feature != nullptr &&
               (frame.width != _width || frame.height != _height || frame.output_width != _output_width ||
                frame.output_height != _output_height || frame.hdr != _hdr || frame.preset != _preset);
    }

    bool available() const { return _params != nullptr; }
    const std::string &error() const { return _error; }

private:
    bool create(ID3D12GraphicsCommandList *list, const dlss_frame &frame);
    void release_feature();

    ID3D12Device *_device = nullptr;
    NVSDK_NGX_Parameter *_params = nullptr;
    NVSDK_NGX_Handle *_feature = nullptr;
    uint32_t _width = 0, _height = 0, _output_width = 0, _output_height = 0, _preset = 0;
    bool _hdr = false;
    std::string _error;
};
