#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "descriptors.h"

#include <algorithm>

using namespace reshade::api;

namespace
{
// Writes come in long runs and a copy touches two heaps. Four entries avoid
// the locked lookup almost every time.
struct heap_cache
{
    struct line
    {
        uint64_t tracker = 0;
        uint64_t handle = 0;
        void *heap = nullptr;
    };
    line lines[4];
    uint32_t next = 0;

    void *find(uint64_t tracker, uint64_t handle) const
    {
        for (const line &l : lines)
            if (l.tracker == tracker && l.handle == handle)
                return l.heap;
        return nullptr;
    }
    void add(uint64_t tracker, uint64_t handle, void *heap)
    {
        lines[next++ % 4] = {tracker, handle, heap};
    }
};
thread_local heap_cache t_cache;
std::atomic<uint64_t> g_next_id{1};

std::atomic<reshade::api::device *> g_device{nullptr};
std::atomic<descriptor_tracker *> g_tracker{nullptr};

bool has_view(descriptor_type type)
{
    return type == descriptor_type::sampler_with_resource_view || type == descriptor_type::buffer_shader_resource_view ||
           type == descriptor_type::buffer_unordered_access_view ||
           type == descriptor_type::texture_shader_resource_view ||
           type == descriptor_type::texture_unordered_access_view || type == descriptor_type::acceleration_structure;
}

bool has_buffer(descriptor_type type)
{
    return type == descriptor_type::constant_buffer || type == descriptor_type::shader_storage_buffer;
}
} // namespace

descriptor_tracker::heap_data::~heap_data()
{
    for (std::atomic<entry *> &chunk : chunks)
        delete[] chunk.load();
}

const descriptor_tracker::entry *descriptor_tracker::heap_data::find(uint32_t offset) const
{
    const uint32_t index = offset >> CHUNK_BITS;
    if (index >= MAX_CHUNKS)
        return nullptr;
    const entry *chunk = chunks[index].load(std::memory_order_acquire);
    return chunk != nullptr ? chunk + (offset & (CHUNK - 1)) : nullptr;
}

descriptor_tracker::entry *descriptor_tracker::heap_data::slot(uint32_t offset)
{
    const uint32_t index = offset >> CHUNK_BITS;
    if (index >= MAX_CHUNKS)
        return nullptr;
    entry *chunk = chunks[index].load(std::memory_order_acquire);
    if (chunk == nullptr)
    {
        // Another thread may race us here. The loser frees its chunk.
        entry *fresh = new entry[CHUNK]();
        if (chunks[index].compare_exchange_strong(chunk, fresh, std::memory_order_acq_rel))
            chunk = fresh;
        else
            delete[] fresh;
    }
    return chunk + (offset & (CHUNK - 1));
}

descriptor_tracker::descriptor_tracker() : _id(g_next_id++)
{
}

descriptor_tracker::~descriptor_tracker()
{
    for (auto &[handle, data] : _heaps)
        delete data;
}

const descriptor_tracker::heap_data *descriptor_tracker::find_heap(uint64_t handle) const
{
    if (void *cached = t_cache.find(_id, handle))
        return static_cast<const heap_data *>(cached);
    std::lock_guard lock(_lock);
    const auto it = _heaps.find(handle);
    if (it == _heaps.end())
        return nullptr;
    t_cache.add(_id, handle, it->second);
    return it->second;
}

descriptor_tracker::heap_data *descriptor_tracker::heap(uint64_t handle)
{
    if (void *cached = t_cache.find(_id, handle))
        return static_cast<heap_data *>(cached);
    std::lock_guard lock(_lock);
    heap_data *&data = _heaps[handle];
    if (data == nullptr)
        data = new heap_data();
    t_cache.add(_id, handle, data);
    return data;
}

resource_view descriptor_tracker::view(descriptor_heap heap, uint32_t offset) const
{
    const heap_data *data = heap.handle != 0 ? find_heap(heap.handle) : nullptr;
    const entry *e = data != nullptr ? data->find(offset) : nullptr;
    return e != nullptr && e->used && has_view(e->type) ? resource_view{e->handle} : resource_view{0};
}

buffer_range descriptor_tracker::buffer(descriptor_heap heap, uint32_t offset) const
{
    const heap_data *data = heap.handle != 0 ? find_heap(heap.handle) : nullptr;
    const entry *e = data != nullptr ? data->find(offset) : nullptr;
    if (e == nullptr || !e->used || !has_buffer(e->type))
        return {};
    buffer_range range;
    range.buffer = {e->handle};
    range.offset = e->offset;
    range.size = e->size;
    return range;
}

bool descriptor_tracker::on_update(device *device, uint32_t count, const descriptor_table_update *updates)
{
    descriptor_tracker *tracker = of(device);
    if (tracker == nullptr)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const descriptor_table_update &update = updates[i];
        descriptor_heap heap = {0};
        uint32_t offset = 0;
        device->get_descriptor_heap_offset(update.table, update.binding, update.array_offset, &heap, &offset);
        heap_data *data = tracker->heap(heap.handle);
        for (uint32_t k = 0; k < update.count; ++k)
        {
            entry *e = data->slot(offset + k);
            if (e == nullptr)
                break;
            // Handle before type, so a reader never pairs a new type with an
            // old handle.
            switch (update.type)
            {
            case descriptor_type::sampler_with_resource_view:
                e->handle = static_cast<const sampler_with_resource_view *>(update.descriptors)[k].view.handle;
                break;
            case descriptor_type::buffer_shader_resource_view:
            case descriptor_type::buffer_unordered_access_view:
            case descriptor_type::texture_shader_resource_view:
            case descriptor_type::texture_unordered_access_view:
            case descriptor_type::acceleration_structure:
                e->handle = static_cast<const resource_view *>(update.descriptors)[k].handle;
                break;
            case descriptor_type::constant_buffer:
            case descriptor_type::shader_storage_buffer:
            {
                const buffer_range &range = static_cast<const buffer_range *>(update.descriptors)[k];
                e->handle = range.buffer.handle;
                e->offset = range.offset;
                e->size = range.size;
                break;
            }
            default:
                e->handle = 0;
                break;
            }
            e->type = update.type;
            e->used = true;
        }
    }
    return false;
}

bool descriptor_tracker::on_copy(device *device, uint32_t count, const descriptor_table_copy *copies)
{
    descriptor_tracker *tracker = of(device);
    if (tracker == nullptr)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const descriptor_table_copy &copy = copies[i];
        descriptor_heap src_heap = {0}, dst_heap = {0};
        uint32_t src = 0, dst = 0;
        device->get_descriptor_heap_offset(copy.source_table, copy.source_binding, copy.source_array_offset, &src_heap,
                                           &src);
        device->get_descriptor_heap_offset(copy.dest_table, copy.dest_binding, copy.dest_array_offset, &dst_heap, &dst);
        const heap_data *from = tracker->find_heap(src_heap.handle);
        heap_data *to = tracker->heap(dst_heap.handle);

        // Runs within one source and one target chunk.
        for (uint32_t k = 0; k < copy.count;)
        {
            const uint32_t run = std::min({copy.count - k, CHUNK - ((src + k) & (CHUNK - 1)),
                                           CHUNK - ((dst + k) & (CHUNK - 1))});
            entry *target = to->slot(dst + k);
            if (target == nullptr)
                break;
            const entry *source = from != nullptr ? from->find(src + k) : nullptr;
            if (source != nullptr)
                std::copy_n(source, run, target);
            else
                std::fill_n(target, run, entry());
            k += run;
        }
    }
    return false;
}

descriptor_tracker *descriptor_tracker::of(device *device)
{
    if (g_device.load(std::memory_order_acquire) == device)
        return g_tracker.load(std::memory_order_acquire);
    return device->get_private_data<descriptor_tracker>();
}

static void on_init_device(device *device)
{
    descriptor_tracker *tracker = device->create_private_data<descriptor_tracker>();
    if (g_device.load() == nullptr)
    {
        g_tracker.store(tracker, std::memory_order_release);
        g_device.store(device, std::memory_order_release);
    }
}

static void on_destroy_device(device *device)
{
    if (g_device.load() == device)
    {
        g_device.store(nullptr, std::memory_order_release);
        g_tracker.store(nullptr, std::memory_order_release);
    }
    device->destroy_private_data<descriptor_tracker>();
}

void descriptor_tracker::register_events()
{
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy);
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(on_update);
}

void descriptor_tracker::unregister_events()
{
    reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::unregister_event<reshade::addon_event::copy_descriptor_tables>(on_copy);
    reshade::unregister_event<reshade::addon_event::update_descriptor_tables>(on_update);
}
