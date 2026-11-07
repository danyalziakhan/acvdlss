// Finds Valhalla's TAA pass and replaces it with DLSS.
//
// The pass is recognized by the descriptor table it binds: seven textures of
// one size in a fixed order of formats, of which the first three are the
// color, the packed motion vectors and the linear depth. Its own constant
// buffer has the jitter, and the view constants in root parameter 1 have the
// camera. On the first draw with that table bound, prepare.hlsl converts the
// inputs, DLSS runs, and the result is copied into the render target the draw
// would have written; the TAA draws themselves are skipped. The game's
// command list then gets back every piece of state we changed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include "acvdlss.h"
#include "dlss.h"
#include "passes.h"

#include <reshade.hpp>
#include <descriptor_tracking.hpp>

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
// The root parameter whose table holds the per-view constant buffer.
const uint32_t VIEW_PARAM = 1;

// Byte offsets into the game's two constant buffers: the TAA pass's own and
// the per-view one.
namespace cb
{
const uint32_t TAA_SIZE = 0x100;
const uint32_t HISTORY_VALID = 0x00; // cleared on camera cuts
const uint32_t JITTER = 0x30;        // x, y in pixels
const uint32_t SKY_RANGE = 0x60;     // (a - b) / (c - d) is the sky's view depth

const uint32_t VIEW_SIZE = 0x490;
const uint32_t CAM_TO_WORLD = 0x60;     // three rows
const uint32_t PROJ_22 = 0x1c8;         // with PROJ_23, turns view depth into device depth
const uint32_t PROJ_23 = 0x1cc;
const uint32_t PREV_VIEW_PROJ_X = 0x260;
const uint32_t PREV_VIEW_PROJ_Y = 0x270;
const uint32_t PREV_VIEW_PROJ_W = 0x290;
const uint32_t SKY_DEVICE_DEPTH = 0x364;
const uint32_t INV_PROJ = 0x450;        // four rows
} // namespace cb
// The game issues a few draws with the TAA table bound; all of them belong to
// the pass we replace.
const uint32_t MAX_SKIPPED_DRAWS = 3;
// Texture slots looked at per table. The game declares generous ranges and
// leaves most of them empty, so this is a bound on the work, not a filter.
const size_t MAX_TABLE_TEXTURES = 64;
// Frames without a TAA pass before the panel says it is waiting.
const uint64_t IDLE_FRAMES = 30;

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

// Seven textures of one size: color, motion, depth (R32F), a mask (R8), color,
// a mask, color. Color is RGBA8, or RGBA16F with HDR on, and slot 1 is always
// RGBA8. Dialogue cutscenes bind a 4x4 placeholder in slot 1 and leave the
// seventh slot empty, so both are allowed. The width only has to be 480 or
// more, 25% resolution scale on a 1920 wide screen; the formats and sizes are
// what identify the pass.
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

// What a root signature's descriptor tables contain, worked out once when the
// game creates it.
struct table_layout
{
    bool is_table = false;
    std::vector<uint32_t> textures; // table offsets of every texture SRV, in order
    std::vector<uint32_t> writes;   // and of every texture UAV, for the debug trace
    int32_t first_cbv = -1;
};

using layout_tables = std::vector<table_layout>;

// Shared so a command list can hold on to its root signature's entry without
// a lock, even if the game destroys the root signature meanwhile.
std::shared_mutex g_layouts_lock;
std::unordered_map<uint64_t, std::shared_ptr<const layout_tables>> g_layouts;

std::shared_ptr<const layout_tables> find_layout(pipeline_layout layout)
{
    std::shared_lock lock(g_layouts_lock);
    const auto it = g_layouts.find(layout.handle);
    return it != g_layouts.end() ? it->second : nullptr;
}

// Root state of one command list, kept so it can be put back after DLSS has
// bound its own heaps, root signature and pipeline.
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
    bool discovering = false; // the screen pass is not known yet; check the target
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
};

struct __declspec(uuid("3c8e1f52-6a4d-4b9e-8d27-91f0a6c4e7b3")) list_state
{
    root_state graphics, compute;
    ID3D12PipelineState *pipeline = nullptr;
    resource_view render_target = {0};

    // Set when the TAA table is bound, cleared when it goes away.
    bool armed = false;
    bool replaced = false;
    uint32_t taa_param = 0;
    uint64_t taa_table = 0;
    uint32_t skipped = 0;
    texture_entry inputs[3];

    // Debug trace of a pass reading the TAA output, written out with the draw
    // or dispatch that follows.
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
        armed = false;
        trace.clear();
        redirect = {};
        delivery = {};
    }
};

list_state &state_of(command_list *cmd)
{
    if (list_state *s = cmd->get_private_data<list_state>())
        return *s;
    return *cmd->create_private_data<list_state>();
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
    bool have_jitter = false;
    float jitter[2] = {};
    std::chrono::steady_clock::time_point last_time;
    taa_status status;
};

std::unique_ptr<context> g_ctx;
std::mutex g_ctx_lock;

//----------------------------------------------------------------------------
// Debug counts, written out and cleared every 600 frames with Debug on.

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
    char text[256] = "ACV DLSS: ";
    va_list args;
    va_start(args, format);
    vsnprintf(text + 10, sizeof(text) - 10, format, args);
    va_end(args);
    reshade::log::message(reshade::log::level::info, text);
}


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

// descriptor_tracking throws for a heap it has not seen yet, and an exception
// leaving an event callback takes the game down with it.
resource_view view_at(const descriptor_tracking &tracking, descriptor_heap heap, uint32_t offset)
{
    if (heap.handle == 0)
        return {0};
    try
    {
        return tracking.get_resource_view(heap, offset);
    }
    catch (const std::out_of_range &)
    {
        return {0};
    }
}

buffer_range buffer_at(const descriptor_tracking &tracking, descriptor_heap heap, uint32_t offset)
{
    if (heap.handle == 0)
        return {};
    try
    {
        return tracking.get_buffer_range(heap, offset);
    }
    catch (const std::out_of_range &)
    {
        return {};
    }
}

// The constant buffer behind a root parameter, whether it is bound as a root
// CBV or as the first CBV in a table.
// Returns why it failed, or nullptr.
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
    const auto *tracking = dev->get_private_data<descriptor_tracking>();
    if (tracking == nullptr)
        return "no descriptor tracking";
    const buffer_range range = buffer_at(*tracking, heap, offset);
    if (range.buffer.handle == 0)
        return "CBV descriptor not tracked";
    return read_buffer(dev, range, out, size) ? nullptr : "CBV buffer could not be mapped";
}

// Everything prepare.hlsl needs except the jitter change, which depends on the
// previous frame.
prepare_constants make_constants(const uint8_t *taa_cb, const uint8_t *view_cb, uint32_t width, uint32_t height)
{
    prepare_constants k = {};
    k.size[0] = float(width);
    k.size[1] = float(height);
    k.size[2] = 1.0f / float(width);
    k.size[3] = 1.0f / float(height);

    // The game's own sky test sits a little inside the far plane.
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

// ReShade keeps a view's resource pointer after the resource is gone, and games
// leave such views in table slots their shaders never read. Asking a dead
// resource for its description crashes, so sizes come from this record of live
// textures instead.
struct texture_info
{
    uint32_t width, height;
    format fmt;
};
std::shared_mutex g_textures_lock;
std::unordered_map<uint64_t, texture_info> g_textures;

// The texture a table slot points at, or false for an empty or stale slot.
bool read_texture(device *dev, const descriptor_tracking &tracking, descriptor_table table, uint32_t binding,
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
        std::shared_lock lock(g_textures_lock);
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

// With Debug on: a table that holds a frame-size depth texture and enough
// others to be the TAA, but did not match. Each is written to the log once.
void log_near_miss(const list_state &st, uint32_t param, const texture_entry *entries, size_t n)
{
    static std::mutex lock;
    static std::unordered_set<uint64_t> seen;
    const bool has_depth = std::any_of(entries, entries + n, [](const texture_entry &e) {
        return e.fmt == format::r32_float && e.width >= 1280;
    });
    if (n < 6 || !has_depth)
        return;
    {
        std::lock_guard guard(lock);
        if (seen.size() >= 32 || !seen.insert(st.graphics.layout.handle ^ (uint64_t(param) << 48)).second)
            return;
    }

    char head[96];
    snprintf(head, sizeof(head), "ACV DLSS: no match for root signature %p, parameter %u:",
             reinterpret_cast<void *>(st.graphics.layout.handle), param);
    std::string line = head;
    for (size_t i = 0; i < n; ++i)
        line += " [" + describe(entries[i]) + (entries[i].texture_2d ? "" : " not 2D") + "]";
    reshade::log::message(reshade::log::level::info, line.c_str());
}

void try_match(command_list *cmd, list_state &st, uint32_t param, descriptor_table table)
{
    const table_layout *layout = st.graphics.table(param);
    if (layout == nullptr || layout->textures.size() < 6)
        return;

    device *dev = cmd->get_device();
    const auto *tracking = dev->get_private_data<descriptor_tracking>();
    if (tracking == nullptr)
        return;

    // Empty slots are left out, which is how the dialogue cutscenes' missing
    // seventh texture still lines the rest up.
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
        if (g_settings.debug)
            log_near_miss(st, param, entries, n);
        return;
    }
    ++g_tables_matched;
    st.armed = true;
    st.replaced = false;
    st.skipped = 0;
    st.taa_param = param;
    st.taa_table = table.handle;
    std::copy_n(entries + at, 3, st.inputs);
}

//----------------------------------------------------------------------------
// Debug trace: the passes that read the TAA output after it. Below 100%
// resolution scale one of them scales the frame up to the screen, and that is
// the pass DLSS has to hand its full-size image to.

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
    const auto *tracking = dev->get_private_data<descriptor_tracking>();
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

    // Once per pass and TAA size: the pass that scales the frame to the screen
    // only shows itself below 100%.
    const uint64_t key = root.layout.handle ^ (uint64_t(param) << 48) ^ (graphics ? 0 : 1ull << 63) ^ (taa_size << 8);
    {
        std::lock_guard lock(g_trace_lock);
        if (g_traced.size() >= MAX_TRACED || !g_traced.insert(key).second)
            return;
    }

    char head[128];
    snprintf(head, sizeof(head), "ACV DLSS: %s pass reads the TAA output; root signature %p, parameter %u; reads",
             graphics ? "graphics" : "compute", reinterpret_cast<void *>(root.layout.handle), param);
    st.trace = head + reads + (writes.empty() ? std::string() : "; writes" + writes);
}

void flush_trace(list_state &st, const std::string &what)
{
    if (st.trace.empty())
        return;
    reshade::log::message(reshade::log::level::info, (st.trace + "; then " + what).c_str());
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

// After DLSS the list holds our heaps, our compute root signature and our
// pipeline. Changing heaps also drops every bound table, the graphics ones
// included, so all of them are bound again.
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

// DLSS only uses the frame time as a hint, so a missing or absurd one becomes
// a 60 fps frame.
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
// Handing the upscaled frame to the screen. Below 100% the game has a pass that
// reads the TAA output and scales it up into a screen-size target. That pass
// gets our screen-size DLSS frame instead: the descriptor it reads the TAA
// output through is rewritten just before it draws, since D3D12 has no plain
// SRV rebind to do it with.

struct redirect_site
{
    uint64_t layout = 0;
    uint32_t param = 0;
    uint32_t binding = 0;
};

// Frames presented so far, for the discovery window below.
std::atomic<uint64_t> g_frames{0};
// After the frame changes size or texture, tables are searched this long for
// passes that read it. A pass found once stays known. More than one can turn
// up: with FSR on, a pass that reads last frame's TAA output for something
// screen-sized was found before the real screen pass, which at 100% read
// FSR 1's screen-size output and so did not count yet.
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
    // What FSR 1 would have written, when FSR is on in the game's settings.
    // The screen pass reads these in place of the TAA output, so they get the
    // same redirect. Two passes, each alternating between two textures.
    uint64_t fsr_chain[4] = {};
    uint32_t fsr_chain_next = 0;
    // Heap slots holding our frame. Put back if the game keeps the table and
    // DLSS stops upscaling.
    std::unordered_set<uint64_t> redirected;
};
upscale_state g_upscale;

void keep_discovering(upscale_state &u)
{
    u.discover_until = g_frames + DISCOVERY_FRAMES;
}

void publish_upscale(uint64_t taa_target, ID3D12Resource *presented, DXGI_FORMAT format, pipeline_layout taa_layout)
{
    std::lock_guard lock(g_upscale.lock);
    if (presented != nullptr && g_upscale.presented != presented)
        keep_discovering(g_upscale);
    g_upscale.presented = presented;
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
}

// At a table bind: does this table read the frame where a known screen pass
// does? While discovering, any table but the TAA's own that reads it is a
// candidate, confirmed at the draw by its target size.
void check_redirect(command_list *cmd, list_state &st, uint32_t param, descriptor_table table)
{
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
    const auto *tracking = dev->get_private_data<descriptor_tracking>();
    if (layout == nullptr || tracking == nullptr)
        return;

    // The TAA output, or what FSR 1 would have made of it.
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

// At the draw that follows: point the slot at our frame, or back at the
// game's if DLSS has stopped upscaling.
void apply_redirect(command_list *cmd, list_state &st)
{
    const pending_redirect r = st.redirect;
    st.redirect.pending = false;
    device *dev = cmd->get_device();

    if (r.discovering)
    {
        // The screen pass draws into a target bigger than the frame it reads.
        if (st.render_target.handle == 0)
            return;
        const resource_desc desc = dev->get_resource_desc(dev->get_resource_from_view(st.render_target));
        if (desc.texture.width <= r.original.width || desc.texture.height <= r.original.height)
            return;
        std::lock_guard lock(g_upscale.lock);
        if (g_upscale.sites.size() >= MAX_SITES)
            return;
        g_upscale.sites.push_back({st.graphics.layout.handle, r.param, r.binding});
        if (g_settings.debug)
        {
            char text[128];
            snprintf(text, sizeof(text), "ACV DLSS: screen pass found, root signature %p, parameter %u, slot %u",
                     reinterpret_cast<void *>(st.graphics.layout.handle), r.param, r.binding);
            reshade::log::message(reshade::log::level::info, text);
        }
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
        // Slots the game hands out afresh every frame would pile up here. A
        // slot it keeps is added again on the next frame.
        if (g_upscale.redirected.size() >= 256)
            g_upscale.redirected.clear();
        g_upscale.redirected.insert(slot_key);
    }
    else if (g_upscale.redirected.erase(slot_key) != 0)
    {
        view.Format = DXGI_FORMAT(r.original.fmt);
        native->CreateShaderResourceView(reinterpret_cast<ID3D12Resource *>(r.original.res.handle), &view, slot);
    }
}

// With FSR switched on in the game's settings, FSR 1 upscales in compute
// between the TAA and the screen pass: EASU scales the TAA output up to the
// resolution-scale size and RCAS sharpens that into another texture, which the
// screen pass then stretches to the screen. EASU and RCAS are skipped, and
// their outputs join the TAA output as what the screen pass is redirected
// from, so the DLSS frame goes from render size to the screen in one step and
// the game's sharpening never lands on top of it. The DLSS frame is also
// resampled into each skipped output, so nothing ever reads one unwritten.
void check_delivery(command_list *cmd, list_state &st, uint32_t param, descriptor_table table)
{
    uint64_t targets[2];
    {
        std::lock_guard lock(g_upscale.lock);
        if (g_upscale.presented == nullptr)
            return;
        std::copy(std::begin(g_upscale.taa_targets), std::end(g_upscale.taa_targets), targets);
    }
    const table_layout *layout = st.compute.table(param);
    device *dev = cmd->get_device();
    const auto *tracking = dev->get_private_data<descriptor_tracking>();
    if (layout == nullptr || layout->writes.empty() || tracking == nullptr)
        return;

    // EASU reads the TAA output and writes something bigger; RCAS reads what
    // EASU wrote and writes something the same size.
    texture_entry input;
    bool easu = false, rcas = false;
    for (uint32_t binding : layout->textures)
    {
        if (!read_texture(dev, *tracking, table, binding, input))
            continue;
        easu = input.res.handle == targets[0] || input.res.handle == targets[1];
        {
            std::lock_guard lock(g_upscale.lock);
            rcas = in_fsr_chain(g_upscale, input.res.handle);
        }
        if (easu || rcas)
            break;
    }
    if (!easu && !rcas)
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
            st.delivery = {true, output.res, output.fmt, output.width, output.height};
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
        {
            g_upscale.fsr_chain[g_upscale.fsr_chain_next++ % std::size(g_upscale.fsr_chain)] = d.target.handle;
            keep_discovering(g_upscale);
            debug_event("FSR 1 pass replaced, its output is %ux%u", d.width, d.height);
        }
    }

    // Written even when the screen pass will be pointed at our frame instead,
    // so a pass that is not, or not yet, still reads a picture and not
    // whatever the texture last held.
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

// With Debug on, says once why the TAA pass was left to the game.
bool give_up(const std::string &reason)
{
    if (!g_settings.debug)
        return false;
    ++g_counts.given_up;
    static std::mutex lock;
    static std::unordered_set<std::string> said;
    {
        std::lock_guard guard(lock);
        if (said.size() >= 32 || !said.insert(reason).second)
            return false;
    }
    reshade::log::message(reshade::log::level::info, ("ACV DLSS: TAA pass left to the game: " + reason).c_str());
    return false;
}

void disarm(list_state &st, const char *why)
{
    if (st.armed && !st.replaced)
        give_up(why);
    st.armed = false;
}

bool replace(command_list *cmd, list_state &st)
{
    context &c = *g_ctx;
    device *dev = cmd->get_device();
    const texture_entry &color = st.inputs[0], &motion = st.inputs[1], &depth = st.inputs[2];
    const uint32_t width = color.width, height = color.height;

    // The draw writes the TAA result into its first render target. Anything
    // else bound there means this is not the pass we know.
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
                       ", color is format " + std::to_string(unsigned(color.fmt)) + " " + std::to_string(width) +
                       "x" + std::to_string(height));
    if (g_settings.debug)
    {
        // The TAA output alternates between two textures, so only a size
        // change is worth a line.
        g_taa_output = target.handle;
        static std::atomic<uint64_t> logged_size{0};
        const uint64_t size = (uint64_t(width) << 32) | height;
        if (logged_size.exchange(size) != size)
        {
            char text[96];
            snprintf(text, sizeof(text), "ACV DLSS: TAA pass at %ux%u, color format %u", width, height, unsigned(color.fmt));
            reshade::log::message(reshade::log::level::info, text);
        }
    }

    uint8_t taa_cb[cb::TAA_SIZE], view_cb[cb::VIEW_SIZE];
    if (const char *why = read_constants(dev, st.graphics, st.taa_param, taa_cb, cb::TAA_SIZE))
        return give_up(std::string("TAA constant buffer: ") + why);
    if (const char *why = read_constants(dev, st.graphics, VIEW_PARAM, view_cb, cb::VIEW_SIZE))
        return give_up(std::string("view constant buffer: ") + why);

    std::lock_guard lock(c.lock);
    c.last_seen = c.frame;
    const bool hdr = color.fmt == format::r16g16b16a16_float;
    const bool camera_only = motion.width != width || motion.height != height;
    if (camera_only != c.status.camera_motion_only)
        debug_event(camera_only ? "the game stopped writing motion vectors (a dialogue cutscene); following the camera"
                                : "motion vectors are back");
    c.status.width = width;
    c.status.height = height;
    c.status.hdr = hdr;
    c.status.camera_motion_only = camera_only;
    if (c.failed)
        return false;

    // Below 100% resolution scale DLSS makes the screen-size frame itself. At
    // 100% and above it takes the TAA's place at the render size (DLAA), and
    // the game scales a larger frame down to the screen as it always does.
    const bool upscale = c.display_width > width && c.display_height > height;
    const uint32_t output_width = upscale ? c.display_width : width;
    const uint32_t output_height = upscale ? c.display_height : height;
    if (output_width != c.status.output_width || output_height != c.status.output_height ||
        c.status.state != taa_state::running)
        debug_event("%s %ux%u to %ux%u, screen %ux%u%s", upscale ? "upscaling" : "DLAA", width, height, output_width,
                    output_height, c.display_width, c.display_height, hdr ? ", HDR" : "");
    c.status.output_width = output_width;
    c.status.output_height = output_height;
    c.status.display_width = c.display_width;

    if (!c.work.resize(width, height, output_width, output_height, DXGI_FORMAT(color.fmt), c.frame))
    {
        c.failed = true;
        set_status(c, taa_state::failed, c.work.error());
        return false;
    }

    const float jx = read_float(taa_cb, cb::JITTER), jy = read_float(taa_cb, cb::JITTER + 4);
    prepare_constants k = make_constants(taa_cb, view_cb, width, height);
    if (c.have_jitter)
    {
        k.jitter_delta[0] = (c.jitter[0] - jx) / float(width);
        k.jitter_delta[1] = (c.jitter[1] - jy) / float(height);
    }
    k.jitter_delta[2] = camera_only ? 0.0f : 1.0f;

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
    frame.frame_ms = frame_time_ms(c);
    frame.hdr = hdr;
    frame.reset = history_valid == 0 || c.force_reset;
    if (frame.reset)
        ++g_counts.resets;
    if (camera_only)
        ++g_counts.camera_only;
    g_counts.frame_us += uint64_t(frame.frame_ms * 1000.0f);
    frame.preset = g_settings.preset;

    // A new preset or size means a new DLSS feature, and the old one may still
    // be in flight.
    if (c.upscaler.will_rebuild(frame) && c.queue != nullptr)
        c.queue->wait_idle();

    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    c.work.prepare(list, {reinterpret_cast<ID3D12Resource *>(color.res.handle), DXGI_FORMAT(color.fmt)},
                   {reinterpret_cast<ID3D12Resource *>(motion.res.handle), DXGI_FORMAT(motion.fmt)},
                   {reinterpret_cast<ID3D12Resource *>(depth.res.handle), DXGI_FORMAT(depth.fmt)}, k);
    if (!c.upscaler.evaluate(list, frame))
    {
        // The game's TAA draws after all.
        c.work.abandon(list);
        restore(dev, list, st);
        c.failed = true;
        set_status(c, taa_state::failed, c.upscaler.error());
        reshade::log::message(reshade::log::level::error, ("ACV DLSS: " + c.upscaler.error()).c_str());
        return false;
    }

    // The slider scales RCAS's lobe directly. AMD's stops would jump from
    // nothing to a quarter strength at the first step.
    c.work.finish(list, g_settings.sharpness, hdr, reinterpret_cast<ID3D12Resource *>(target.handle));
    restore(dev, list, st);
    publish_upscale(target.handle, upscale ? c.work.presented() : nullptr, c.work.color_format(), st.graphics.layout);

    c.jitter[0] = jx;
    c.jitter[1] = jy;
    c.have_jitter = true;
    c.force_reset = false;
    c.last_replaced = c.frame;
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
    if (!c->upscaler.init(native, g_settings.debug))
        c->error = c->upscaler.error();
    else if (!c->work.init(native))
        c->error = c->work.error();
    else
        c->ready = true;

    if (c->ready)
    {
        set_status(*c, taa_state::waiting);
        reshade::log::message(reshade::log::level::info, "ACV DLSS: DLSS is ready");
    }
    else
    {
        set_status(*c, taa_state::unavailable, c->error);
        reshade::log::message(reshade::log::level::warning, ("ACV DLSS: " + c->error).c_str());
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
        // An unbounded range reports a count of UINT32_MAX; its first slots
        // are all that is looked at.
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
    std::unique_lock lock(g_textures_lock);
    g_textures[res.handle] = {desc.texture.width, desc.texture.height, desc.texture.format};
}

// A freed texture's address soon comes back as a different one, so nothing may
// keep recognizing it as the frame.
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
    {
        std::unique_lock lock(g_textures_lock);
        if (g_textures.erase(res.handle) == 0)
            return;
    }
    forget_frame(res.handle);
}

void on_destroy_command_list(command_list *cmd)
{
    if (cmd->get_private_data<list_state>() != nullptr)
        cmd->destroy_private_data<list_state>();
}

void on_reset_command_list(command_list *cmd)
{
    if (list_state *s = cmd->get_private_data<list_state>())
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
        if (g_settings.debug)
            trace_reads(cmd, st, graphics, first + i, tables[i]);
        if (!graphics)
        {
            check_delivery(cmd, st, first + i, tables[i]);
            continue;
        }
        check_redirect(cmd, st, first + i, tables[i]);
        if (st.armed && first + i == st.taa_param)
        {
            if (tables[i].handle == st.taa_table)
                continue;
            disarm(st, "TAA table replaced before a draw");
        }
        if (g_settings.enabled && g_ctx->ready)
            try_match(cmd, st, first + i, tables[i]);
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
        // A root CBV arrives as a buffer and an offset; the list wants the address.
        const auto &range = *static_cast<const buffer_range *>(update.descriptors);
        auto *buffer = reinterpret_cast<ID3D12Resource *>(range.buffer.handle);
        v.kind = root_kind::cbv;
        v.buffer = range;
        v.value = buffer != nullptr ? buffer->GetGPUVirtualAddress() + range.offset : 0;
        break;
    }
    // Root SRVs and UAVs arrive as the GPU address itself.
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

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipeline)
{
    if (g_ctx != nullptr && stages == pipeline_stage::all)
        state_of(cmd).pipeline = reinterpret_cast<ID3D12PipelineState *>(pipeline.handle);
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
    list_state *st = cmd->get_private_data<list_state>();
    if (st == nullptr)
        return false;
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
    list_state *st = g_ctx != nullptr ? cmd->get_private_data<list_state>() : nullptr;
    if (st == nullptr)
        return false;
    if (!st->trace.empty())
        flush_trace(*st, "a dispatch of " + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z) + " groups");
    // Skipping the dispatch is what keeps EASU from overwriting the frame.
    return st->delivery.pending && deliver(cmd, *st);
}
// With Debug on, once a frame from present: the device check and, every 600
// frames, a summary of what happened since the last one.
void debug_frame(context &c)
{
    // A driver crash shows up here before anything else notices.
    static bool device_lost = false;
    const HRESULT removed = reinterpret_cast<ID3D12Device *>(c.dev->get_native())->GetDeviceRemovedReason();
    if (!device_lost && FAILED(removed))
    {
        device_lost = true;
        char text[96];
        snprintf(text, sizeof(text), "ACV DLSS: the graphics device was removed, reason 0x%08lX", static_cast<unsigned long>(removed));
        reshade::log::message(reshade::log::level::error, text);
    }
    if (c.frame % 600 != 0)
        return;

    const uint32_t replaced = g_counts.replaced.exchange(0);
    const uint64_t frame_us = g_counts.frame_us.exchange(0);
    debug_event("last 600 frames: DLSS ran %u times, %.2f ms a frame; %u history resets, %u frames without motion "
                "vectors, %u TAA draws skipped, %u screen pass redirects, %u FSR 1 passes replaced, %u give-ups",
                replaced, replaced != 0 ? double(frame_us) / 1000.0 / replaced : 0.0, g_counts.resets.exchange(0),
                g_counts.camera_only.exchange(0), g_counts.skipped_draws.exchange(0), g_counts.redirects.exchange(0),
                g_counts.deliveries.exchange(0), g_counts.given_up.exchange(0));
    debug_event("now: %s, %ux%u to %ux%u, preset %u, sharpness %.0f%%, LOD bias %+.2f on %u samplers",
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
    descriptor_tracking::register_events();
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
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
    reshade::register_event<reshade::addon_event::draw>(on_draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
    reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
}

void taa_unregister()
{
    descriptor_tracking::unregister_events();
}

void taa_present(command_queue *queue, uint32_t display_width, uint32_t display_height)
{
    std::lock_guard ctx_lock(g_ctx_lock);
    if (g_ctx == nullptr)
        return;
    context &c = *g_ctx;
    std::lock_guard lock(c.lock);
    c.queue = queue;
    ++g_frames;
    c.display_width = display_width;
    c.display_height = display_height;
    // A frame without DLSS hands the screen pass the game's own picture again.
    if (c.frame > c.last_replaced)
        stop_upscaling();
    ++c.frame;
    c.work.collect(c.frame);
    if (g_settings.debug)
        debug_frame(c);
    if (!c.ready || c.failed)
        return;
    const taa_state before = c.status.state;
    if (!g_settings.enabled)
        set_status(c, taa_state::off);
    else if (c.frame > c.last_seen + IDLE_FRAMES)
        set_status(c, taa_state::waiting);
    if (before == taa_state::running && c.status.state == taa_state::waiting)
        debug_event("no TAA pass for %llu frames (a menu, a loading screen or the map)",
                    static_cast<unsigned long long>(IDLE_FRAMES));
}

void taa_restart()
{
    std::lock_guard ctx_lock(g_ctx_lock);
    if (g_ctx == nullptr)
        return;
    std::lock_guard lock(g_ctx->lock);
    g_ctx->force_reset = true;
    g_ctx->have_jitter = false;
}

taa_status taa_get_status()
{
    std::lock_guard ctx_lock(g_ctx_lock);
    if (g_ctx == nullptr)
        return {};
    std::lock_guard lock(g_ctx->lock);
    return g_ctx->status;
}
