#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "acvdlss.h"
#include "config.h"
#include "default_ini.h" // acvdlss.ini from the repository, built in

#include <cstdio>

namespace
{
std::wstring g_path;
bool g_took_over = false;


std::wstring wide(const char *text)
{
    wchar_t out[256];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, 256);
    return out;
}

// Earlier versions kept settings in ReShade.ini's [ACVDLSS] section.
void take_over_reshade_ini()
{
    static const struct
    {
        const char *old_key, *section, *key;
    } MOVED[] = {
        {"Enabled", "General", "Enabled"},       {"ToggleKey", "General", "ToggleKey"},
        {"Preset", "DLSS", "Preset"},            {"Sharpness", "DLSS", "Sharpness"},
        {"Reflex", "Latency", "Reflex"},         {"HudlessImage", "OptiScaler", "HudlessImage"},
        {"Debug", "Diagnostics", "Debug"},
    };
    int moved = 0;
    for (const auto &m : MOVED)
    {
        char value[64] = "";
        size_t size = sizeof(value);
        if (!reshade::get_config_value(nullptr, "ACVDLSS", m.old_key, value, &size))
            continue;
        config_set(m.section, m.key, value);
        reshade::set_config_value(nullptr, "ACVDLSS", m.old_key, static_cast<const char *>(nullptr));
        ++moved;
    }
    g_took_over = moved != 0;
}
} // namespace

void config_open()
{
    g_path = game_folder() + L"\\acvdlss.ini";
    // Normally shipped in the zip.
    if (GetFileAttributesW(g_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (FILE *file = _wfopen(g_path.c_str(), L"wb"))
        {
            fwrite(DEFAULT_INI, 1, sizeof(DEFAULT_INI) - 1, file);
            fclose(file);
        }
    }
    take_over_reshade_ini();
}

bool config_took_over()
{
    return g_took_over;
}

int config_int(const char *section, const char *key, int fallback)
{
    return int(GetPrivateProfileIntW(wide(section).c_str(), wide(key).c_str(), fallback, g_path.c_str()));
}

std::string config_string(const char *section, const char *key, const char *fallback)
{
    wchar_t value[64] = L"";
    GetPrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(fallback).c_str(), value, 64,
                             g_path.c_str());
    char out[64];
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

void config_set(const char *section, const char *key, const char *value)
{
    WritePrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(value).c_str(), g_path.c_str());
}

void config_set(const char *section, const char *key, int value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    config_set(section, key, text);
}
