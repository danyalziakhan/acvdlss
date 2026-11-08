// Negative mip LOD bias for the game's material textures, as far as the game
// streams them. The engine picks the texture detail it loads, and the detail
// it bakes into the ground's texture pages, for the resolution scale's size,
// not for the size FSR renders inside it. Asking for finer detail than that
// reads mips that are not loaded, and the ground pages that bake from them
// come out black up close. So the bias is log2(FSR's render size / the
// resolution scale's size): 0 with FSR off, -1 with FSR Performance. That
// gives the textures the full detail of the size DLSS outputs at when the
// resolution scale is 100%. NVIDIA's guide adds another -1 on top, which is
// exactly what read past the loaded mips here, at any resolution.
//
// Valhalla's texture samplers are static samplers inside its D3D12 root
// signatures, so there is nothing to swap at bind time. The bias goes in as the
// game creates them, since ReShade raises create_sampler for static samplers
// too. That has two consequences. The bias follows the resolution scale the
// game saved, so a new scale takes a restart. And the game's pipeline cache
// does not notice a changed static sampler: it keeps handing out pipelines
// built with the old one, which once left interiors without their indirect
// light. So whenever the bias changes, the cache is cleared before the game
// opens it, and the game spends one slow start rebuilding it.
//
// A rebuild cut short leaves a cache that is just as broken: shadows that
// blink and blotch, a banded sky. So the note in the cache says "building"
// until the session that cleared it has run REBUILD_TIME of gameplay with DLSS
// on, and a start that still finds "building" clears it again. The note is
// finished while the game runs: the game ends its process without unloading
// the add-on, so nothing written at exit can be relied on.

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
// Goes up whenever wants_bias() picks different samplers or the bias is worked
// out differently, so the cache is rebuilt for a new rule just as for a new
// value.
const int RULE_VERSION = 2;

// Gameplay long enough for the game to have built and saved its pipelines.
const auto REBUILD_TIME = std::chrono::minutes(3);
// A longer gap between frames is a pause, a loading stall or the game in the
// background, and does not count as gameplay.
const auto MAX_FRAME_TIME = std::chrono::milliseconds(250);

float g_bias = 0.0f;
std::atomic<uint32_t> g_biased{0};

std::filesystem::path g_note; // inside the cache, so it goes when the cache does
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

// How much smaller FSR renders inside the resolution scale's size, from the
// game's settings file: 1 with FSR off. FSRQuality 1 was seen rendering half
// the size, Performance, and 4 rendering 744 of 960, Ultra Quality; 3 and 2
// follow FSR 1's order between them.
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

// Writes the note to a file beside it and moves that over it, so the game
// ending mid-write leaves the old note rather than half a new one.
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

// Clears the game's pipeline cache unless it was fully built with the bias
// about to be applied. Runs while the add-on loads, before the game has a
// device to build pipelines with.
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
        acv_log(reshade::log::level::info, "pipeline cache matches the LOD bias, kept");
        return;
    }
    // Without a bias, a cache this add-on never touched is the game's own and
    // already right.
    if (note.empty() && g_bias == 0.0f)
    {
        std::error_code error;
        std::filesystem::create_directories(cache, error);
        write_note(g_finished);
        return;
    }

    const char *why = "the LOD bias changed";
    if (note.empty())
        why = "it has no note from this add-on";
    else if (note == "building, " + g_finished)
        why = "its last rebuild did not finish (that takes 3 minutes of play with DLSS on)";
    std::error_code error;
    if (std::filesystem::exists(cache, error))
    {
        std::filesystem::remove_all(cache, error);
        if (error)
        {
            acv_log(reshade::log::level::warning,
                    "could not clear the game's pipeline cache, so the LOD bias stays off this session");
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
                "could not write to the game's pipeline cache, so the LOD bias stays off this session");
        g_bias = 0.0f;
    }
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
    g_folder = folder;
    const float ratio = saved_fsr_ratio(folder);
    g_bias = lod_bias_for(ratio);
    char text[96];
    if (ratio < 1.0f)
        snprintf(text, sizeof(text), "LOD bias %+.2f for FSR rendering at %.0f%%", g_bias, ratio * 100.0f);
    else
        snprintf(text, sizeof(text), "LOD bias 0, FSR is off");
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
                "could not mark the pipeline cache as rebuilt, so the next start clears it again");
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
