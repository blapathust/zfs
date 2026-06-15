#include "../src/allocator.h"
#include <iostream>
#include <cassert>
#include <cstdio>

int main() {
    std::string test_img = "test_alloc.img";
    std::remove(test_img.c_str());

    VDev vdev(test_img);
    // 512MB
    vdev.format(512 * 1024 * 1024);
    vdev.open();

    BlockAllocator alloc(&vdev, (512 * 1024 * 1024) / VDEV_BLOCK_SIZE);
    
    // Test Init
    std::cout << "Testing allocator init..." << std::endl;
    bool res = alloc.init_empty();
    assert(res == true);

    // Test Alloc
    std::cout << "Testing alloc_block..." << std::endl;
    uint64_t blk1 = alloc.alloc_block();
    uint64_t blk2 = alloc.alloc_block();
    
    assert(blk1 == 1 + alloc.get_bitmap_blocks()); // First free data block
    assert(blk2 == blk1 + 1);

    // Test Free
    std::cout << "Testing dec_ref (free)..." << std::endl;
    alloc.dec_ref(blk1);
    uint64_t blk3 = alloc.alloc_block();
    assert(blk3 == blk1); // Should reuse

    // Test persistence
    alloc.save();
    
    BlockAllocator alloc2(&vdev, (512 * 1024 * 1024) / VDEV_BLOCK_SIZE);
    alloc2.load();
    uint64_t blk4 = alloc2.alloc_block();
    assert(blk4 == blk2 + 1);

    std::cout << "All Allocator tests passed!" << std::endl;
    std::remove(test_img.c_str());
    return 0;
}
