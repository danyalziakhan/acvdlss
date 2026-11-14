#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "scan.h"

#include <algorithm>
#include <cstring>

namespace
{
// PAGE_EXECUTE alone isn't readable.
bool readable_code(DWORD protect)
{
    return (protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
           (protect & PAGE_GUARD) == 0;
}

bool matches(const uint8_t *p, const int16_t *pattern, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        if (pattern[i] >= 0 && p[i] != uint8_t(pattern[i]))
            return false;
    return true;
}
} // namespace

uint8_t *scan_game_code(const int16_t *pattern, size_t size, int &count)
{
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    uint8_t *const end = base + nt->OptionalHeader.SizeOfImage;
    const uint8_t first = uint8_t(pattern[0]); // patterns start with a fixed byte

    uint8_t *found = nullptr;
    count = 0;
    MEMORY_BASIC_INFORMATION region;
    for (uint8_t *at = base; at < end && VirtualQuery(at, &region, sizeof(region)) == sizeof(region);
         at = static_cast<uint8_t *>(region.BaseAddress) + region.RegionSize)
    {
        if (region.State != MEM_COMMIT || !readable_code(region.Protect))
            continue;
        uint8_t *from = static_cast<uint8_t *>(region.BaseAddress);
        uint8_t *to = std::min(from + region.RegionSize, end);
        for (uint8_t *p = from; p + size <= to; ++p)
        {
            p = static_cast<uint8_t *>(memchr(p, first, size_t(to - p)));
            if (p == nullptr || p + size > to)
                break;
            if (matches(p, pattern, size))
            {
                found = p;
                ++count;
            }
        }
    }
    return count == 1 ? found : nullptr;
}
