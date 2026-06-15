#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include "vdev.h"
#include <vector>
#include <cstdint>

class BlockAllocator {
public:
    BlockAllocator(VDev* vdev, uint64_t total_blocks);
    ~BlockAllocator();

    // Initialize an empty bitmap (called during format)
    bool init_empty();
    
    // Load bitmap from VDev
    bool load();
    
    // Save bitmap to VDev
    bool save();

    // Allocate a free block, returns block number or 0 if full
    uint64_t alloc_block();
    
    // Increment reference count for a block
    void inc_ref(uint64_t blk_no);
    
    // Decrement reference count. If 0, it's free.
    void dec_ref(uint64_t blk_no);

    // Number of blocks used by the bitmap itself
    uint32_t get_bitmap_blocks() const;

    // Get total and free blocks
    void get_stats(uint64_t* total, uint64_t* free) const;

private:
    VDev* vdev;
    uint64_t total_blocks;
    uint32_t bitmap_blocks;
    std::vector<uint8_t> refcounts;

    uint8_t get_ref(uint64_t blk_no) const;
};

#endif // ALLOCATOR_H
