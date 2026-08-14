#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include "vdev.h"
#include <vector>
#include <cstdint>

class BlockAllocator {
public:
    BlockAllocator(VDev* vdev, uint64_t total_blocks);
    ~BlockAllocator();

    bool init_empty();
    
    bool load();
    
    bool save();

    // Returns block number or 0 if full
    uint64_t alloc_block();
    
    void inc_ref(uint64_t blk_no);
    
    void dec_ref(uint64_t blk_no);

    uint32_t get_bitmap_blocks() const;

    void get_stats(uint64_t* total, uint64_t* free) const;

private:
    VDev* vdev;
    uint64_t total_blocks;
    uint32_t bitmap_blocks;
    std::vector<uint8_t> refcounts;

    uint8_t get_ref(uint64_t blk_no) const;
};

#endif // ALLOCATOR_H
