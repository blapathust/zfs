#ifndef ZPOOL_H
#define ZPOOL_H

#include "vdev.h"
#include "allocator.h"
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

// A storage pool that aggregates multiple VDevs, each with its own BlockAllocator.
// Presents a unified block address space to the ZFS engine using encoded global block IDs.
// Encoding: upper 16 bits = VDev index, lower 48 bits = local block number.
// For VDev 0, global_blk == local_blk (backward compatible with single-device mode).

class ZPool {
public:
    ZPool();
    ~ZPool();

    // Add a VDev to the pool (call before format/open)
    void add_vdev(const std::string& img_path, uint64_t size_bytes);

    // Format all VDevs (creates fresh image files)
    bool format();

    // Open all VDevs (expects image files to already exist)
    bool open();

    // Returns true if all VDevs opened successfully
    bool is_open() const;

    // --- Block I/O (uses global block IDs) ---

    bool read_block(uint64_t global_blk, void* buffer);
    bool write_block(uint64_t global_blk, const void* buffer);
    void sync();

    // --- Allocator operations (routed to per-VDev allocators) ---

    // Initialize all allocators (called during format)
    bool init_allocators();

    // Load all allocators from their respective VDevs
    bool load_allocators();

    // Save all allocators to their respective VDevs
    bool save_allocators();

    // Allocate a free block using round-robin striping. Returns global block ID, or 0 on failure.
    uint64_t alloc_block();

    // Increment reference count for a global block
    void inc_ref(uint64_t global_blk);

    // Decrement reference count for a global block
    void dec_ref(uint64_t global_blk);

    // --- Stats ---

    // Get aggregated total and free block counts across all VDevs
    void get_stats(uint64_t* total, uint64_t* free) const;

    // Get total blocks across all VDevs
    uint64_t get_total_blocks() const;

    // Get number of VDevs in the pool
    size_t get_vdev_count() const;

    // --- Global/Local block encoding ---

    static uint64_t encode_block(uint16_t vdev_idx, uint64_t local_blk);
    static void decode_block(uint64_t global_blk, uint16_t& vdev_idx, uint64_t& local_blk);

private:
    struct PoolMember {
        std::unique_ptr<VDev> vdev;
        std::unique_ptr<BlockAllocator> alloc;
        std::string img_path;
        uint64_t size_bytes;
    };

    std::vector<PoolMember> members;
    size_t next_vdev_idx; // Round-robin index for striping allocations
    bool opened;
};

#endif // ZPOOL_H
