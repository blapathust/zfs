#define FUSE_USE_VERSION 29
#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <mutex>
#include "zfs.h"

#ifdef _WIN32
    typedef struct fuse_stat     zfsl_stat_t;
    typedef struct fuse_statvfs  zfsl_statvfs_t;
    typedef fuse_off_t           zfsl_off_t;
    typedef fuse_mode_t          zfsl_mode_t;
    typedef fuse_uid_t           zfsl_uid_t;
    typedef fuse_gid_t           zfsl_gid_t;
    typedef struct fuse_timespec zfsl_timespec_t;
#else
    #include <sys/statvfs.h>
    typedef struct stat          zfsl_stat_t;
    typedef struct statvfs       zfsl_statvfs_t;
    typedef off_t                zfsl_off_t;
    typedef mode_t               zfsl_mode_t;
    typedef uid_t                zfsl_uid_t;
    typedef gid_t                zfsl_gid_t;
    typedef struct timespec      zfsl_timespec_t;
#endif

static ZFS* zfs = nullptr;
static std::mutex zfs_mutex;

static int zfsl_getattr(const char *path, zfsl_stat_t *stbuf) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    struct stat st;
    memset(&st, 0, sizeof(st));
    int res = zfs->getattr(path, &st);
    if (res == 0) {
        memset(stbuf, 0, sizeof(zfsl_stat_t));
        stbuf->st_mode = st.st_mode | 0777; // Ensure full permissions
        stbuf->st_nlink = st.st_nlink;
        stbuf->st_size = st.st_size;
        
        struct fuse_context* ctx = fuse_get_context();
        stbuf->st_uid = ctx ? ctx->uid : st.st_uid;
        stbuf->st_gid = ctx ? ctx->gid : st.st_gid;
        
        stbuf->st_atim.tv_sec = st.st_atime;
        stbuf->st_mtim.tv_sec = st.st_mtime;
        stbuf->st_ctim.tv_sec = st.st_ctime;
    }
    return res;
}

static int zfsl_readdir(const char *path, void *buf, fuse_fill_dir_t filler, zfsl_off_t offset, struct fuse_file_info *fi) {
    (void)offset; (void)fi;
    std::lock_guard<std::mutex> lock(zfs_mutex);
    
    std::vector<std::string> entries;
    int res = zfs->readdir(path, &entries, nullptr);
    if (res < 0) return res;
    
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for (const auto& entry : entries) {
        filler(buf, entry.c_str(), NULL, 0);
    }
    return 0;
}

static int zfsl_mkdir(const char *path, zfsl_mode_t mode) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->mkdir(path, (mode_t)mode);
}

static int zfsl_create(const char *path, zfsl_mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->create(path, (mode_t)mode);
}

static int zfsl_read(const char *path, char *buf, size_t size, zfsl_off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->read(path, buf, size, (off_t)offset);
}

static int zfsl_write(const char *path, const char *buf, size_t size, zfsl_off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->write(path, buf, size, (off_t)offset);
}

static int zfsl_unlink(const char *path) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->unlink(path);
}

static int zfsl_rmdir(const char *path) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->rmdir(path);
}

static int zfsl_truncate(const char *path, zfsl_off_t size) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->truncate(path, (off_t)size);
}

static int zfsl_rename(const char *from, const char *to) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->rename(from, to);
}

static int zfsl_chmod(const char *path, zfsl_mode_t mode) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->chmod(path, (mode_t)mode);
}

static int zfsl_chown(const char *path, zfsl_uid_t uid, zfsl_gid_t gid) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->chown(path, (uint32_t)uid, (uint32_t)gid);
}

static int zfsl_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    (void)isdatasync; (void)fi;
    std::lock_guard<std::mutex> lock(zfs_mutex);
    return zfs->fsync(path);
}

static int zfsl_open(const char *path, struct fuse_file_info *fi) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    struct stat st;
    memset(&st, 0, sizeof(st));
    int res = zfs->getattr(path, &st);
    if (res != 0) return res;
    if (S_ISDIR(st.st_mode)) return -EISDIR;
    return 0;
}

static int zfsl_release(const char *path, struct fuse_file_info *fi) {
    return 0;
}

static int zfsl_utimens(const char *path, const zfsl_timespec_t tv[2]) {
    return 0; // Dummy success for Windows Explorer
}

static int zfsl_statfs(const char *path, zfsl_statvfs_t *stbuf) {
    std::lock_guard<std::mutex> lock(zfs_mutex);
    uint64_t total_blocks = 0, free_blocks = 0;
    zfs->get_space_info(&total_blocks, &free_blocks);

    memset(stbuf, 0, sizeof(zfsl_statvfs_t));
    stbuf->f_bsize = 4096; // VDEV_BLOCK_SIZE
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = total_blocks;
    stbuf->f_bfree = free_blocks;
    stbuf->f_bavail = free_blocks;
    stbuf->f_namemax = 248;
    return 0;
}

static struct fuse_operations zfsl_oper;

void init_zfsl_oper() {
    memset(&zfsl_oper, 0, sizeof(zfsl_oper));
    zfsl_oper.getattr  = zfsl_getattr;
    zfsl_oper.readdir  = zfsl_readdir;
    zfsl_oper.mkdir    = zfsl_mkdir;
    zfsl_oper.create   = zfsl_create;
    zfsl_oper.read     = zfsl_read;
    zfsl_oper.write    = zfsl_write;
    zfsl_oper.unlink   = zfsl_unlink;
    zfsl_oper.rmdir    = zfsl_rmdir;
    zfsl_oper.truncate = zfsl_truncate;
    zfsl_oper.rename   = zfsl_rename;
    zfsl_oper.chmod    = zfsl_chmod;
    zfsl_oper.chown    = zfsl_chown;
    zfsl_oper.fsync    = zfsl_fsync;
    zfsl_oper.statfs   = zfsl_statfs;
    zfsl_oper.open     = zfsl_open;
    zfsl_oper.release  = zfsl_release;
    zfsl_oper.utimens  = zfsl_utimens;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image_file> <mountpoint> [fuse options...]\n", argv[0]);
        return 1;
    }

    const char* img_path = argv[1];
    zfs = new ZFS(img_path);
    
    // Mount the virtual device
    if (!zfs->mount()) {
        fprintf(stderr, "Failed to mount ZFS virtual device.\n");
        return 1;
    }

    // Rewrite args to pass to FUSE
    char** fuse_argv = new char*[argc - 1];
    fuse_argv[0] = argv[0];
    for (int i = 2; i < argc; ++i) {
        fuse_argv[i - 1] = argv[i];
    }
    int fuse_argc = argc - 1;

    init_zfsl_oper();

    printf("Starting FUSE driver on mountpoint: %s\n", fuse_argv[1]);
    int ret = fuse_main(fuse_argc, fuse_argv, &zfsl_oper, NULL);
    
    delete[] fuse_argv;
    return ret;
}
