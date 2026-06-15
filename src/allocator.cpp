#include "allocator.h"
#include <iostream>
#include <cstring>

// Block 0: Uberblock
// Block 1..1+bitmap_blocks: Bitmap

BlockAllocator::BlockAllocator(VDev* vdev, uint64_t total_blocks) 
    : vdev(vdev), total_blocks(total_blocks) {
    uint64_t total_bytes = total_blocks; // 1 byte per block
    bitmap_blocks = (total_bytes + VDEV_BLOCK_SIZE - 1) / VDEV_BLOCK_SIZE;
    
    refcounts.resize(bitmap_blocks * VDEV_BLOCK_SIZE, 0);
}

BlockAllocator::~BlockAllocator() {
    // Optionally save on destruction, but usually saved atomically
}

bool BlockAllocator::init_empty() {
    std::fill(refcounts.begin(), refcounts.end(), 0);
    
    // Reserve Block 0 (Uberblock) and Bitmap blocks
    inc_ref(0); // Uberblock
    for (uint32_t i = 1; i <= bitmap_blocks; ++i) {
        inc_ref(i);
    }
    
    return save();
}

bool BlockAllocator::load() {
    for (uint32_t i = 0; i < bitmap_blocks; ++i) {
        if (!vdev->read_block(1 + i, (char*)refcounts.data() + (i * VDEV_BLOCK_SIZE))) {
            return false;
        }
    }
    return true;
}

bool BlockAllocator::save() {
    for (uint32_t i = 0; i < bitmap_blocks; ++i) {
        if (!vdev->write_block(1 + i, (char*)refcounts.data() + (i * VDEV_BLOCK_SIZE))) {
            return false;
        }
    }
    return true;
}

uint64_t BlockAllocator::alloc_block() {
    // Start searching from first data block (after uberblock + bitmap)
    uint64_t first_data = 1 + bitmap_blocks;
    for (uint64_t i = first_data; i < total_blocks; ++i) {
        if (get_ref(i) == 0) {
            inc_ref(i);
            return i;
        }
    }
    return 0; // Out of space
}

void BlockAllocator::inc_ref(uint64_t blk_no) {
    if (blk_no < total_blocks && refcounts[blk_no] < 255) {
        refcounts[blk_no]++;
    }
}

void BlockAllocator::dec_ref(uint64_t blk_no) {
    uint64_t first_data = 1 + bitmap_blocks;
    if (blk_no >= first_data && blk_no < total_blocks) {
        if (refcounts[blk_no] > 0) {
            refcounts[blk_no]--;
        }
    }
}

uint32_t BlockAllocator::get_bitmap_blocks() const {
    return bitmap_blocks;
}

uint8_t BlockAllocator::get_ref(uint64_t blk_no) const {
    if (blk_no >= total_blocks) return 255;
    return refcounts[blk_no];
}

void BlockAllocator::get_stats(uint64_t* total, uint64_t* free) const {
    if (total) *total = total_blocks;
    if (free) {
        uint64_t free_cnt = 0;
        uint64_t first_data = 1 + bitmap_blocks;
        for (uint64_t i = first_data; i < total_blocks; ++i) {
            if (refcounts[i] == 0) free_cnt++;
        }
        *free = free_cnt;
    }
}
