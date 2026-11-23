// acvdlss.log next to the game's exe. Flushed per line so a crash keeps everything before it.

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
#include <cctype>
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
// Set while this thread walks another thread's stack. A fault there is expected and not the game's.
thread_local bool t_walking = false;

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

// The file's description, or its product name, or empty. Wine's own DLLs and vkd3d-proton name themselves
// there.
static std::string file_description(const std::wstring &path)
{
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0)
        return {};
    std::string data(size, '\0');
    struct translation
    {
        WORD language, code_page;
    } *list = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&list), &length) ||
        list == nullptr || length < sizeof(translation))
        return {};
    for (const wchar_t *field : {L"FileDescription", L"ProductName"})
    {
        wchar_t key[96];
        swprintf(key, std::size(key), L"\\StringFileInfo\\%04x%04x\\%ls", list->language, list->code_page, field);
        wchar_t *value = nullptr;
        if (VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&value), &length) && value != nullptr &&
            length > 1)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
            std::string text(size_t(n > 0 ? n - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), n, nullptr, nullptr);
            return text;
        }
    }
    return {};
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

// The exe has no version resource. Steam copies get the build ID from the app manifest.
// Others get the exe's date.
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

static std::string utf8(const wchar_t *text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// First line of a Linux file through Wine's Z: drive, or empty.
static std::string linux_first_line(const std::wstring &unix_path)
{
    std::wstring path = L"Z:" + unix_path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    FILE *file = _wfopen(path.c_str(), L"rb");
    if (file == nullptr)
        return {};
    char text[256] = "";
    const bool read = fgets(text, int(std::size(text)), file) != nullptr;
    fclose(file);
    std::string line = read ? text : "";
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

// Under Wine dxvk-nvapi reports a made-up driver (DXVK_NVAPI_DRIVER_VERSION). The kernel module has the real one.
// Proton's tool folder names its build. vkd3d-proton and dxvk-nvapi carry no version resource.
static void log_linux_versions()
{
    // "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.95.05  ..." or "... Open Kernel Module for x86_64  ..."
    const std::string module = linux_first_line(L"/proc/driver/nvidia/version");
    std::string driver;
    size_t at = module.find("Module");
    while (at != std::string::npos)
    {
        const size_t start = module.find_first_not_of(' ', at);
        if (start == std::string::npos)
            break;
        at = module.find(' ', start);
        const std::string word = module.substr(start, at == std::string::npos ? std::string::npos : at - start);
        if (isdigit(static_cast<unsigned char>(word[0])) && word.find('.') != std::string::npos)
        {
            driver = word;
            break;
        }
    }
    std::string line = "Linux side: NVIDIA driver " + (driver.empty() ? std::string("unknown") : driver);
    if (module.find("Open Kernel Module") != std::string::npos)
        line += " (open kernel module)";

    // Steam lists the Proton folder first. Launchers that use umu set PROTONPATH.
    std::wstring tool;
    for (const wchar_t *name : {L"STEAM_COMPAT_TOOL_PATHS", L"PROTONPATH"})
    {
        wchar_t value[1024] = L"";
        const DWORD n = GetEnvironmentVariableW(name, value, DWORD(std::size(value)));
        if (n == 0 || n >= std::size(value))
            continue;
        tool = value;
        tool = tool.substr(0, tool.find(L':'));
        while (!tool.empty() && tool.back() == L'/')
            tool.pop_back();
        if (!tool.empty())
            break;
    }
    if (!tool.empty())
    {
        // "1712345678 proton-9.0-4". The number is a build time.
        std::string version = linux_first_line(tool + L"/version");
        const size_t space = version.find(' ');
        if (space != std::string::npos && version.find_first_not_of("0123456789") == space)
            version = version.substr(space + 1);
        if (version.empty())
            version = utf8(tool.substr(tool.find_last_of(L'/') + 1).c_str());
        line += ", Proton " + version;
    }
    acv_log(reshade::log::level::info, line.c_str());
}

// The Windows version. Under Wine or Proton also the Wine version, the host and the settings that decide DLSS.
static void log_system()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    // GetVersionEx reports an older Windows to programs without a manifest.
    using get_version = LONG(WINAPI *)(OSVERSIONINFOW *);
    OSVERSIONINFOW version = {sizeof(version)};
    if (auto get = reinterpret_cast<get_version>(GetProcAddress(ntdll, "RtlGetVersion")))
        get(&version);
    char text[256];
    snprintf(text, sizeof(text), "system: Windows %lu.%lu build %lu", version.dwMajorVersion, version.dwMinorVersion,
             version.dwBuildNumber);
    std::string line = text;

    using wine_version = const char *(CDECL *)();
    using wine_host = void(CDECL *)(const char **system, const char **release);
    const auto wine = reinterpret_cast<wine_version>(GetProcAddress(ntdll, "wine_get_version"));
    if (wine != nullptr)
    {
        line += std::string(" under Wine ") + wine();
        const char *system = nullptr, *release = nullptr;
        if (auto host = reinterpret_cast<wine_host>(GetProcAddress(ntdll, "wine_get_host_version")))
            host(&system, &release);
        if (system != nullptr)
            line += std::string(" on ") + system + (release != nullptr ? std::string(" ") + release : "");
    }
    acv_log(reshade::log::level::info, line.c_str());
    if (wine == nullptr)
        return;

    // All of Proton's, dxvk-nvapi's and vkd3d-proton's switches. dxvk-nvapi's DRS ones can override the preset.
    // Values with a path are left out. They can hold a user name.
    line = "Wine settings:";
    if (wchar_t *block = GetEnvironmentStringsW())
    {
        for (const wchar_t *entry = block; *entry != L'\0'; entry += wcslen(entry) + 1)
        {
            bool wanted = false;
            for (const wchar_t *prefix : {L"PROTON_", L"DXVK_NVAPI", L"DXVK_ENABLE_NVAPI", L"VKD3D_",
                                          L"WINEDLLOVERRIDES=", L"WINEFSYNC=", L"WINEESYNC=", L"SteamDeck="})
                wanted |= wcsncmp(entry, prefix, wcslen(prefix)) == 0;
            if (!wanted)
                continue;
            const std::string text = utf8(entry);
            const size_t equals = text.find('=');
            if (equals != std::string::npos && text.find_first_of("/\\", equals) != std::string::npos)
                line += " " + text.substr(0, equals) + "=(a path)";
            else
                line += " " + text;
        }
        FreeEnvironmentStringsW(block);
    }
    acv_log(reshade::log::level::info,
            line == "Wine settings:" ? "Wine settings: none of the known ones set" : line.c_str());
    log_linux_versions();
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
    log_system();

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

// Return addresses up the stack from a context. Stops at a fault, which a running thread's stack can cause.
static int walk(const CONTEXT *start, DWORD64 *out, int max)
{
    CONTEXT context = *start;
    int n = 0;
    __try
    {
        while (n < max && context.Rip != 0)
        {
            out[n++] = context.Rip;
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
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame,
                             nullptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return n;
}

static std::string stack_text(const CONTEXT *context)
{
    DWORD64 at[24];
    t_walking = true;
    const int n = walk(context, at, 24);
    t_walking = false;
    std::string text;
    for (int i = 0; i < n; ++i)
        text += " " + where(at[i]);
    return text;
}

std::string thread_stack(unsigned long thread_id)
{
    if (thread_id == 0 || thread_id == GetCurrentThreadId())
        return " (unknown)";
    const HANDLE thread =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
    if (thread == nullptr)
        return " (the thread has ended)";
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    bool read = SuspendThread(thread) != DWORD(-1);
    if (read)
    {
        read = GetThreadContext(thread, &context) != 0;
        ResumeThread(thread);
    }
    CloseHandle(thread);
    if (!read)
        return " (could not be read)";
    // Walked after the resume. A thread that is stuck keeps its stack. Nothing that may lock runs while the
    // thread is suspended.
    return stack_text(&context);
}

static const char *fatal_name(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "division by zero";
    case STATUS_HEAP_CORRUPTION: return "heap corruption";
    default: return nullptr;
    }
}

// Wine writes this into the DOS stub of its own PE DLLs.
static bool wine_builtin(const std::wstring &path)
{
    FILE *file = _wfopen(path.c_str(), L"rb");
    if (file == nullptr)
        return false;
    char head[0x60] = {};
    const size_t n = fread(head, 1, sizeof(head), file);
    fclose(file);
    static const char MARK[] = "Wine builtin DLL";
    return n >= 0x40 + sizeof(MARK) - 1 && memcmp(head + 0x40, MARK, sizeof(MARK) - 1) == 0;
}

// The game's crash handler kills the process before anything reaches our log. Games also raise and handle
// these on purpose. Hence the cap and the wording.
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const char *name = fatal_name(code);
    if (name == nullptr || t_walking)
        return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> written{0};
    if (written++ >= 3)
        return EXCEPTION_CONTINUE_SEARCH;

    char text[192];
    const std::string site = where(DWORD64(info->ExceptionRecord->ExceptionAddress));
    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        const ULONG_PTR *params = info->ExceptionRecord->ExceptionInformation;
        const char *access = params[0] == 0 ? "reading" : params[0] == 1 ? "writing" : "executing";
        snprintf(text, sizeof(text), "access violation %s 0x%llx at %s (the crash site if the game crashed)",
                 access, static_cast<unsigned long long>(params[1]), site.c_str());
    }
    else
        snprintf(text, sizeof(text), "%s at %s (the crash site if the game crashed)", name, site.c_str());
    acv_log(reshade::log::level::error, text);
    // A stack overflow leaves no room to walk it.
    if (code != EXCEPTION_STACK_OVERFLOW)
        acv_log(reshade::log::level::error, ("call stack:" + stack_text(info->ContextRecord)).c_str());
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

    // What really runs D3D12 and NVAPI under ReShade: Windows' own, or vkd3d-proton and dxvk-nvapi under Wine.
    // d3d12.dll and dxgi.dll by name would be ReShade's. Those come from the system folder.
    wchar_t system[MAX_PATH] = L"";
    GetSystemDirectoryW(system, MAX_PATH);
    // Wine's own d3d12 has no DLSS support. DLSS needs vkd3d-proton and dxvk-nvapi there.
    const bool wine = running_under_wine();
    line = "runtime:";
    for (const wchar_t *name : {L"d3d12.dll", L"d3d12core.dll", L"dxgi.dll", L"nvapi64.dll"})
    {
        std::wstring path = std::wstring(system) + L"\\" + name;
        const bool proxied = wcscmp(name, L"d3d12.dll") == 0 || wcscmp(name, L"dxgi.dll") == 0;
        if (HMODULE loaded = GetModuleHandleW(name); loaded != nullptr && !proxied)
        {
            wchar_t found[MAX_PATH] = L"";
            GetModuleFileNameW(loaded, found, MAX_PATH);
            path = found;
        }
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        const std::string version = file_version(path), description = file_description(path);
        snprintf(text, sizeof(text), " %ls %s%s%s%s%s;", path.c_str(), version.empty() ? "no version" : version.c_str(),
                 description.empty() ? "" : " (", description.c_str(), description.empty() ? "" : ")",
                 wine && wine_builtin(path) ? " Wine builtin" : "");
        line += text;
    }
    if (line.back() == ';')
        line.pop_back();
    acv_log(reshade::log::level::info, line.c_str());
}

// Bug reports often come down to another upscaler, an old nvngx_dlss.dll or a second DLSS-NR.
void log_modules()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    std::wstring folder = game_folder();
    for (wchar_t &ch : folder)
        ch = wchar_t(towlower(ch));
    wchar_t windows_path[MAX_PATH] = L"";
    GetWindowsDirectoryW(windows_path, MAX_PATH);
    std::wstring windows = windows_path;
    for (wchar_t &ch : windows)
        ch = wchar_t(towlower(ch));
    std::string in_folder, elsewhere, others;
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
        // Overlays, recorders and other injected tools. Names only: their folders can carry a user name.
        if (!ours && !ngx && lower.compare(0, windows.size(), windows) != 0)
        {
            const std::string version = file_version(path);
            char item[MAX_PATH + 64];
            snprintf(item, sizeof(item), "%s%ls%s%s", others.empty() ? "" : ", ", entry.szModule,
                     version.empty() ? "" : " ", version.c_str());
            others += item;
            continue;
        }
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
    acv_log(reshade::log::level::info, ("other modules: " + (others.empty() ? std::string("none") : others)).c_str());
}

bool running_under_wine()
{
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

void log_wine_dlss_help()
{
    wchar_t system[MAX_PATH] = L"";
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring nvapi = std::wstring(system) + L"\\nvapi64.dll";
    const char *state = GetModuleHandleW(L"nvapi64.dll") != nullptr                       ? "loaded"
                        : GetFileAttributesW(nvapi.c_str()) != INVALID_FILE_ATTRIBUTES ? "in the system folder"
                                                                                         : "missing";
    const std::string text = std::string("under Wine DLSS needs vkd3d-proton and dxvk-nvapi. nvapi64.dll is ") +
                             state + ". Proton turns dxvk-nvapi on with PROTON_ENABLE_NVAPI=1. Plain Wine uses "
                                     "DXVK_ENABLE_NVAPI=1";
    acv_log(reshade::log::level::info, text.c_str());
}

void log_exit()
{
    // Other threads are gone at process exit. One may have died holding the lock.
    if (!g_lock.try_lock())
        return;
    if (g_file != nullptr)
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        fprintf(g_file, "%02u:%02u:%02u:%03u [%5lu] | INFO  | the game is closing normally\n", now.wHour, now.wMinute,
                now.wSecond, now.wMilliseconds, GetCurrentThreadId());
        fflush(g_file);
    }
    g_lock.unlock();
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
