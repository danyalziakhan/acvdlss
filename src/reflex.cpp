// NVIDIA Reflex. The game has none. We place the markers:
//
//   BeginFrame       the wait (NvAPI_D3D_Sleep), then SIMULATION_START
//   first submission SIMULATION_END and RENDERSUBMIT_START
//   present          RENDERSUBMIT_END and PRESENT_START
//   finish_present   PRESENT_END
//
// BeginFrame is the game's own frame start. frame_start.cpp hooks it. The game reads input right after it.
// With placement 0 or without the hook, the wait and SIMULATION_START come right after PRESENT_END instead.
// Right before Present the GPU would idle for a whole frame of CPU work every frame.
//
// The game works on two frames at once. The frame starting at a BeginFrame is presented after the next
// Present. Tested end to end: mouse movement read at a BeginFrame first shows on the Present that carries
// that BeginFrame's ID.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <reshade.hpp>

#include "acvdlss.h"
#include "frame_start.h"
#include "reflex.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <string>

using namespace reshade::api;

namespace
{
// From NVIDIA's nvapi.h (MIT license). Same packing as the header (8).
using NvU8 = unsigned char;
using NvU32 = unsigned long;
using NvU64 = unsigned long long;
using NvBool = NvU8;
using NvAPI_Status = int;
constexpr NvAPI_Status NVAPI_OK = 0;

constexpr NvU32 version_of(size_t size, NvU32 version)
{
    return NvU32(size) | (version << 16);
}

struct NV_SET_SLEEP_MODE_PARAMS
{
    NvU32 version;
    NvBool bLowLatencyMode;
    NvBool bLowLatencyBoost;
    NvU32 minimumIntervalUs; // 0: no frame limit
    NvBool bUseMarkersToOptimize;
    NvBool bUseMinQueueTime;
    NvU8 rsvd[30];
};

enum NV_LATENCY_MARKER_TYPE
{
    SIMULATION_START = 0,
    SIMULATION_END = 1,
    RENDERSUBMIT_START = 2,
    RENDERSUBMIT_END = 3,
    PRESENT_START = 4,
    PRESENT_END = 5,
};

struct NV_LATENCY_MARKER_PARAMS
{
    NvU32 version;
    NvU64 frameID;
    NV_LATENCY_MARKER_TYPE markerType;
    NvU64 rsvd0;
    NvU8 rsvd[56];
};

struct NV_LATENCY_RESULT_PARAMS
{
    NvU32 version;
    struct FrameReport
    {
        NvU64 frameID;
        NvU64 inputSampleTime;
        NvU64 simStartTime;
        NvU64 simEndTime;
        NvU64 renderSubmitStartTime;
        NvU64 renderSubmitEndTime;
        NvU64 presentStartTime;
        NvU64 presentEndTime;
        NvU64 driverStartTime;
        NvU64 driverEndTime;
        NvU64 osRenderQueueStartTime;
        NvU64 osRenderQueueEndTime;
        NvU64 gpuRenderStartTime;
        NvU64 gpuRenderEndTime;
        NvU32 gpuActiveRenderTimeUs;
        NvU32 gpuFrameTimeUs;
        NvU64 cameraConstructedTime;
        NvU32 crossAdapterCopyTimeUs;
        NvU32 aiFrameTimeUs;
        NvU8 rsvd[104];
    } frameReport[64];
    NvU8 rsvd[32];
};

// From nvapi_interface.h.
constexpr NvU32 ID_INITIALIZE = 0x0150e828;
constexpr NvU32 ID_SET_SLEEP_MODE = 0xac1ca9e0;
constexpr NvU32 ID_SLEEP = 0x852cd1d2;
constexpr NvU32 ID_SET_LATENCY_MARKER = 0xd9984c05;
constexpr NvU32 ID_GET_LATENCY = 0x1a587f9c;

using query_interface_fn = void *(__cdecl *)(NvU32);
using initialize_fn = NvAPI_Status(__cdecl *)();
using set_sleep_mode_fn = NvAPI_Status(__cdecl *)(IUnknown *, NV_SET_SLEEP_MODE_PARAMS *);
using sleep_fn = NvAPI_Status(__cdecl *)(IUnknown *);
using set_latency_marker_fn = NvAPI_Status(__cdecl *)(IUnknown *, NV_LATENCY_MARKER_PARAMS *);
using get_latency_fn = NvAPI_Status(__cdecl *)(IUnknown *, NV_LATENCY_RESULT_PARAMS *);

set_sleep_mode_fn g_set_sleep_mode = nullptr;
sleep_fn g_sleep = nullptr;
set_latency_marker_fn g_set_marker = nullptr;
get_latency_fn g_get_latency = nullptr;

enum class load_state
{
    not_tried,
    loaded,
    failed,
};
std::atomic<load_state> g_load{load_state::not_tried};

// Present thread only, except as noted.
ID3D12Device *g_device = nullptr;
std::atomic<ID3D12Device *> g_marking{nullptr}; // read by submitting threads
std::atomic<uint64_t> g_frame{1};
std::atomic<bool> g_submitted{false};  // SIMULATION_END sent this frame
std::atomic<bool> g_presenting{false}; // between present and finish_present. Read by BeginFrame
int g_applied = -1;                    // last mode sent to the driver, -1 for none
bool g_applied_paced = false;          // and whether the driver paces by the markers

double g_sleep_ms = 0.0; // for the latency line with Debug on
uint32_t g_sleeps = 0;
std::atomic<int64_t> g_frame_start_wait_us{0}; // waits on the BeginFrame thread for the same line
uint32_t g_frames_since_report = 0;

void log_text(const char *text)
{
    acv_log(reshade::log::level::info, text);
}

// The game loads nvapi64.dll itself. Not loaded at all while Reflex is off.
bool load()
{
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi == nullptr)
        nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr)
    {
        log_text("Reflex: not available without an NVIDIA driver");
        return false;
    }
    const auto query = reinterpret_cast<query_interface_fn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (query == nullptr)
    {
        log_text("Reflex: not available. The NVIDIA driver does not offer it");
        return false;
    }
    const auto initialize = reinterpret_cast<initialize_fn>(query(ID_INITIALIZE));
    g_set_sleep_mode = reinterpret_cast<set_sleep_mode_fn>(query(ID_SET_SLEEP_MODE));
    g_sleep = reinterpret_cast<sleep_fn>(query(ID_SLEEP));
    g_set_marker = reinterpret_cast<set_latency_marker_fn>(query(ID_SET_LATENCY_MARKER));
    g_get_latency = reinterpret_cast<get_latency_fn>(query(ID_GET_LATENCY));
    if (initialize == nullptr || g_set_sleep_mode == nullptr || g_sleep == nullptr || g_set_marker == nullptr)
    {
        log_text("Reflex: not available in this driver");
        return false;
    }
    const NvAPI_Status status = initialize();
    if (status != NVAPI_OK)
    {
        char text[80];
        snprintf(text, sizeof(text), "Reflex: not available. NvAPI did not start (%d)", status);
        log_text(text);
        return false;
    }
    return true;
}

void marker(ID3D12Device *device, NV_LATENCY_MARKER_TYPE type, uint64_t frame)
{
    NV_LATENCY_MARKER_PARAMS params = {};
    params.version = version_of(sizeof(params), 1);
    params.frameID = frame;
    params.markerType = type;
    g_set_marker(device, &params);
}

// The ini's placement once BeginFrame is hooked. After Present until then or if the hook failed.
reflex_placement current_placement()
{
    const auto wanted = reflex_placement(g_settings.reflex_placement.load());
    return frame_start_hooked() ? wanted : reflex_placement::after_present;
}

void tell_driver(int mode)
{
    NV_SET_SLEEP_MODE_PARAMS params = {};
    params.version = version_of(sizeof(params), 1);
    params.bLowLatencyMode = mode != int(reflex_mode::off);
    params.bLowLatencyBoost = mode == int(reflex_mode::boost);
    // A limiter that goes through Reflex overrides this.
    params.minimumIntervalUs = 0;
    // The driver may pace frames by the markers only when the frame start marker is at the real start.
    g_applied_paced = current_placement() == reflex_placement::frame_start_paced;
    params.bUseMarkersToOptimize = g_applied_paced;
    const NvAPI_Status status = g_set_sleep_mode(g_device, &params);
    char text[96];
    if (status == NVAPI_OK)
    {
        static const char *NAMES[] = {"off", "on", "on + boost"};
        snprintf(text, sizeof(text), "Reflex: %s%s", NAMES[mode],
                 mode != 0 && g_applied_paced ? " (the driver paces frames by the markers)" : "");
    }
    else
    {
        snprintf(text, sizeof(text), "Reflex: the driver refused the setting (%d)", status);
    }
    log_text(text);
    g_applied = mode;
}

// Debug only. The driver's latency breakdown, every 600 frames.
void report()
{
    if (g_get_latency == nullptr)
        return;
    static NV_LATENCY_RESULT_PARAMS result; // 16 KB, not on the stack
    result = {};
    result.version = version_of(sizeof(result), 1);
    if (g_get_latency(g_device, &result) != NVAPI_OK)
    {
        log_text("Reflex latency: the driver gave no report");
        return;
    }
    // The GPU often starts a frame before its Present. So "after Present" runs to GPU done, not GPU start.
    double total = 0, to_present = 0, after_present = 0, gpu_active = 0, frame_time = 0;
    uint32_t count = 0, timed = 0, reported = 0, bad_markers = 0;
    for (const auto &f : result.frameReport)
    {
        if (f.frameID == 0)
            continue;
        // Every frame must carry all six markers in order.
        ++reported;
        const NvU64 markers[] = {f.simStartTime,        f.simEndTime,       f.renderSubmitStartTime,
                                 f.renderSubmitEndTime, f.presentStartTime, f.presentEndTime};
        if (std::find(std::begin(markers), std::end(markers), NvU64(0)) != std::end(markers) ||
            !std::is_sorted(std::begin(markers), std::end(markers)))
            ++bad_markers;
        if (f.simStartTime == 0 || f.presentEndTime < f.presentStartTime ||
            f.presentStartTime < f.simStartTime || f.gpuRenderEndTime < f.presentEndTime)
            continue;
        total += double(f.gpuRenderEndTime - f.simStartTime);
        to_present += double(f.presentStartTime - f.simStartTime);
        after_present += double(f.gpuRenderEndTime - f.presentEndTime);
        gpu_active += double(f.gpuActiveRenderTimeUs);
        if (f.gpuFrameTimeUs != 0)
        {
            frame_time += double(f.gpuFrameTimeUs);
            ++timed;
        }
        ++count;
    }
    char text[320];
    if (count == 0)
    {
        snprintf(text, sizeof(text), "Reflex latency: no complete frames in the report yet");
    }
    else
    {
        // Driver times are in microseconds.
        const double n = 1000.0 * count;
        const double fps = frame_time > 0.0 ? 1e6 * timed / frame_time : 0.0;
        snprintf(text, sizeof(text),
                 "Reflex latency (%s): %.2f ms from frame start to GPU done = %.2f to Present + %.2f from Present "
                 "to GPU done | GPU busy %.2f ms | %.1f fps | waited %.2f ms a frame | %u frames, %u with markers "
                 "missing or out of order",
                 g_applied == 1 ? "on" : "on + boost", total / n, to_present / n, after_present / n, gpu_active / n,
                 fps, g_sleeps != 0 ? g_sleep_ms / g_sleeps : 0.0, reported, bad_markers);
    }
    log_text(text);
    g_sleep_ms = 0.0;
    g_sleeps = 0;
}

// Off with FSR or XeSS frame generation. FSR's swapchain plus Reflex hung the GPU every time. XeSS has its
// own latency handling. DLSS frame generation was stable in every test. Streamline takes our markers there.
//
// OptiScaler loads every frame generation library up front. Its FGOutput setting tells which one is in use.
// Without OptiScaler a loaded AMD library counts as in use.
bool frame_generation_in_use()
{
    static const int configured = [] {
        const std::wstring ini = game_folder() + L"\\OptiScaler.ini";
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
            return -1;
        wchar_t value[32] = L"";
        GetPrivateProfileStringW(L"FrameGen", L"FGOutput", L"", value, 32, ini.c_str());
        return _wcsicmp(value, L"fsrfg") == 0 || _wcsicmp(value, L"xefg") == 0 ? 1 : 0;
    }();
    if (configured >= 0)
        return configured == 1;
    return GetModuleHandleW(L"amd_fidelityfx_framegeneration_dx12.dll") != nullptr;
}

// Every 120 frames only. A module lookup takes the loader lock.
bool step_aside()
{
    static uint32_t frames = 0;
    static bool aside = false;
    if (frames++ % 120 == 0)
    {
        const bool now = frame_generation_in_use();
        if (now && !aside)
            log_text("Reflex: off while FSR or XeSS frame generation is in use");
        aside = now;
    }
    return aside;
}

// Applies setting and device changes first.
bool active(swapchain *swapchain)
{
    int wanted = int(g_settings.reflex.load());
    if (wanted == int(reflex_mode::off) && g_applied <= 0)
        return false; // off and the driver already knows
    if (wanted != int(reflex_mode::off) && step_aside())
        wanted = int(reflex_mode::off);
    if (wanted == int(reflex_mode::off) && g_applied <= 0)
        return false;

    device *dev = swapchain->get_device();
    if (dev->get_api() != device_api::d3d12)
        return false;
    if (g_load == load_state::not_tried)
        g_load = load() ? load_state::loaded : load_state::failed;
    if (g_load != load_state::loaded)
        return false;

    auto *native = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if (native != g_device)
    {
        // New device. Resend the setting and restart frame IDs.
        g_device = native;
        g_applied = -1;
        g_marking = nullptr;
    }
    // The pacing changes once. The hook goes in a second after start.
    const bool paced = current_placement() == reflex_placement::frame_start_paced;
    if (wanted != g_applied || (wanted != 0 && paced != g_applied_paced))
        tell_driver(wanted);
    const bool on = g_applied > 0;
    g_marking = on ? g_device : nullptr;
    return on;
}

void on_execute(command_queue *queue, command_list *)
{
    ID3D12Device *device = g_marking.load(std::memory_order_relaxed);
    if (device == nullptr)
        return;
    if (reinterpret_cast<ID3D12Device *>(queue->get_device()->get_native()) != device ||
        g_submitted.exchange(true))
        return;
    const uint64_t frame = g_frame.load();
    marker(device, SIMULATION_END, frame);
    marker(device, RENDERSUBMIT_START, frame);
}

void on_present(command_queue *, swapchain *swapchain, const rect *, const rect *, uint32_t, const rect *)
{
    if (!active(swapchain))
        return;
    g_presenting = true;
    const uint64_t frame = g_frame.load();
    // A frame that submitted nothing still gets its pair in order.
    if (!g_submitted.exchange(true))
    {
        marker(g_device, SIMULATION_END, frame);
        marker(g_device, RENDERSUBMIT_START, frame);
    }
    marker(g_device, RENDERSUBMIT_END, frame);
    marker(g_device, PRESENT_START, frame);
}

void on_finish_present(command_queue *, swapchain *swapchain)
{
    // Switched on since present. Wait for the next frame.
    if (g_marking.load() == nullptr || !active(swapchain))
    {
        g_presenting = false;
        return;
    }
    marker(g_device, PRESENT_END, g_frame.load());

    const bool here = current_placement() == reflex_placement::after_present;
    const auto start = std::chrono::steady_clock::now();
    if (here)
        g_sleep(g_device);
    if (g_settings.debug)
    {
        g_sleep_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        g_sleep_ms += double(g_frame_start_wait_us.exchange(0)) / 1000.0;
        ++g_sleeps;
        if (++g_frames_since_report >= 600)
        {
            g_frames_since_report = 0;
            report();
        }
    }

    const uint64_t next = g_frame.fetch_add(1) + 1;
    g_presenting = false;
    g_submitted = false;
    if (here)
        marker(g_device, SIMULATION_START, next);
}

// The ID the frame starting now will be presented with: the one after the next Present's. g_frame is the
// next Present's ID unless a Present is running now. Then that one has g_frame and the next has g_frame + 1.
// g_frame is read twice so a Present finishing in between can't pair the old ID with the new flag.
uint64_t starting_frame_id()
{
    for (;;)
    {
        const uint64_t frame = g_frame.load();
        const bool presenting = g_presenting.load();
        if (g_frame.load() == frame)
            return frame + (presenting ? 2 : 1);
    }
}

void on_destroy_device(device *dev)
{
    if (reinterpret_cast<ID3D12Device *>(dev->get_native()) == g_device)
    {
        g_marking = nullptr;
        g_device = nullptr;
        g_applied = -1;
    }
}
} // namespace

void reflex_register()
{
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::execute_command_list>(on_execute);
    reshade::register_event<reshade::addon_event::present>(on_present);
    reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
}

void reflex_unregister()
{
    reshade::unregister_event<reshade::addon_event::finish_present>(on_finish_present);
    reshade::unregister_event<reshade::addon_event::present>(on_present);
    reshade::unregister_event<reshade::addon_event::execute_command_list>(on_execute);
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
}

void reflex_frame_start()
{
    if (current_placement() == reflex_placement::after_present)
        return;
    ID3D12Device *device = g_marking.load();
    if (device == nullptr)
        return;
    const auto start = std::chrono::steady_clock::now();
    g_sleep(device);
    if (g_settings.debug)
        g_frame_start_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
    // After the wait: the frame starts when the wait ends.
    marker(device, SIMULATION_START, starting_frame_id());
}

const char *reflex_unavailable()
{
    if (g_load == load_state::failed)
        return "Needs an NVIDIA graphics card.";
    if (frame_generation_in_use())
        return "Off while FSR or XeSS frame generation is in use.";
    return nullptr;
}
