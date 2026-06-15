#define FUSE_USE_VERSION 29

#include <fuse.h>
#include <iostream>
#include <cstring>
#include "zfs.h"

// Global ZFS instance
ZFS* zfs_instance = nullptr;

static int zfsl_getattr(const char *path, struct stat *stbuf) {
    return zfs_instance->getattr(path, stbuf);
}

static int zfsl_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;
    return zfs_instance->readdir(path, buf, (void*)filler);
}

static int zfsl_mkdir(const char *path, mode_t mode) {
    return zfs_instance->mkdir(path, mode);
}

static int zfsl_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void) fi;
    return zfs_instance->create(path, mode);
}

static int zfsl_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    return zfs_instance->read(path, buf, size, offset);
}

static int zfsl_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    return zfs_instance->write(path, buf, size, offset);
}

static int zfsl_unlink(const char *path) {
    return zfs_instance->unlink(path);
}

static int zfsl_rmdir(const char *path) {
    return zfs_instance->rmdir(path);
}

static int zfsl_truncate(const char *path, off_t size) {
    return zfs_instance->truncate(path, size);
}

static struct fuse_operations zfsl_oper = [] {
    struct fuse_operations ops = {};
    ops.getattr  = zfsl_getattr;
    ops.readdir  = zfsl_readdir;
    ops.mkdir    = zfsl_mkdir;
    ops.create   = zfsl_create;
    ops.read     = zfsl_read;
    ops.write    = zfsl_write;
    ops.unlink   = zfsl_unlink;
    ops.rmdir    = zfsl_rmdir;
    ops.truncate = zfsl_truncate;
    return ops;
}();

int main(int argc, char *argv[]) {
    std::cout << "Starting ZFS-Lite..." << std::endl;
    
    std::string img_path = "zfs.img";
    zfs_instance = new ZFS(img_path);
    
    if (!zfs_instance->mount()) {
        std::cerr << "Failed to mount ZFS-Lite." << std::endl;
        delete zfs_instance;
        return 1;
    }
    
    std::cout << "ZFS-Lite mounted successfully." << std::endl;
    
    int ret = fuse_main(argc, argv, &zfsl_oper, NULL);
    
    delete zfs_instance;
    return ret;
}
