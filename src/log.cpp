// acvdlss.log next to the game's exe. Flushed per line so a crash keeps
// everything before it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "acvdlss.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

namespace
{
const char PREFIX[] = "ACV DLSS: ";

std::mutex g_lock;
FILE *g_file = nullptr;
bool g_opened = false;

FILE *log_file()
{
    if (!g_opened)
    {
        g_opened = true;
        g_file = _wfopen((game_folder() + L"\\acvdlss.log").c_str(), L"w");
    }
    return g_file;
}
} // namespace

std::string file_version(const std::wstring &path)
{
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0)
        return {};
    std::string data(size, '\0');
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || info == nullptr)
        return {};
    char text[32];
    snprintf(text, sizeof(text), "%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
             HIWORD(info->dwFileVersionLS));
    return text;
}

static std::string module_path(HMODULE module)
{
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string text(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, text.data(), n, nullptr, nullptr);
    return text;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info);

static std::string acf_value(const std::string &text, const char *key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t at = text.find(quoted);
    if (at == std::string::npos)
        return {};
    at = text.find('"', at + quoted.size());
    const size_t end = at == std::string::npos ? at : text.find('"', at + 1);
    return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
}

// The exe has no version resource. Steam copies get the build ID from the app
// manifest. Others get the exe's date.
static std::string game_build()
{
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring path(exe);
    const size_t folder_end = path.find_last_of(L'\\');
    const size_t common_end = folder_end == std::wstring::npos ? folder_end : path.find_last_of(L'\\', folder_end - 1);
    if (common_end != std::wstring::npos)
    {
        const std::wstring folder = path.substr(common_end + 1, folder_end - common_end - 1);
        const std::wstring steamapps = path.substr(0, common_end) + L"\\..";
        WIN32_FIND_DATAW found;
        const HANDLE search = FindFirstFileW((steamapps + L"\\appmanifest_*.acf").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE)
        {
            std::string build;
            do
            {
                FILE *file = _wfopen((steamapps + L"\\" + found.cFileName).c_str(), L"rb");
                if (file == nullptr)
                    continue;
                std::string text(size_t(std::min<DWORD>(found.nFileSizeLow, 1 << 16)), '\0');
                text.resize(fread(text.data(), 1, text.size(), file));
                fclose(file);
                const std::string dir = acf_value(text, "installdir");
                const int n = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), int(dir.size()), nullptr, 0);
                std::wstring wide(size_t(n), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), int(dir.size()), wide.data(), n);
                if (!wide.empty() && _wcsicmp(wide.c_str(), folder.c_str()) == 0)
                    build = acf_value(text, "buildid");
            } while (build.empty() && FindNextFileW(search, &found));
            FindClose(search);
            if (!build.empty())
                return "Steam build " + build;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    SYSTEMTIME date;
    if (!GetFileAttributesExW(exe, GetFileExInfoStandard, &data) || !FileTimeToSystemTime(&data.ftLastWriteTime, &date))
        return "unknown";
    char text[48];
    snprintf(text, sizeof(text), "exe dated %04u-%02u-%02u", date.wYear, date.wMonth, date.wDay);
    return text;
}

void log_start()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&log_start), &self);
    const auto version = [](HMODULE module) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(module, path, MAX_PATH);
        const std::string text = file_version(path);
        return text.empty() ? std::string("unknown") : text;
    };
    std::string text = "ACV DLSS " + version(self) + ", ReShade " +
                       version(reshade::internal::get_reshade_module_handle()) + ", game " + game_build() + " at " +
                       module_path(nullptr);
    acv_log(reshade::log::level::info, text.c_str());
    AddVectoredExceptionHandler(0, on_exception);

    wchar_t documents[MAX_PATH] = L"";
    if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents) != S_OK)
        return;
    const std::wstring ini = std::wstring(documents) + L"\\Assassin's Creed Valhalla\\ACValhalla.ini";
    text = "game settings:";
    for (const wchar_t *key : {L"FullscreenWidth", L"FullscreenHeight", L"WindowMode", L"PixelDensity",
                               L"UpsampleType", L"FSRQuality", L"AdaptiveQuality", L"Antialiasing", L"HDREnabled"})
    {
        wchar_t value[32] = L"";
        GetPrivateProfileStringW(L"Graphics", key, L"?", value, DWORD(std::size(value)), ini.c_str());
        char line[96];
        snprintf(line, sizeof(line), " %ls=%ls", key, value);
        text += line;
    }
    acv_log(reshade::log::level::info, text.c_str());
}

// "module+0x1234", or the bare address outside any module.
static std::string where(DWORD64 address)
{
    HMODULE module = nullptr;
    char text[160];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr)
    {
        snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t *name = wcsrchr(path, L'\\');
    snprintf(text, sizeof(text), "%ls+0x%llx", name != nullptr ? name + 1 : path,
             static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    return text;
}

// The game's crash handler kills the process before anything reaches our log.
// Games also raise and handle these on purpose. Hence the cap and the wording.
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> written{0};
    if (written++ >= 3)
        return EXCEPTION_CONTINUE_SEARCH;

    const ULONG_PTR *params = info->ExceptionRecord->ExceptionInformation;
    const char *access = params[0] == 0 ? "reading" : params[0] == 1 ? "writing" : "executing";
    char text[160];
    snprintf(text, sizeof(text), "access violation %s 0x%llx at %s (if the game crashed, this is where)", access,
             static_cast<unsigned long long>(params[1]), where(DWORD64(info->ExceptionRecord->ExceptionAddress)).c_str());
    acv_log(reshade::log::level::error, text);

    CONTEXT context = *info->ContextRecord;
    std::string stack = "call stack:";
    for (int i = 0; i < 24 && context.Rip != 0; ++i)
    {
        stack += " " + where(context.Rip);
        DWORD64 image = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (function == nullptr)
        {
            // Leaf function. The return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void *handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame, nullptr);
    }
    acv_log(reshade::log::level::error, stack.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

void log_adapter(ID3D12Device *device)
{
    IDXGIFactory4 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;
    IDXGIAdapter1 *adapter = nullptr;
    factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    factory->Release();
    if (adapter == nullptr)
        return;
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    LARGE_INTEGER driver = {};
    const bool known = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
    adapter->Release();

    // Windows numbers NVIDIA's driver 621.14 as 32.0.16.2114.
    char text[256];
    const unsigned part3 = HIWORD(driver.LowPart), part4 = LOWORD(driver.LowPart);
    const unsigned nvidia = (part3 % 10) * 10000 + part4;
    snprintf(text, sizeof(text), "GPU: %ls, %llu MB, driver %s", desc.Description,
             static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20), known ? "" : "unknown");
    std::string line = text;
    if (known)
    {
        snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(driver.HighPart), LOWORD(driver.HighPart), part3, part4);
        line += text;
        if (desc.VendorId == 0x10DE)
        {
            snprintf(text, sizeof(text), " (NVIDIA %u.%02u)", nvidia / 100, nvidia % 100);
            line += text;
        }
    }
    acv_log(reshade::log::level::info, line.c_str());
}

// Most bug reports come down to another upscaler, an old nvngx_dlss.dll or a
// second DLSS-NR.
void log_modules()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    std::wstring folder = game_folder();
    for (wchar_t &ch : folder)
        ch = wchar_t(towlower(ch));
    std::string in_folder, elsewhere;
    MODULEENTRY32W entry = {sizeof(entry)};
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
    {
        std::wstring path = entry.szExePath;
        std::wstring lower = path;
        for (wchar_t &ch : lower)
            ch = wchar_t(towlower(ch));
        const std::wstring name = lower.substr(lower.find_last_of(L'\\') + 1);
        const bool ours = lower.compare(0, folder.size(), folder) == 0 && name != L"acvalhalla.exe";
        const bool ngx = name.find(L"nvngx") != std::wstring::npos;
        if (!ours && !ngx)
            continue;
        const std::string version = file_version(path);
        char item[MAX_PATH + 64];
        snprintf(item, sizeof(item), "%s%ls%s%s", ours ? (in_folder.empty() ? "" : ", ") : "\n  ",
                 ours ? entry.szModule : entry.szExePath, version.empty() ? "" : " ", version.c_str());
        (ours ? in_folder : elsewhere) += item;
    }
    CloseHandle(snapshot);
    acv_log(reshade::log::level::info, ("loaded from the game folder: " + in_folder).c_str());
    if (!elsewhere.empty())
        acv_log(reshade::log::level::info, ("NGX modules elsewhere:" + elsewhere).c_str());
}

std::wstring game_folder()
{
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::wstring exe(path);
    return exe.substr(0, exe.find_last_of(L"\\/"));
}

void acv_log(reshade::log::level level, const char *message)
{
    const bool prefixed = strncmp(message, PREFIX, sizeof(PREFIX) - 1) == 0;
    if (level == reshade::log::level::error || level == reshade::log::level::warning)
        reshade::log::message(level, prefixed ? message : (std::string(PREFIX) + message).c_str());

    const char *text = prefixed ? message + sizeof(PREFIX) - 1 : message;
    const char *tag = level == reshade::log::level::error     ? "ERROR"
                      : level == reshade::log::level::warning ? "WARN "
                                                              : "INFO ";
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::lock_guard lock(g_lock);
    if (FILE *file = log_file())
    {
        fprintf(file, "%02u:%02u:%02u:%03u [%5lu] | %s | %s\n", now.wHour, now.wMinute, now.wSecond,
                now.wMilliseconds, GetCurrentThreadId(), tag, text);
        fflush(file);
    }
}
