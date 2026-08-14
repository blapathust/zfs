#ifndef VDEV_H
#define VDEV_H

#include <string>
#include <cstdint>

#define VDEV_BLOCK_SIZE 4096

class VDev {
public:
    VDev(const std::string& path);
    ~VDev();

    bool format(uint64_t size_bytes);
    
    bool open();

    bool read_block(uint64_t blk_no, void* buffer);

    bool write_block(uint64_t blk_no, const void* buffer);

    void sync();

    uint64_t get_total_blocks() const;

private:
    std::string filepath;
    int fd;
    uint64_t total_blocks;
};

#endif // VDEV_H
