#pragma once

#include <string>
#include <vector>

// Keeps DLSS 5 (neural rendering) out of our own NGX start-up. NGX loads every
// DLSS feature it can find while it starts; a signed nvngx_dlssnr.dll accepted
// there is set up with our device first, and RenoDX DLSS running neural
// rendering later crashed inside ReShade (an RTX 50 with the official DLLs).
// We only need super resolution, so for the length of our start-up the DLSS 5
// files are held open without sharing: NGX cannot load them and carries on
// without, as it does for an unsigned one. RenoDX DLSS loads them afterwards.
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
