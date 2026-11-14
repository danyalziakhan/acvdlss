// Negative mip LOD bias for material textures. Limited to what the game streams. The engine loads mips and
// bakes ground texture pages for the resolution scale's size and not FSR's render size. Reading past the
// loaded mips turns ground pages black up close. So the bias is log2(FSR render size / scale size). NVIDIA's
// suggested extra -1 is left out. It read past the loaded mips at any resolution.
//
// The samplers are static samplers in root signatures. The bias goes in at creation. ReShade raises
// create_sampler for those too. A new scale needs a restart. The game's pipeline cache doesn't notice a
// changed static sampler. Stale pipelines once left interiors without indirect light. So the cache is
// cleared when the bias changes.
//
// A rebuild cut short is just as broken: blinking, blotchy shadows and banded sky. The note says "building"
// until REBUILD_TIME of gameplay. A start that still finds "building" clears again. The game exits without
// unloading the add-on. So the note is finished while the game runs.
//
// Later damage can't be detected. The game writes most of its cache after our last call. In one log ReShade
// was gone at .766 and the cache was written .870 to .952. So a cache without our note is never trusted.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include "acvdlss.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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
// Bump when wants_bias() or the formula changes. 3 forces one rebuild in 0.4 to replace damaged caches.
const int RULE_VERSION = 3;

const auto REBUILD_TIME = std::chrono::minutes(3);
// Longer gaps are pauses, loading stalls or the game in the background.
const auto MAX_FRAME_TIME = std::chrono::milliseconds(250);

float g_bias = 0.0f;
std::atomic<uint32_t> g_biased{0};

std::filesystem::path g_note; // inside the cache. Goes when the cache does
std::string g_finished;       // what the note says once the rebuild is done
bool g_building = false;      // this session cleared the cache
std::chrono::steady_clock::duration g_gameplay{};
std::chrono::steady_clock::time_point g_last_frame;

std::filesystem::path game_documents()
{
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        return {};
    std::filesystem::path path = std::filesystem::path(documents) / L"Assassin's Creed Valhalla";
    CoTaskMemFree(documents);
    return path;
}

// Measured: FSRQuality 1 renders at half size. 4 renders 744 of 960.
// 2 and 3 are assumed to follow FSR 1's ratios.
float saved_fsr_ratio(const std::filesystem::path &folder)
{
    const std::wstring ini = (folder / L"ACValhalla.ini").wstring();
    if (GetPrivateProfileIntW(L"Graphics", L"UpsampleType", 0, ini.c_str()) != 1)
        return 1.0f;
    static const float FSR_RATIO[] = {1.0f, 1.0f / 2.0f, 1.0f / 1.7f, 1.0f / 1.5f, 1.0f / 1.3f};
    const UINT quality = GetPrivateProfileIntW(L"Graphics", L"FSRQuality", 0, ini.c_str());
    return quality < std::size(FSR_RATIO) ? FSR_RATIO[quality] : 1.0f;
}

std::filesystem::path g_folder;

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return text;
}

// Temp file and rename. Exiting mid-write can't leave half a note.
bool write_note(const std::string &line)
{
    std::filesystem::path temp = g_note;
    temp += L".new";
    const HANDLE file =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const std::string text = line + "\n";
    DWORD written = 0;
    const bool wrote = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) &&
                       written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (wrote && MoveFileExW(temp.c_str(), g_note.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(temp.c_str());
    return false;
}

// Runs at load, before the game can build pipelines.
void match_pipeline_cache(const std::filesystem::path &folder)
{
    const std::filesystem::path cache = folder / L"cache";
    g_note = cache / L"acvdlss.bias";
    char finished[48];
    snprintf(finished, sizeof(finished), "rule %d, bias %.2f", RULE_VERSION, g_bias);
    g_finished = finished;

    const std::string note = read_file(g_note);
    if (note == g_finished)
    {
        acv_log(reshade::log::level::info, "kept the pipeline cache. It matches this LOD bias");
        return;
    }

    char rule[16];
    snprintf(rule, sizeof(rule), "rule %d,", RULE_VERSION);
    const char *why = "the LOD bias changed";
    if (note.empty())
        why = "it has no note from this add-on";
    else if (note == "building, " + g_finished)
        why = "its last rebuild did not finish (that takes 3 minutes of play)";
    else if (note.find(rule) == std::string::npos)
        why = "this version of the add-on rebuilds it once";
    std::error_code error;
    if (std::filesystem::exists(cache, error))
    {
        std::filesystem::remove_all(cache, error);
        if (error)
        {
            acv_log(reshade::log::level::warning,
                    "could not clear the game's pipeline cache. The LOD bias stays off this session");
            g_bias = 0.0f;
            return;
        }
        const std::string text =
            std::string("cleared the game's pipeline cache because ") + why + ". This start will be slow.";
        acv_log(reshade::log::level::info, text.c_str());
    }
    std::filesystem::create_directories(cache, error);
    g_building = write_note("building, " + g_finished);
    if (!g_building)
    {
        acv_log(reshade::log::level::warning,
                "could not write to the game's pipeline cache. The LOD bias stays off this session");
        g_bias = 0.0f;
    }
}

// Material textures wrap. Screen-space passes clamp and must not be biased.
// The exposure pass reads a small mip of the frame.
bool wants_bias(const sampler_desc &desc)
{
    const uint32_t filter = uint32_t(desc.filter);
    const bool comparison = (filter & 0x80) != 0;
    const bool mip_linear = (filter & 0x1) != 0;
    const bool tiles = desc.address_u == texture_address_mode::wrap || desc.address_v == texture_address_mode::wrap;
    return tiles && !comparison && mip_linear && desc.max_lod > 0.0f && desc.mip_lod_bias > g_bias;
}

// Debug only.
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
    // Applied even with DLSS off. Toggling it shouldn't cost a cache rebuild.
    g_folder = folder;
    const float ratio = saved_fsr_ratio(folder);
    g_bias = lod_bias_for(ratio);
    char text[96];
    if (ratio < 1.0f)
        snprintf(text, sizeof(text), "LOD bias %+.2f for FSR rendering at %.0f%%", g_bias, ratio * 100.0f);
    else
        snprintf(text, sizeof(text), "LOD bias 0 with FSR off");
    acv_log(reshade::log::level::info, text);
    match_pipeline_cache(folder);
    reshade::register_event<reshade::addon_event::create_sampler>(on_create_sampler);
}

// Once a frame, on the present thread.
void lod_frame(bool gameplay)
{
    if (!g_building)
        return;
    const auto now = std::chrono::steady_clock::now();
    const auto step = now - g_last_frame;
    g_last_frame = now;
    if (!gameplay || step > MAX_FRAME_TIME)
        return;
    g_gameplay += step;
    if (g_gameplay < REBUILD_TIME)
        return;
    g_building = false;
    if (write_note(g_finished))
        acv_log(reshade::log::level::info, "pipeline cache rebuilt");
    else
        acv_log(reshade::log::level::warning,
                "could not mark the pipeline cache as rebuilt. The next start clears it again");
}

float lod_bias_for(float fsr_ratio)
{
    return std::log2(std::min(fsr_ratio, 1.0f));
}

float lod_bias_saved()
{
    return g_folder.empty() ? g_bias : lod_bias_for(saved_fsr_ratio(g_folder));
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
