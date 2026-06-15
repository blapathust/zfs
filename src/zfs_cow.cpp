#include "zfs.h"
#include <iostream>

void ZFS::cow_propagate(std::vector<PathNode>& trace, zfsl_dnode& leaf) {
    zfsl_dnode cur = leaf;
    std::vector<uint64_t> deferred_frees;

    for (int i = (int)trace.size() - 1; i >= 0; --i) {
        PathNode& node = trace[i];

        // 1. CoW this dnode to a fresh block
        uint64_t new_blk = alloc->alloc_block();
        std::cout << "[cow] writing dnode type " << (int)cur.type << " to block " << new_blk << std::endl;
        write_dnode(new_blk, &cur);
        deferred_frees.push_back(node.dnode_blk);   // defer free

        // 2. If this is the root, update the uberblock and stop
        if (i == 0) {
            uberblock.root_blk = new_blk;
            compute_sha256(&cur, sizeof(zfsl_dnode), uberblock.root_hash);
            break;
        }

        // 3. Otherwise, update the parent's dirent to point to new_blk
        PathNode& parent = trace[i - 1];

        uint64_t old_db = node.dirent_blk;
        uint64_t new_db = alloc->alloc_block();
        std::cout << "[cow] writing dirent to block " << new_db << " (was " << old_db << ")" << std::endl;

        char d_buf[VDEV_BLOCK_SIZE];
        vdev->read_block(old_db, d_buf);
        ((zfsl_dirent*)d_buf)[node.dirent_idx].dnode_blk = new_blk;
        vdev->write_block(new_db, d_buf);

        // Update parent dnode's direct[] slot that held old_db
        bool updated = false;
        for (int s = 0; s < ZFSL_DIRECT_BLKS; ++s) {
            if (parent.dnode.direct[s].blk_no == old_db) {
                compute_sha256(d_buf, VDEV_BLOCK_SIZE, parent.dnode.direct[s].hash);
                parent.dnode.direct[s].blk_no = new_db;
                updated = true;
                break;
            }
        }
        if (!updated) {
            std::cout << "[cow] ERROR: failed to find old dirent block " << old_db << " in parent's direct[]" << std::endl;
        }
        deferred_frees.push_back(old_db);            // defer free

        cur = parent.dnode;
    }

    // Free all old blocks AFTER all allocations are done
    for (uint64_t blk : deferred_frees) {
        std::cout << "[cow] freeing block " << blk << std::endl;
        cow_free_block(blk);
    }
}
