#pragma once

#include <reshade.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// What the game wrote into each descriptor heap slot, so a bound table can be
// read back: the view of an SRV or UAV, the buffer range of a CBV. The game
// writes and copies thousands of descriptors a frame on several threads, so
// this is built for that: a slot is found by shift and mask in chunks that
// never move, without a lock, and a run of slots in one chunk is copied at
// once. It answers the same as ReShade's descriptor_tracking example for the
// two questions asked of it; samplers and pipeline layouts are not kept.
class __declspec(uuid("6a0e3c94-58d1-4f7b-a2c6-0b9e7d51f3a8")) descriptor_tracker
{
public:
    static void register_events();
    static void unregister_events();

    // The device's tracker. The game has one device, so it is kept rather than
    // looked up through ReShade on every call.
    static descriptor_tracker *of(reshade::api::device *device);

    // The view in a slot, or 0 for an empty slot, a slot without one, or a
    // heap never written to.
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

    // Tells trackers apart in the per-thread cache, where an address could be
    // a later tracker's.
    uint64_t _id;
    mutable std::mutex _lock; // guards _heaps; lookups take it only when the per-thread cache misses
    std::unordered_map<uint64_t, heap_data *> _heaps;

    static bool on_copy(reshade::api::device *device, uint32_t count, const reshade::api::descriptor_table_copy *copies);
    static bool on_update(reshade::api::device *device, uint32_t count,
                          const reshade::api::descriptor_table_update *updates);
};
