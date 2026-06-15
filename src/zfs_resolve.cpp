#include "zfs.h"
#include <iostream>
#include <sstream>

uint64_t ZFS::resolve_path(const std::string& path, zfsl_dnode* out,
                           std::vector<PathNode>* trace) {
    if (!read_dnode(uberblock.root_blk, out)) {
        std::cout << "[resolve] failed to read root dnode at " << uberblock.root_blk << std::endl;
        return 0;
    }

    auto push = [&](uint64_t par, uint64_t blk, const zfsl_dnode& dn,
                    uint64_t db, int di) {
        if (trace) trace->push_back({par, blk, dn, db, di});
    };

    if (path == "/" || path.empty()) {
        push(0, uberblock.root_blk, *out, 0, -1);
        return uberblock.root_blk;
    }

    std::vector<std::string> parts = split_path(path);
    uint64_t cur = uberblock.root_blk;
    push(0, cur, *out, 0, -1);

    for (const auto& part : parts) {
        if (out->type != ZFSL_DNODE_DIR) {
            std::cout << "[resolve] out->type is not DIR! it is " << (int)out->type << std::endl;
            return 0;
        }

        bool found = false;
        for (int i = 0; i < ZFSL_DIRECT_BLKS && !found; ++i) {
            uint64_t db = out->direct[i].blk_no;
            if (db == 0) continue;

            char buf[VDEV_BLOCK_SIZE];
            if (!vdev->read_block(db, buf)) {
                std::cout << "[resolve] failed to read dirent block " << db << std::endl;
                continue;
            }
            int entries = out->size / sizeof(zfsl_dirent);
            std::cout << "[resolve] searching " << entries << " entries in dirent block " << db << std::endl;
            zfsl_dirent* d = (zfsl_dirent*)buf;

            for (int j = 0; j < entries; ++j) {
                if (std::string(d[j].name) == part) {
                    uint64_t next = d[j].dnode_blk;
                    uint64_t par  = cur;
                    cur = next;
                    if (!read_dnode(cur, out)) {
                        std::cout << "[resolve] failed to read child dnode at " << cur << std::endl;
                        return 0;
                    }
                    std::cout << "[resolve] found " << part << " at dnode block " << cur << std::endl;
                    push(par, cur, *out, db, j);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            std::cout << "[resolve] could not find part: " << part << std::endl;
            return 0;
        }
    }
    return cur;
}
