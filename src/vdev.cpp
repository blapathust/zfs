#include "vdev.h"
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

VDev::VDev(const std::string& path) : filepath(path), fd(-1), total_blocks(0) {}

VDev::~VDev() {
    if (fd >= 0) {
        ::close(fd);
    }
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

bool VDev::format(uint64_t size_bytes) {
    int out_fd = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (out_fd < 0) {
        std::cerr << "VDev format error: " << strerror(errno) << std::endl;
        return false;
    }
    
    char buf[VDEV_BLOCK_SIZE] = {0};
    uint64_t blocks = size_bytes / VDEV_BLOCK_SIZE;
    for (uint64_t i = 0; i < blocks; ++i) {
        if (::write(out_fd, buf, VDEV_BLOCK_SIZE) != VDEV_BLOCK_SIZE) {
            std::cerr << "VDev format write error: " << strerror(errno) << std::endl;
            ::close(out_fd);
            return false;
        }
    }
    ::close(out_fd);
    return true;
}

bool VDev::open() {
    fd = ::open(filepath.c_str(), O_RDWR | O_BINARY);
    if (fd < 0) return false;
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        ::close(fd);
        fd = -1;
        return false;
    }
    
    total_blocks = st.st_size / VDEV_BLOCK_SIZE;
    return true;
}

bool VDev::read_block(uint64_t blk_no, void* buffer) {
    if (fd < 0 || blk_no >= total_blocks) {
        std::cerr << "[vdev] read_block failed: fd=" << fd << " blk_no=" << blk_no << " total=" << total_blocks << std::endl;
        return false;
    }
    lseek(fd, blk_no * VDEV_BLOCK_SIZE, SEEK_SET);
    int ret = ::read(fd, buffer, VDEV_BLOCK_SIZE);
    if (ret != VDEV_BLOCK_SIZE) {
        std::cerr << "[vdev] read_block failed: ::read returned " << ret << " errno=" << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool VDev::write_block(uint64_t blk_no, const void* buffer) {
    if (fd < 0 || blk_no >= total_blocks) return false;
    lseek(fd, blk_no * VDEV_BLOCK_SIZE, SEEK_SET);
    return ::write(fd, buffer, VDEV_BLOCK_SIZE) == VDEV_BLOCK_SIZE;
}

uint64_t VDev::get_total_blocks() const {
    return total_blocks;
}

void VDev::sync() {
    if (fd >= 0) {
#ifdef _WIN32
        _commit(fd);
#elif defined(__APPLE__)
        fcntl(fd, F_FULLFSYNC);
#else
        fdatasync(fd);
#endif
    }
}
