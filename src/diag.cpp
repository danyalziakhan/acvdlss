// Diagnostics for hangs and conflicts (see diag.h).
//
// The watchdog thread holds a reference to this DLL and drops it as it exits. It ends with the device. So it
// never runs code from an unloaded add-on.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3d12.h>

#include "acvdlss.h"
#include "diag.h"
#include "fg_report.h"

#include <reshade.hpp>

#include <atomic>
#include <cstdio>
#include <cwctype>
#include <string>

namespace
{
// Reports of a stuck stage, in seconds since it started. Then every minute.
const uint64_t STAGE_REPORTS[] = {5, 15, 60};
// Reports while no frame comes, in seconds. Then every two minutes.
const uint64_t FRAME_REPORTS[] = {10, 30, 60, 120};

std::atomic<const char *> g_stage{nullptr};
std::atomic<uint64_t> g_stage_start{0};
std::atomic<unsigned long> g_stage_thread{0};

std::atomic<uint64_t> g_device_time{0};
std::atomic<unsigned long> g_device_thread{0};
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_last_frame{0};
std::atomic<unsigned long> g_present_thread{0};
std::atomic<bool> g_removed_logged{false};

HANDLE g_stop = nullptr;
std::atomic<bool> g_watching{false};
void *g_notify_cookie = nullptr;
std::wstring g_windows; // lowercase, with a trailing backslash

uint64_t now_ms()
{
    return GetTickCount64();
}

std::string utf8(const wchar_t *text, int length)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), n, nullptr, nullptr);
    return out;
}

// The next report time after `elapsed`, or the repeat interval past the list.
bool due(uint64_t elapsed, const uint64_t *list, size_t count, uint64_t repeat, size_t &next)
{
    const uint64_t at = next < count ? list[next] : list[count - 1] + repeat * (next - count + 1);
    if (elapsed < at * 1000)
        return false;
    ++next;
    return true;
}

void check_stage(size_t &next, const char *&last)
{
    const char *stage = g_stage.load();
    if (stage != last)
    {
        last = stage;
        next = 0;
    }
    if (stage == nullptr)
        return;
    const uint64_t elapsed = now_ms() - g_stage_start.load();
    if (!due(elapsed, STAGE_REPORTS, std::size(STAGE_REPORTS), 60, next))
        return;
    char text[160];
    snprintf(text, sizeof(text), "still in \"%s\" after %llu s. Its thread:", stage,
             static_cast<unsigned long long>(elapsed / 1000));
    acv_log(reshade::log::level::warning, (text + thread_stack(g_stage_thread.load())).c_str());
}

void check_frames(size_t &next, uint64_t &seen, uint64_t &stopped)
{
    const uint64_t device = g_device_time.load();
    if (device == 0)
        return;
    const uint64_t frames = g_frames.load();
    if (frames != seen)
    {
        if (next > 0)
        {
            char text[96];
            snprintf(text, sizeof(text), "frames again after a stop of %llu s",
                     static_cast<unsigned long long>((now_ms() - stopped) / 1000));
            acv_log(reshade::log::level::info, text);
        }
        seen = frames;
        next = 0;
        return;
    }
    const uint64_t since = frames == 0 ? device : g_last_frame.load();
    const uint64_t elapsed = now_ms() - since;
    if (!due(elapsed, FRAME_REPORTS, std::size(FRAME_REPORTS), 120, next))
        return;
    stopped = since;
    char text[192];
    snprintf(text, sizeof(text), "no frame for %llu s %s. Normal while the game is minimized or loading",
             static_cast<unsigned long long>(elapsed / 1000),
             frames == 0 ? "since the device was made (before the title screen)" : "since the last one");
    acv_log(reshade::log::level::warning, text);
    acv_log(reshade::log::level::info, ("  device thread:" + thread_stack(g_device_thread.load())).c_str());
    const unsigned long present = g_present_thread.load();
    if (present != 0 && present != g_device_thread.load())
        acv_log(reshade::log::level::info, ("  frame thread:" + thread_stack(present)).c_str());
}

DWORD WINAPI watchdog(void *module)
{
    size_t stage_next = 0, frame_next = 0;
    const char *stage = nullptr;
    uint64_t frames = 0, stopped = 0;
    while (WaitForSingleObject(g_stop, 1000) == WAIT_TIMEOUT)
    {
        check_stage(stage_next, stage);
        check_frames(frame_next, frames, stopped);
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// ntdll's DLL notification. Not in the SDK headers.
struct dll_notification
{
    ULONG flags;
    const UNICODE_STRING *full_name;
    const UNICODE_STRING *base_name;
    void *base;
    ULONG size;
};
using notification_function = void(CALLBACK *)(ULONG reason, const dll_notification *data, void *context);
using register_function = LONG(NTAPI *)(ULONG, notification_function, void *, void **);
using unregister_function = LONG(NTAPI *)(void *);

// Graphics and NVIDIA modules from the Windows folder are worth a line too. The rest of it is noise.
bool worth_logging(const std::wstring &lower)
{
    if (lower.compare(0, g_windows.size(), g_windows) != 0)
        return true;
    const std::wstring name = lower.substr(lower.find_last_of(L'\\') + 1);
    for (const wchar_t *part : {L"d3d", L"dxgi", L"nv", L"vulkan", L"amd", L"igx", L"xess", L"ffx", L"sl."})
        if (name.find(part) != std::wstring::npos)
            return true;
    return false;
}

// Runs under the loader lock. Only logs the path. A version read here could load more DLLs.
void CALLBACK on_dll(ULONG reason, const dll_notification *data, void *)
{
    if (data == nullptr || data->full_name == nullptr || data->full_name->Buffer == nullptr)
        return;
    const std::wstring path(data->full_name->Buffer, data->full_name->Length / sizeof(wchar_t));
    std::wstring lower = path;
    for (wchar_t &ch : lower)
        ch = wchar_t(std::towlower(ch));
    if (!worth_logging(lower))
        return;
    const std::string text = std::string(reason == 1 ? "DLL loaded: " : "DLL unloaded: ") +
                             utf8(path.c_str(), int(path.size()));
    acv_log(reshade::log::level::info, text.c_str());
}

const char *removed_reason(HRESULT result)
{
    switch (result)
    {
    case DXGI_ERROR_DEVICE_HUNG: return "the GPU hung (DXGI_ERROR_DEVICE_HUNG)";
    case DXGI_ERROR_DEVICE_REMOVED: return "the GPU was removed or the driver crashed (DXGI_ERROR_DEVICE_REMOVED)";
    case DXGI_ERROR_DEVICE_RESET: return "a bad command reset the GPU (DXGI_ERROR_DEVICE_RESET)";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "the driver failed (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
    case DXGI_ERROR_INVALID_CALL: return "an invalid call (DXGI_ERROR_INVALID_CALL)";
    default: return nullptr;
    }
}
} // namespace

void diag_start()
{
    if (!g_settings.debug)
        return;
    wchar_t windows[MAX_PATH] = L"";
    GetWindowsDirectoryW(windows, MAX_PATH);
    g_windows = windows;
    for (wchar_t &ch : g_windows)
        ch = wchar_t(std::towlower(ch));
    if (!g_windows.empty() && g_windows.back() != L'\\')
        g_windows += L'\\';

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (auto reg = reinterpret_cast<register_function>(GetProcAddress(ntdll, "LdrRegisterDllNotification")))
        reg(0, on_dll, nullptr, &g_notify_cookie);
    acv_log(reshade::log::level::info, "detailed logging: start-up stages, a watchdog for hangs and later DLL loads");
}

void diag_stop(bool exiting)
{
    if (g_notify_cookie != nullptr)
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (auto unreg = reinterpret_cast<unregister_function>(GetProcAddress(ntdll, "LdrUnregisterDllNotification")))
            unreg(g_notify_cookie);
        g_notify_cookie = nullptr;
    }
    if (exiting)
        log_exit();
}

void diag_device_created()
{
    g_device_thread = GetCurrentThreadId();
    g_device_time = now_ms();
    if (!g_settings.debug || g_watching.exchange(true))
        return;
    // A reference of its own. It is dropped as the thread exits.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&watchdog), &self);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (HANDLE thread = CreateThread(nullptr, 0, watchdog, self, 0, nullptr))
        CloseHandle(thread);
    else
        FreeLibrary(self);
}

void diag_device_destroyed()
{
    g_device_time = 0;
    if (g_stop != nullptr)
        SetEvent(g_stop);
}

void diag_present(ID3D12Device *device)
{
    const uint64_t now = now_ms();
    const bool first = g_frames.fetch_add(1) == 0;
    if (first)
        fg_report_start();
    if (first && g_settings.debug)
    {
        char text[96];
        snprintf(text, sizeof(text), "first frame %.1f s after the device was made",
                 double(now - g_device_time.load()) / 1000.0);
        acv_log(reshade::log::level::info, text);
    }
    g_last_frame = now;
    g_present_thread = GetCurrentThreadId();

    if (device == nullptr || g_removed_logged.load())
        return;
    const HRESULT result = device->GetDeviceRemovedReason();
    if (SUCCEEDED(result) || g_removed_logged.exchange(true))
        return;
    const char *reason = removed_reason(result);
    char text[192];
    if (reason != nullptr)
        snprintf(text, sizeof(text), "the GPU device was lost: %s", reason);
    else
        snprintf(text, sizeof(text), "the GPU device was lost (0x%08lX)", static_cast<unsigned long>(result));
    acv_log(reshade::log::level::error, text);
}

diag_stage::diag_stage(const char *name)
{
    if (!g_settings.debug)
        return;
    _start = now_ms();
    g_stage_thread = GetCurrentThreadId();
    g_stage_start = _start;
    g_stage = name;
    acv_log(reshade::log::level::info, (std::string("starting: ") + name).c_str());
}

diag_stage::~diag_stage()
{
    if (_start == 0)
        return;
    const char *name = g_stage.exchange(nullptr);
    char text[128];
    snprintf(text, sizeof(text), "%s took %llu ms", name != nullptr ? name : "a stage",
             static_cast<unsigned long long>(now_ms() - _start));
    acv_log(reshade::log::level::info, text);
}
