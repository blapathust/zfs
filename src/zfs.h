#ifndef ZFS_H
#define ZFS_H

#include "zpool.h"
#include "zfs_structures.h"
#include "sha256.h"
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

struct PathNode {
    uint64_t parent_blk;
    uint64_t dnode_blk;
    zfsl_dnode dnode;
    uint64_t dirent_blk;
    int dirent_idx;
};

class ZFS {
public:
    ZFS(const std::string& img_path, uint64_t vdev_size = 512ULL * 1024 * 1024);

    ZFS(const std::vector<std::string>& img_paths, uint64_t per_vdev_size = 512ULL * 1024 * 1024);

    ~ZFS();

    bool mount();

    int getattr(const char *path, struct stat *stbuf);
    int readdir(const char *path, void *buf, void* filler);
    int mkdir(const char *path, mode_t mode);
    int create(const char *path, mode_t mode);
    int read(const char *path, char *buf, size_t size, off_t offset);
    int write(const char *path, const char *buf, size_t size, off_t offset);
    int unlink(const char *path);
    int rmdir(const char *path);
    int truncate(const char *path, off_t new_size);

    int take_snapshot();
    int list_snapshots(std::vector<uint64_t>& txg_list);
    int restore_snapshot(uint64_t txg);
    int delete_snapshot(uint64_t txg);

    int rename(const char *oldpath, const char *newpath);
    int chmod(const char *path, mode_t mode);
    int chown(const char *path, uint32_t uid, uint32_t gid);
    int fsync(const char *path);

    uint64_t resolve_path_public(const std::string& path, zfsl_dnode* out_dnode);

    void get_space_info(uint64_t* total_blocks, uint64_t* free_blocks);

private:
    ZPool* pool;
    zfsl_uberblock uberblock;
    uint64_t root_dir_blk;

    bool load_uberblock();
    bool save_uberblock();
    bool create_root();
    
    uint64_t resolve_path(const std::string& path, zfsl_dnode* out_dnode,
                          std::vector<PathNode>* path_trace = nullptr);

    uint64_t alloc_dnode();
    uint64_t alloc_data_block();

    bool read_dnode(uint64_t blk_no, zfsl_dnode* dnode);
    bool write_dnode(uint64_t blk_no, const zfsl_dnode* dnode);
    std::vector<std::string> split_path(const std::string& path);

    void cow_propagate(std::vector<PathNode>& trace, zfsl_dnode& leaf_dnode);

    void cow_free_block(uint64_t blk_no);

    void inc_tree_refcounts(uint64_t dn_blk);
    void dec_tree_refcounts(uint64_t dn_blk);

    bool entry_exists(const zfsl_dnode& dir_dnode, const std::string& name);

    uint64_t current_time();
};

#endif // ZFS_H
