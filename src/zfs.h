#ifndef ZFS_H
#define ZFS_H

#include "vdev.h"
#include "allocator.h"
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
    uint64_t dirent_blk; // block containing the dirent pointing to this node
    int dirent_idx;      // index of dirent in the dirent_blk
};

class ZFS {
public:
    ZFS(const std::string& img_path, uint64_t vdev_size = 512ULL * 1024 * 1024);
    ~ZFS();

    bool mount();

    // FUSE operations
    int getattr(const char *path, struct stat *stbuf);
    int readdir(const char *path, void *buf, void* filler);
    int mkdir(const char *path, mode_t mode);
    int create(const char *path, mode_t mode);
    int read(const char *path, char *buf, size_t size, off_t offset);
    int write(const char *path, const char *buf, size_t size, off_t offset);
    int unlink(const char *path);
    int rmdir(const char *path);
    int truncate(const char *path, off_t new_size);

    // Snapshot operations
    int take_snapshot();
    int list_snapshots(std::vector<uint64_t>& txg_list);
    int restore_snapshot(uint64_t txg);
    int delete_snapshot(uint64_t txg);

    // Advanced FUSE Operations
    int rename(const char *oldpath, const char *newpath);
    int chmod(const char *path, mode_t mode);
    int chown(const char *path, uint32_t uid, uint32_t gid);
    int fsync(const char *path);

    // Expose resolve_path for eval/testing
    uint64_t resolve_path_public(const std::string& path, zfsl_dnode* out_dnode);

    // Get total and free space (in blocks)
    void get_space_info(uint64_t* total_blocks, uint64_t* free_blocks);

private:
    VDev* vdev;
    BlockAllocator* alloc;
    zfsl_uberblock uberblock;
    uint64_t vdev_size;

    bool load_uberblock();
    bool save_uberblock();
    bool create_root();

    uint64_t resolve_path(const std::string& path, zfsl_dnode* out_dnode,
                          std::vector<PathNode>* path_trace = nullptr);

    bool read_dnode(uint64_t blk_no, zfsl_dnode* dnode);
    bool write_dnode(uint64_t blk_no, const zfsl_dnode* dnode);
    std::vector<std::string> split_path(const std::string& path);

    // CoW propagation: walk trace from leaf to root, CoW'ing each dnode
    void cow_propagate(std::vector<PathNode>& trace, zfsl_dnode& leaf_dnode);

    // Free a block by decrementing refcount
    void cow_free_block(uint64_t blk_no);

    // Traverse the tree from a dnode and increment/decrement all refcounts
    void inc_tree_refcounts(uint64_t dn_blk);
    void dec_tree_refcounts(uint64_t dn_blk);

    // Check if a name already exists in a directory
    bool entry_exists(const zfsl_dnode& dir_dnode, const std::string& name);

    // Get current Unix timestamp
    uint64_t current_time();
};

#endif // ZFS_H
