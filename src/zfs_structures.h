#ifndef ZFS_STRUCTURES_H
#define ZFS_STRUCTURES_H

#include <cstdint>
#include <cstring>

#define ZFSL_MAGIC        0x2F5117E00ULL
#define ZFSL_DIRECT_BLKS  10
#define ZFSL_MAX_SNAPSHOTS 16
#define ZFSL_DNODE_FILE    1
#define ZFSL_DNODE_DIR     2

#pragma pack(push, 1)

struct zfsl_blkptr {
    uint64_t blk_no;
    uint8_t hash[32]; // SHA-256
};

struct zfsl_snapshot {
    uint64_t txg;
    uint64_t root_blk;
    uint8_t root_hash[32];
};

struct zfsl_uberblock {
    uint64_t magic;
    uint64_t txg;      // Transaction group ID
    uint64_t root_blk; // Pointer to the root directory dnode block
    uint8_t root_hash[32];
    uint32_t snapshot_count;
    zfsl_snapshot snapshots[ZFSL_MAX_SNAPSHOTS];
};

struct zfsl_dnode {
    uint32_t type;     // ZFSL_DNODE_FILE or ZFSL_DNODE_DIR
    uint64_t size;     // Size in bytes (file data or total dirent bytes)
    uint64_t mtime;    // Last modification time (Unix timestamp)
    uint64_t ctime;    // Creation time
    uint64_t atime;    // Last access time
    uint32_t uid;      // User ID
    uint32_t gid;      // Group ID
    uint32_t mode;     // Permissions
    zfsl_blkptr direct[ZFSL_DIRECT_BLKS];
    zfsl_blkptr indirect;
};

// Represents a directory entry (256 bytes each, 16 per 4KB block)
struct zfsl_dirent {
    uint64_t dnode_blk;
    char name[248];
};

#pragma pack(pop)

#endif // ZFS_STRUCTURES_H
