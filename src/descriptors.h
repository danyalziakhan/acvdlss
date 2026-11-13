#pragma once

#include <reshade.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// Tracks descriptor heap writes so bound tables can be read back. The game
// writes thousands a frame from several threads, so slots live in fixed
// chunks indexed by shift and mask with no lock. Samplers aren't tracked.
class __declspec(uuid("6a0e3c94-58d1-4f7b-a2c6-0b9e7d51f3a8")) descriptor_tracker
{
public:
    static void register_events();
    static void unregister_events();

    // Cached. The game has one device.
    static descriptor_tracker *of(reshade::api::device *device);

    reshade::api::resource_view view(reshade::api::descriptor_heap heap, uint32_t offset) const;
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
    // Twice D3D12's limit of a million in a shader-visible heap.
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

    // For the per-thread cache, since a later tracker could reuse the address.
    uint64_t _id;
    mutable std::mutex _lock; // guards _heaps, taken on per-thread cache misses
    std::unordered_map<uint64_t, heap_data *> _heaps;

    static bool on_copy(reshade::api::device *device, uint32_t count, const reshade::api::descriptor_table_copy *copies);
    static bool on_update(reshade::api::device *device, uint32_t count,
                          const reshade::api::descriptor_table_update *updates);
};
