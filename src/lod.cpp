// Negative mip LOD bias for the game's material textures, so they keep the
// detail of the output resolution rather than the render resolution. The value
// is NVIDIA's: log2(render width / output width) - 1.
//
// Valhalla's texture samplers are static samplers inside its D3D12 root
// signatures, so there is nothing to swap at bind time. The bias goes in as the
// game creates them, since ReShade raises create_sampler for static samplers
// too. That has two consequences. The bias follows the resolution scale the game saved, so a new
// scale takes a restart. And the game's pipeline cache does not notice a
// changed static sampler: it keeps handing out pipelines built with the old
// one, which once left interiors without their indirect light. So whenever the
// bias changes, the cache is cleared before the game opens it, and the game
// spends one slow start rebuilding it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include "acvdlss.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <system_error>

using namespace reshade::api;

namespace
{
// Goes up whenever wants_bias() picks different samplers, so the cache is
// rebuilt for a new rule just as for a new value.
const int RULE_VERSION = 1;

float g_bias = 0.0f;
std::atomic<uint32_t> g_biased{0};

std::filesystem::path game_documents()
{
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        return {};
    std::filesystem::path path = std::filesystem::path(documents) / L"Assassin's Creed Valhalla";
    CoTaskMemFree(documents);
    return path;
}

// The fraction of the screen width the game renders at, from its own
// settings, as it will start with.
float saved_render_scale(const std::filesystem::path &folder)
{
    wchar_t value[32] = L"";
    GetPrivateProfileStringW(L"Graphics", L"PixelDensity", L"1", value, DWORD(std::size(value)),
                             (folder / L"ACValhalla.ini").c_str());
    float scale = std::wcstof(value, nullptr);
    if (scale < 0.25f || scale > 4.0f)
        scale = 1.0f;

    // With FSR on, FSR 1 renders smaller again inside that size, by its own
    // ratios. 4 was seen rendering 744 of 960, Ultra Quality; the rest follow
    // FSR 1's order down to Performance.
    const std::wstring ini = (folder / L"ACValhalla.ini").wstring();
    if (GetPrivateProfileIntW(L"Graphics", L"UpsampleType", 0, ini.c_str()) == 1)
    {
        static const float FSR_RATIO[] = {1.0f, 1.0f / 2.0f, 1.0f / 1.7f, 1.0f / 1.5f, 1.0f / 1.3f};
        const UINT quality = GetPrivateProfileIntW(L"Graphics", L"FSRQuality", 0, ini.c_str());
        if (quality < std::size(FSR_RATIO))
            scale *= FSR_RATIO[quality];
    }
    return scale;
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return text;
}

// Clears the game's pipeline cache when the bias it was built with differs
// from the one about to be applied. The bias it was built with is noted in a
// file inside the cache, so it goes when the cache does. Runs while the
// add-on loads, before the game has a device to build pipelines with.
void match_pipeline_cache(const std::filesystem::path &folder)
{
    const std::filesystem::path cache = folder / L"cache";
    const std::filesystem::path note = cache / L"acvdlss.bias";
    char wanted[48];
    snprintf(wanted, sizeof(wanted), "rule %d, bias %.2f", RULE_VERSION, g_bias);

    std::error_code error;
    if (!std::filesystem::exists(cache, error) || read_file(note) == wanted)
    {
        // A missing cache is built fresh with this bias anyway.
        std::filesystem::create_directories(cache, error);
        std::ofstream(note) << wanted << '\n';
        return;
    }

    std::filesystem::remove_all(cache, error);
    if (error)
    {
        reshade::log::message(reshade::log::level::warning,
                              "ACV DLSS: could not clear the game's pipeline cache; the LOD bias stays off");
        g_bias = 0.0f;
        return;
    }
    std::filesystem::create_directories(cache, error);
    std::ofstream(note) << wanted << '\n';
    reshade::log::message(reshade::log::level::info,
                          "ACV DLSS: cleared the game's pipeline cache for a new LOD bias; this start will be slow");
}

// Material textures only: they tile, so their samplers wrap, and they are
// mipmapped and filtered. Screen-space passes clamp, and biasing them is
// harmful: the exposure pass reads a small mip of the frame to average its
// brightness. Shadow comparisons, and samplers the game already biases
// further, are left alone too.
bool wants_bias(const sampler_desc &desc)
{
    const uint32_t filter = uint32_t(desc.filter);
    const bool comparison = (filter & 0x80) != 0;
    const bool mip_linear = (filter & 0x1) != 0;
    const bool tiles = desc.address_u == texture_address_mode::wrap || desc.address_v == texture_address_mode::wrap;
    return tiles && !comparison && mip_linear && desc.max_lod > 0.0f && desc.mip_lod_bias > g_bias;
}

// With Debug on: the biases the game itself gives its tiling texture samplers,
// before ours, to see what the game does about resolution on its own.
std::mutex g_game_biases_lock;
std::map<float, uint32_t> g_game_biases;

void note_game_bias(const sampler_desc &desc)
{
    const uint32_t filter = uint32_t(desc.filter);
    const bool tiles = desc.address_u == texture_address_mode::wrap || desc.address_v == texture_address_mode::wrap;
    if (!tiles || (filter & 0x80) != 0 || (filter & 0x1) == 0 || desc.max_lod <= 0.0f)
        return;
    std::lock_guard lock(g_game_biases_lock);
    ++g_game_biases[desc.mip_lod_bias];
}

bool on_create_sampler(device *dev, sampler_desc &desc)
{
    if (dev->get_api() != device_api::d3d12)
        return false;
    if (g_settings.debug)
        note_game_bias(desc);
    if (g_bias == 0.0f || !wants_bias(desc))
        return false;
    desc.mip_lod_bias = g_bias;
    ++g_biased;
    return true;
}
} // namespace

void lod_register()
{
    const std::filesystem::path folder = game_documents();
    if (folder.empty())
        return;
    // Applied whether DLSS starts on or off, so switching it does not cost a
    // cache rebuild.
    g_bias = lod_bias_for(saved_render_scale(folder));
    match_pipeline_cache(folder);
    reshade::register_event<reshade::addon_event::create_sampler>(on_create_sampler);
}

// NVIDIA's DLSS programming guide: log2(render width / output width) - 1.
// At 100% and above DLSS outputs at the render size, so the ratio is 1.
float lod_bias_for(float resolution_scale)
{
    return std::log2(std::min(resolution_scale, 1.0f)) - 1.0f;
}

float lod_bias()
{
    return g_bias;
}

uint32_t lod_biased_samplers()
{
    return g_biased;
}

std::string lod_game_biases()
{
    std::lock_guard lock(g_game_biases_lock);
    std::string text;
    for (const auto &[bias, count] : g_game_biases)
    {
        char entry[48];
        snprintf(entry, sizeof(entry), "%s%u at %+.2f", text.empty() ? "" : ", ", count, bias);
        text += entry;
    }
    return text.empty() ? "none seen" : text;
}
