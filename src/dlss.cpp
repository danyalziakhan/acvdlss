#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "dlss.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <reshade.hpp>

#include <cstdio>
#include <string>

// Any stable GUID works as a project ID for NGX; this one is ours.
static const char *PROJECT_ID = "5c3f9a1e-7b2d-4e8a-9f61-0d4c2b7e3a95";

static std::wstring module_folder()
{
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring folder(path);
    return folder.substr(0, folder.find_last_of(L"\\/"));
}

static std::string describe(const char *what, NVSDK_NGX_Result result)
{
    char text[160];
    snprintf(text, sizeof(text), "%s failed (0x%08X)", what, unsigned(result));
    return text;
}

static void NVSDK_CONV log_ngx(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string line = std::string("ACV DLSS: NGX: ") + message;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    reshade::log::message(reshade::log::level::info, line.c_str());
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
    _log = log;
    const std::wstring folder = module_folder();
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

    NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", folder.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(result))
    {
        _error = result == NVSDK_NGX_Result_FAIL_FeatureNotSupported
                     ? "this graphics card does not support DLSS"
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
        _error = needs_driver ? "the graphics driver is too old for DLSS"
                              : "DLSS is not available; is nvngx_dlss.dll next to the game?";
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
    if (_log)
    {
        char text[192];
        snprintf(text, sizeof(text), "ACV DLSS: DLSS feature built: %ux%u to %ux%u, %s mode, preset %u, flags 0x%X",
                 frame.width, frame.height, frame.output_width, frame.output_height,
                 quality_name(create.Feature.InPerfQualityValue), frame.preset, unsigned(create.InFeatureCreateFlags));
        reshade::log::message(reshade::log::level::info, text);
    }
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
