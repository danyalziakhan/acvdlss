#pragma once

#include <string>
#include <vector>

// Keeps DLSS 5 (neural rendering) out of our NGX startup. NGX loads every DLSS
// feature it finds on init, so a signed nvngx_dlssnr.dll got set up with our
// device first. RenoDX DLSS then crashed inside ReShade when it ran neural
// rendering (RTX 50, official DLLs). We only need super resolution, so the
// DLSS 5 files are held open without sharing during our startup. NGX skips
// them as it would an unsigned DLL, and RenoDX DLSS loads them afterwards.
class ngx_guard
{
public:
    explicit ngx_guard(const std::wstring &game_folder);
    ~ngx_guard();
    ngx_guard(const ngx_guard &) = delete;
    ngx_guard &operator=(const ngx_guard &) = delete;

private:
    std::vector<void *> _held; // file handles
};
