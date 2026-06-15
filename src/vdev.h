#ifndef VDEV_H
#define VDEV_H

#include <string>
#include <cstdint>

#define VDEV_BLOCK_SIZE 4096

class VDev {
public:
    VDev(const std::string& path);
    ~VDev();

    // Initialize or format the virtual device
    bool format(uint64_t size_bytes);
    
    // Open the virtual device
    bool open();

    // Read a 4KB block
    bool read_block(uint64_t blk_no, void* buffer);

    // Write a 4KB block
    bool write_block(uint64_t blk_no, const void* buffer);

    // Flush to disk
    void sync();

    // Get total number of blocks
    uint64_t get_total_blocks() const;

private:
    std::string filepath;
    int fd;
    uint64_t total_blocks;
};

#endif // VDEV_H
