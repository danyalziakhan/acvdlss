// Replaces Valhalla's TAA pass with DLSS.
//
// The pass is found by its descriptor table: seven same-size textures in a fixed format order. At its first
// draw prepare.hlsl converts the inputs and DLSS writes into the draw's target. The TAA draws are skipped.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "acvdlss.h"
#include "descriptors.h"
#include "effects.h"
#include "jitter.h"
#include "dlss.h"
#include "passes.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

namespace
{
// Holds the per-view constant buffer.
const uint32_t VIEW_PARAM = 1;

// Byte offsets into the TAA and per-view constant buffers.
namespace cb
{
const uint32_t TAA_SIZE = 0x100;
const uint32_t HISTORY_VALID = 0x00; // cleared on camera cuts
const uint32_t JITTER = 0x30;        // x, y in pixels
const uint32_t SKY_RANGE = 0x60;     // (a - b) / (c - d) is the sky's view depth

const uint32_t VIEW_SIZE = 0x490;
const uint32_t CAM_TO_WORLD = 0x60;     // three rows
const uint32_t PROJ_11 = 0x1b4;         // y scale: 1 / tan(vertical field of view / 2)
const uint32_t PROJ_22 = 0x1c8;         // with PROJ_23, turns view depth into device depth
const uint32_t PROJ_23 = 0x1cc;
const uint32_t PREV_VIEW_PROJ_X = 0x260;
const uint32_t PREV_VIEW_PROJ_Y = 0x270;
const uint32_t PREV_VIEW_PROJ_W = 0x290;
const uint32_t SKY_DEVICE_DEPTH = 0x364;
const uint32_t INV_PROJ = 0x450;        // four rows
} // namespace cb
// All draws with the TAA table bound belong to the pass we replace.
const uint32_t MAX_SKIPPED_DRAWS = 3;
// Only caps work. The game declares large, mostly empty ranges.
const size_t MAX_TABLE_TEXTURES = 64;
// Frames without a TAA pass before the overlay shows "waiting".
const uint64_t IDLE_FRAMES = 30;
// No TAA pass for this long. Log possible causes and stop holding the pipeline cache rebuild for DLSS.
const uint64_t NOTE_FRAMES = 3600;

struct texture_entry
{
    resource res = {0};
    uint32_t width = 0, height = 0;
    format fmt = format::unknown;
    bool texture_2d = false;
};

bool same(const texture_entry &e, format fmt, uint32_t width, uint32_t height)
{
    return e.fmt == fmt && e.width == width && e.height == height && e.texture_2d;
}

// Slot order: color, motion, depth R32F, mask R8, color, mask, color. Color is RGBA8 (RGBA16F with HDR).
// Motion is always RGBA8. Dialogue cutscenes bind a 4x4 placeholder for motion and leave slot 6 empty.
// Min width 480 is 25% scale at 1920.
int match_taa(const texture_entry *e, size_t n)
{
    for (size_t i = 0; i + 6 <= n; ++i)
    {
        const texture_entry *t = e + i;
        const format c = t[0].fmt;
        const uint32_t w = t[0].width, h = t[0].height;
        if ((c != format::r8g8b8a8_unorm && c != format::r16g16b16a16_float) || w < 480 || !t[0].texture_2d)
            continue;
        const bool placeholder = t[1].width <= 16 && t[1].height <= 16 && t[1].texture_2d;
        if (!same(t[1], format::r8g8b8a8_unorm, w, h) && !placeholder)
            continue;
        if (!same(t[2], format::r32_float, w, h) || !same(t[3], format::r8_unorm, w, h) ||
            !same(t[4], c, w, h) || !same(t[5], format::r8_unorm, w, h))
            continue;
        if (i + 6 < n && !same(t[6], c, w, h))
            continue;
        return int(i);
    }
    return -1;
}

// Parsed once when the root signature is created.
struct table_layout
{
    bool is_table = false;
    std::vector<uint32_t> textures; // table offsets of the texture SRVs in order
    std::vector<uint32_t> writes;   // table offsets of the texture UAVs
    int32_t first_cbv = -1;
    // Table offset of the CBV at b0 of each space, 0 for a root CBV, -1 if absent.
    int32_t b0[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

using layout_tables = std::vector<table_layout>;

// shared_ptr so a list keeps its entry lock-free even if the game destroys the root signature.
std::shared_mutex g_layouts_lock;
std::unordered_map<uint64_t, std::shared_ptr<const layout_tables>> g_layouts;

std::shared_ptr<const layout_tables> find_layout(pipeline_layout layout)
{
    std::shared_lock lock(g_layouts_lock);
    const auto it = g_layouts.find(layout.handle);
    return it != g_layouts.end() ? it->second : nullptr;
}

// Saved so it can be restored after DLSS binds its own heaps and root signature.
enum class root_kind : uint8_t
{
    none,
    table,
    cbv,
    srv,
    uav,
    constants,
};

struct root_value
{
    root_kind kind = root_kind::none;
    uint64_t value = 0; // table handle or GPU address
    buffer_range buffer = {};
    std::vector<uint32_t> constants;
};

struct root_state
{
    pipeline_layout layout = {0};
    std::shared_ptr<const layout_tables> tables;
    std::vector<root_value> params;

    root_value &at(uint32_t index)
    {
        if (index >= params.size())
            params.resize(index + 1);
        return params[index];
    }

    const table_layout *table(uint32_t param) const
    {
        if (tables == nullptr || param >= tables->size() || !(*tables)[param].is_table)
            return nullptr;
        return &(*tables)[param];
    }

    void set_layout(pipeline_layout new_layout)
    {
        layout = new_layout;
        tables = find_layout(new_layout);
        for (root_value &p : params)
            p.kind = root_kind::none;
    }
};

struct pending_redirect
{
    bool pending = false;
    bool discovering = false; // screen pass not known yet. Check the target at the draw
    descriptor_table table = {0};
    uint32_t param = 0;
    uint32_t binding = 0;
    texture_entry original; // the game's TAA output in that slot
};

// An FSR 1 table seen at a compute bind, acted on at the dispatch after it.
struct pending_delivery
{
    bool pending = false;
    resource target = {0};
    format fmt = format::unknown;
    uint32_t width = 0, height = 0;
    bool new_site = false; // the first delivery at this pass's root signature
};

struct screen_effect;

struct __declspec(uuid("3c8e1f52-6a4d-4b9e-8d27-91f0a6c4e7b3")) list_state
{
    root_state graphics, compute;
    ID3D12PipelineState *pipeline = nullptr;
    resource_view render_target = {0};
    screen_effect *effect_draw = nullptr; // the bound pipeline draws one of these

    // Set when the TAA table is bound. Cleared when it goes away.
    bool armed = false;
    bool replaced = false;
    uint32_t taa_param = 0;
    uint64_t taa_table = 0;
    uint32_t skipped = 0;
    texture_entry inputs[3];

    // Logged at the next draw or dispatch.
    std::string trace;

    // A screen-pass table seen at a bind, acted on at the draw after it.
    pending_redirect redirect;
    pending_delivery delivery;

    void reset()
    {
        graphics = {};
        compute = {};
        pipeline = nullptr;
        render_target = {0};
        effect_draw = nullptr;
        armed = false;
        trace.clear();
        redirect = {};
        delivery = {};
    }
};

// Looked up thousands of times a frame and ReShade's lookup is slow. A list records on one thread at a time
// and the cache is per thread. Destroying a list bumps the generation to invalidate the caches.
std::atomic<uint64_t> g_list_generation{0};

struct list_cache
{
    command_list *cmd = nullptr;
    list_state *state = nullptr;
    uint64_t generation = 0;
};
thread_local list_cache t_list;

list_state *find_state(command_list *cmd)
{
    const uint64_t generation = g_list_generation.load(std::memory_order_acquire);
    if (t_list.cmd == cmd && t_list.generation == generation)
        return t_list.state;
    list_state *s = cmd->get_private_data<list_state>();
    if (s != nullptr)
        t_list = {cmd, s, generation};
    return s;
}

list_state &state_of(command_list *cmd)
{
    if (list_state *s = find_state(cmd))
        return *s;
    const uint64_t generation = g_list_generation.load(std::memory_order_acquire);
    list_state *s = cmd->create_private_data<list_state>();
    t_list = {cmd, s, generation};
    return *s;
}

struct context
{
    device *dev = nullptr;
    command_queue *queue = nullptr;
    dlss upscaler;
    passes work;
    bool ready = false;
    std::string error;

    // The TAA pass is recorded on whichever thread the game picks.
    std::mutex lock;
    uint64_t frame = 0;
    uint64_t last_seen = 0;
    uint64_t last_replaced = 0;
    uint32_t display_width = 0, display_height = 0;
    bool failed = false;
    bool force_reset = true;
    std::chrono::steady_clock::time_point last_time;
    taa_status status;
};

std::unique_ptr<context> g_ctx;
std::mutex g_ctx_lock;

//----------------------------------------------------------------------------
// Debug counts. Logged and cleared every 600 frames with Debug on.

struct debug_counts
{
    std::atomic<uint32_t> replaced{0}, resets{0}, camera_only{0}, skipped_draws{0};
    std::atomic<uint32_t> redirects{0}, deliveries{0}, given_up{0};
    std::atomic<uint64_t> frame_us{0};
};
debug_counts g_counts;

void debug_event(const char *format, ...)
{
    if (!g_settings.debug)
        return;
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    acv_log(reshade::log::level::info, text);
}

//----------------------------------------------------------------------------
// Debug only: our CPU time per handler. Tells our cost from the game's when camera movement adds binds.

enum probe : size_t
{
    P_BIND,
    P_MATCH,
    P_REDIRECT,
    P_DELIVERY,
    P_DRAW,
    P_REPLACE,
    P_CONTEXT_LOCK,
    P_EVALUATE,
    P_INIT_RESOURCE,
    P_DESTROY_RESOURCE,
    P_TRACKING,
    P_TEXTURE_LOCK,
    P_PIPELINE,
    P_COUNT
};
const char *const PROBE_NAMES[P_COUNT] = {
    "table binds",
    "TAA matching",
    "redirect checks",
    "FSR checks",
    "draws",
    "DLSS recording",
    "waiting for the frame lock",
    "DLSS evaluate",
    "texture creation",
    "texture release",
    "descriptor tracking",
    "texture lock waits",
    "pipeline binds",
};

// The work takes well under 1 us. A call this long was probably waiting on a lock.
const uint64_t LONG_CALL_NS = 500'000;

struct probe_total
{
    std::atomic<uint64_t> ns{0}, calls{0}, worst_ns{0};
    std::atomic<uint64_t> long_calls{0}, long_ns{0};
};
probe_total g_probes[P_COUNT];

std::atomic<uint64_t> g_frames{0};

uint64_t now_ns()
{
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void probe_add(size_t p, uint64_t ns)
{
    probe_total &t = g_probes[p];
    t.ns += ns;
    ++t.calls;
    if (ns >= LONG_CALL_NS)
    {
        ++t.long_calls;
        t.long_ns += ns;
    }
    uint64_t worst = t.worst_ns;
    while (ns > worst && !t.worst_ns.compare_exchange_weak(worst, ns))
    {
    }
}

class probe_scope
{
public:
    explicit probe_scope(size_t p) : _p(p), _start(g_settings.debug ? now_ns() : 0) {}
    ~probe_scope()
    {
        if (_start != 0)
            probe_add(_p, now_ns() - _start);
    }
    probe_scope(const probe_scope &) = delete;
    probe_scope &operator=(const probe_scope &) = delete;

private:
    size_t _p;
    uint64_t _start;
};

// ReShade calls handlers in registration order. The descriptor tracker's run between these two.
thread_local uint64_t t_tracking_start = 0;

bool on_tracking_begin(device *, uint32_t, const void *)
{
    t_tracking_start = g_settings.debug ? now_ns() : 0;
    return false;
}

bool on_tracking_end(device *, uint32_t, const void *)
{
    if (t_tracking_start != 0)
        probe_add(P_TRACKING, now_ns() - t_tracking_start);
    t_tracking_start = 0;
    return false;
}

bool on_copy_tables_begin(device *dev, uint32_t count, const descriptor_table_copy *copies)
{
    return on_tracking_begin(dev, count, copies);
}
bool on_copy_tables_end(device *dev, uint32_t count, const descriptor_table_copy *copies)
{
    return on_tracking_end(dev, count, copies);
}
bool on_update_tables_begin(device *dev, uint32_t count, const descriptor_table_update *updates)
{
    return on_tracking_begin(dev, count, updates);
}
bool on_update_tables_end(device *dev, uint32_t count, const descriptor_table_update *updates)
{
    return on_tracking_end(dev, count, updates);
}

// Debug only: GPU timestamps around our work, read back a few frames later.
class gpu_timer
{
public:
    static const uint32_t RING = 8;
    static const uint32_t MARKS = 4; // before prepare, after prepare, after DLSS, after finish

    bool ready() const { return _heap != nullptr; }

    void init(ID3D12Device *device)
    {
        D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, RING * MARKS, 0};
        if (FAILED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&_heap))))
            return;
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = RING * MARKS * sizeof(uint64_t);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&_readback))))
        {
            _heap->Release();
            _heap = nullptr;
        }
    }

    void shutdown()
    {
        if (_readback != nullptr)
            _readback->Release();
        if (_heap != nullptr)
            _heap->Release();
        _readback = nullptr;
        _heap = nullptr;
    }

    void mark(ID3D12GraphicsCommandList *list, uint64_t frame, uint32_t index)
    {
        list->EndQuery(_heap, D3D12_QUERY_TYPE_TIMESTAMP, uint32_t(frame % RING) * MARKS + index);
    }

    void resolve(ID3D12GraphicsCommandList *list, uint64_t frame)
    {
        const uint32_t first = uint32_t(frame % RING) * MARKS;
        list->ResolveQueryData(_heap, D3D12_QUERY_TYPE_TIMESTAMP, first, MARKS, _readback, first * sizeof(uint64_t));
        _written[frame % RING] = frame + 1;
    }

    // The three spans in ticks of a frame at least RING - 2 frames old. False if that frame didn't run DLSS.
    bool read(uint64_t frame, uint64_t spans[MARKS - 1])
    {
        if (_heap == nullptr || _written[frame % RING] != frame + 1)
            return false;
        const uint32_t first = uint32_t(frame % RING) * MARKS;
        D3D12_RANGE range = {first * sizeof(uint64_t), (first + MARKS) * sizeof(uint64_t)};
        void *data = nullptr;
        if (FAILED(_readback->Map(0, &range, &data)))
            return false;
        uint64_t t[MARKS];
        memcpy(t, static_cast<const uint8_t *>(data) + range.Begin, sizeof(t));
        const D3D12_RANGE nothing = {0, 0};
        _readback->Unmap(0, &nothing);
        for (uint32_t i = 0; i + 1 < MARKS; ++i)
            spans[i] = t[i + 1] >= t[i] ? t[i + 1] - t[i] : 0;
        return true;
    }

private:
    ID3D12QueryHeap *_heap = nullptr;
    ID3D12Resource *_readback = nullptr;
    uint64_t _written[RING] = {};
};
gpu_timer g_timer;

float read_float(const uint8_t *cb, size_t offset)
{
    float v;
    memcpy(&v, cb + offset, sizeof(v));
    return v;
}

bool read_buffer(device *dev, const buffer_range &range, uint8_t *out, uint32_t size)
{
    if (range.buffer.handle == 0)
        return false;
    void *data = nullptr;
    if (!dev->map_buffer_region(range.buffer, range.offset, size, map_access::read_only, &data) || data == nullptr)
        return false;
    memcpy(out, data, size);
    dev->unmap_buffer_region(range.buffer);
    return true;
}

resource_view view_at(const descriptor_tracker &tracking, descriptor_heap heap, uint32_t offset)
{
    return tracking.view(heap, offset);
}

buffer_range buffer_at(const descriptor_tracker &tracking, descriptor_heap heap, uint32_t offset)
{
    return tracking.buffer(heap, offset);
}

// Root CBV or first CBV in a table. Returns the failure reason or nullptr.
const char *read_constants(device *dev, const root_state &root, uint32_t param, uint8_t *out, uint32_t size)
{
    if (param >= root.params.size() || root.params[param].kind == root_kind::none)
        return "nothing bound";
    const root_value &v = root.params[param];
    if (v.kind == root_kind::cbv)
        return read_buffer(dev, v.buffer, out, size) ? nullptr : "root CBV could not be mapped";
    const table_layout *layout = root.table(param);
    if (v.kind != root_kind::table || layout == nullptr || layout->first_cbv < 0)
        return "no CBV in the table";

    descriptor_heap heap = {0};
    uint32_t offset = 0;
    dev->get_descriptor_heap_offset({v.value}, uint32_t(layout->first_cbv), 0, &heap, &offset);
    const auto *tracking = descriptor_tracker::of(dev);
    if (tracking == nullptr)
        return "no descriptor tracking";
    const buffer_range range = buffer_at(*tracking, heap, offset);
    if (range.buffer.handle == 0)
        return "CBV descriptor not tracked";
    return read_buffer(dev, range, out, size) ? nullptr : "CBV buffer could not be mapped";
}

// Everything prepare.hlsl needs except flags, which the caller sets.
prepare_constants make_constants(const uint8_t *taa_cb, const uint8_t *view_cb, uint32_t width, uint32_t height)
{
    prepare_constants k = {};
    k.size[0] = float(width);
    k.size[1] = float(height);
    k.size[2] = 1.0f / float(width);
    k.size[3] = 1.0f / float(height);

    // The game's sky test is slightly inside the far plane.
    float sky[4];
    memcpy(sky, taa_cb + cb::SKY_RANGE, sizeof(sky));
    k.depth_params[0] = sky[2] != sky[3] ? (sky[0] - sky[1]) / (sky[2] - sky[3]) * 0.98f : FLT_MAX;
    k.depth_params[1] = read_float(view_cb, cb::PROJ_22);
    k.depth_params[2] = read_float(view_cb, cb::PROJ_23);
    k.depth_params[3] = read_float(view_cb, cb::SKY_DEVICE_DEPTH);

    memcpy(k.inv_proj, view_cb + cb::INV_PROJ, sizeof(k.inv_proj));
    memcpy(k.cam_to_world, view_cb + cb::CAM_TO_WORLD, sizeof(k.cam_to_world));
    memcpy(k.prev_view_proj + 0, view_cb + cb::PREV_VIEW_PROJ_X, 16);
    memcpy(k.prev_view_proj + 4, view_cb + cb::PREV_VIEW_PROJ_Y, 16);
    memcpy(k.prev_view_proj + 8, view_cb + cb::PREV_VIEW_PROJ_W, 16);
    return k;
}

void set_status(context &c, taa_state state, const std::string &message = std::string())
{
    c.status.state = state;
    c.status.message = message;
}

// Games leave views to destroyed resources in unused slots. ReShade still returns them and querying one
// crashes. So sizes come from live textures.
struct texture_info
{
    uint32_t width, height;
    format fmt;
};
std::shared_mutex g_textures_lock;
std::unordered_map<uint64_t, texture_info> g_textures;

// May be stale. Compare it, never use it.
uint64_t resource_at(device *dev, const descriptor_tracker &tracking, descriptor_table table, uint32_t binding)
{
    descriptor_heap heap = {0};
    uint32_t offset = 0;
    dev->get_descriptor_heap_offset(table, binding, 0, &heap, &offset);
    const resource_view view = view_at(tracking, heap, offset);
    return view.handle != 0 ? dev->get_resource_from_view(view).handle : 0;
}

// False for an empty or stale slot.
bool read_texture(device *dev, const descriptor_tracker &tracking, descriptor_table table, uint32_t binding,
                  texture_entry &e)
{
    descriptor_heap heap = {0};
    uint32_t offset = 0;
    dev->get_descriptor_heap_offset(table, binding, 0, &heap, &offset);
    const resource_view view = view_at(tracking, heap, offset);
    if (view.handle == 0)
        return false;
    const resource res = dev->get_resource_from_view(view);
    if (res.handle == 0)
        return false;

    texture_info info;
    {
        const uint64_t asked = g_settings.debug ? now_ns() : 0;
        std::shared_lock lock(g_textures_lock);
        if (asked != 0)
            probe_add(P_TEXTURE_LOCK, now_ns() - asked);
        const auto it = g_textures.find(res.handle);
        if (it == g_textures.end())
            return false;
        info = it->second;
    }
    const resource_view_desc view_desc = dev->get_resource_view_desc(view);
    e.res = res;
    e.width = info.width;
    e.height = info.height;
    e.fmt = view_desc.format != format::unknown ? view_desc.format : info.fmt;
    e.texture_2d = view_desc.type == resource_view_type::texture_2d;
    return true;
}

std::atomic<uint32_t> g_tables_checked{0}, g_tables_unreadable{0}, g_tables_matched{0};

std::string describe(const texture_entry &e)
{
    char text[48];
    snprintf(text, sizeof(text), "format %u %ux%u", unsigned(e.fmt), e.width, e.height);
    return text;
}

// Tables that look like the TAA's but didn't match. Debug logs them at once. Otherwise the first few go in
// the "no TAA pass found" note.
std::mutex g_near_miss_lock;
std::unordered_set<uint64_t> g_near_miss_seen;
std::vector<std::string> g_near_misses;

void note_near_miss(const list_state &st, uint32_t param, const texture_entry *entries, size_t n)
{
    const bool has_depth = std::any_of(entries, entries + n, [](const texture_entry &e) {
        return e.fmt == format::r32_float && e.width >= 1280;
    });
    if (n < 6 || !has_depth)
        return;
    {
        std::lock_guard guard(g_near_miss_lock);
        if (g_near_miss_seen.size() >= 32 ||
            !g_near_miss_seen.insert(st.graphics.layout.handle ^ (uint64_t(param) << 48)).second)
            return;
    }

    char head[96];
    snprintf(head, sizeof(head), "root signature %p, parameter %u:", reinterpret_cast<void *>(st.graphics.layout.handle),
             param);
    std::string line = head;
    for (size_t i = 0; i < n; ++i)
        line += " [" + describe(entries[i]) + (entries[i].texture_2d ? "" : " not 2D") + "]";
    if (g_settings.debug)
    {
        acv_log(reshade::log::level::info, ("near miss for the TAA table: " + line).c_str());
        return;
    }
    std::lock_guard guard(g_near_miss_lock);
    if (g_near_misses.size() < 8)
        g_near_misses.push_back(line);
}

uint64_t site_key(pipeline_layout layout, uint32_t param)
{
    return layout.handle ^ (uint64_t(param) << 56);
}

// Checked on every table bind, so reads are lock-free. Adds are rare.
class site_set
{
public:
    bool empty() const { return _count == 0; }

    bool contains(uint64_t key) const
    {
        const uint32_t count = _count;
        for (uint32_t i = 0; i < count; ++i)
            if (_keys[i] == key)
                return true;
        return false;
    }

    // True if the key was not there before.
    bool add(uint64_t key)
    {
        std::lock_guard lock(_lock);
        const uint32_t count = _count;
        if (contains(key) || count == CAPACITY)
            return false;
        _keys[count] = key;
        _count = count + 1;
        return true;
    }

private:
    static const uint32_t CAPACITY = 8;
    std::atomic<uint64_t> _keys[CAPACITY] = {};
    std::atomic<uint32_t> _count{0};
    std::mutex _lock;
};

// Reading every table costs real CPU. Once a site is known only that site is read. A full scan resumes the
// first frame the TAA goes missing. A periodic full scan cost 30 ms in busy scenes and hitched every 2 s.
site_set g_taa_sites;
std::atomic<uint64_t> g_taa_frame{0}; // the last frame DLSS replaced the TAA in

// Screen-fixed effects smear in DLSS history when the camera turns. We neutralize them at the draw and redo
// them on DLSS's output. The two record on different threads in either order. The last values are the
// fallback.
struct screen_effect
{
    const char *name;
    uint32_t shaders[2]; // FNV-1a of the game's pixel shaders. 0 if unused
    uint32_t space;      // their constants are at register b0 of this space
    uint32_t floats;     // how many of them we read
    std::atomic<uint64_t> pipelines[8] = {};

    std::mutex lock;
    bool waiting = false;
    float values[16] = {};
    float last[16] = {};
    uint64_t frame = 0; // g_frames when last taken out
    uint32_t reads = 0, passes = 0, passes_late = 0; // debug counters
};

// Pixel shader hashes from Steam build 26142239. The math is in the shaders.
// The vignette is in the tonemapping pass, which has an underwater variant.
screen_effect g_vignette{"vignette", {0xc5f74a85, 0xebab6b39}, 2, 4};
screen_effect g_radial{"radial blur", {0x641896d3, 0}, 4, 16};
screen_effect g_lens{"lens distortion", {0x1754fdb5, 0}, 4, 16};
// Two underwater passes draw straight into the TAA input. One is a radial blur without the raven's
// darkening. The other mixes the image with a shifted copy of itself.
screen_effect g_radial_plain{"radial blur without darkening", {0x9d4b57d8, 0}, 4, 12};
screen_effect g_double{"double image", {0xa315986d, 0}, 4, 8};
// One bit per effect pipeline, keyed by address. Most binds miss it and skip the search through the slots.
std::atomic<uint64_t> g_effect_pipeline_bits{0};

uint64_t pipeline_bit(uint64_t pipeline)
{
    return 1ull << ((pipeline >> 4) & 63);
}

screen_effect *const SCREEN_EFFECTS[] = {&g_vignette, &g_radial, &g_lens, &g_radial_plain, &g_double};

// Nothing to draw without blur or darkening.
std::optional<radial_constants> make_radial(const float *v)
{
    if (v[8] == 0.0f && v[12] == 0.0f && v[13] == 0.0f && v[14] <= 1.0f && v[15] >= 1.0f)
        return std::nullopt;
    radial_constants k = {};
    memcpy(k.c0, v, sizeof(k.c0));
    memcpy(k.c1, v + 4, sizeof(k.c1));
    memcpy(k.c2, v + 8, sizeof(k.c2));
    memcpy(k.c3, v + 12, sizeof(k.c3));
    return k;
}

// The radial blur pass with the darkening at 1.
std::optional<radial_constants> make_radial_plain(const float *v)
{
    if (v[8] == 0.0f)
        return std::nullopt;
    radial_constants k = {};
    memcpy(k.c0, v, sizeof(k.c0));
    memcpy(k.c1, v + 4, sizeof(k.c1));
    memcpy(k.c2, v + 8, sizeof(k.c2));
    k.c3[2] = k.c3[3] = 1.0f;
    return k;
}

std::optional<double_image_constants> make_double(const float *v)
{
    if (v[4] == 0.0f)
        return std::nullopt;
    return double_image_constants{{v[0], v[1]}, v[4], 0.0f};
}

// Nothing to draw without distortion or an added color.
std::optional<lens_constants> make_lens(const float *v)
{
    if (v[6] == 0.0f && v[12] == v[9] && v[13] == v[9] && v[14] == v[9])
        return std::nullopt;
    lens_constants k = {};
    memcpy(k.c1, v + 4, sizeof(k.c1));
    memcpy(k.c2, v + 8, sizeof(k.c2));
    memcpy(k.c3, v + 12, sizeof(k.c3));
    return k;
}

// False if the effect wasn't drawn lately.
bool take_values(screen_effect &e, float *out)
{
    std::lock_guard lock(e.lock);
    if (g_frames > e.frame + 2)
        return false;
    ++e.passes;
    e.passes_late += !e.waiting;
    memcpy(out, e.waiting ? e.values : e.last, sizeof(e.values));
    e.waiting = false;
    return true;
}

void log_screen_effects()
{
    static auto next = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now < next)
        return;
    next = now + std::chrono::seconds(1);
    for (screen_effect *e : SCREEN_EFFECTS)
    {
        std::lock_guard lock(e->lock);
        if (e->reads == 0 && e->passes == 0)
            continue;
        char text[200];
        snprintf(text, sizeof(text), "%s: taken out %u times. %u DLSS passes, %u before its draw.%s", e->name,
                 e->reads, e->passes, e->passes_late,
                 e == &g_vignette ? (" Strength " +std::to_string(e->last[0])).c_str() : "");
        acv_log(reshade::log::level::info, text);
        e->reads = e->passes = e->passes_late = 0;
    }
}

void try_match(command_list *cmd, list_state &st, uint32_t param, descriptor_table table)
{
    const table_layout *layout = st.graphics.table(param);
    if (layout == nullptr || layout->textures.size() < 6)
        return;
    const uint64_t key = site_key(st.graphics.layout, param);
    const uint64_t frame = g_frames;
    const bool scan = g_taa_sites.empty() || frame > g_taa_frame + 1;
    if (!scan && !g_taa_sites.contains(key))
        return;

    device *dev = cmd->get_device();
    const auto *tracking = descriptor_tracker::of(dev);
    if (tracking == nullptr)
        return;

    // Skip empty slots so the cutscene table still lines up.
    texture_entry entries[MAX_TABLE_TEXTURES];
    size_t n = 0;
    for (uint32_t binding : layout->textures)
        if (read_texture(dev, *tracking, table, binding, entries[n]))
            ++n;

    ++g_tables_checked;
    if (n == 0)
        ++g_tables_unreadable;
    const int at = match_taa(entries, n);
    if (at < 0)
    {
        note_near_miss(st, param, entries, n);
        return;
    }
    ++g_tables_matched;
    g_taa_sites.add(key);
    st.armed = true;
    st.replaced = false;
    st.skipped = 0;
    st.taa_param = param;
    st.taa_table = table.handle;
    std::copy_n(entries + at, 3, st.inputs);
}

//----------------------------------------------------------------------------
// Debug trace of passes reading the TAA output, to find the screen pass.

// Reads every table. Sampled so the logged timings stay honest.
const uint64_t TRACE_EVERY = 600;
std::atomic<uint64_t> g_taa_output{0};
std::mutex g_trace_lock;
std::unordered_set<uint64_t> g_traced;
const size_t MAX_TRACED = 256;

void trace_reads(command_list *cmd, list_state &st, bool graphics, uint32_t param, descriptor_table table)
{
    const uint64_t watched = g_taa_output;
    const root_state &root = graphics ? st.graphics : st.compute;
    const table_layout *layout = root.table(param);
    if (watched == 0 || layout == nullptr)
        return;
    device *dev = cmd->get_device();
    const auto *tracking = descriptor_tracker::of(dev);
    if (tracking == nullptr)
        return;

    std::string reads;
    bool reads_taa = false;
    uint64_t taa_size = 0;
    for (uint32_t binding : layout->textures)
    {
        texture_entry e;
        if (!read_texture(dev, *tracking, table, binding, e))
            continue;
        const bool is_taa = e.res.handle == watched;
        if (is_taa)
        {
            reads_taa = true;
            taa_size = (uint64_t(e.width) << 16) ^ e.height;
        }
        reads += " [" + std::to_string(binding) + "] " + describe(e) + (is_taa ? " (TAA output)" : "");
    }
    if (!reads_taa)
        return;

    std::string writes;
    for (uint32_t binding : layout->writes)
    {
        texture_entry e;
        if (read_texture(dev, *tracking, table, binding, e))
            writes += " [" + std::to_string(binding) + "] " + describe(e);
    }

    // Keyed on TAA size too. The upscale pass only appears below 100%.
    const uint64_t key = root.layout.handle ^ (uint64_t(param) << 48) ^ (graphics ? 0 : 1ull << 63) ^ (taa_size << 8);
    {
        std::lock_guard lock(g_trace_lock);
        if (g_traced.size() >= MAX_TRACED || !g_traced.insert(key).second)
            return;
    }

    char head[128];
    snprintf(head, sizeof(head), "%s pass reads the TAA output (root signature %p, parameter %u); reads",
             graphics ? "graphics" : "compute", reinterpret_cast<void *>(root.layout.handle), param);
    st.trace = head + reads + (writes.empty() ? std::string() : "; writes" + writes);
}

void flush_trace(list_state &st, const std::string &what)
{
    if (st.trace.empty())
        return;
    acv_log(reshade::log::level::info, (st.trace + "; then " + what).c_str());
    st.trace.clear();
}

std::string draw_target(command_list *cmd, const list_state &st)
{
    if (st.render_target.handle == 0)
        return "a draw with no render target";
    device *dev = cmd->get_device();
    const resource_desc desc = dev->get_resource_desc(dev->get_resource_from_view(st.render_target));
    char text[80];
    snprintf(text, sizeof(text), "a draw into format %u %ux%u", unsigned(desc.texture.format), desc.texture.width,
             desc.texture.height);
    return text;
}

// Changing heaps invalidates every bound table, graphics included.
void restore(device *dev, ID3D12GraphicsCommandList *list, const list_state &st)
{
    ID3D12DescriptorHeap *heaps[2] = {};
    auto note_heap = [&](uint64_t table) {
        descriptor_heap heap = {0};
        dev->get_descriptor_heap_offset({table}, 0, 0, &heap, nullptr);
        if (heap.handle == 0)
            return;
        auto *native = reinterpret_cast<ID3D12DescriptorHeap *>(heap.handle);
        heaps[native->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0] = native;
    };
    for (const root_state *root : {&st.graphics, &st.compute})
        for (const root_value &v : root->params)
            if (v.kind == root_kind::table)
                note_heap(v.value);

    ID3D12DescriptorHeap *bound[2];
    UINT count = 0;
    for (ID3D12DescriptorHeap *heap : heaps)
        if (heap != nullptr)
            bound[count++] = heap;
    if (count != 0)
        list->SetDescriptorHeaps(count, bound);

    if (st.compute.layout.handle != 0)
    {
        list->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature *>(st.compute.layout.handle));
        for (UINT i = 0; i < UINT(st.compute.params.size()); ++i)
        {
            const root_value &v = st.compute.params[i];
            switch (v.kind)
            {
            case root_kind::table: list->SetComputeRootDescriptorTable(i, {v.value}); break;
            case root_kind::cbv: list->SetComputeRootConstantBufferView(i, v.value); break;
            case root_kind::srv: list->SetComputeRootShaderResourceView(i, v.value); break;
            case root_kind::uav: list->SetComputeRootUnorderedAccessView(i, v.value); break;
            case root_kind::constants:
                list->SetComputeRoot32BitConstants(i, UINT(v.constants.size()), v.constants.data(), 0);
                break;
            case root_kind::none: break;
            }
        }
    }

    for (UINT i = 0; i < UINT(st.graphics.params.size()); ++i)
        if (st.graphics.params[i].kind == root_kind::table)
            list->SetGraphicsRootDescriptorTable(i, {st.graphics.params[i].value});

    if (st.pipeline != nullptr)
        list->SetPipelineState(st.pipeline);
}

// Only a hint to DLSS.
float frame_time_ms(context &c)
{
    const auto now = std::chrono::steady_clock::now();
    const float ms = c.last_time.time_since_epoch().count() != 0
                         ? std::chrono::duration<float, std::milli>(now - c.last_time).count()
                         : 0.0f;
    c.last_time = now;
    return ms > 0.0f && ms <= 100.0f ? ms : 16.6f;
}

//----------------------------------------------------------------------------
// Below 100% a game pass upscales the TAA output to the screen. We rewrite the SRV it reads right before its
// draw so it gets the DLSS frame instead.

struct redirect_site
{
    uint64_t layout = 0;
    uint32_t param = 0;
    uint32_t binding = 0;
};

// Search window after the frame changes. There can be several readers. With FSR on a pass reading last
// frame's TAA output showed up before the real one.
const uint64_t DISCOVERY_FRAMES = 120;
const size_t MAX_SITES = 4;

struct upscale_state
{
    std::mutex lock;
    ID3D12Resource *presented = nullptr; // null while DLSS is not upscaling
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t taa_layout = 0;
    uint64_t taa_targets[2] = {}; // the game alternates between two
    std::vector<redirect_site> sites;
    uint64_t discover_until = 0;
    // FSR 1 outputs. The screen pass reads them when FSR is on. Two passes each alternate two textures.
    uint64_t fsr_chain[4] = {};
    uint32_t fsr_chain_next = 0;
    // Restored if DLSS stops upscaling while the game still uses the table.
    std::unordered_set<uint64_t> redirected;
};
upscale_state g_upscale;

// Lets per-bind checks skip the lock. Written under it.
std::atomic<bool> g_upscale_active{false};

void refresh_active(const upscale_state &u)
{
    g_upscale_active = u.presented != nullptr || !u.redirected.empty();
}

// Lock-free mirrors for the per-bind checks.
std::atomic<uint64_t> g_discover_until{0};
site_set g_screen_sites; // the same pairs as g_upscale.sites
site_set g_fsr_sites;    // where EASU and RCAS bind their tables

bool discovering_now()
{
    return g_frames < g_discover_until;
}

void keep_discovering(upscale_state &u)
{
    u.discover_until = g_frames + DISCOVERY_FRAMES;
    g_discover_until = u.discover_until;
}

void publish_upscale(uint64_t taa_target, ID3D12Resource *presented, DXGI_FORMAT format, pipeline_layout taa_layout)
{
    std::lock_guard lock(g_upscale.lock);
    if (presented != nullptr && g_upscale.presented != presented)
        keep_discovering(g_upscale);
    g_upscale.presented = presented;
    refresh_active(g_upscale);
    g_upscale.format = format;
    g_upscale.taa_layout = taa_layout.handle;
    if (g_upscale.taa_targets[0] != taa_target && g_upscale.taa_targets[1] != taa_target)
    {
        g_upscale.taa_targets[1] = g_upscale.taa_targets[0];
        g_upscale.taa_targets[0] = taa_target;
        keep_discovering(g_upscale);
    }
}

bool in_fsr_chain(const upscale_state &u, uint64_t handle)
{
    return std::find(std::begin(u.fsr_chain), std::end(u.fsr_chain), handle) != std::end(u.fsr_chain);
}

void stop_upscaling()
{
    std::lock_guard lock(g_upscale.lock);
    g_upscale.presented = nullptr;
    refresh_active(g_upscale);
}

// While discovering, any non-TAA table reading the frame is a candidate. The draw's target size confirms it.
void check_redirect(command_list *cmd, list_state &st, uint32_t param, descriptor_table table)
{
    if (!g_upscale_active)
        return;
    if (!discovering_now() && !g_screen_sites.contains(site_key(st.graphics.layout, param)))
        return;
    std::vector<redirect_site> sites;
    uint64_t frames[6];
    bool upscaling, discovering;
    uint64_t taa_layout;
    {
        std::lock_guard lock(g_upscale.lock);
        upscaling = g_upscale.presented != nullptr;
        discovering = upscaling && g_frames < g_upscale.discover_until && g_upscale.sites.size() < MAX_SITES;
        sites = g_upscale.sites;
        taa_layout = g_upscale.taa_layout;
        std::copy(std::begin(g_upscale.taa_targets), std::end(g_upscale.taa_targets), frames);
        std::copy(std::begin(g_upscale.fsr_chain), std::end(g_upscale.fsr_chain), frames + 2);
    }
    const auto site = std::find_if(sites.begin(), sites.end(), [&](const redirect_site &s) {
        return s.layout == st.graphics.layout.handle && s.param == param;
    });
    if (site == sites.end() && (!discovering || st.graphics.layout.handle == taa_layout))
        return;
    const table_layout *layout = st.graphics.table(param);
    device *dev = cmd->get_device();
    const auto *tracking = descriptor_tracker::of(dev);
    if (layout == nullptr || tracking == nullptr)
        return;

    // The TAA output or an FSR 1 output.
    auto is_frame = [&](const texture_entry &e) {
        return e.res.handle != 0 && std::find(std::begin(frames), std::end(frames), e.res.handle) != std::end(frames);
    };
    texture_entry e;
    if (site != sites.end())
    {
        if (read_texture(dev, *tracking, table, site->binding, e) && is_frame(e))
            st.redirect = {true, false, table, param, site->binding, e};
        return;
    }
    for (uint32_t binding : layout->textures)
    {
        if (read_texture(dev, *tracking, table, binding, e) && is_frame(e))
        {
            st.redirect = {true, true, table, param, binding, e};
            return;
        }
    }
}

// At the next draw, point the slot at our frame. If DLSS stopped upscaling, point it back at the game's.
void apply_redirect(command_list *cmd, list_state &st)
{
    const pending_redirect r = st.redirect;
    st.redirect.pending = false;
    device *dev = cmd->get_device();

    if (r.discovering)
    {
        // The screen pass's target is bigger than the frame it reads.
        if (st.render_target.handle == 0)
            return;
        const resource_desc desc = dev->get_resource_desc(dev->get_resource_from_view(st.render_target));
        if (desc.texture.width <= r.original.width || desc.texture.height <= r.original.height)
            return;
        std::lock_guard lock(g_upscale.lock);
        if (g_upscale.sites.size() >= MAX_SITES)
            return;
        g_upscale.sites.push_back({st.graphics.layout.handle, r.param, r.binding});
        g_screen_sites.add(site_key(st.graphics.layout, r.param));
        char text[160];
        snprintf(text, sizeof(text),
                 "found the pass that scales the frame to the screen (root signature %p, parameter %u, slot %u)",
                 reinterpret_cast<void *>(st.graphics.layout.handle), r.param, r.binding);
        acv_log(reshade::log::level::info, text);
    }

    descriptor_heap heap = {0};
    uint32_t offset = 0;
    dev->get_descriptor_heap_offset(r.table, r.binding, 0, &heap, &offset);
    if (heap.handle == 0)
        return;
    auto *native_heap = reinterpret_cast<ID3D12DescriptorHeap *>(heap.handle);
    auto *native = reinterpret_cast<ID3D12Device *>(dev->get_native());
    D3D12_CPU_DESCRIPTOR_HANDLE slot = native_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += SIZE_T(offset) * native->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint64_t slot_key = slot.ptr;

    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;

    std::lock_guard lock(g_upscale.lock);
    if (g_upscale.presented != nullptr)
    {
        view.Format = g_upscale.format;
        native->CreateShaderResourceView(g_upscale.presented, &view, slot);
        ++g_counts.redirects;
        // Fresh per-frame slots would pile up. Reused ones get re-added.
        if (g_upscale.redirected.size() >= 256)
            g_upscale.redirected.clear();
        g_upscale.redirected.insert(slot_key);
        refresh_active(g_upscale);
    }
    else if (g_upscale.redirected.erase(slot_key) != 0)
    {
        refresh_active(g_upscale);
        view.Format = DXGI_FORMAT(r.original.fmt);
        native->CreateShaderResourceView(reinterpret_cast<ID3D12Resource *>(r.original.res.handle), &view, slot);
    }
}

// With FSR on, FSR 1 (EASU then RCAS) runs in compute before the screen pass. We skip both so DLSS goes to
// screen size in one step without the game's sharpening. Their outputs still get the DLSS frame so nothing
// reads garbage.
void check_delivery(command_list *cmd, list_state &st, uint32_t param, descriptor_table table)
{
    if (!g_upscale_active)
        return;
    const uint64_t key = site_key(st.compute.layout, param);
    if (!discovering_now() && !g_fsr_sites.contains(key))
        return;
    const table_layout *layout = st.compute.table(param);
    device *dev = cmd->get_device();
    const auto *tracking = descriptor_tracker::of(dev);
    if (layout == nullptr || layout->writes.empty() || tracking == nullptr)
        return;

    uint64_t targets[2], chain[std::size(upscale_state().fsr_chain)];
    {
        std::lock_guard lock(g_upscale.lock);
        if (g_upscale.presented == nullptr)
            return;
        std::copy(std::begin(g_upscale.taa_targets), std::end(g_upscale.taa_targets), targets);
        std::copy(std::begin(g_upscale.fsr_chain), std::end(g_upscale.fsr_chain), chain);
    }

    // EASU reads the TAA output and writes bigger. RCAS keeps the size.
    bool easu = false, rcas = false;
    uint32_t input_binding = 0;
    for (uint32_t binding : layout->textures)
    {
        const uint64_t handle = resource_at(dev, *tracking, table, binding);
        if (handle == 0)
            continue;
        easu = handle == targets[0] || handle == targets[1];
        rcas = std::find(std::begin(chain), std::end(chain), handle) != std::end(chain);
        if (easu || rcas)
        {
            input_binding = binding;
            break;
        }
    }
    texture_entry input;
    if ((!easu && !rcas) || !read_texture(dev, *tracking, table, input_binding, input))
        return;

    for (uint32_t binding : layout->writes)
    {
        texture_entry output;
        if (!read_texture(dev, *tracking, table, binding, output) || output.res.handle == input.res.handle)
            continue;
        const bool fits = easu ? output.width > input.width && output.height > input.height
                               : output.width == input.width && output.height == input.height;
        if (fits)
        {
            st.delivery = {true, output.res, output.fmt, output.width, output.height, g_fsr_sites.add(key)};
            return;
        }
    }
}

bool deliver(command_list *cmd, list_state &st)
{
    const pending_delivery d = st.delivery;
    st.delivery.pending = false;
    {
        std::lock_guard lock(g_upscale.lock);
        if (g_upscale.presented == nullptr)
            return false;
        if (!in_fsr_chain(g_upscale, d.target.handle))
            g_upscale.fsr_chain[g_upscale.fsr_chain_next++ % std::size(g_upscale.fsr_chain)] = d.target.handle;
        // The game cycles more outputs than the chain holds. Only a new site reopens discovery.
        if (d.new_site)
        {
            keep_discovering(g_upscale);
            char text[96];
            snprintf(text, sizeof(text), "found an FSR 1 pass. DLSS fills its %ux%u output instead", d.width, d.height);
            acv_log(reshade::log::level::info, text);
        }
    }

    // Written even when redirected. A pass not redirected yet still reads a real image.
    context &c = *g_ctx;
    device *dev = cmd->get_device();
    const resource_desc desc = dev->get_resource_desc(d.target);
    std::lock_guard lock(c.lock);
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    c.work.deliver(list, reinterpret_cast<ID3D12Resource *>(d.target.handle), DXGI_FORMAT(d.fmt), desc.texture.width,
                   desc.texture.height);
    ++g_counts.deliveries;
    restore(dev, list, st);
    return true;
}

// Logs each reason once.
bool give_up(const std::string &reason)
{
    ++g_counts.given_up;
    static std::mutex lock;
    static std::unordered_set<std::string> said;
    {
        std::lock_guard guard(lock);
        if (said.size() >= 32 || !said.insert(reason).second)
            return false;
    }
    acv_log(reshade::log::level::info, ("left a TAA pass to the game: " + reason).c_str());
    return false;
}

void disarm(list_state &st, const char *why)
{
    if (st.armed && !st.replaced)
        give_up(why);
    st.armed = false;
}

// Debug only. Jitter is in render pixels and DLSS takes it as is. The projection holds it as -2x / width at
// 0x1a8 and +2y / height at 0x1b8. A sign or unit mistake only shows when upscaling.
void note_jitter(uint32_t width, uint32_t height, uint32_t output_width, float jx, float jy, const uint8_t *view_cb)
{
    static uint64_t size = 0;
    static uint32_t positions = 0, frames = 0, distinct = 0, detail = 0;
    static float seen[128][2];
    static float low[2], high[2];
    const uint64_t now = (uint64_t(width) << 32) | (uint64_t(height) << 16) | output_width;
    if (now != size || jitter_positions() != positions)
    {
        if (now != size)
            detail = 8;
        size = now;
        positions = jitter_positions();
        frames = distinct = 0;
        low[0] = low[1] = FLT_MAX;
        high[0] = high[1] = -FLT_MAX;
    }
    if (detail != 0)
    {
        --detail;
        debug_event("jitter %.4f, %.4f px at %ux%u to %u wide; in NDC %.6f, %.6f by render size, %.6f by output "
                    "size; view constants 0x1a8 %.6f, 0x1b8 %.6f, 0x1c0 %.6f, 0x1c4 %.6f",
                    jx, jy, width, height, output_width, 2.0f * jx / float(width), -2.0f * jy / float(height),
                    2.0f * jx / float(output_width), read_float(view_cb, 0x1a8), read_float(view_cb, 0x1b8),
                    read_float(view_cb, 0x1c0), read_float(view_cb, 0x1c4));
    }
    if (frames < 128)
    {
        bool known = false;
        for (uint32_t i = 0; i < distinct && !known; ++i)
            known = std::abs(seen[i][0] - jx) < 1e-4f && std::abs(seen[i][1] - jy) < 1e-4f;
        if (!known)
        {
            seen[distinct][0] = jx;
            seen[distinct][1] = jy;
            ++distinct;
        }
        low[0] = std::min(low[0], jx);
        low[1] = std::min(low[1], jy);
        high[0] = std::max(high[0], jx);
        high[1] = std::max(high[1], jy);
        if (++frames == 128)
            debug_event("jitter at %ux%u: the game set %u positions and %u were distinct in 128 frames. x %.3f to "
                        "%.3f, y %.3f to %.3f. DLSS wants at least %.0f at this scale",
                        width, height, positions, distinct, low[0], high[0], low[1], high[1],
                        8.0 * double(output_width) * double(output_width) / (double(width) * double(width)));
    }
}

bool replace(command_list *cmd, list_state &st)
{
    context &c = *g_ctx;
    device *dev = cmd->get_device();
    const texture_entry &color = st.inputs[0], &motion = st.inputs[1], &depth = st.inputs[2];
    const uint32_t width = color.width, height = color.height;

    // Anything else in the first render target means this isn't the pass we know.
    if (st.render_target.handle == 0)
        return give_up("no render target bound");
    const resource target = dev->get_resource_from_view(st.render_target);
    if (target.handle == 0)
        return give_up("render target view without a resource");
    const resource_desc target_desc = dev->get_resource_desc(target);
    if (target_desc.type != resource_type::texture_2d || target_desc.texture.width != width ||
        target_desc.texture.height != height ||
        format_to_typeless(target_desc.texture.format) != format_to_typeless(color.fmt))
        return give_up("render target is format " + std::to_string(unsigned(target_desc.texture.format)) + " " +
                       std::to_string(target_desc.texture.width) + "x" + std::to_string(target_desc.texture.height) +
                       " but color is format " + std::to_string(unsigned(color.fmt)) + " " + std::to_string(width) +
                       "x" + std::to_string(height));
    if (g_settings.debug)
    {
        // The output alternates two textures. Log size changes only.
        g_taa_output = target.handle;
        static std::atomic<uint64_t> logged_size{0};
        const uint64_t size = (uint64_t(width) << 32) | height;
        if (logged_size.exchange(size) != size)
        {
            char text[96];
            snprintf(text, sizeof(text), "TAA pass at %ux%u, color format %u", width, height, unsigned(color.fmt));
            acv_log(reshade::log::level::info, text);
        }
    }

    uint8_t taa_cb[cb::TAA_SIZE], view_cb[cb::VIEW_SIZE];
    if (const char *why = read_constants(dev, st.graphics, st.taa_param, taa_cb, cb::TAA_SIZE))
        return give_up(std::string("TAA constant buffer: ") + why);
    if (const char *why = read_constants(dev, st.graphics, VIEW_PARAM, view_cb, cb::VIEW_SIZE))
        return give_up(std::string("view constant buffer: ") + why);

    std::unique_lock<std::mutex> lock;
    {
        probe_scope waiting(P_CONTEXT_LOCK);
        lock = std::unique_lock<std::mutex>(c.lock);
    }
    c.last_seen = c.frame;
    const bool hdr = color.fmt == format::r16g16b16a16_float;
    const bool camera_only = motion.width != width || motion.height != height;
    if (camera_only != c.status.camera_motion_only)
        debug_event(camera_only ? "no motion vectors from the game (a dialogue cutscene). Using camera motion"
                                : "motion vectors are back");
    c.status.width = width;
    c.status.height = height;
    c.status.hdr = hdr;
    c.status.camera_motion_only = camera_only;
    if (c.failed)
        return false;

    // At 100% and above it's DLAA at render size. The game downscales as usual.
    const bool upscale = c.display_width > width && c.display_height > height;
    const uint32_t output_width = upscale ? c.display_width : width;
    const uint32_t output_height = upscale ? c.display_height : height;
    // Rare enough to log without Debug.
    static uint64_t logged_mode = 0;
    const uint64_t mode = (uint64_t(width) << 48) ^ (uint64_t(height) << 32) ^ (uint64_t(output_width) << 16) ^
                          output_height ^ (hdr ? 1ull << 63 : 0);
    if (mode != logged_mode)
    {
        logged_mode = mode;
        char text[128];
        if (upscale)
            snprintf(text, sizeof(text), "DLSS running: %ux%u upscaled to %ux%u%s", width, height, output_width,
                     output_height, hdr ? ", HDR" : "");
        else
            snprintf(text, sizeof(text), "DLSS running: DLAA at %ux%u%s", width, height, hdr ? ", HDR" : "");
        acv_log(reshade::log::level::info, text);
    }
    c.status.output_width = output_width;
    c.status.output_height = output_height;
    c.status.display_width = c.display_width;

    if (!c.work.resize(width, height, output_width, output_height, DXGI_FORMAT(color.fmt), effects_hudless_wanted(),
                       c.frame))
    {
        c.failed = true;
        set_status(c, taa_state::failed, c.work.error());
        return false;
    }

    const float jx = read_float(taa_cb, cb::JITTER), jy = read_float(taa_cb, cb::JITTER + 4);
    prepare_constants k = make_constants(taa_cb, view_cb, width, height);
    k.flags[0] = camera_only ? 0.0f : 1.0f;

    uint32_t history_valid;
    memcpy(&history_valid, taa_cb + cb::HISTORY_VALID, sizeof(history_valid));

    dlss_frame frame;
    frame.color = c.work.color();
    frame.motion = c.work.motion();
    frame.depth = c.work.depth();
    frame.output = c.work.output();
    frame.width = width;
    frame.height = height;
    frame.output_width = output_width;
    frame.output_height = output_height;
    frame.jitter_x = jx;
    frame.jitter_y = jy;
    if (g_settings.debug)
        note_jitter(width, height, output_width, jx, jy, view_cb);
    frame.frame_ms = frame_time_ms(c);
    {
        // Reversed device depth is PROJ_23 / z - PROJ_22. Far is infinite when PROJ_22 is 0.
        const float p11 = read_float(view_cb, cb::PROJ_11);
        const float p22 = read_float(view_cb, cb::PROJ_22), p23 = read_float(view_cb, cb::PROJ_23);
        if (p11 > 0.0f && p23 > 0.0f && p22 > -1.0f)
        {
            frame.camera_near = p23 / (1.0f + p22);
            frame.camera_far = p22 > 1e-7f ? p23 / p22 : FLT_MAX;
            frame.camera_fov = 2.0f * std::atan(1.0f / p11);
        }
    }
    frame.hdr = hdr;
    frame.reset = history_valid == 0 || c.force_reset;
    if (frame.reset)
        ++g_counts.resets;
    if (camera_only)
        ++g_counts.camera_only;
    g_counts.frame_us += uint64_t(frame.frame_ms * 1000.0f);
    frame.preset = g_settings.preset;
    // Read once so screen_image() and finish() agree.
    const float sharpness = g_settings.sharpness;
    screen_fx fx;
    float values[16];
    if (take_values(g_vignette, values))
        fx.vignette = values[0] * g_settings.vignette_strength.load();
    if (take_values(g_radial, values))
        fx.radial = make_radial(values);
    if (take_values(g_lens, values))
        fx.lens = make_lens(values);
    // One radial blur pass. The raven's and the underwater one don't overlap.
    if (take_values(g_radial_plain, values) && !fx.radial)
        fx.radial = make_radial_plain(values);
    if (take_values(g_double, values))
        fx.doubling = make_double(values);
    if (g_settings.hudless)
        frame.hudless = c.work.screen_image(sharpness, fx, frame.hudless_state);
    jitter_apply(width, output_width);

    // A rebuild frees the old feature, which may still be in flight.
    if (c.upscaler.will_rebuild(frame) && c.queue != nullptr)
        c.queue->wait_idle();

    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    const bool timing = g_settings.debug;
    if (timing && !g_timer.ready())
        g_timer.init(reinterpret_cast<ID3D12Device *>(dev->get_native()));
    const bool timed = timing && g_timer.ready();
    if (timed)
        g_timer.mark(list, c.frame, 0);
    c.work.prepare(list, {reinterpret_cast<ID3D12Resource *>(color.res.handle), DXGI_FORMAT(color.fmt)},
                   {reinterpret_cast<ID3D12Resource *>(motion.res.handle), DXGI_FORMAT(motion.fmt)},
                   {reinterpret_cast<ID3D12Resource *>(depth.res.handle), DXGI_FORMAT(depth.fmt)}, k);
    if (timed)
        g_timer.mark(list, c.frame, 1);
    bool evaluated;
    {
        probe_scope evaluating(P_EVALUATE);
        evaluated = c.upscaler.evaluate(list, frame);
    }
    if (!evaluated)
    {
        // Let the game's TAA draw.
        c.work.abandon(list);
        restore(dev, list, st);
        jitter_restore();
        c.failed = true;
        set_status(c, taa_state::failed, c.upscaler.error());
        acv_log(reshade::log::level::error,
                ("DLSS failed. The game's TAA is back until a restart: " + c.upscaler.error()).c_str());
        return false;
    }

    if (timed)
        g_timer.mark(list, c.frame, 2);
    // The slider scales RCAS's lobe directly. AMD's stops jump from off to quarter strength.
    c.work.finish(list, sharpness, hdr, fx, reinterpret_cast<ID3D12Resource *>(target.handle));
    if (timed)
    {
        g_timer.mark(list, c.frame, 3);
        g_timer.resolve(list, c.frame);
    }
    restore(dev, list, st);
    publish_upscale(target.handle, upscale ? c.work.presented() : nullptr, c.work.color_format(), st.graphics.layout);

    c.force_reset = false;
    c.last_replaced = c.frame;
    g_taa_frame = g_frames.load();
    ++g_counts.replaced;
    set_status(c, taa_state::running);
    return true;
}

//----------------------------------------------------------------------------

void on_init_device(device *dev)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    std::lock_guard lock(g_ctx_lock);
    if (g_ctx != nullptr)
        return;
    auto c = std::make_unique<context>();
    c->dev = dev;
    auto *native = reinterpret_cast<ID3D12Device *>(dev->get_native());
    log_adapter(native);
    if (!c->upscaler.init(native, g_settings.debug))
        c->error = c->upscaler.error();
    else if (!c->work.init(native))
        c->error = c->work.error();
    else
        c->ready = true;
    // After NGX init so its modules show up too.
    log_modules();

    if (c->ready)
    {
        set_status(*c, taa_state::waiting);
        acv_log(reshade::log::level::info, "DLSS is ready. Waiting for the game's TAA pass");
    }
    else
    {
        set_status(*c, taa_state::unavailable, c->error);
        acv_log(reshade::log::level::warning, ("DLSS can't start: " + c->error).c_str());
    }
    g_ctx = std::move(c);
}

void on_destroy_device(device *dev)
{
    std::lock_guard lock(g_ctx_lock);
    if (g_ctx == nullptr || g_ctx->dev != dev)
        return;
    if (g_ctx->queue != nullptr)
        g_ctx->queue->wait_idle();
    g_ctx->work.shutdown();
    g_ctx->upscaler.shutdown();
    g_timer.shutdown();
    g_ctx.reset();
}

template <typename Range>
table_layout describe_table(uint32_t count, const Range *ranges)
{
    table_layout t;
    t.is_table = true;
    for (uint32_t r = 0; r < count; ++r)
    {
        const descriptor_range &range = ranges[r];
        if (range.type == descriptor_type::constant_buffer && (t.first_cbv < 0 || int32_t(range.binding) < t.first_cbv))
            t.first_cbv = int32_t(range.binding);
        if (range.type == descriptor_type::constant_buffer && range.dx_register_index == 0 &&
            range.dx_register_space < std::size(t.b0))
            t.b0[range.dx_register_space] = int32_t(range.binding);
        // Unbounded ranges report UINT32_MAX. Only the first slots are used.
        const uint32_t slots = std::min<uint32_t>(range.count, MAX_TABLE_TEXTURES);
        std::vector<uint32_t> *into = range.type == descriptor_type::shader_resource_view    ? &t.textures
                                      : range.type == descriptor_type::unordered_access_view ? &t.writes
                                                                                             : nullptr;
        if (into != nullptr)
            for (uint32_t k = 0; k < slots; ++k)
                into->push_back(range.binding + k);
    }
    std::sort(t.textures.begin(), t.textures.end());
    if (t.textures.size() > MAX_TABLE_TEXTURES)
        t.textures.resize(MAX_TABLE_TEXTURES);
    return t;
}

void on_init_pipeline_layout(device *dev, uint32_t count, const pipeline_layout_param *params, pipeline_layout layout)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    auto tables = std::make_shared<layout_tables>(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const pipeline_layout_param &p = params[i];
        if (p.type == pipeline_layout_param_type::descriptor_table)
            (*tables)[i] = describe_table(p.descriptor_table.count, p.descriptor_table.ranges);
        else if (p.type == pipeline_layout_param_type::descriptor_table_with_flags)
            (*tables)[i] = describe_table(p.descriptor_table_with_flags.count, p.descriptor_table_with_flags.ranges);
        else if (p.type == pipeline_layout_param_type::push_descriptors &&
                 p.push_descriptors.type == descriptor_type::constant_buffer &&
                 p.push_descriptors.dx_register_index == 0 &&
                 p.push_descriptors.dx_register_space < std::size(table_layout{}.b0))
            (*tables)[i].b0[p.push_descriptors.dx_register_space] = 0;
    }

    std::unique_lock lock(g_layouts_lock);
    g_layouts[layout.handle] = std::move(tables);
}

void on_destroy_pipeline_layout(device *, pipeline_layout layout)
{
    std::unique_lock lock(g_layouts_lock);
    g_layouts.erase(layout.handle);
}

void on_init_resource(device *dev, const resource_desc &desc, const subresource_data *, resource_usage, resource res)
{
    if (dev->get_api() != device_api::d3d12 || desc.type != resource_type::texture_2d)
        return;
    probe_scope probe(P_INIT_RESOURCE);
    const uint64_t asked = g_settings.debug ? now_ns() : 0;
    std::unique_lock lock(g_textures_lock);
    if (asked != 0)
        probe_add(P_TEXTURE_LOCK, now_ns() - asked);
    g_textures[res.handle] = {desc.texture.width, desc.texture.height, desc.texture.format};
}

// Freed addresses get reused by other textures.
void forget_frame(uint64_t handle)
{
    uint64_t expected = handle;
    g_taa_output.compare_exchange_strong(expected, 0);

    std::lock_guard lock(g_upscale.lock);
    for (uint64_t &target : g_upscale.taa_targets)
        if (target == handle)
            target = 0;
    for (uint64_t &output : g_upscale.fsr_chain)
        if (output == handle)
            output = 0;
}

void on_destroy_resource(device *, resource res)
{
    probe_scope probe(P_DESTROY_RESOURCE);
    {
        const uint64_t asked = g_settings.debug ? now_ns() : 0;
        std::unique_lock lock(g_textures_lock);
        if (asked != 0)
            probe_add(P_TEXTURE_LOCK, now_ns() - asked);
        if (g_textures.erase(res.handle) == 0)
            return;
    }
    forget_frame(res.handle);
}

void on_destroy_command_list(command_list *cmd)
{
    if (cmd->get_private_data<list_state>() != nullptr)
    {
        ++g_list_generation;
        cmd->destroy_private_data<list_state>();
    }
}

void on_reset_command_list(command_list *cmd)
{
    if (list_state *s = find_state(cmd))
    {
        disarm(*s, "command list ended before a draw");
        s->reset();
    }
}

bool is_graphics(shader_stage stages)
{
    return (stages & shader_stage::all_compute) == 0;
}

void on_bind_descriptor_tables(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t first,
                               uint32_t count, const descriptor_table *tables, uint32_t, const uint32_t *)
{
    if (g_ctx == nullptr)
        return;
    probe_scope probe(P_BIND);
    list_state &st = state_of(cmd);
    const bool graphics = is_graphics(stages);
    root_state &root = graphics ? st.graphics : st.compute;

    // A root signature change arrives as an empty bind.
    if (count == 0)
    {
        root.set_layout(layout);
        if (graphics)
            disarm(st, "root signature changed before a draw");
        return;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        root_value &v = root.at(first + i);
        v.kind = root_kind::table;
        v.value = tables[i].handle;
        if (g_settings.debug && g_frames % TRACE_EVERY == 0)
            trace_reads(cmd, st, graphics, first + i, tables[i]);
        if (!graphics)
        {
            probe_scope delivery(P_DELIVERY);
            check_delivery(cmd, st, first + i, tables[i]);
            continue;
        }
        {
            probe_scope redirect(P_REDIRECT);
            check_redirect(cmd, st, first + i, tables[i]);
        }
        if (st.armed && first + i == st.taa_param)
        {
            if (tables[i].handle == st.taa_table)
                continue;
            disarm(st, "TAA table replaced before a draw");
        }
        if (g_settings.enabled && g_ctx->ready)
        {
            probe_scope match(P_MATCH);
            try_match(cmd, st, first + i, tables[i]);
        }
    }
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t param,
                         const descriptor_table_update &update)
{
    if (g_ctx == nullptr || update.count == 0)
        return;
    list_state &st = state_of(cmd);
    root_value &v = (is_graphics(stages) ? st.graphics : st.compute).at(param);
    switch (update.type)
    {
    case descriptor_type::constant_buffer:
    {
        // D3D12 wants the GPU address.
        const auto &range = *static_cast<const buffer_range *>(update.descriptors);
        auto *buffer = reinterpret_cast<ID3D12Resource *>(range.buffer.handle);
        v.kind = root_kind::cbv;
        v.buffer = range;
        v.value = buffer != nullptr ? buffer->GetGPUVirtualAddress() + range.offset : 0;
        break;
    }
    // Root SRVs and UAVs come as GPU addresses.
    case descriptor_type::buffer_shader_resource_view:
        v.kind = root_kind::srv;
        v.value = *static_cast<const uint64_t *>(update.descriptors);
        break;
    case descriptor_type::buffer_unordered_access_view:
        v.kind = root_kind::uav;
        v.value = *static_cast<const uint64_t *>(update.descriptors);
        break;
    default:
        break;
    }
}

void on_push_constants(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t param, uint32_t first,
                       uint32_t count, const void *values)
{
    if (g_ctx == nullptr)
        return;
    list_state &st = state_of(cmd);
    root_value &v = (is_graphics(stages) ? st.graphics : st.compute).at(param);
    v.kind = root_kind::constants;
    if (v.constants.size() < first + count)
        v.constants.resize(first + count);
    memcpy(v.constants.data() + first, values, count * sizeof(uint32_t));
}

uint32_t fnv1a(const void *data, size_t size)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i)
        h = (h ^ static_cast<const uint8_t *>(data)[i]) * 16777619u;
    return h;
}

void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *objects, pipeline handle)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (objects[i].type != pipeline_subobject_type::pixel_shader || objects[i].data == nullptr)
            continue;
        const auto *shader = static_cast<const shader_desc *>(objects[i].data);
        if (shader->code == nullptr)
            return;
        const uint32_t hash = fnv1a(shader->code, shader->code_size);
        for (screen_effect *e : SCREEN_EFFECTS)
        {
            if (hash != e->shaders[0] && hash != e->shaders[1])
                continue;
            for (auto &slot : e->pipelines)
            {
                uint64_t empty = 0;
                if (slot.compare_exchange_strong(empty, handle.handle))
                {
                    g_effect_pipeline_bits.fetch_or(pipeline_bit(handle.handle));
                    return;
                }
            }
        }
        return;
    }
}

void on_destroy_pipeline(device *, pipeline handle)
{
    for (screen_effect *e : SCREEN_EFFECTS)
        for (auto &slot : e->pipelines)
        {
            uint64_t mine = handle.handle;
            slot.compare_exchange_strong(mine, 0);
        }
}

screen_effect *screen_effect_of(uint64_t pipeline)
{
    if (pipeline == 0 || (g_effect_pipeline_bits.load(std::memory_order_relaxed) & pipeline_bit(pipeline)) == 0)
        return nullptr;
    for (screen_effect *e : SCREEN_EFFECTS)
        for (const auto &slot : e->pipelines)
            if (slot.load(std::memory_order_relaxed) == pipeline)
                return e;
    return nullptr;
}

buffer_range find_b0(device *dev, const root_state &root, uint32_t space)
{
    if (root.tables == nullptr || space >= std::size(table_layout{}.b0))
        return {};
    for (uint32_t i = 0; i < uint32_t(root.params.size()) && i < root.tables->size(); ++i)
    {
        const int32_t at = (*root.tables)[i].b0[space];
        if (at < 0)
            continue;
        const root_value &v = root.params[i];
        if (v.kind == root_kind::cbv)
            return v.buffer;
        if (v.kind != root_kind::table)
            return {};
        const auto *tracking = descriptor_tracker::of(dev);
        if (tracking == nullptr)
            return {};
        descriptor_heap heap = {0};
        uint32_t offset = 0;
        dev->get_descriptor_heap_offset({v.value}, uint32_t(at), 0, &heap, &offset);
        return buffer_at(*tracking, heap, offset);
    }
    return {};
}

// The GPU reads the constants when the list runs. Writing now still changes this draw.
void take_out(screen_effect &e, command_list *cmd, const list_state &st)
{
    if (!g_settings.enabled || g_taa_frame == 0 || g_frames > g_taa_frame + 2)
        return;
    device *dev = cmd->get_device();
    const buffer_range range = find_b0(dev, st.graphics, e.space);
    void *data = nullptr;
    if (range.buffer.handle == 0 ||
        !dev->map_buffer_region(range.buffer, range.offset, e.floats * 4, map_access::read_write, &data) ||
        data == nullptr)
        return;
    float values[16] = {};
    memcpy(values, data, e.floats * 4);
    float neutral[16];
    memcpy(neutral, values, sizeof(neutral));
    if (&e == &g_vignette)
    {
        if (g_settings.vignette_mode.load() == 0)
        {
            // The game's own vignette, scaled. Nothing to redraw.
            float strength = values[0] * g_settings.vignette_strength.load();
            memcpy(data, &strength, sizeof(strength));
            dev->unmap_buffer_region(range.buffer);
            std::lock_guard lock(e.lock);
            ++e.reads;
            e.values[0] = e.last[0] = values[0];
            return;
        }
        neutral[0] = 0.0f; // strength
    }
    else if (&e == &g_radial)
    {
        neutral[8] = 0.0f; // no blur
        neutral[12] = neutral[13] = 0.0f;
        neutral[14] = neutral[15] = 1.0f; // no darkening
    }
    else if (&e == &g_radial_plain)
    {
        neutral[8] = 0.0f; // no blur
    }
    else if (&e == &g_double)
    {
        neutral[4] = 0.0f; // no second image
    }
    else
    {
        neutral[6] = 0.0f; // no distortion
        neutral[9] = neutral[12] = neutral[13] = neutral[14] = 0.0f; // nothing added
    }
    memcpy(data, neutral, e.floats * 4);
    dev->unmap_buffer_region(range.buffer);
    std::lock_guard lock(e.lock);
    ++e.reads;
    e.waiting = true;
    memcpy(e.values, values, sizeof(values));
    memcpy(e.last, values, sizeof(values));
    e.frame = g_frames;
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipeline)
{
    if (g_ctx == nullptr || stages != pipeline_stage::all)
        return;
    probe_scope probe(P_PIPELINE);
    list_state &st = state_of(cmd);
    st.pipeline = reinterpret_cast<ID3D12PipelineState *>(pipeline.handle);
    st.effect_draw = screen_effect_of(pipeline.handle);
}

void on_bind_render_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view)
{
    if (g_ctx != nullptr)
        state_of(cmd).render_target = count != 0 ? rtvs[0] : resource_view{0};
}

bool on_any_draw(command_list *cmd)
{
    if (g_ctx == nullptr)
        return false;
    probe_scope probe(P_DRAW);
    list_state *st = find_state(cmd);
    if (st == nullptr)
        return false;
    if (st->effect_draw != nullptr)
    {
        take_out(*st->effect_draw, cmd, *st);
        st->effect_draw = nullptr;
    }
    if (!st->trace.empty())
        flush_trace(*st, draw_target(cmd, *st));
    if (st->redirect.pending)
        apply_redirect(cmd, *st);
    if (!st->armed)
        return false;
    if (st->skipped >= MAX_SKIPPED_DRAWS || !g_settings.enabled)
    {
        st->armed = false;
        return false;
    }
    if (!st->replaced)
    {
        probe_scope recording(P_REPLACE);
        if (!replace(cmd, *st))
        {
            st->armed = false;
            return false;
        }
        st->replaced = true;
    }
    ++st->skipped;
    ++g_counts.skipped_draws;
    return true;
}

bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t)
{
    return on_any_draw(cmd);
}

bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
    return on_any_draw(cmd);
}

// ExecuteIndirect with mixed arguments reports unknown, which may draw too.
bool on_draw_indirect(command_list *cmd, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
{
    if (type == indirect_command::draw || type == indirect_command::draw_indexed || type == indirect_command::unknown)
        return on_any_draw(cmd);
    return false;
}

bool on_dispatch(command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{
    list_state *st = g_ctx != nullptr ? find_state(cmd) : nullptr;
    if (st == nullptr)
        return false;
    probe_scope probe(P_DRAW);
    if (!st->trace.empty())
        flush_trace(*st, "a dispatch of " + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z) + " groups");
    // Skip the dispatch so EASU doesn't overwrite our frame.
    return st->delivery.pending && deliver(cmd, *st);
}

// Debug only. Hitches are logged with our share of that frame.
struct frame_stats
{
    uint64_t last_present = 0;
    double average_ms = 0.0; // moving average. Hitches stand out against it
    std::vector<float> times;
    uint32_t hitches = 0, hitches_logged = 0, dlss_frames = 0;
    uint64_t seen_ns[P_COUNT] = {}, seen_calls[P_COUNT] = {};
    double timestamp_hz = 0.0;
    double gpu_ms[3] = {}, gpu_worst_ms[3] = {}, last_gpu_ms[3] = {};
    uint32_t gpu_frames = 0;
    // Over budget Windows evicts memory and the GPU stalls.
    IDXGIAdapter3 *adapter = nullptr;
    double vram_gb = 0.0, budget_gb = 0.0, worst_vram_gb = 0.0;
    uint32_t over_budget = 0;
};
frame_stats g_stats;

const uint32_t MAX_HITCH_LINES = 20;
const char *const GPU_SPANS[3] = {"input conversion", "DLSS", "sharpening and copies"};

IDXGIAdapter3 *adapter_of(ID3D12Device *device)
{
    IDXGIFactory4 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return nullptr;
    IDXGIAdapter3 *adapter = nullptr;
    factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    factory->Release();
    return adapter;
}

void time_frame(context &c)
{
    frame_stats &f = g_stats;
    const uint64_t now = now_ns();

    if (f.adapter == nullptr)
        f.adapter = adapter_of(reinterpret_cast<ID3D12Device *>(c.dev->get_native()));
    DXGI_QUERY_VIDEO_MEMORY_INFO memory = {};
    if (f.adapter != nullptr && SUCCEEDED(f.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
    {
        f.vram_gb = double(memory.CurrentUsage) / double(1 << 30);
        f.budget_gb = double(memory.Budget) / double(1 << 30);
        f.worst_vram_gb = std::max(f.worst_vram_gb, f.vram_gb);
        if (memory.CurrentUsage > memory.Budget)
            ++f.over_budget;
    }
    const bool ran = c.last_replaced + 1 == c.frame;
    if (ran)
        ++f.dlss_frames;

    if (f.timestamp_hz == 0.0 && c.queue != nullptr)
    {
        UINT64 hz = 0;
        if (SUCCEEDED(reinterpret_cast<ID3D12CommandQueue *>(c.queue->get_native())->GetTimestampFrequency(&hz)))
            f.timestamp_hz = double(hz);
    }
    uint64_t spans[gpu_timer::MARKS - 1];
    if (f.timestamp_hz != 0.0 && c.frame > gpu_timer::RING && g_timer.read(c.frame - (gpu_timer::RING - 2), spans))
    {
        ++f.gpu_frames;
        for (int i = 0; i < 3; ++i)
        {
            f.last_gpu_ms[i] = double(spans[i]) * 1000.0 / f.timestamp_hz;
            f.gpu_ms[i] += f.last_gpu_ms[i];
            f.gpu_worst_ms[i] = std::max(f.gpu_worst_ms[i], f.last_gpu_ms[i]);
        }
    }

    // All threads since the last present.
    uint64_t frame_ns[P_COUNT], frame_calls[P_COUNT];
    for (size_t p = 0; p < P_COUNT; ++p)
    {
        const uint64_t ns = g_probes[p].ns, calls = g_probes[p].calls;
        frame_ns[p] = ns - f.seen_ns[p];
        frame_calls[p] = calls - f.seen_calls[p];
        f.seen_ns[p] = ns;
        f.seen_calls[p] = calls;
    }

    if (f.last_present != 0)
    {
        const double ms = double(now - f.last_present) / 1e6;
        f.times.push_back(float(ms));
        const bool hitch = f.average_ms > 0.0 && ms > f.average_ms * 1.5 && ms > f.average_ms + 5.0;
        if (hitch)
        {
            ++f.hitches;
            if (f.hitches_logged < MAX_HITCH_LINES)
            {
                ++f.hitches_logged;
                std::string line;
                char part[96];
                for (size_t p = 0; p < P_COUNT; ++p)
                {
                    if (frame_ns[p] < 100000)
                        continue;
                    snprintf(part, sizeof(part), "%s%s %.2f ms (%llu)", line.empty() ? "" : ", ", PROBE_NAMES[p],
                             double(frame_ns[p]) / 1e6, static_cast<unsigned long long>(frame_calls[p]));
                    line += part;
                }
                debug_event("slow frame: %.1f ms against %.1f ms, DLSS %s, video memory %.2f of %.2f GB; add-on CPU "
                            "in it: %s; GPU last measured: %.2f / %.2f / %.2f ms",
                            ms, f.average_ms, ran ? "on" : "off", f.vram_gb, f.budget_gb,
                            line.empty() ? "under 0.1 ms everywhere" : line.c_str(), f.last_gpu_ms[0],
                            f.last_gpu_ms[1], f.last_gpu_ms[2]);
            }
        }
        f.average_ms = f.average_ms == 0.0 ? ms : f.average_ms * 0.95 + ms * 0.05;
    }
    f.last_present = now;
}

void summarize_frames()
{
    frame_stats &f = g_stats;
    if (!f.times.empty())
    {
        std::vector<float> sorted = f.times;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (float t : sorted)
            sum += t;
        debug_event("frame time: average %.2f ms, 1%% slowest from %.2f ms, worst %.2f ms, %u slow frames; DLSS ran "
                    "in %u of %zu frames",
                    sum / double(sorted.size()), sorted[sorted.size() * 99 / 100], sorted.back(), f.hitches,
                    f.dlss_frames, sorted.size());
    }

    const double frames = f.times.empty() ? 1.0 : double(f.times.size());
    std::string line, waits;
    char part[128];
    for (size_t p = 0; p < P_COUNT; ++p)
    {
        const uint64_t ns = g_probes[p].ns.exchange(0), calls = g_probes[p].calls.exchange(0);
        const uint64_t worst = g_probes[p].worst_ns.exchange(0);
        snprintf(part, sizeof(part), "%s%s %.3f ms in %.0f calls, slowest %.0f us", line.empty() ? "" : "; ",
                 PROBE_NAMES[p], double(ns) / 1e6 / frames, double(calls) / frames, double(worst) / 1e3);
        line += part;
        // The slowest value alone doesn't show if it happened once or often.
        const uint64_t long_calls = g_probes[p].long_calls.exchange(0), long_ns = g_probes[p].long_ns.exchange(0);
        if (long_calls != 0)
        {
            snprintf(part, sizeof(part), "%s%s %llu times, %.1f ms in all", waits.empty() ? "" : "; ", PROBE_NAMES[p],
                     static_cast<unsigned long long>(long_calls), double(long_ns) / 1e6);
            waits += part;
        }
        f.seen_ns[p] = 0;
        f.seen_calls[p] = 0;
    }
    debug_event("add-on CPU per frame: %s", line.c_str());
    if (!waits.empty())
        debug_event("calls over 0.5 ms in these %.0f frames: %s", frames, waits.c_str());

    if (f.gpu_frames != 0)
        debug_event("GPU per DLSS frame: %s %.3f ms (worst %.3f), %s %.3f ms (worst %.3f), %s %.3f ms (worst %.3f)",
                    GPU_SPANS[0], f.gpu_ms[0] / f.gpu_frames, f.gpu_worst_ms[0], GPU_SPANS[1],
                    f.gpu_ms[1] / f.gpu_frames, f.gpu_worst_ms[1], GPU_SPANS[2], f.gpu_ms[2] / f.gpu_frames,
                    f.gpu_worst_ms[2]);

    debug_event("video memory: %.2f GB now, highest %.2f GB, budget %.2f GB, over budget in %u frames", f.vram_gb,
                f.worst_vram_gb, f.budget_gb, f.over_budget);

    const uint64_t last_present = f.last_present;
    const double average_ms = f.average_ms, timestamp_hz = f.timestamp_hz;
    IDXGIAdapter3 *adapter = f.adapter;
    f = frame_stats();
    f.last_present = last_present;
    f.average_ms = average_ms;
    f.timestamp_hz = timestamp_hz;
    f.adapter = adapter;
    f.times.reserve(600);
}

// Runs every frame with or without Debug. It's cheap and crash reports need it.
void check_device(context &c)
{
    // Catches a driver crash or TDR first.
    static bool device_lost = false;
    const HRESULT removed = reinterpret_cast<ID3D12Device *>(c.dev->get_native())->GetDeviceRemovedReason();
    if (!device_lost && FAILED(removed))
    {
        device_lost = true;
        char text[96];
        snprintf(text, sizeof(text), "the graphics device was lost (0x%08lX)", static_cast<unsigned long>(removed));
        acv_log(reshade::log::level::error, text);
    }

    // Menus have no TAA. Log once after a minute if DLSS never ran.
    if (c.frame != NOTE_FRAMES || !c.ready || c.failed || !g_settings.enabled || g_taa_frame != 0)
        return;
    std::string text = "no TAA pass found in the first " + std::to_string(NOTE_FRAMES) +
                       " frames. Normal in the menus. In play it means the game's Anti-aliasing is not on "
                       "High or another mod replaces it.";
    std::lock_guard guard(g_near_miss_lock);
    if (g_near_misses.empty())
        text += " No table came close.";
    else
        text += " Tables that came closest:";
    acv_log(reshade::log::level::info, text.c_str());
    for (const std::string &line : g_near_misses)
        acv_log(reshade::log::level::info, ("  " + line).c_str());
}

// Debug only. Called from present for frame timing and a summary every 600 frames.
void debug_frame(context &c)
{
    time_frame(c);

    if (c.frame % 600 != 0)
        return;
    summarize_frames();

    const uint32_t replaced = g_counts.replaced.exchange(0);
    const uint64_t frame_us = g_counts.frame_us.exchange(0);
    debug_event("last 600 frames: DLSS ran %u times, %.2f ms a frame; %u history resets, %u frames without motion "
                "vectors, %u TAA draws skipped, %u screen pass redirects, %u FSR 1 passes replaced, %u give-ups",
                replaced, replaced != 0 ? double(frame_us) / 1000.0 / replaced : 0.0, g_counts.resets.exchange(0),
                g_counts.camera_only.exchange(0), g_counts.skipped_draws.exchange(0), g_counts.redirects.exchange(0),
                g_counts.deliveries.exchange(0), g_counts.given_up.exchange(0));
    debug_event("state: %s, %ux%u to %ux%u, preset %u, sharpness %.0f%%, LOD bias %+.2f on %u samplers",
                c.status.state == taa_state::running ? "running" : "not running", c.status.width, c.status.height,
                c.status.output_width, c.status.output_height, unsigned(g_settings.preset),
                g_settings.sharpness * 100.0f, lod_bias(), lod_biased_samplers());

    size_t textures, layouts;
    {
        std::shared_lock lock_textures(g_textures_lock);
        textures = g_textures.size();
    }
    {
        std::shared_lock lock_layouts(g_layouts_lock);
        layouts = g_layouts.size();
    }
    debug_event("tables: %u checked, %u with no readable texture, %u matched; %zu textures and %zu root signatures known",
                g_tables_checked.exchange(0), g_tables_unreadable.exchange(0), g_tables_matched.exchange(0), textures,
                layouts);

    static bool biases_logged = false;
    if (!biases_logged)
    {
        biases_logged = true;
        debug_event("the game's own bias on its tiling texture samplers: %s", lod_game_biases().c_str());
    }
}
} // namespace

void taa_register()
{
    reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_tables_begin);
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(on_update_tables_begin);
    descriptor_tracker::register_events();
    reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_tables_end);
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(on_update_tables_end);
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(on_init_pipeline_layout);
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);
    reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
    reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
    reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
    reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
    reshade::register_event<reshade::addon_event::draw>(on_draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
    reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
}

void taa_unregister()
{
    reshade::unregister_event<reshade::addon_event::copy_descriptor_tables>(on_copy_tables_begin);
    reshade::unregister_event<reshade::addon_event::update_descriptor_tables>(on_update_tables_begin);
    descriptor_tracker::unregister_events();
    reshade::unregister_event<reshade::addon_event::copy_descriptor_tables>(on_copy_tables_end);
    reshade::unregister_event<reshade::addon_event::update_descriptor_tables>(on_update_tables_end);
}

// Whether this frame counts toward a pipeline cache rebuild. Normally only gameplay frames count. Without
// DLSS menus can't be told from play. Then every frame counts or the cache would be rebuilt at every start.
static bool present_frame(command_queue *queue, uint32_t display_width, uint32_t display_height)
{
    std::lock_guard ctx_lock(g_ctx_lock);
    if (g_ctx == nullptr)
        return false;
    context &c = *g_ctx;
    std::lock_guard lock(c.lock);
    c.queue = queue;
    ++g_frames;
    c.display_width = display_width;
    c.display_height = display_height;
    // Ran without DLSS. Give the screen pass the game's image again.
    if (c.frame > c.last_replaced)
        stop_upscaling();
    ++c.frame;
    c.work.collect(c.frame);
    check_device(c);
    if (g_settings.debug)
    {
        debug_frame(c);
        log_screen_effects();
    }
    if (!c.ready || c.failed)
        return true;
    const taa_state before = c.status.state;
    if (!g_settings.enabled)
        set_status(c, taa_state::off);
    else if (c.frame > c.last_seen + IDLE_FRAMES)
        set_status(c, taa_state::waiting);
    if (before == taa_state::running && c.status.state == taa_state::waiting)
        debug_event("no TAA pass for %llu frames (a menu, a loading screen or the map)",
                    static_cast<unsigned long long>(IDLE_FRAMES));
    const bool never_found = g_taa_frame == 0 && c.frame > NOTE_FRAMES;
    return c.status.state == taa_state::running || !g_settings.enabled || never_found;
}

void taa_present(command_queue *queue, uint32_t display_width, uint32_t display_height)
{
    // Outside the lock. It may write a file.
    lod_frame(present_frame(queue, display_width, display_height));
}

void taa_restart()
{
    std::lock_guard ctx_lock(g_ctx_lock);
    if (g_ctx == nullptr)
        return;
    std::lock_guard lock(g_ctx->lock);
    g_ctx->force_reset = true;
}

bool taa_shared_textures(shared_textures &out)
{
    std::lock_guard ctx_lock(g_ctx_lock);
    if (g_ctx == nullptr)
        return false;
    context &c = *g_ctx;
    std::lock_guard lock(c.lock);
    // A frame or two without the pass is normal. A longer gap (a menu, DLSS off) counts as stopped.
    if (c.last_replaced == 0 || c.frame > c.last_replaced + 3)
        return false;
    out.motion = c.work.motion();
    out.depth = c.work.depth();
    out.hudless = c.work.hudless();
    out.hudless_format = uint32_t(c.work.color_format());
    return out.motion != nullptr && out.depth != nullptr;
}

taa_status taa_get_status()
{
    std::lock_guard ctx_lock(g_ctx_lock);
    if (g_ctx == nullptr)
        return {};
    std::lock_guard lock(g_ctx->lock);
    return g_ctx->status;
}
