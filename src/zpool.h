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

    void add_vdev(const std::string& img_path, uint64_t size_bytes);

    bool format();

    bool open();

    bool is_open() const;

    bool read_block(uint64_t global_blk, void* buffer);
    bool write_block(uint64_t global_blk, const void* buffer);
    void sync();

    bool init_allocators();

    bool load_allocators();

    bool save_allocators();

    // Returns global block ID, or 0 on failure.
    uint64_t alloc_block();

    void inc_ref(uint64_t global_blk);

    void dec_ref(uint64_t global_blk);

    void get_stats(uint64_t* total, uint64_t* free) const;

    uint64_t get_total_blocks() const;

    size_t get_vdev_count() const;

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
