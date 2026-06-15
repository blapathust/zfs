#include "../src/vdev.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    std::string test_img = "test_vdev.img";
    std::remove(test_img.c_str());

    VDev vdev(test_img);

    // Test Format
    std::cout << "Testing format..." << std::endl;
    bool res = vdev.format(1024 * 1024); // 1 MB
    assert(res == true);

    // Test Open
    std::cout << "Testing open..." << std::endl;
    res = vdev.open();
    assert(res == true);

    // Test Write & Read Block
    std::cout << "Testing write and read block..." << std::endl;
    char write_buf[VDEV_BLOCK_SIZE];
    memset(write_buf, 'Z', VDEV_BLOCK_SIZE);
    
    res = vdev.write_block(0, write_buf);
    assert(res == true);

    char read_buf[VDEV_BLOCK_SIZE];
    memset(read_buf, 0, VDEV_BLOCK_SIZE);
    res = vdev.read_block(0, read_buf);
    assert(res == true);

    assert(memcmp(write_buf, read_buf, VDEV_BLOCK_SIZE) == 0);

    std::cout << "All VDEV tests passed!" << std::endl;
    std::remove(test_img.c_str());
    return 0;
}
