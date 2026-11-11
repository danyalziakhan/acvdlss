#pragma once

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

// Root constants of prepare.hlsl, in the order its cbuffer declares them.
struct prepare_constants
{
    float size[4];
    float depth_params[4];
    float inv_proj[16];
    float cam_to_world[12];
    float prev_view_proj[12];
    float flags[4]; // x: 1 when the game wrote motion vectors
};
static_assert(sizeof(prepare_constants) == 52 * 4, "prepare.hlsl expects 52 root constants");

struct game_texture
{
    ID3D12Resource *resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // the format the game reads it with
};

// The compute passes around DLSS and the textures they share. Everything is
// recorded with plain D3D12 into the game's command list.
class passes
{
public:
    bool init(ID3D12Device *device);
    void shutdown();

    // Recreates the work textures when a size or the color format changes. Old
    // ones are kept until `frame` is far enough behind.
    bool resize(uint32_t width, uint32_t height, uint32_t output_width, uint32_t output_height,
                DXGI_FORMAT color_format, uint64_t frame);
    void collect(uint64_t frame);

    // Fills color, motion and depth and leaves them readable by DLSS.
    void prepare(ID3D12GraphicsCommandList *list, const game_texture &color, const game_texture &motion,
                 const game_texture &depth, const prepare_constants &constants);

    // Optionally sharpens, then copies the result into the game's TAA target.
    // When upscaling, the full-size result goes to presented() for the pass
    // that scales to the screen, and a downsampled copy to the TAA target for
    // everything else. Leaves the work textures in the state prepare() expects.
    void finish(ID3D12GraphicsCommandList *list, float sharpness, bool hdr, ID3D12Resource *taa_target);
    // Returns the inputs to the UAV state prepare() expects. Called by finish(),
    // and on its own when DLSS didn't run.
    void abandon(ID3D12GraphicsCommandList *list);

    // Resamples presented() into a game texture (in UAV state) at its size and
    // format. This is how the frame reaches the screen when the game's FSR 1
    // upscales.
    void deliver(ID3D12GraphicsCommandList *list, ID3D12Resource *target, DXGI_FORMAT format, uint32_t width,
                 uint32_t height);

    bool upscaling() const { return _output_width != _width || _output_height != _height; }

    ID3D12Resource *color() const { return _color; }
    ID3D12Resource *motion() const { return _motion; }
    ID3D12Resource *depth() const { return _depth; }
    ID3D12Resource *output() const { return _output; }
    // The upscaled frame. Readable by any shader between frames.
    ID3D12Resource *presented() const { return _presented; }
    // The image finish() produced for this sharpness, and its state after the
    // frame's passes.
    ID3D12Resource *screen_image(float sharpness, uint32_t &state) const;
    DXGI_FORMAT color_format() const { return _color_format; }
    const std::string &error() const { return _error; }

private:
    ID3D12Resource *make_texture(DXGI_FORMAT format, uint32_t width, uint32_t height, D3D12_RESOURCE_STATES state);
    void release_textures(uint64_t frame);
    D3D12_GPU_DESCRIPTOR_HANDLE next_slot(D3D12_CPU_DESCRIPTOR_HANDLE &cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE offset(D3D12_CPU_DESCRIPTOR_HANDLE base, uint32_t index) const;
    void write_srv(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *resource, DXGI_FORMAT format);
    void write_uav(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *resource, DXGI_FORMAT format);
    void dispatch(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline, const void *constants,
                  uint32_t constant_count, ID3D12Resource *source, ID3D12Resource *target, DXGI_FORMAT target_format,
                  uint32_t width, uint32_t height);

    ID3D12Device *_device = nullptr;
    ID3D12RootSignature *_root = nullptr;
    ID3D12PipelineState *_prepare = nullptr;
    ID3D12PipelineState *_rcas = nullptr;
    ID3D12PipelineState *_downsample = nullptr;
    ID3D12DescriptorHeap *_heap = nullptr;
    uint32_t _increment = 0;
    uint32_t _next_slot = 0;

    // Render size.
    ID3D12Resource *_color = nullptr, *_motion = nullptr, *_depth = nullptr, *_downsampled = nullptr;
    // Output size.
    ID3D12Resource *_output = nullptr, *_sharpened = nullptr, *_presented = nullptr;
    uint32_t _width = 0, _height = 0, _output_width = 0, _output_height = 0;
    DXGI_FORMAT _color_format = DXGI_FORMAT_UNKNOWN;

    struct retired
    {
        ID3D12Resource *resource;
        uint64_t frame;
    };
    std::vector<retired> _retired;
    std::string _error;
};
