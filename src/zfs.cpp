#include "zfs.h"
#include <iostream>
#include <sstream>
#include <ctime>
#include <cerrno>
#include <sys/stat.h>

// Mock the FUSE filler callback type so we don't need <fuse.h> for the core engine
typedef int (*fuse_fill_dir_t)(void *buf, const char *name,
                               const struct stat *stbuf, off_t off);

// Constructor / Destructor

// Single-VDev constructor (backward compatible)
ZFS::ZFS(const std::string& img_path, uint64_t vdev_size)
    : pool(new ZPool()) {
    pool->add_vdev(img_path, vdev_size);
    memset(&uberblock, 0, sizeof(uberblock));
}

// Multi-VDev constructor (pooled storage)
ZFS::ZFS(const std::vector<std::string>& img_paths, uint64_t per_vdev_size)
    : pool(new ZPool()) {
    for (const auto& path : img_paths) {
        pool->add_vdev(path, per_vdev_size);
    }
    memset(&uberblock, 0, sizeof(uberblock));
}

ZFS::~ZFS() {
    delete pool;
}

uint64_t ZFS::current_time() {
    return (uint64_t)time(nullptr);
}

// Mount / Format

bool ZFS::mount() {
    bool is_new = false;
    if (!pool->open()) {
        uint64_t total_size = pool->get_total_blocks() * VDEV_BLOCK_SIZE;
        // Pool couldn't open, so format all VDevs
        std::cout << "Formatting VDEV pool (" << (total_size / (1024*1024)) << " MB across "
                  << pool->get_vdev_count() << " device(s))..." << std::endl;
        if (!pool->format()) {
            std::cerr << "Failed to format pool." << std::endl;
            return false;
        }
        is_new = true;
    }

    if (is_new) {
        pool->init_allocators();
        create_root();
    } else {
        pool->load_allocators();
        if (!load_uberblock()) {
            std::cerr << "Invalid uberblock magic. Re-formatting." << std::endl;
            pool->init_allocators();
            create_root();
        }
    }
    return true;
}

bool ZFS::load_uberblock() {
    char buf[VDEV_BLOCK_SIZE];
    pool->read_block(0, buf);
    memcpy(&uberblock, buf, sizeof(zfsl_uberblock));
    return uberblock.magic == ZFSL_MAGIC;
}

bool ZFS::save_uberblock() {
    uberblock.txg++;
    char buf[VDEV_BLOCK_SIZE] = {0};
    memcpy(buf, &uberblock, sizeof(zfsl_uberblock));
    return pool->write_block(0, buf);
}

bool ZFS::create_root() {
    uberblock.magic = ZFSL_MAGIC;
    uberblock.txg = 0; // save_uberblock will increment to 1
    uberblock.snapshot_count = 0;

    uberblock.root_blk = pool->alloc_block();

    zfsl_dnode root = {};
    root.type  = ZFSL_DNODE_DIR;
    root.size  = 0;
    root.ctime = root.mtime = root.atime = current_time();

    write_dnode(uberblock.root_blk, &root);
    compute_sha256(&root, sizeof(zfsl_dnode), uberblock.root_hash);

    pool->save_allocators();
    return save_uberblock();
}


// Low-level helpers


bool ZFS::read_dnode(uint64_t blk_no, zfsl_dnode* dnode) {
    char buf[VDEV_BLOCK_SIZE];
    if (!pool->read_block(blk_no, buf)) return false;
    memcpy(dnode, buf, sizeof(zfsl_dnode));
    return true;
}

bool ZFS::write_dnode(uint64_t blk_no, const zfsl_dnode* dnode) {
    char buf[VDEV_BLOCK_SIZE] = {0};
    memcpy(buf, dnode, sizeof(zfsl_dnode));
    return pool->write_block(blk_no, buf);
}

std::vector<std::string> ZFS::split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/')) {
        if (!item.empty()) parts.push_back(item);
    }
    return parts;
}

void ZFS::cow_free_block(uint64_t blk_no) {
    pool->dec_ref(blk_no);
}

bool ZFS::entry_exists(const zfsl_dnode& dir, const std::string& name) {
    int total_entries = dir.size / sizeof(zfsl_dirent);
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
    
    for (int i = 0; i < ZFSL_DIRECT_BLKS; ++i) {
        if (dir.direct[i].blk_no == 0) continue;
        
        char buf[VDEV_BLOCK_SIZE];
        pool->read_block(dir.direct[i].blk_no, buf);
        
        int start_idx = i * max_per_blk;
        int blk_entries = total_entries - start_idx;
        if (blk_entries > max_per_blk) blk_entries = max_per_blk;
        if (blk_entries <= 0) continue;
        
        zfsl_dirent* d = (zfsl_dirent*)buf;
        for (int j = 0; j < blk_entries; ++j) {
            if (std::string(d[j].name) == name) return true;
        }
    }
    return false;
}

// Path resolution

uint64_t ZFS::resolve_path(const std::string& path, zfsl_dnode* out,
                           std::vector<PathNode>* trace) {
    if (!read_dnode(uberblock.root_blk, out)) return 0;

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
        if (out->type != ZFSL_DNODE_DIR) return 0;

        bool found = false;
        for (int i = 0; i < ZFSL_DIRECT_BLKS && !found; ++i) {
            uint64_t db = out->direct[i].blk_no;
            if (db == 0) continue;

            char buf[VDEV_BLOCK_SIZE];
            if (!pool->read_block(db, buf)) continue;
            int total_entries = out->size / sizeof(zfsl_dirent);
            int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
            int start_idx = i * max_per_blk;
            int blk_entries = total_entries - start_idx;
            if (blk_entries > max_per_blk) blk_entries = max_per_blk;
            if (blk_entries <= 0) continue;

            zfsl_dirent* d = (zfsl_dirent*)buf;

            for (int j = 0; j < blk_entries; ++j) {
                if (std::string(d[j].name) == part) {
                    uint64_t next = d[j].dnode_blk;
                    uint64_t par  = cur;
                    cur = next;
                    if (!read_dnode(cur, out)) return 0;
                    push(par, cur, *out, db, j);
                    found = true;
                    break;
                }
            }
        }
        if (!found) return 0;
    }
    return cur;
}

uint64_t ZFS::resolve_path_public(const std::string& path, zfsl_dnode* out) {
    return resolve_path(path, out);
}

// CoW propagation Ã¢â‚¬â€ walk from leaf to root

void ZFS::cow_propagate(std::vector<PathNode>& trace, zfsl_dnode& leaf) {
    zfsl_dnode cur = leaf;
    std::vector<uint64_t> deferred_frees;

    for (int i = (int)trace.size() - 1; i >= 0; --i) {
        PathNode& node = trace[i];

        // 1. CoW this dnode to a fresh block
        uint64_t new_blk = pool->alloc_block();
        if (new_blk == 0) { std::cerr << "ENOSPC during cow_propagate" << std::endl; return; }
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
        uint64_t new_db = pool->alloc_block();
        if (new_db == 0) { std::cerr << "ENOSPC during cow_propagate" << std::endl; return; }

        char d_buf[VDEV_BLOCK_SIZE];
        pool->read_block(old_db, d_buf);
        ((zfsl_dirent*)d_buf)[node.dirent_idx].dnode_blk = new_blk;
        pool->write_block(new_db, d_buf);

        // Update parent dnode's direct[] slot that held old_db
        for (int s = 0; s < ZFSL_DIRECT_BLKS; ++s) {
            if (parent.dnode.direct[s].blk_no == old_db) {
                compute_sha256(d_buf, VDEV_BLOCK_SIZE, parent.dnode.direct[s].hash);
                parent.dnode.direct[s].blk_no = new_db;
                break;
            }
        }
        deferred_frees.push_back(old_db);            // defer free

        cur = parent.dnode;
    }

    // Free all old blocks AFTER all allocations are done
    for (uint64_t blk : deferred_frees)
        cow_free_block(blk);
}

// getattr

int ZFS::getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));
    zfsl_dnode dn;
    if (resolve_path(path, &dn) == 0) return -ENOENT;

    if (dn.type == ZFSL_DNODE_DIR) {
        stbuf->st_mode  = S_IFDIR | (dn.mode ? dn.mode : 0755);
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode  = S_IFREG | (dn.mode ? dn.mode : 0644);
        stbuf->st_nlink = 1;
        stbuf->st_size  = dn.size;
    }
    stbuf->st_uid   = dn.uid;
    stbuf->st_gid   = dn.gid;
    stbuf->st_mtime = dn.mtime;
    stbuf->st_ctime = dn.ctime;
    stbuf->st_atime = dn.atime;
    return 0;
}

// readdir

int ZFS::readdir(const char *path, void *buf, void *filler_ptr) {
    std::vector<std::string>* entries = filler_ptr == nullptr ? (std::vector<std::string>*)buf : nullptr;
    fuse_fill_dir_t filler = (fuse_fill_dir_t)filler_ptr;
    zfsl_dnode dn;
    if (resolve_path(path, &dn) == 0 || dn.type != ZFSL_DNODE_DIR)
        return -ENOENT;

    if (!entries) {
        filler(buf, ".",  NULL, 0);
        filler(buf, "..", NULL, 0);
    }

    int total_entries = dn.size / sizeof(zfsl_dirent);
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
    
    for (int i = 0; i < ZFSL_DIRECT_BLKS; ++i) {
        if (dn.direct[i].blk_no == 0) continue;
        char blk[VDEV_BLOCK_SIZE];
        pool->read_block(dn.direct[i].blk_no, blk);
        
        int start_idx = i * max_per_blk;
        int blk_entries = total_entries - start_idx;
        if (blk_entries > max_per_blk) blk_entries = max_per_blk;
        if (blk_entries <= 0) continue;
        
        zfsl_dirent* d = (zfsl_dirent*)blk;
        for (int j = 0; j < blk_entries; ++j) {
            if (d[j].dnode_blk != 0) {
                if (entries) entries->push_back(d[j].name);
                else filler(buf, d[j].name, NULL, 0);
            }
        }
    }
    return 0;
}

// mkdir  (with duplicate check & full CoW)

int ZFS::mkdir(const char *path, mode_t mode) {
    std::string p(path);
    size_t sl = p.find_last_of('/');
    if (sl == std::string::npos) return -EINVAL;
    std::string par_path = p.substr(0, sl);
    if (par_path.empty()) par_path = "/";
    std::string name = p.substr(sl + 1);
    if (name.length() >= 248) return -ENAMETOOLONG;

    std::vector<PathNode> trace;
    zfsl_dnode par;
    uint64_t par_blk = resolve_path(par_path, &par, &trace);
    if (par_blk == 0 || par.type != ZFSL_DNODE_DIR) return -ENOENT;
    if (entry_exists(par, name)) return -EEXIST;

    // Allocate new directory dnode
    uint64_t child_blk = pool->alloc_block();
    if (child_blk == 0) return -ENOSPC;
    zfsl_dnode child = {};
    child.type  = ZFSL_DNODE_DIR;
    child.mode  = mode & 0777;
    child.uid   = 1000;
    child.gid   = 1000;
    child.ctime = child.mtime = child.atime = current_time();
    write_dnode(child_blk, &child);

    // Add dirent to parent & CoW
    int entries = par.size / sizeof(zfsl_dirent);
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
    if (entries >= ZFSL_DIRECT_BLKS * max_per_blk) return -ENOSPC;

    int target_b = entries / max_per_blk;
    int target_idx = entries % max_per_blk;

    bool new_dirent_needed = (par.direct[target_b].blk_no == 0);
    char dbuf[VDEV_BLOCK_SIZE] = {0};
    uint64_t old_db = par.direct[target_b].blk_no;

    if (!new_dirent_needed)
        pool->read_block(old_db, dbuf);

    zfsl_dirent* d = (zfsl_dirent*)dbuf;
    d[target_idx].dnode_blk = child_blk;
    strncpy(d[target_idx].name, name.c_str(), 247);
    d[target_idx].name[247] = '\0';

    uint64_t new_db = pool->alloc_block();
    if (new_db == 0) { cow_free_block(child_blk); return -ENOSPC; }
    pool->write_block(new_db, dbuf);
    compute_sha256(dbuf, VDEV_BLOCK_SIZE, par.direct[target_b].hash);
    par.direct[target_b].blk_no = new_db;
    if (!new_dirent_needed) cow_free_block(old_db);

    par.size += sizeof(zfsl_dirent);
    par.mtime = current_time();

    cow_propagate(trace, par);
    pool->save_allocators();
    save_uberblock();
    return 0;
}

// create  (with duplicate check & full CoW)

int ZFS::create(const char *path, mode_t mode) {
    std::string p(path);
    size_t sl = p.find_last_of('/');
    if (sl == std::string::npos) return -EINVAL;
    std::string par_path = p.substr(0, sl);
    if (par_path.empty()) par_path = "/";
    std::string name = p.substr(sl + 1);
    if (name.length() >= 248) return -ENAMETOOLONG;

    std::vector<PathNode> trace;
    zfsl_dnode par;
    uint64_t par_blk = resolve_path(par_path, &par, &trace);
    if (par_blk == 0 || par.type != ZFSL_DNODE_DIR) return -ENOENT;
    if (entry_exists(par, name)) return -EEXIST;

    // Allocate new file dnode
    uint64_t child_blk = pool->alloc_block();
    if (child_blk == 0) return -ENOSPC;
    zfsl_dnode child = {};
    child.type  = ZFSL_DNODE_FILE;
    child.mode  = mode & 0777;
    child.uid   = 1000;
    child.gid   = 1000;
    child.ctime = child.mtime = child.atime = current_time();
    write_dnode(child_blk, &child);

    // Add dirent to parent & CoW
    int entries = par.size / sizeof(zfsl_dirent);
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
    if (entries >= ZFSL_DIRECT_BLKS * max_per_blk) return -ENOSPC;

    int target_b = entries / max_per_blk;
    int target_idx = entries % max_per_blk;

    bool new_dirent_needed = (par.direct[target_b].blk_no == 0);
    char dbuf[VDEV_BLOCK_SIZE] = {0};
    uint64_t old_db = par.direct[target_b].blk_no;

    if (!new_dirent_needed)
        pool->read_block(old_db, dbuf);

    zfsl_dirent* d = (zfsl_dirent*)dbuf;
    d[target_idx].dnode_blk = child_blk;
    strncpy(d[target_idx].name, name.c_str(), 247);
    d[target_idx].name[247] = '\0';

    uint64_t new_db = pool->alloc_block();
    if (new_db == 0) { cow_free_block(child_blk); return -ENOSPC; }
    pool->write_block(new_db, dbuf);
    compute_sha256(dbuf, VDEV_BLOCK_SIZE, par.direct[target_b].hash);
    par.direct[target_b].blk_no = new_db;
    if (!new_dirent_needed) cow_free_block(old_db);

    par.size += sizeof(zfsl_dirent);
    par.mtime = current_time();

    cow_propagate(trace, par);
    pool->save_allocators();
    save_uberblock();
    return 0;
}


// read  (multi-block, with Merkle integrity check)


int ZFS::read(const char *path, char *buf, size_t size, off_t offset) {
    zfsl_dnode dn;
    if (resolve_path(path, &dn) == 0 || dn.type != ZFSL_DNODE_FILE)
        return -ENOENT;

    if ((uint64_t)offset >= dn.size) return 0;
    if ((uint64_t)(offset + size) > dn.size) size = dn.size - offset;
    if (size == 0) return 0;

    uint64_t indirect_capacity = VDEV_BLOCK_SIZE / sizeof(zfsl_blkptr);
    uint64_t start_b = offset / VDEV_BLOCK_SIZE;
    uint64_t end_b   = (offset + size - 1) / VDEV_BLOCK_SIZE;

    char indirect_buf[VDEV_BLOCK_SIZE] = {0};
    zfsl_blkptr* indirect_ptrs = (zfsl_blkptr*)indirect_buf;
    bool indirect_loaded = false;

    size_t total = 0;
    for (uint64_t b = start_b; b <= end_b; ++b) {
        uint64_t blk_off   = b * VDEV_BLOCK_SIZE;
        uint64_t copy_from = (b == start_b) ? (offset - blk_off) : 0;
        uint64_t copy_to   = (b == end_b)   ? (offset + size - blk_off) : VDEV_BLOCK_SIZE;
        uint64_t len       = copy_to - copy_from;

        uint64_t target_blk = 0;
        uint8_t* target_hash = nullptr;

        if (b < ZFSL_DIRECT_BLKS) {
            target_blk = dn.direct[b].blk_no;
            target_hash = dn.direct[b].hash;
        } else {
            if (!indirect_loaded && dn.indirect.blk_no != 0) {
                pool->read_block(dn.indirect.blk_no, indirect_buf);
                uint8_t h[32];
                compute_sha256(indirect_buf, VDEV_BLOCK_SIZE, h);
                if (memcmp(h, dn.indirect.hash, 32) != 0) {
                    std::cerr << "INTEGRITY ERROR: hash mismatch on indirect block " << dn.indirect.blk_no << std::endl;
                    return -EIO;
                }
                indirect_loaded = true;
            }
            if (dn.indirect.blk_no != 0) {
                target_blk = indirect_ptrs[b - ZFSL_DIRECT_BLKS].blk_no;
                target_hash = indirect_ptrs[b - ZFSL_DIRECT_BLKS].hash;
            }
        }

        if (target_blk == 0) {
            memset(buf + total, 0, len);            // hole
            total += len;
            continue;
        }

        char blk[VDEV_BLOCK_SIZE];
        pool->read_block(target_blk, blk);

        // Merkle integrity check
        uint8_t h[32];
        compute_sha256(blk, VDEV_BLOCK_SIZE, h);
        if (memcmp(h, target_hash, 32) != 0) {
            std::cerr << "INTEGRITY ERROR: hash mismatch on block " << target_blk << std::endl;
            return -EIO;
        }

        memcpy(buf + total, blk + copy_from, len);
        total += len;
    }
    return (int)total;
}


// write  (multi-block CoW with Merkle hash propagation)


int ZFS::write(const char *path, const char *buf, size_t size, off_t offset) {
    if (size == 0) return 0;
    
    std::vector<PathNode> trace;
    zfsl_dnode dn;
    uint64_t dn_blk = resolve_path(path, &dn, &trace);
    if (dn_blk == 0 || dn.type != ZFSL_DNODE_FILE) return -ENOENT;

    uint64_t indirect_capacity = VDEV_BLOCK_SIZE / sizeof(zfsl_blkptr);
    uint64_t max_sz = (uint64_t)(ZFSL_DIRECT_BLKS + indirect_capacity) * VDEV_BLOCK_SIZE;
    if ((uint64_t)(offset + size) > max_sz) return -EFBIG;

    uint64_t start_b = offset / VDEV_BLOCK_SIZE;
    uint64_t end_b   = (offset + size - 1) / VDEV_BLOCK_SIZE;

    char indirect_buf[VDEV_BLOCK_SIZE] = {0};
    zfsl_blkptr* indirect_ptrs = (zfsl_blkptr*)indirect_buf;
    bool indirect_dirty = false;
    
    if (end_b >= ZFSL_DIRECT_BLKS && dn.indirect.blk_no != 0) {
        pool->read_block(dn.indirect.blk_no, indirect_buf);
    }

    for (uint64_t b = start_b; b <= end_b; ++b) {
        uint64_t blk_off   = b * VDEV_BLOCK_SIZE;
        uint64_t copy_from = (b == start_b) ? (offset - blk_off) : 0;
        uint64_t copy_to   = (b == end_b)   ? (offset + size - blk_off) : VDEV_BLOCK_SIZE;
        uint64_t len       = copy_to - copy_from;

        char blk[VDEV_BLOCK_SIZE] = {0};
        uint64_t old_blk = 0;

        if (b < ZFSL_DIRECT_BLKS) {
            old_blk = dn.direct[b].blk_no;
        } else {
            old_blk = indirect_ptrs[b - ZFSL_DIRECT_BLKS].blk_no;
        }

        // Preserve existing data for partial-block writes
        if (old_blk != 0 && (copy_from > 0 || copy_to < VDEV_BLOCK_SIZE))
            pool->read_block(old_blk, blk);

        uint64_t src_off = blk_off + copy_from - offset;
        memcpy(blk + copy_from, buf + src_off, len);

        // CoW: new block
        uint64_t new_blk = pool->alloc_block();
        if (new_blk == 0) return -ENOSPC;
        pool->write_block(new_blk, blk);
        
        if (b < ZFSL_DIRECT_BLKS) {
            compute_sha256(blk, VDEV_BLOCK_SIZE, dn.direct[b].hash);
            dn.direct[b].blk_no = new_blk;
        } else {
            compute_sha256(blk, VDEV_BLOCK_SIZE, indirect_ptrs[b - ZFSL_DIRECT_BLKS].hash);
            indirect_ptrs[b - ZFSL_DIRECT_BLKS].blk_no = new_blk;
            indirect_dirty = true;
        }

        if (old_blk != 0) cow_free_block(old_blk);
    }

    if (indirect_dirty) {
        uint64_t new_indirect = pool->alloc_block();
        if (new_indirect == 0) return -ENOSPC;
        pool->write_block(new_indirect, indirect_buf);
        compute_sha256(indirect_buf, VDEV_BLOCK_SIZE, dn.indirect.hash);
        if (dn.indirect.blk_no != 0) cow_free_block(dn.indirect.blk_no);
        dn.indirect.blk_no = new_indirect;
    }

    if ((uint64_t)(offset + size) > dn.size) dn.size = offset + size;
    dn.mtime = current_time();

    cow_propagate(trace, dn);
    pool->save_allocators();
    save_uberblock();
    return (int)size;
}

void ZFS::get_space_info(uint64_t* total_blocks, uint64_t* free_blocks) {
    if (pool) {
        pool->get_stats(total_blocks, free_blocks);
    } else {
        if (total_blocks) *total_blocks = 0;
        if (free_blocks) *free_blocks = 0;
    }
}


// unlink


int ZFS::unlink(const char *path) {
    std::string p(path);
    size_t sl = p.find_last_of('/');
    if (sl == std::string::npos) return -EINVAL;
    std::string par_path = p.substr(0, sl);
    if (par_path.empty()) par_path = "/";
    std::string name = p.substr(sl + 1);

    std::vector<PathNode> trace;
    zfsl_dnode par;
    uint64_t par_blk = resolve_path(par_path, &par, &trace);
    if (par_blk == 0 || par.type != ZFSL_DNODE_DIR) return -ENOENT;

    int total_entries = par.size / sizeof(zfsl_dirent);
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
    
    int found_b = -1;
    int found_j = -1;
    uint64_t target_blk = 0;
    
    for (int b = 0; b < ZFSL_DIRECT_BLKS; ++b) {
        if (par.direct[b].blk_no == 0) continue;
        char dbuf[VDEV_BLOCK_SIZE];
        pool->read_block(par.direct[b].blk_no, dbuf);
        int blk_entries = total_entries - (b * max_per_blk);
        if (blk_entries > max_per_blk) blk_entries = max_per_blk;
        if (blk_entries <= 0) continue;
        
        zfsl_dirent* d = (zfsl_dirent*)dbuf;
        for (int j = 0; j < blk_entries; ++j) {
            if (std::string(d[j].name) == name) {
                found_b = b;
                found_j = j;
                target_blk = d[j].dnode_blk;
                break;
            }
        }
        if (found_b >= 0) break;
    }
    if (found_b < 0) return -ENOENT;

    // Check target is a file
    zfsl_dnode target;
    read_dnode(target_blk, &target);
    if (target.type != ZFSL_DNODE_FILE) return -EISDIR;

    // Free target's data blocks and dnode
    for (int i = 0; i < ZFSL_DIRECT_BLKS; ++i)
        if (target.direct[i].blk_no) cow_free_block(target.direct[i].blk_no);
        
    if (target.indirect.blk_no) {
        char indirect_buf[VDEV_BLOCK_SIZE];
        pool->read_block(target.indirect.blk_no, indirect_buf);
        zfsl_blkptr* indirect_ptrs = (zfsl_blkptr*)indirect_buf;
        int indirect_capacity = VDEV_BLOCK_SIZE / sizeof(zfsl_blkptr);
        for (int i = 0; i < indirect_capacity; ++i) {
            if (indirect_ptrs[i].blk_no) cow_free_block(indirect_ptrs[i].blk_no);
        }
        cow_free_block(target.indirect.blk_no);
    }
    
    cow_free_block(target_blk);

    int last_global = total_entries - 1;
    int last_b = last_global / max_per_blk;
    int last_j = last_global % max_per_blk;
    
    char found_buf[VDEV_BLOCK_SIZE];
    pool->read_block(par.direct[found_b].blk_no, found_buf);
    
    if (found_b == last_b) {
        zfsl_dirent* d_fb = (zfsl_dirent*)found_buf;
        if (found_j < last_j) d_fb[found_j] = d_fb[last_j];
        memset(&d_fb[last_j], 0, sizeof(zfsl_dirent));
        
        uint64_t old_db = par.direct[found_b].blk_no;
        uint64_t new_db = pool->alloc_block();
        pool->write_block(new_db, found_buf);
        compute_sha256(found_buf, VDEV_BLOCK_SIZE, par.direct[found_b].hash);
        par.direct[found_b].blk_no = new_db;
        cow_free_block(old_db);
        
        if (last_j == 0) {
           cow_free_block(new_db);
           par.direct[found_b].blk_no = 0;
           memset(par.direct[found_b].hash, 0, 32);
        }
    } else {
        char last_buf[VDEV_BLOCK_SIZE];
        pool->read_block(par.direct[last_b].blk_no, last_buf);
        zfsl_dirent* d_found = (zfsl_dirent*)found_buf;
        zfsl_dirent* d_last  = (zfsl_dirent*)last_buf;
        
        d_found[found_j] = d_last[last_j];
        memset(&d_last[last_j], 0, sizeof(zfsl_dirent));
        
        uint64_t old_found_db = par.direct[found_b].blk_no;
        uint64_t new_found_db = pool->alloc_block();
        pool->write_block(new_found_db, found_buf);
        compute_sha256(found_buf, VDEV_BLOCK_SIZE, par.direct[found_b].hash);
        par.direct[found_b].blk_no = new_found_db;
        cow_free_block(old_found_db);
        
        uint64_t old_last_db = par.direct[last_b].blk_no;
        if (last_j == 0) {
            cow_free_block(old_last_db);
            par.direct[last_b].blk_no = 0;
            memset(par.direct[last_b].hash, 0, 32);
        } else {
            uint64_t new_last_db = pool->alloc_block();
            pool->write_block(new_last_db, last_buf);
            compute_sha256(last_buf, VDEV_BLOCK_SIZE, par.direct[last_b].hash);
            par.direct[last_b].blk_no = new_last_db;
            cow_free_block(old_last_db);
        }
    }

    par.size -= sizeof(zfsl_dirent);
    par.mtime = current_time();

    cow_propagate(trace, par);
    pool->save_allocators();
    save_uberblock();
    return 0;
}


// rmdir


int ZFS::rmdir(const char *path) {
    std::string p(path);
    if (p == "/") return -EACCES;
    size_t sl = p.find_last_of('/');
    if (sl == std::string::npos) return -EINVAL;
    std::string par_path = p.substr(0, sl);
    if (par_path.empty()) par_path = "/";
    std::string name = p.substr(sl + 1);

    std::vector<PathNode> trace;
    zfsl_dnode par;
    uint64_t par_blk = resolve_path(par_path, &par, &trace);
    if (par_blk == 0 || par.type != ZFSL_DNODE_DIR) return -ENOENT;

    int total_entries = par.size / sizeof(zfsl_dirent);
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);

    int found_b = -1;
    int found_j = -1;
    uint64_t target_blk = 0;
    
    for (int b = 0; b < ZFSL_DIRECT_BLKS; ++b) {
        if (par.direct[b].blk_no == 0) continue;
        char dbuf[VDEV_BLOCK_SIZE];
        pool->read_block(par.direct[b].blk_no, dbuf);
        int blk_entries = total_entries - (b * max_per_blk);
        if (blk_entries > max_per_blk) blk_entries = max_per_blk;
        if (blk_entries <= 0) continue;
        
        zfsl_dirent* d = (zfsl_dirent*)dbuf;
        for (int j = 0; j < blk_entries; ++j) {
            if (std::string(d[j].name) == name) {
                found_b = b;
                found_j = j;
                target_blk = d[j].dnode_blk;
                break;
            }
        }
        if (found_b >= 0) break;
    }
    if (found_b < 0) return -ENOENT;

    zfsl_dnode target;
    read_dnode(target_blk, &target);
    if (target.type != ZFSL_DNODE_DIR) return -ENOTDIR;
    if (target.size > 0) return -ENOTEMPTY;

    // Free target's dirent block (if any) and dnode
    for (int i = 0; i < ZFSL_DIRECT_BLKS; ++i)
        if (target.direct[i].blk_no) cow_free_block(target.direct[i].blk_no);
    cow_free_block(target_blk);

    int last_global = total_entries - 1;
    int last_b = last_global / max_per_blk;
    int last_j = last_global % max_per_blk;
    
    char found_buf[VDEV_BLOCK_SIZE];
    pool->read_block(par.direct[found_b].blk_no, found_buf);
    
    if (found_b == last_b) {
        zfsl_dirent* d_fb = (zfsl_dirent*)found_buf;
        if (found_j < last_j) d_fb[found_j] = d_fb[last_j];
        memset(&d_fb[last_j], 0, sizeof(zfsl_dirent));
        
        uint64_t old_db = par.direct[found_b].blk_no;
        uint64_t new_db = pool->alloc_block();
        pool->write_block(new_db, found_buf);
        compute_sha256(found_buf, VDEV_BLOCK_SIZE, par.direct[found_b].hash);
        par.direct[found_b].blk_no = new_db;
        cow_free_block(old_db);
        
        if (last_j == 0) {
           cow_free_block(new_db);
           par.direct[found_b].blk_no = 0;
           memset(par.direct[found_b].hash, 0, 32);
        }
    } else {
        char last_buf[VDEV_BLOCK_SIZE];
        pool->read_block(par.direct[last_b].blk_no, last_buf);
        zfsl_dirent* d_found = (zfsl_dirent*)found_buf;
        zfsl_dirent* d_last  = (zfsl_dirent*)last_buf;
        
        d_found[found_j] = d_last[last_j];
        memset(&d_last[last_j], 0, sizeof(zfsl_dirent));
        
        uint64_t old_found_db = par.direct[found_b].blk_no;
        uint64_t new_found_db = pool->alloc_block();
        pool->write_block(new_found_db, found_buf);
        compute_sha256(found_buf, VDEV_BLOCK_SIZE, par.direct[found_b].hash);
        par.direct[found_b].blk_no = new_found_db;
        cow_free_block(old_found_db);
        
        uint64_t old_last_db = par.direct[last_b].blk_no;
        if (last_j == 0) {
            cow_free_block(old_last_db);
            par.direct[last_b].blk_no = 0;
            memset(par.direct[last_b].hash, 0, 32);
        } else {
            uint64_t new_last_db = pool->alloc_block();
            pool->write_block(new_last_db, last_buf);
            compute_sha256(last_buf, VDEV_BLOCK_SIZE, par.direct[last_b].hash);
            par.direct[last_b].blk_no = new_last_db;
            cow_free_block(old_last_db);
        }
    }

    par.size -= sizeof(zfsl_dirent);
    par.mtime = current_time();

    cow_propagate(trace, par);
    pool->save_allocators();
    save_uberblock();
    return 0;
}


// truncate


int ZFS::truncate(const char *path, off_t new_size) {
    std::vector<PathNode> trace;
    zfsl_dnode dn;
    uint64_t blk = resolve_path(path, &dn, &trace);
    if (blk == 0 || dn.type != ZFSL_DNODE_FILE) return -ENOENT;

    uint64_t indirect_capacity = VDEV_BLOCK_SIZE / sizeof(zfsl_blkptr);
    uint64_t max_sz = (uint64_t)(ZFSL_DIRECT_BLKS + indirect_capacity) * VDEV_BLOCK_SIZE;
    if ((uint64_t)new_size > max_sz) return -EFBIG;

    // Free blocks beyond new size
    uint64_t new_last = (new_size > 0) ? ((new_size - 1) / VDEV_BLOCK_SIZE) : 0;
    uint64_t first_to_free = new_size > 0 ? new_last + 1 : 0;
    
    char indirect_buf[VDEV_BLOCK_SIZE] = {0};
    zfsl_blkptr* indirect_ptrs = (zfsl_blkptr*)indirect_buf;
    bool indirect_dirty = false;
    bool indirect_loaded = false;

    if (first_to_free < ZFSL_DIRECT_BLKS + indirect_capacity && dn.indirect.blk_no != 0) {
        pool->read_block(dn.indirect.blk_no, indirect_buf);
        indirect_loaded = true;
    }

    for (uint64_t i = first_to_free; i < ZFSL_DIRECT_BLKS + indirect_capacity; ++i) {
        if (i < ZFSL_DIRECT_BLKS) {
            if (dn.direct[i].blk_no) {
                cow_free_block(dn.direct[i].blk_no);
                dn.direct[i].blk_no = 0;
                memset(dn.direct[i].hash, 0, 32);
            }
        } else {
            if (indirect_loaded && indirect_ptrs[i - ZFSL_DIRECT_BLKS].blk_no) {
                cow_free_block(indirect_ptrs[i - ZFSL_DIRECT_BLKS].blk_no);
                indirect_ptrs[i - ZFSL_DIRECT_BLKS].blk_no = 0;
                memset(indirect_ptrs[i - ZFSL_DIRECT_BLKS].hash, 0, 32);
                indirect_dirty = true;
            }
        }
    }

    if (indirect_dirty) {
        // Check if indirect block is now empty
        bool any_indirect = false;
        for (uint64_t i = 0; i < indirect_capacity; ++i) {
            if (indirect_ptrs[i].blk_no != 0) { any_indirect = true; break; }
        }
        if (!any_indirect) {
            cow_free_block(dn.indirect.blk_no);
            dn.indirect.blk_no = 0;
            memset(dn.indirect.hash, 0, 32);
        } else {
            uint64_t new_indirect = pool->alloc_block();
            pool->write_block(new_indirect, indirect_buf);
            compute_sha256(indirect_buf, VDEV_BLOCK_SIZE, dn.indirect.hash);
            cow_free_block(dn.indirect.blk_no);
            dn.indirect.blk_no = new_indirect;
        }
    }

    dn.size  = new_size;
    dn.mtime = current_time();

    cow_propagate(trace, dn);
    pool->save_allocators();
    save_uberblock();
    return 0;
}


// Snapshots


int ZFS::take_snapshot() {
    if (uberblock.snapshot_count >= ZFSL_MAX_SNAPSHOTS) return -ENOSPC;

    uint32_t idx = uberblock.snapshot_count;
    uberblock.snapshots[idx].txg      = uberblock.txg;
    uberblock.snapshots[idx].root_blk = uberblock.root_blk;
    memcpy(uberblock.snapshots[idx].root_hash, uberblock.root_hash, 32);
    uberblock.snapshot_count++;

    // Increment reference counts for the entire tree
    inc_tree_refcounts(uberblock.root_blk);
    pool->save_allocators();

    save_uberblock();
    return 0;
}

int ZFS::list_snapshots(std::vector<uint64_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < uberblock.snapshot_count; ++i)
        out.push_back(uberblock.snapshots[i].txg);
    return 0;
}

int ZFS::restore_snapshot(uint64_t txg) {
    for (uint32_t i = 0; i < uberblock.snapshot_count; ++i) {
        if (uberblock.snapshots[i].txg == txg) {
            uberblock.root_blk = uberblock.snapshots[i].root_blk;
            memcpy(uberblock.root_hash, uberblock.snapshots[i].root_hash, 32);
            save_uberblock();
            return 0;
        }
    }
    return -ENOENT;
}

int ZFS::delete_snapshot(uint64_t txg) {
    for (uint32_t i = 0; i < uberblock.snapshot_count; ++i) {
        if (uberblock.snapshots[i].txg == txg) {
            uint64_t snap_root = uberblock.snapshots[i].root_blk;
            dec_tree_refcounts(snap_root);
            
            // Shift remaining snapshots
            for (uint32_t j = i; j < uberblock.snapshot_count - 1; ++j) {
                uberblock.snapshots[j] = uberblock.snapshots[j + 1];
            }
            uberblock.snapshot_count--;
            
            pool->save_allocators();
            save_uberblock();
            return 0;
        }
    }
    return -ENOENT;
}


// fsync, chmod, chown


int ZFS::fsync(const char *path) {
    pool->sync();
    return 0;
}

int ZFS::chmod(const char *path, mode_t mode) {
    std::vector<PathNode> trace;
    zfsl_dnode dn;
    uint64_t dn_blk = resolve_path(path, &dn, &trace);
    if (dn_blk == 0) return -ENOENT;
    dn.mode = mode & 0777;
    dn.ctime = current_time();
    cow_propagate(trace, dn);

    pool->save_allocators();
    save_uberblock();
    return 0;
}

int ZFS::chown(const char *path, uint32_t uid, uint32_t gid) {
    std::vector<PathNode> trace;
    zfsl_dnode dn;
    uint64_t dn_blk = resolve_path(path, &dn, &trace);
    if (dn_blk == 0) return -ENOENT;
    if (uid != (uint32_t)-1) dn.uid = uid;
    if (gid != (uint32_t)-1) dn.gid = gid;
    dn.ctime = current_time();
    cow_propagate(trace, dn);
    pool->save_allocators();
    save_uberblock();
    return 0;
}

static void traverse_tree_refcounts(ZPool* pool, uint64_t dn_blk, bool inc) {
    if (dn_blk == 0) return;
    if (inc) pool->inc_ref(dn_blk); else pool->dec_ref(dn_blk);

    char node_buf[VDEV_BLOCK_SIZE];
    if (!pool->read_block(dn_blk, node_buf)) return;
    zfsl_dnode dn;
    memcpy(&dn, node_buf, sizeof(zfsl_dnode));

    int total_entries = dn.type == ZFSL_DNODE_DIR ? (dn.size / sizeof(zfsl_dirent)) : 0;
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
    int entries_processed = 0;

    for (int i = 0; i < ZFSL_DIRECT_BLKS; ++i) {
        if (dn.direct[i].blk_no) {
            if (inc) pool->inc_ref(dn.direct[i].blk_no); else pool->dec_ref(dn.direct[i].blk_no);
            
            if (dn.type == ZFSL_DNODE_DIR && entries_processed < total_entries) {
                char dbuf[VDEV_BLOCK_SIZE];
                pool->read_block(dn.direct[i].blk_no, dbuf);
                zfsl_dirent* d = (zfsl_dirent*)dbuf;
                int to_process = std::min(max_per_blk, total_entries - entries_processed);
                for (int j = 0; j < to_process; ++j) {
                    if (d[j].dnode_blk) traverse_tree_refcounts(pool, d[j].dnode_blk, inc);
                }
                entries_processed += to_process;
            }
        }
    }

    if (dn.indirect.blk_no) {
        if (inc) pool->inc_ref(dn.indirect.blk_no); else pool->dec_ref(dn.indirect.blk_no);
        char ibuf[VDEV_BLOCK_SIZE];
        pool->read_block(dn.indirect.blk_no, ibuf);
        zfsl_blkptr* p = (zfsl_blkptr*)ibuf;
        int max_p = VDEV_BLOCK_SIZE / sizeof(zfsl_blkptr);
        
        for (int i = 0; i < max_p; ++i) {
            if (p[i].blk_no) {
                if (inc) pool->inc_ref(p[i].blk_no); else pool->dec_ref(p[i].blk_no);
                
                if (dn.type == ZFSL_DNODE_DIR && entries_processed < total_entries) {
                    char dbuf[VDEV_BLOCK_SIZE];
                    pool->read_block(p[i].blk_no, dbuf);
                    zfsl_dirent* d = (zfsl_dirent*)dbuf;
                    int to_process = std::min(max_per_blk, total_entries - entries_processed);
                    for (int j = 0; j < to_process; ++j) {
                        if (d[j].dnode_blk) traverse_tree_refcounts(pool, d[j].dnode_blk, inc);
                    }
                    entries_processed += to_process;
                }
            }
        }
    }
}

void ZFS::inc_tree_refcounts(uint64_t dn_blk) {
    traverse_tree_refcounts(pool, dn_blk, true);
}

void ZFS::dec_tree_refcounts(uint64_t dn_blk) {
    traverse_tree_refcounts(pool, dn_blk, false);
}

int ZFS::rename(const char *oldpath, const char *newpath) {
    if (std::string(oldpath) == newpath) return 0;
    
    std::string op(oldpath);
    size_t osl = op.find_last_of('/');
    if (osl == std::string::npos) return -EINVAL;
    std::string opar_path = op.substr(0, osl);
    if (opar_path.empty()) opar_path = "/";
    std::string oname = op.substr(osl + 1);

    std::string np(newpath);
    size_t nsl = np.find_last_of('/');
    if (nsl == std::string::npos) return -EINVAL;
    std::string npar_path = np.substr(0, nsl);
    if (npar_path.empty()) npar_path = "/";
    std::string nname = np.substr(nsl + 1);

    std::vector<PathNode> trace_old;
    zfsl_dnode opar;
    uint64_t opar_blk = resolve_path(opar_path, &opar, &trace_old);
    if (opar_blk == 0) return -ENOENT;

    // Find old entry
    int ototal = opar.size / sizeof(zfsl_dirent);
    int max_per_blk = VDEV_BLOCK_SIZE / sizeof(zfsl_dirent);
    int ob = -1, oj = -1;
    uint64_t target_blk = 0;
    for (int b = 0; b < ZFSL_DIRECT_BLKS; ++b) {
        if (opar.direct[b].blk_no == 0) continue;
        char dbuf[VDEV_BLOCK_SIZE];
        pool->read_block(opar.direct[b].blk_no, dbuf);
        int bentries = std::min(max_per_blk, ototal - (b * max_per_blk));
        zfsl_dirent* d = (zfsl_dirent*)dbuf;
        for (int j = 0; j < bentries; ++j) {
            if (std::string(d[j].name) == oname) {
                ob = b; oj = j; target_blk = d[j].dnode_blk; break;
            }
        }
        if (ob >= 0) break;
    }
    if (ob < 0) return -ENOENT;

    // Check new parent
    std::vector<PathNode> trace_new;
    zfsl_dnode npar;
    uint64_t npar_blk = resolve_path(npar_path, &npar, &trace_new);
    if (npar_blk == 0) return -ENOENT;

    if (entry_exists(npar, nname)) {
        // Overwrite: unlink the existing target first
        unlink(newpath);
        // re-resolve both parents because unlink changes the tree via CoW
        trace_new.clear();
        npar_blk = resolve_path(npar_path, &npar, &trace_new);
        trace_old.clear();
        opar_blk = resolve_path(opar_path, &opar, &trace_old);
        // Re-find old entry since opar may have changed
        ototal = opar.size / sizeof(zfsl_dirent);
        ob = -1; oj = -1;
        for (int b = 0; b < ZFSL_DIRECT_BLKS; ++b) {
            if (opar.direct[b].blk_no == 0) continue;
            char dbuf[VDEV_BLOCK_SIZE];
            pool->read_block(opar.direct[b].blk_no, dbuf);
            int bentries = std::min(max_per_blk, ototal - (b * max_per_blk));
            zfsl_dirent* d = (zfsl_dirent*)dbuf;
            for (int j = 0; j < bentries; ++j) {
                if (std::string(d[j].name) == oname) {
                    ob = b; oj = j; target_blk = d[j].dnode_blk; break;
                }
            }
            if (ob >= 0) break;
        }
        if (ob < 0) return -ENOENT;
    }

    if (opar_blk == npar_blk) {
        // Same directory, just update name in dirent block
        char dbuf[VDEV_BLOCK_SIZE];
        pool->read_block(opar.direct[ob].blk_no, dbuf);
        zfsl_dirent* d = (zfsl_dirent*)dbuf;
        strncpy(d[oj].name, nname.c_str(), 247);
        d[oj].name[247] = '\0';
        
        uint64_t old_db = opar.direct[ob].blk_no;
        uint64_t new_db = pool->alloc_block();
        if (new_db == 0) return -ENOSPC;
        pool->write_block(new_db, dbuf);
        compute_sha256(dbuf, VDEV_BLOCK_SIZE, opar.direct[ob].hash);
        opar.direct[ob].blk_no = new_db;
        cow_free_block(old_db);

        opar.mtime = current_time();
        cow_propagate(trace_old, opar);
        pool->save_allocators(); save_uberblock();
        return 0;
    } else {
        // Cross directory
        // Trans 1: Add to npar
        int ntotal = npar.size / sizeof(zfsl_dirent);
        int nb = ntotal / max_per_blk;
        int nj = ntotal % max_per_blk;
        
        char nbuf[VDEV_BLOCK_SIZE] = {0};
        if (npar.direct[nb].blk_no) pool->read_block(npar.direct[nb].blk_no, nbuf);
        zfsl_dirent* d = (zfsl_dirent*)nbuf;
        d[nj].dnode_blk = target_blk;
        strncpy(d[nj].name, nname.c_str(), 247);
        
        uint64_t old_ndb = npar.direct[nb].blk_no;
        uint64_t new_ndb = pool->alloc_block();
        pool->write_block(new_ndb, nbuf);
        compute_sha256(nbuf, VDEV_BLOCK_SIZE, npar.direct[nb].hash);
        npar.direct[nb].blk_no = new_ndb;
        if (old_ndb) cow_free_block(old_ndb);
        
        npar.size += sizeof(zfsl_dirent);
        npar.mtime = current_time();
        cow_propagate(trace_new, npar);
        pool->save_allocators(); save_uberblock();

        // Trans 2: Remove from opar. Must re-resolve opar!
        trace_old.clear();
        opar_blk = resolve_path(opar_path, &opar, &trace_old);
        
        // Re-find entry
        ototal = opar.size / sizeof(zfsl_dirent);
        ob = -1; oj = -1;
        for (int b = 0; b < ZFSL_DIRECT_BLKS; ++b) {
            if (opar.direct[b].blk_no == 0) continue;
            char dbuf[VDEV_BLOCK_SIZE];
            pool->read_block(opar.direct[b].blk_no, dbuf);
            int bentries = std::min(max_per_blk, ototal - (b * max_per_blk));
            zfsl_dirent* dx = (zfsl_dirent*)dbuf;
            for (int j = 0; j < bentries; ++j) {
                if (std::string(dx[j].name) == oname) {
                    ob = b; oj = j; break;
                }
            }
            if (ob >= 0) break;
        }

        int last_global = ototal - 1;
        int last_b = last_global / max_per_blk;
        int last_j = last_global % max_per_blk;
        
        char found_buf[VDEV_BLOCK_SIZE];
        pool->read_block(opar.direct[ob].blk_no, found_buf);
        
        if (ob == last_b) {
            zfsl_dirent* d_fb = (zfsl_dirent*)found_buf;
            if (oj < last_j) d_fb[oj] = d_fb[last_j];
            memset(&d_fb[last_j], 0, sizeof(zfsl_dirent));
            
            uint64_t old_db = opar.direct[ob].blk_no;
            uint64_t new_db = pool->alloc_block();
            pool->write_block(new_db, found_buf);
            compute_sha256(found_buf, VDEV_BLOCK_SIZE, opar.direct[ob].hash);
            opar.direct[ob].blk_no = new_db;
            cow_free_block(old_db);
            
            if (last_j == 0) {
               cow_free_block(new_db);
               opar.direct[ob].blk_no = 0;
               memset(opar.direct[ob].hash, 0, 32);
            }
        } else {
            char last_buf[VDEV_BLOCK_SIZE];
            pool->read_block(opar.direct[last_b].blk_no, last_buf);
            zfsl_dirent* d_found = (zfsl_dirent*)found_buf;
            zfsl_dirent* d_last  = (zfsl_dirent*)last_buf;
            
            d_found[oj] = d_last[last_j];
            memset(&d_last[last_j], 0, sizeof(zfsl_dirent));
            
            uint64_t old_found_db = opar.direct[ob].blk_no;
            uint64_t new_found_db = pool->alloc_block();
            pool->write_block(new_found_db, found_buf);
            compute_sha256(found_buf, VDEV_BLOCK_SIZE, opar.direct[ob].hash);
            opar.direct[ob].blk_no = new_found_db;
            cow_free_block(old_found_db);
            
            uint64_t old_last_db = opar.direct[last_b].blk_no;
            if (last_j == 0) {
                cow_free_block(old_last_db);
                opar.direct[last_b].blk_no = 0;
                memset(opar.direct[last_b].hash, 0, 32);
            } else {
                uint64_t new_last_db = pool->alloc_block();
                pool->write_block(new_last_db, last_buf);
                compute_sha256(last_buf, VDEV_BLOCK_SIZE, opar.direct[last_b].hash);
                opar.direct[last_b].blk_no = new_last_db;
                cow_free_block(old_last_db);
            }
        }

        opar.size -= sizeof(zfsl_dirent);
        opar.mtime = current_time();
        cow_propagate(trace_old, opar);
        pool->save_allocators(); save_uberblock();
        return 0;
    }
}
