// Frame generation report (see fg_report.h).
//
// Runs on a thread of its own, never on the game's: the signature check can take a while. The thread holds a
// reference to this DLL and drops it as it exits, so it never runs code from an unloaded add-on.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include "acvdlss.h"
#include "fg_report.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> g_started{false};

std::string utf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring text)
{
    for (wchar_t &ch : text)
        ch = wchar_t(std::towlower(ch));
    return text;
}

std::wstring file_name(const std::wstring &path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::string file_version(const std::wstring &path)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
        return "no version";
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || info == nullptr)
        return "no version";
    char text[48];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
             HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
}

// The signer of a file's embedded signature, if Windows accepts it. Empty when unsigned or not trusted.
// No revocation check: that would go online.
std::wstring signer(const std::wstring &path)
{
    WINTRUST_FILE_INFO file = {sizeof(file)};
    file.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data = {sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    std::wstring name;
    if (WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == ERROR_SUCCESS)
    {
        CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR *sgnr = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT *cert = sgnr ? WTHelperGetProvCertFromChain(sgnr, 0) : nullptr;
        if (cert != nullptr && cert->pCert != nullptr)
        {
            wchar_t text[256] = L"";
            CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, text, 256);
            name = text;
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return name;
}

// Loaded modules that belong to frame generation: NVIDIA's DLSS frame generation (nvngx_dlssg and anything named
// like it, so replacements show too), Streamline, AMD's and Intel's frame generation libraries.
bool frame_generation_module(const std::wstring &name)
{
    return name.find(L"dlssg") != std::wstring::npos || name.compare(0, 3, L"sl.") == 0 ||
           name.find(L"framegeneration") != std::wstring::npos || name.find(L"xess_fg") != std::wstring::npos ||
           name.find(L"xell") != std::wstring::npos;
}

// DLLs in the game folder under names that Windows would otherwise load from System32: how ReShade and
// OptiScaler get in.
bool proxy_name(const std::wstring &name)
{
    for (const wchar_t *proxy : {L"dxgi.dll", L"winmm.dll", L"version.dll", L"d3d12.dll", L"dbghelp.dll",
                                 L"wininet.dll", L"winhttp.dll", L"optiscaler.dll", L"reshade64.dll"})
        if (name == proxy)
            return true;
    return false;
}

std::string report()
{
    const std::wstring game = lower(game_folder()) + L"\\";
    std::string text;

    const std::wstring ini = game_folder() + L"\\OptiScaler.ini";
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        wchar_t input[32] = L"", output[32] = L"";
        GetPrivateProfileStringW(L"FrameGen", L"FGInput", L"", input, 32, ini.c_str());
        GetPrivateProfileStringW(L"FrameGen", L"FGOutput", L"", output, 32, ini.c_str());
        text += "frame generation: OptiScaler.ini found, FGInput = " + utf8(*input ? input : L"(not set)") +
                ", FGOutput = " + utf8(*output ? output : L"(not set)") + "\n";
    }
    else
        text += "frame generation: no OptiScaler.ini in the game folder\n";

    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return text + "frame generation: the module list could not be read\n";
    std::vector<std::wstring> proxies, found;
    for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024); ++i)
    {
        wchar_t path[MAX_PATH] = L"";
        if (GetModuleFileNameW(modules[i], path, MAX_PATH) == 0)
            continue;
        const std::wstring full = path, low = lower(full), name = file_name(low);
        if (low.compare(0, game.size(), game) == 0 && proxy_name(name))
            proxies.push_back(file_name(full));
        if (frame_generation_module(name))
            found.push_back(full);
    }
    std::string list;
    for (const std::wstring &p : proxies)
        list += (list.empty() ? "" : ", ") + utf8(p);
    text += "frame generation: loader DLLs in the game folder: " + (list.empty() ? std::string("none") : list) + "\n";
    if (found.empty())
        text += "frame generation: no frame generation libraries are loaded\n";
    for (const std::wstring &path : found)
    {
        const std::wstring who = signer(path);
        std::string line = "frame generation: loaded " + utf8(file_name(path)) + " " + file_version(path) + ", ";
        if (who.empty())
            line += "not signed";
        else
            line += "signed by " + utf8(who);
        const std::wstring name = lower(file_name(path));
        if (name.find(L"dlssg") != std::wstring::npos)
            line += lower(who).find(L"nvidia") != std::wstring::npos
                        ? " (signed by NVIDIA; a loader DLL in the game folder, listed above, can still redirect it)"
                        : " (not NVIDIA's: a replacement library)";
        line += ", from " + utf8(path) + "\n";
        text += line;
    }
    return text;
}

// Two reports, both away from loading: the first once DLSS has run for 60 s in a row (in game, past the loading
// screens), or after 5 minutes if it never does (DLSS off). The second 4 minutes later, only when something changed.
DWORD WINAPI worker(void *self)
{
    // Lowest CPU, disk and memory priority: the game's own threads and streaming always go first.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    std::string last;
    // One status read a second (two short locks). No work on the game's threads.
    int running_for = 0;
    for (int waited = 0; waited < 300 && running_for < 60; ++waited)
    {
        Sleep(1000);
        running_for = taa_get_status().state == taa_state::running ? running_for + 1 : 0;
    }
    for (const DWORD wait : {0u, 240000u})
    {
        Sleep(wait);
        const std::string now = report();
        if (now == last)
            continue;
        if (!last.empty())
            acv_log(reshade::log::level::info, "frame generation: changed since the last report");
        size_t start = 0;
        while (start < now.size())
        {
            const size_t end = now.find('\n', start);
            acv_log(reshade::log::level::info, now.substr(start, end - start).c_str());
            start = end == std::string::npos ? now.size() : end + 1;
        }
        last = now;
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(self), 0);
}
} // namespace

void fg_report_start()
{
    if (g_started.exchange(true))
        return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &self);
    if (HANDLE thread = CreateThread(nullptr, 0, worker, self, 0, nullptr))
        CloseHandle(thread);
    else
        FreeLibrary(self);
}
