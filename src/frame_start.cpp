// The game's ::Engine::BeginFrame (Steam build 26142239, RVA 0x10f8eb0) runs once a frame on one thread.
// The game reads input 0.03 ms after it. It starts with:
//
//   48 83 EC 28      sub rsp, 28h
//   80 79 09 00      cmp byte [rcx + 9], 0
//   75 05            jne ...
//   E8 ...           call ...
//
// Those first 8 bytes become a jump to a stub. The stub saves the registers a call may change and calls
// reflex_frame_start(). Then it runs the two replaced instructions and jumps back to entry + 8. The cmp runs
// there because the jne needs its flags. The entry is 8-byte aligned. One atomic store swaps all 8 bytes and
// no thread can run half old and half new code.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "acvdlss.h"
#include "frame_start.h"
#include "reflex.h"
#include "scan.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

#ifdef ACVDLSS_PLAIN

// This build doesn't change the game's code. Reflex waits after Present.
void frame_start_hook() {}
bool frame_start_hooked()
{
    return false;
}

#else

namespace
{
// -1 marks a byte that differs between builds (an address).
const int16_t BEGIN_FRAME[] = {0x48, 0x83, 0xEC, 0x28, 0x80, 0x79, 0x09, 0x00, 0x75, 0x05, 0xE8, -1,
                               -1,   -1,   -1,   0x48, 0x8D, 0x0D, -1,   -1,   -1,   -1,   0xE8, -1,
                               -1,   -1,   -1,   0x8B, 0x05, -1,   -1,   -1,   -1,   0x48, 0x8D, 0x0D,
                               -1,   -1,   -1,   -1,   0xFF, 0xC8, 0x83, 0xE0, 0x01, 0x89, 0x05};
const size_t REPLACED = 8;

std::atomic<bool> g_hooked{false};

extern "C" void frame_start_callback()
{
    reflex_frame_start();
}

// Executable memory below the entry within rel32 jump range (2 GB).
// The first tries land inside the game's image and fail.
uint8_t *alloc_near(const uint8_t *entry)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t step = info.dwAllocationGranularity;
    for (uintptr_t at = (uintptr_t(entry) & ~(step - 1)) - step; uintptr_t(entry) - at < 0x7FF00000; at -= step)
        if (void *p = VirtualAlloc(reinterpret_cast<void *>(at), 4096, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE))
            return static_cast<uint8_t *>(p);
    return nullptr;
}

// At the game's entry rsp is 8 below a 16-byte boundary. Seven pushes and 0x80 bytes leave it aligned for
// the call. The 0x20 bytes of shadow space sit below xmm0 to xmm5.
size_t write_stub(uint8_t *s, const uint8_t *entry)
{
    static const uint8_t save[] = {
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, // push rax rcx rdx r8 r9 r10 r11
        0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                         // sub rsp, 80h
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, // movdqu [rsp+20h..70h], xmm0..5
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
        0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70};
    static const uint8_t restore[] = {
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, // movdqu xmm0..5, [rsp+20h..70h]
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
        0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
        0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00,                         // add rsp, 80h
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58}; // pop r11 r10 r9 r8 rdx rcx rax
    size_t n = 0;
    memcpy(s + n, save, sizeof(save));
    n += sizeof(save);
    s[n++] = 0x48; // mov rax, frame_start_callback
    s[n++] = 0xB8;
    const uint64_t callback = uint64_t(&frame_start_callback);
    memcpy(s + n, &callback, 8);
    n += 8;
    s[n++] = 0xFF; // call rax
    s[n++] = 0xD0;
    memcpy(s + n, restore, sizeof(restore));
    n += sizeof(restore);
    memcpy(s + n, entry, REPLACED); // sub rsp, 28h ; cmp byte [rcx + 9], 0
    n += REPLACED;
    s[n++] = 0xFF; // jmp [rip], back to entry + 8
    s[n++] = 0x25;
    memset(s + n, 0, 4);
    n += 4;
    const uint64_t back = uint64_t(entry + REPLACED);
    memcpy(s + n, &back, 8);
    n += 8;
    return n;
}

void hook()
{
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    int count = 0;
    uint8_t *entry = scan_game_code(BEGIN_FRAME, std::size(BEGIN_FRAME), count);
    if (entry == nullptr || (uintptr_t(entry) & 7) != 0)
    {
        acv_log(reshade::log::level::warning,
                count == 0  ? "Reflex: the game's frame start was not found in this build. Reflex waits after Present"
                : count > 1 ? "Reflex: the game's frame start was found more than once. Reflex waits after Present"
                            : "Reflex: the game's frame start is not aligned as expected. Reflex waits after Present");
        return;
    }
    uint8_t *stub = alloc_near(entry);
    if (stub == nullptr)
    {
        acv_log(reshade::log::level::warning, "Reflex: no memory near the game's code. Reflex waits after Present");
        return;
    }
    write_stub(stub, entry);
    FlushInstructionCache(GetCurrentProcess(), stub, 4096);

    uint8_t patch[REPLACED] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90}; // jmp stub ; nop x3
    const int32_t rel = int32_t(stub - (entry + 5));
    memcpy(patch + 1, &rel, 4);
    LONG64 value;
    memcpy(&value, patch, REPLACED);
    DWORD old = 0;
    if (!VirtualProtect(entry, REPLACED, PAGE_EXECUTE_READWRITE, &old))
    {
        VirtualFree(stub, 0, MEM_RELEASE);
        acv_log(reshade::log::level::warning,
                "Reflex: could not make the game's frame start writable. Reflex waits after Present");
        return;
    }
    InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(entry), value);
    VirtualProtect(entry, REPLACED, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, REPLACED);
    g_hooked = true;

    char text[96];
    snprintf(text, sizeof(text), "Reflex: hooked the game's frame start at ACValhalla.exe+0x%llx",
             static_cast<unsigned long long>(entry - base));
    acv_log(reshade::log::level::info, text);
}

DWORD WINAPI hook_thread(void *)
{
    hook();
    return 0;
}
} // namespace

void frame_start_hook()
{
    // The game calls into this module from now on. It must never unload.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&frame_start_callback), &self))
        return;
    if (HANDLE thread = CreateThread(nullptr, 0, hook_thread, nullptr, 0, nullptr))
        CloseHandle(thread);
}

bool frame_start_hooked()
{
    return g_hooked.load(std::memory_order_relaxed);
}

#endif
