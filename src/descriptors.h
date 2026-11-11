#pragma once

#include <reshade.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// Tracks what the game writes into each descriptor heap slot (SRV/UAV views,
// CBV buffer ranges) so bound tables can be read back. The game writes
// thousands of descriptors a frame from several threads, so slots live in
// fixed chunks indexed by shift and mask with no lock, and copies go a chunk
// run at a time. Same answers as ReShade's descriptor_tracking example for
// views and buffers; samplers and pipeline layouts aren't tracked.
class __declspec(uuid("6a0e3c94-58d1-4f7b-a2c6-0b9e7d51f3a8")) descriptor_tracker
{
public:
    static void register_events();
    static void unregister_events();

    // The game has one device, so its tracker is cached instead of looked up
    // through ReShade on every call.
    static descriptor_tracker *of(reshade::api::device *device);

    // 0 if the slot is empty, holds no view, or the heap was never written.
    reshade::api::resource_view view(reshade::api::descriptor_heap heap, uint32_t offset) const;
    // The buffer range of a constant buffer or storage buffer slot, or empty.
    reshade::api::buffer_range buffer(reshade::api::descriptor_heap heap, uint32_t offset) const;

    descriptor_tracker();
    ~descriptor_tracker();

private:
    struct entry
    {
        uint64_t handle = 0, offset = 0, size = 0; // a view, or a buffer and its range
        reshade::api::descriptor_type type = reshade::api::descriptor_type::sampler;
        bool used = false;
    };

    static const uint32_t CHUNK_BITS = 12;
    static const uint32_t CHUNK = 1u << CHUNK_BITS;
    // D3D12 allows a million descriptors in a shader-visible heap; this covers
    // twice that.
    static const uint32_t MAX_CHUNKS = 512;

    struct heap_data
    {
        std::atomic<entry *> chunks[MAX_CHUNKS] = {};
        ~heap_data();
        const entry *find(uint32_t offset) const;
        entry *slot(uint32_t offset); // allocates the chunk if needed
    };

    const heap_data *find_heap(uint64_t handle) const;
    heap_data *heap(uint64_t handle);

    // Identifies the tracker in the per-thread cache. An address could be
    // reused by a later tracker.
    uint64_t _id;
    mutable std::mutex _lock; // guards _heaps; lookups take it only when the per-thread cache misses
    std::unordered_map<uint64_t, heap_data *> _heaps;

    static bool on_copy(reshade::api::device *device, uint32_t count, const reshade::api::descriptor_table_copy *copies);
    static bool on_update(reshade::api::device *device, uint32_t count,
                          const reshade::api::descriptor_table_update *updates);
};
