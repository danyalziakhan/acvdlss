#pragma once

#include <string>
#include <vector>

// Keeps DLSS 5 out of our NGX startup. NGX sets up every feature it finds on init. A DLSS 5 already set up
// on our device crashed the next user of it inside ReShade (RTX 50, official DLLs). Files held open without
// sharing get skipped like an unsigned DLL.
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
