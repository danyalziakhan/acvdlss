#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <reshade.hpp>

#include "ngx_guard.h"
#include "acvdlss.h"

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <system_error>

namespace
{
// No sharing: nothing else can open or load the file until the handle closes.
HANDLE hold(const std::filesystem::path &path)
{
    return CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool is_dlss5(const std::filesystem::path &file)
{
    std::wstring name = file.filename().wstring();
    for (wchar_t &ch : name)
        ch = wchar_t(std::towlower(ch));
    return name.rfind(L"nvngx_dlssnr", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, L".dll") == 0;
}

std::filesystem::path known_folder(const KNOWNFOLDERID &id)
{
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
        result = path;
    CoTaskMemFree(path);
    return result;
}
} // namespace

ngx_guard::ngx_guard(const std::wstring &game_folder)
{
    std::error_code error;
    const auto hold_dlss5_in = [&](const std::filesystem::path &folder) {
        for (const auto &entry : std::filesystem::directory_iterator(folder, error))
            if (entry.is_regular_file(error) && is_dlss5(entry.path()))
                if (HANDLE file = hold(entry.path()); file != INVALID_HANDLE_VALUE)
                    _held.push_back(file);
    };

    hold_dlss5_in(game_folder);

    // In case a driver ships DLSS 5 in its own folder.
    const std::filesystem::path repository = known_folder(FOLDERID_System) / L"DriverStore" / L"FileRepository";
    for (const auto &entry : std::filesystem::directory_iterator(repository, error))
        if (entry.is_directory(error) && entry.path().filename().wstring().rfind(L"nv_dispi.inf", 0) == 0)
            hold_dlss5_in(entry.path() / L"ngx-builtins");

    // Downloaded by the driver.
    const std::filesystem::path models = known_folder(FOLDERID_ProgramData) / L"NVIDIA" / L"NGX" / L"models" / L"dlssnr";
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             models, std::filesystem::directory_options::skip_permission_denied, error))
        if (entry.is_regular_file(error))
            if (HANDLE file = hold(entry.path()); file != INVALID_HANDLE_VALUE)
                _held.push_back(file);

    if (!_held.empty())
    {
        char text[160];
        snprintf(text, sizeof(text),
                 "kept %zu DLSS 5 (neural rendering) file%s out of ACV DLSS's own DLSS start-up on purpose. Other "
                 "add-ons still load DLSS 5 for themselves",
                 _held.size(), _held.size() == 1 ? "" : "s");
        acv_log(reshade::log::level::info, text);
    }
}

ngx_guard::~ngx_guard()
{
    for (void *file : _held)
        CloseHandle(file);
}
