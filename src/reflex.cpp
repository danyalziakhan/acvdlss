// NVIDIA Reflex. The game has none, so the add-on calls it as a game would:
//
//   finish_present   PRESENT_END, then the wait (NvAPI_D3D_Sleep), then the
//                    next frame's SIMULATION_START
//   first submission SIMULATION_END and RENDERSUBMIT_START
//   present          RENDERSUBMIT_END and PRESENT_START
//
// The wait belongs at the start of a frame. Placed right before Present, the
// GPU idles for a whole frame of CPU work every frame.
//
// When off, NvAPI isn't even loaded. Switching off after being on tells the
// driver once, then stops calling.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <reshade.hpp>

#include "acvdlss.h"
#include "reflex.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

using namespace reshade::api;

namespace
{
// The parts of NVIDIA's nvapi.h (MIT license) Reflex needs. Same packing as
// the header (8).
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

// The interface IDs from nvapi_interface.h.
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

// All of these belong to the present thread, except as noted.
ID3D12Device *g_device = nullptr; // the device of the swapchain being presented
std::atomic<ID3D12Device *> g_marking{nullptr}; // read by submitting threads: markers go to this device
std::atomic<uint64_t> g_frame{1};
std::atomic<bool> g_submitted{false}; // SIMULATION_END already sent for the current frame
int g_applied = -1;                   // what the driver was last told; -1: nothing yet

double g_sleep_ms = 0.0; // with Debug on, for the latency line
uint32_t g_sleeps = 0;
uint32_t g_frames_since_report = 0;

void log_text(const char *text)
{
    acv_log(reshade::log::level::info, text);
}

// The game loads nvapi64.dll itself. Tools that wrap Reflex hook the same
// entry points, so they see our calls.
bool load()
{
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi == nullptr)
        nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr)
    {
        log_text("Reflex: not available, no NVIDIA driver");
        return false;
    }
    const auto query = reinterpret_cast<query_interface_fn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (query == nullptr)
    {
        log_text("Reflex: not available, the NVIDIA driver does not offer it");
        return false;
    }
    const auto initialize = reinterpret_cast<initialize_fn>(query(ID_INITIALIZE));
    g_set_sleep_mode = reinterpret_cast<set_sleep_mode_fn>(query(ID_SET_SLEEP_MODE));
    g_sleep = reinterpret_cast<sleep_fn>(query(ID_SLEEP));
    g_set_marker = reinterpret_cast<set_latency_marker_fn>(query(ID_SET_LATENCY_MARKER));
    g_get_latency = reinterpret_cast<get_latency_fn>(query(ID_GET_LATENCY));
    if (initialize == nullptr || g_set_sleep_mode == nullptr || g_sleep == nullptr || g_set_marker == nullptr)
    {
        log_text("Reflex: not available, this driver does not have it");
        return false;
    }
    const NvAPI_Status status = initialize();
    if (status != NVAPI_OK)
    {
        char text[80];
        snprintf(text, sizeof(text), "Reflex: not available, NvAPI did not start (%d)", status);
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

void tell_driver(int mode)
{
    NV_SET_SLEEP_MODE_PARAMS params = {};
    params.version = version_of(sizeof(params), 1);
    params.bLowLatencyMode = mode != int(reflex_mode::off);
    params.bLowLatencyBoost = mode == int(reflex_mode::boost);
    // No frame limit. A limiter that goes through Reflex overrides this.
    params.minimumIntervalUs = 0;
    // Our markers come from around Present, not the game's real frame start,
    // so the driver must not schedule work around them. Streamline also
    // defaults this to off.
    params.bUseMarkersToOptimize = 0;
    const NvAPI_Status status = g_set_sleep_mode(g_device, &params);
    char text[96];
    if (status == NVAPI_OK)
    {
        static const char *NAMES[] = {"off", "on", "on + boost"};
        snprintf(text, sizeof(text), "Reflex: %s", NAMES[mode]);
    }
    else
    {
        snprintf(text, sizeof(text), "Reflex: the driver refused the setting (%d)", status);
    }
    log_text(text);
    g_applied = mode;
}

// Debug only, every 600 frames: the driver's latency breakdown from frame
// start to GPU done.
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
    // Complete frames only. The GPU often starts a frame before its Present,
    // so "after Present" runs to GPU done, not GPU start.
    double total = 0, to_present = 0, after_present = 0, gpu_active = 0, frame_time = 0;
    uint32_t count = 0, timed = 0;
    for (const auto &f : result.frameReport)
    {
        if (f.frameID == 0 || f.simStartTime == 0 || f.presentEndTime < f.presentStartTime ||
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
    char text[256];
    if (count == 0)
    {
        snprintf(text, sizeof(text), "Reflex latency: no complete frames in the report yet");
    }
    else
    {
        // The driver's times are in microseconds.
        const double n = 1000.0 * count;
        const double fps = frame_time > 0.0 ? 1e6 * timed / frame_time : 0.0;
        snprintf(text, sizeof(text),
                 "Reflex latency (%s): %.2f ms from frame start to GPU done = %.2f to Present + %.2f from Present "
                 "to GPU done | GPU busy %.2f ms | %.1f fps | waited %.2f ms a frame | %u frames",
                 g_applied == 1 ? "on" : "on + boost", total / n, to_present / n, after_present / n, gpu_active / n,
                 fps, g_sleeps != 0 ? g_sleep_ms / g_sleeps : 0.0, count);
    }
    log_text(text);
    g_sleep_ms = 0.0;
    g_sleeps = 0;
}

// Reflex stays off with FSR or XeSS frame generation. With FSR's swapchain
// and Reflex on, the GPU hung every time. XeSS runs Intel's own latency
// handling, so ours gains nothing there. DLSS frame generation keeps it:
// Streamline takes our markers and was stable in every test.
//
// OptiScaler loads every frame generation library up front, so check its
// FGOutput setting instead. Changes to it only apply after a restart. Without
// OptiScaler, a loaded AMD library counts as in use.
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

// Only every 120 frames: a module lookup takes the loader lock.
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

// True when Reflex has work to do this frame. Applies setting and device
// changes first.
bool active(swapchain *swapchain)
{
    int wanted = int(g_settings.reflex.load());
    if (wanted == int(reflex_mode::off) && g_applied <= 0)
        return false; // never on, or already switched off: nothing to do
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
        // New device: resend the setting and restart frame IDs.
        g_device = native;
        g_applied = -1;
        g_marking = nullptr;
    }
    if (wanted != g_applied)
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
    const uint64_t frame = g_frame.load();
    // A frame that submitted nothing still gets its pair, in order.
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
    // If Reflex was switched on since present, wait for the next frame.
    if (g_marking.load() == nullptr || !active(swapchain))
        return;
    marker(g_device, PRESENT_END, g_frame.load());

    const auto start = std::chrono::steady_clock::now();
    g_sleep(g_device);
    if (g_settings.debug)
    {
        g_sleep_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++g_sleeps;
        if (++g_frames_since_report >= 600)
        {
            g_frames_since_report = 0;
            report();
        }
    }

    const uint64_t next = g_frame.fetch_add(1) + 1;
    g_submitted = false;
    marker(g_device, SIMULATION_START, next);
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

const char *reflex_unavailable()
{
    if (g_load == load_state::failed)
        return "Needs an NVIDIA graphics card.";
    if (frame_generation_in_use())
        return "Off while FSR or XeSS frame generation is in use.";
    return nullptr;
}
