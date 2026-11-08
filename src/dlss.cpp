#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "acvdlss.h"
#include "dlss.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <reshade.hpp>

#include <cstdio>
#include <mutex>
#include <string>

// Any stable GUID works as a project ID for NGX; this one is ours.
static const char *PROJECT_ID = "5c3f9a1e-7b2d-4e8a-9f61-0d4c2b7e3a95";

static std::mutex g_versions_lock;
static std::string g_versions_warning;

// RenoDX DLSS runs DLSS-NR from nvngx_dlssnr.dll on the same NVIDIA runtime as
// our DLSS. Both from 310.9.16 work together; a user with nvngx_dlss.dll
// 310.9.1 had the game crash as DLSS-NR started. So the versions go in the log,
// and the window says when they differ.
static void check_versions(const std::wstring &folder)
{
    const std::string dlss = file_version(folder + L"\\nvngx_dlss.dll");
    const std::string nr = file_version(folder + L"\\nvngx_dlssnr.dll");
    std::string text = "nvngx_dlss.dll " + (dlss.empty() ? std::string("not found") : dlss);
    if (!nr.empty())
        text += ", nvngx_dlssnr.dll " + nr;
    acv_log(reshade::log::level::info, text.c_str());
    if (dlss.empty() || nr.empty() || dlss == nr)
        return;
    std::lock_guard lock(g_versions_lock);
    g_versions_warning = "nvngx_dlss.dll is " + dlss + " and nvngx_dlssnr.dll is " + nr +
                         ". If DLSS-NR crashes the game, use both from the same DLSS release.";
    acv_log(reshade::log::level::warning, g_versions_warning.c_str());
}

std::string dlss_versions_warning()
{
    std::lock_guard lock(g_versions_lock);
    return g_versions_warning;
}

static std::string describe(const char *what, NVSDK_NGX_Result result)
{
    char text[160];
    snprintf(text, sizeof(text), "%s failed (0x%08X)", what, unsigned(result));
    return text;
}

static void NVSDK_CONV log_ngx(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string line = std::string("NGX: ") + message;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    acv_log(reshade::log::level::info, line.c_str());
}

// NVIDIA's numbers: 0 lets DLSS choose, 1 to 13 are A to M.
static std::string preset_name(uint32_t preset)
{
    if (preset == 0 || preset > 26)
        return "Auto";
    return std::string(1, char('A' + preset - 1));
}

static const char *quality_name(NVSDK_NGX_PerfQuality_Value quality)
{
    switch (quality)
    {
    case NVSDK_NGX_PerfQuality_Value_DLAA: return "DLAA";
    case NVSDK_NGX_PerfQuality_Value_MaxQuality: return "Quality";
    case NVSDK_NGX_PerfQuality_Value_Balanced: return "Balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "Performance";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "Ultra Performance";
    default: return "other";
    }
}

bool dlss::init(ID3D12Device *device, bool log)
{
    _device = device;
    const std::wstring folder = game_folder();
    check_versions(folder);
    // NGX looks next to the exe on its own; naming the folder also covers
    // launchers that start the game from somewhere else.
    const wchar_t *paths[] = {folder.c_str()};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    if (log)
    {
        info.LoggingInfo.LoggingCallback = log_ngx;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    }

    // NGX does not say when the DLL is missing, only that DLSS is, so the file
    // is looked for here to give the likeliest reason first.
    const std::wstring dll = folder + L"\\nvngx_dlss.dll";
    const bool dll_missing = GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES;
    const char *const MISSING = "nvngx_dlss.dll is not in the game folder";

    // NGX writes its own log files into the application data folder, so it
    // gets a folder of its own.
    const std::wstring data = folder + L"\\acvdlss";
    CreateDirectoryW(data.c_str(), nullptr);
    NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(result))
    {
        _error = result == NVSDK_NGX_Result_FAIL_FeatureNotSupported ? "this graphics card does not support DLSS"
                 : dll_missing                                       ? MISSING
                                                                     : describe("NGX start-up", result);
        return false;
    }

    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&_params);
    if (NVSDK_NGX_FAILED(result))
    {
        _params = nullptr;
        _error = describe("Reading DLSS capabilities", result);
        return false;
    }

    int needs_driver = 0, supported = 0;
    NVSDK_NGX_Parameter_GetI(_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetI(_params, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    if (needs_driver || !supported)
    {
        _error = needs_driver  ? "the graphics driver is too old for DLSS"
                 : dll_missing ? MISSING
                               : "it needs an NVIDIA RTX graphics card and a recent nvngx_dlss.dll";
        NVSDK_NGX_D3D12_DestroyParameters(_params);
        _params = nullptr;
        return false;
    }
    return true;
}

void dlss::shutdown()
{
    release_feature();
    if (_params != nullptr)
        NVSDK_NGX_D3D12_DestroyParameters(_params);
    _params = nullptr;
    if (_device != nullptr)
        NVSDK_NGX_D3D12_Shutdown1(_device);
    _device = nullptr;
}

void dlss::release_feature()
{
    if (_feature != nullptr)
        NVSDK_NGX_D3D12_ReleaseFeature(_feature);
    _feature = nullptr;
}

// DLSS takes the exact render size whatever the mode; the mode only picks the
// tuning, so the nearest one to the game's resolution scale is used.
static NVSDK_NGX_PerfQuality_Value quality_for(const dlss_frame &frame)
{
    const float scale = float(frame.width) / float(frame.output_width);
    if (scale >= 0.99f)
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (scale >= 0.62f)
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (scale >= 0.54f)
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (scale >= 0.42f)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

bool dlss::create(ID3D12GraphicsCommandList *list, const dlss_frame &frame)
{
    // The preset is a hint per quality mode; the same one goes to all of them
    // so it holds whichever mode the resolution scale picks.
    const char *hints[] = {
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    };
    for (const char *hint : hints)
        NVSDK_NGX_Parameter_SetUI(_params, hint, frame.preset);

    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = frame.width;
    create.Feature.InHeight = frame.height;
    create.Feature.InTargetWidth = frame.output_width;
    create.Feature.InTargetHeight = frame.output_height;
    create.Feature.InPerfQualityValue = quality_for(frame);
    // The game's depth is reversed and its motion vectors carry no jitter.
    // An HDR frame has no exposure value we could hand over, so DLSS measures it.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (frame.hdr)
        create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &_feature, _params, &create);
    if (NVSDK_NGX_FAILED(result))
    {
        _feature = nullptr;
        _error = describe("Creating DLSS", result);
        return false;
    }
    _width = frame.width;
    _height = frame.height;
    _output_width = frame.output_width;
    _output_height = frame.output_height;
    _hdr = frame.hdr;
    _preset = frame.preset;
    _error.clear();
    // Once per size or preset change, so it is logged without Debug too.
    char text[160];
    snprintf(text, sizeof(text), "DLSS created: %ux%u to %ux%u, %s mode, preset %s%s", frame.width, frame.height,
             frame.output_width, frame.output_height, quality_name(create.Feature.InPerfQualityValue),
             preset_name(frame.preset).c_str(), frame.hdr ? ", HDR" : "");
    acv_log(reshade::log::level::info, text);
    return true;
}

bool dlss::evaluate(ID3D12GraphicsCommandList *list, const dlss_frame &frame)
{
    if (_params == nullptr)
        return false;
    const bool changed = will_rebuild(frame);
    if (changed)
        release_feature();
    if (_feature == nullptr && !create(list, frame))
        return false;

    NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = frame.color;
    eval.Feature.pInOutput = frame.output;
    eval.pInDepth = frame.depth;
    eval.pInMotionVectors = frame.motion;
    eval.InJitterOffsetX = frame.jitter_x;
    eval.InJitterOffsetY = frame.jitter_y;
    eval.InRenderSubrectDimensions = {frame.width, frame.height};
    eval.InReset = frame.reset || changed ? 1 : 0;
    // Motion vectors are in UV units; DLSS wants pixels.
    eval.InMVScaleX = float(frame.width);
    eval.InMVScaleY = float(frame.height);
    eval.InFrameTimeDeltaInMsec = frame.frame_ms;

    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(list, _feature, _params, &eval);
    if (NVSDK_NGX_FAILED(result))
    {
        _error = describe("Running DLSS", result);
        return false;
    }
    return true;
}
