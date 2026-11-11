#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "acvdlss.h"
#include "ngx_guard.h"
#include "dlss.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <reshade.hpp>

#include <softpub.h>
#include <wintrust.h>

#include <cfloat>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <string>

// Any stable GUID works as a project ID for NGX; this one is ours.
static const char *PROJECT_ID = "5c3f9a1e-7b2d-4e8a-9f61-0d4c2b7e3a95";

// Lets a report tell an official nvngx_dlssnr.dll from a modified one, since
// NGX only loads signed DLLs. Cached certificate data only, so startup never
// waits on the network.
static const char *signature_of(const std::wstring &path)
{
#ifdef ACVDLSS_PLAIN
    (void)path;
    return "(signature not checked in this build)";
#else
    WINTRUST_FILE_INFO file = {sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data = {sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return result == ERROR_SUCCESS ? "signed" : result == LONG(TRUST_E_NOSIGNATURE) ? "unsigned" : "signature invalid";
#endif
}

// The DLSS DLLs beside the game, for reports: RenoDX DLSS runs DLSS 5 (neural
// rendering) from nvngx_dlssnr.dll on the same NVIDIA runtime as our DLSS.
static void log_versions(const std::wstring &folder)
{
    std::string text;
    for (const wchar_t *name : {L"nvngx_dlss.dll", L"nvngx_dlssnr.dll"})
    {
        const std::wstring path = folder + L"\\" + name;
        const std::string version = file_version(path);
        if (version.empty() && wcscmp(name, L"nvngx_dlssnr.dll") == 0)
            continue;
        char part[96];
        snprintf(part, sizeof(part), "%s%ls %s", text.empty() ? "" : ", ", name,
                 version.empty() ? "not found" : (version + " " + signature_of(path)).c_str());
        text += part;
    }
    acv_log(reshade::log::level::info, text.c_str());
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
    log_versions(folder);
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

    // NGX doesn't report a missing DLL as such, so check for it ourselves to
    // give a useful error.
    const std::wstring dll = folder + L"\\nvngx_dlss.dll";
    const bool dll_missing = GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES;
    const char *const MISSING = "nvngx_dlss.dll is not in the game folder";

    // NGX writes log files into its data folder, so give it a separate one.
    const std::wstring data = folder + L"\\acvdlss";
    CreateDirectoryW(data.c_str(), nullptr);
    // Keep DLSS 5 out of our startup (see ngx_guard.h). NGX may load features
    // both in Init and in GetCapabilityParameters.
    std::optional<ngx_guard> guard(std::in_place, folder);
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
    guard.reset();
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
    // Depth is reversed and motion vectors have no jitter. There's no exposure
    // value to pass, so use auto exposure, in SDR too: players report less
    // ghosting with it in other games.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (frame.hdr)
        create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

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
    // Only on size or preset changes, so log it even without Debug.
    char text[160];
    snprintf(text, sizeof(text), "DLSS created: %ux%u to %ux%u, %s mode, preset %s%s", frame.width, frame.height,
             frame.output_width, frame.output_height, quality_name(create.Feature.InPerfQualityValue),
             preset_name(frame.preset).c_str(), frame.hdr ? ", HDR" : "");
    acv_log(reshade::log::level::info, text);
    const float degrees = frame.camera_fov * 180.0f / 3.14159265f;
    if (frame.camera_far >= FLT_MAX)
        snprintf(text, sizeof(text), "camera: near %.2f, far infinite, vertical field of view %.1f degrees",
                 frame.camera_near, degrees);
    else
        snprintf(text, sizeof(text), "camera: near %.2f, far %.0f, vertical field of view %.1f degrees",
                 frame.camera_near, frame.camera_far, degrees);
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
    // Ignored by DLSS. Frame generation tools that hook DLSS read the camera
    // from these names.
    NVSDK_NGX_Parameter_SetF(_params, "FSR.cameraNear", frame.camera_near);
    NVSDK_NGX_Parameter_SetF(_params, "FSR.cameraFar", frame.camera_far);
    NVSDK_NGX_Parameter_SetF(_params, "FSR.cameraFovAngleVertical", frame.camera_fov);
    // OptiScaler's frame generation uses this as the HUD-less image instead of
    // searching for one (its HUD fix). The parameter block persists across
    // frames, so this is also set (to null) when off.
    NVSDK_NGX_Parameter_SetD3d12Resource(_params, "Opti.Hudless", frame.hudless);
    NVSDK_NGX_Parameter_SetUI(_params, "Opti.Hudless.State", frame.hudless_state);

    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(list, _feature, _params, &eval);
    if (NVSDK_NGX_FAILED(result))
    {
        _error = describe("Running DLSS", result);
        return false;
    }
    return true;
}
