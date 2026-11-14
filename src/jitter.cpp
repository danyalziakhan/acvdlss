// The game picks its jitter sequence with a switch on a setting (Steam build 26142239, RVA 0xaa1f99):
//
//   48 8B C8                mov rcx, rax
//   8B 05 ?? ?? ?? ??       mov eax, [rip + ...]
//   48 FF C0 48 03 C0       inc rax ; add rax, rax
//   48 8B 84 C1 08 02 00 00 mov rax, [rcx + rax*8 + 0x208]
//   48 63 48 14             movsxd rcx, [rax + 0x14]     the setting
//   83 F9 0B 0F 87 ...      cmp ecx, 11 ; ja
//   48 8D 15 ...            lea rdx, [image base]
//   8B 8C 8A <table RVA>    mov ecx, [rdx + rcx*4 + table]
//   48 03 CA FF E1          add rcx, rdx ; jmp rcx
//
// Cases 1 to 4 mask the frame number with 3, 7, 15 and 31 and call the same generator. The game always takes
// case 2. Pointing its entry at case 3 or 4 gives 16 or 32 positions from the game's own code. The
// projection and TAA constants stay in sync. Nothing is patched unless every case matches.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "jitter.h"
#include "acvdlss.h"
#include "scan.h"

#ifdef ACVDLSS_PLAIN

// This build leaves the game's own 8 positions alone.
void jitter_start() {}
void jitter_apply(uint32_t, uint32_t) {}
void jitter_restore() {}
uint32_t jitter_positions()
{
    return 0;
}

#else

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{
// -1 marks a byte that differs between builds (an address).
const int16_t SWITCH[] = {
    0x48, 0x8B, 0xC8, 0x8B, 0x05, -1,   -1,   -1,   -1,   0x48, 0xFF, 0xC0, 0x48, 0x03, 0xC0, 0x48, 0x8B, 0x84,
    0xC1, 0x08, 0x02, 0x00, 0x00, 0x48, 0x63, 0x48, 0x14, 0x83, 0xF9, 0x0B, 0x0F, 0x87, -1,   -1,   -1,   -1,
    0x48, 0x8D, 0x15, -1,   -1,   -1,   -1,   0x8B, 0x8C, 0x8A, -1,   -1,   -1,   -1,   0x48, 0x03, 0xCA, 0xFF,
    0xE1};
const size_t SWITCH_SIZE = sizeof(SWITCH) / sizeof(SWITCH[0]);
const size_t LEA_AT = 36;   // lea rdx, [rip + disp]
const size_t TABLE_AT = 46; // disp32 of the table in mov ecx, [rdx + rcx*4 + disp]
const uint32_t CASES = 12;
const uint32_t GAME_CASE = 2;  // 8 positions
const uint32_t CASE_SIZE = 10; // and ebx, mask ; mov ecx, ebx ; call

std::atomic<uint32_t *> g_entry{nullptr};         // the table entry the game takes
uint32_t g_case8 = 0, g_case16 = 0, g_case32 = 0; // RVAs of the three cases
// The render and present threads both write the entry.
std::mutex g_write_lock;
uint32_t g_positions = 0; // under g_write_lock

// and ebx, mask ; mov ecx, ebx ; call generator
bool case_is(const uint8_t *base, uint32_t image_size, uint32_t rva, uint8_t mask, int32_t &call_target)
{
    if (uint64_t(rva) + CASE_SIZE > image_size)
        return false;
    const uint8_t *p = base + rva;
    if (p[0] != 0x83 || p[1] != 0xE3 || p[2] != mask || p[3] != 0x8B || p[4] != 0xCB || p[5] != 0xE8)
        return false;
    int32_t rel;
    memcpy(&rel, p + 6, 4);
    call_target = int32_t(rva + CASE_SIZE) + rel;
    return true;
}

void find()
{
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const uint32_t image_size = nt->OptionalHeader.SizeOfImage;

    int count = 0;
    uint8_t *found = scan_game_code(SWITCH, SWITCH_SIZE, count);
    if (found == nullptr)
    {
        acv_log(reshade::log::level::warning,
                count == 0 ? "jitter: the game's jitter code is not in this build. The game keeps its 8 positions"
                           : "jitter: the game's jitter code was found more than once. The game keeps its 8 positions");
        return;
    }

    // The table must be 4-byte aligned so one store swaps an entry.
    int32_t lea;
    memcpy(&lea, found + LEA_AT + 3, 4);
    uint32_t table_rva;
    memcpy(&table_rva, found + TABLE_AT, 4);
    bool ok = found + LEA_AT + 7 + lea == base && (table_rva & 3) == 0 &&
              uint64_t(table_rva) + CASES * 4 <= image_size;
    const auto *table = reinterpret_cast<const uint32_t *>(base + table_rva);
    int32_t call8 = 0, call16 = 0, call32 = 0;
    ok = ok && case_is(base, image_size, table[2], 0x07, call8) && case_is(base, image_size, table[3], 0x0F, call16) &&
         case_is(base, image_size, table[4], 0x1F, call32) && call8 == call16 && call16 == call32;
    if (!ok)
    {
        acv_log(reshade::log::level::warning,
                "jitter: the game's jitter code does not look as expected. The game keeps its 8 positions");
        return;
    }
    g_case8 = table[2];
    g_case16 = table[3];
    g_case32 = table[4];
    {
        std::lock_guard lock(g_write_lock);
        g_positions = 8;
    }
    g_entry = const_cast<uint32_t *>(table) + GAME_CASE;
    char text[96];
    snprintf(text, sizeof(text), "jitter: found the game's jitter code at ACValhalla.exe+0x%llx",
             static_cast<unsigned long long>(found - base));
    acv_log(reshade::log::level::info, text);
}

// Holds a module reference so the add-on can't unload mid-scan.
DWORD WINAPI find_thread(void *module)
{
    find();
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

void write_case(uint32_t positions)
{
    uint32_t *entry = g_entry;
    if (entry == nullptr)
        return;
    std::lock_guard lock(g_write_lock);
    if (g_positions == positions)
        return;
    const uint32_t target = positions == 32 ? g_case32 : positions == 16 ? g_case16 : g_case8;
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old))
    {
        acv_log(reshade::log::level::warning,
                "jitter: could not make the game's jump table writable. The game keeps its positions");
        return;
    }
    // Aligned 4-byte store. Game threads see either the old case or the new.
    reinterpret_cast<std::atomic<uint32_t> *>(entry)->store(target);
    VirtualProtect(entry, sizeof(*entry), old, &old);
    g_positions = positions;
    char text[64];
    snprintf(text, sizeof(text), "jitter: the game now cycles through %u positions", positions);
    acv_log(reshade::log::level::info, text);
}
} // namespace

void jitter_start()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&find_thread),
                            &module))
        return;
    if (HANDLE thread = CreateThread(nullptr, 0, find_thread, module, 0, nullptr))
        CloseHandle(thread);
    else
        FreeLibrary(module);
}

void jitter_apply(uint32_t render_width, uint32_t output_width)
{
    // A frame in flight when DLSS was switched off must not undo the restore.
    if (g_entry.load() == nullptr || render_width == 0 || !g_settings.enabled)
        return;
    if (const uint32_t forced = g_settings.jitter_positions; forced != 0)
    {
        write_case(forced);
        return;
    }
    const double scale = double(output_width) / double(render_width);
    const double wanted = 8.0 * scale * scale;
    write_case(wanted <= 8.0 + 1e-3 ? 8 : wanted <= 16.0 + 1e-3 ? 16 : 32);
}

void jitter_restore()
{
    write_case(8);
}

uint32_t jitter_positions()
{
    if (g_entry.load() == nullptr)
        return 0;
    std::lock_guard lock(g_write_lock);
    return g_positions;
}

#endif
