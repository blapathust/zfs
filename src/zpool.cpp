#include "zpool.h"
#include <iostream>

// --- Encoding helpers ---

uint64_t ZPool::encode_block(uint16_t vdev_idx, uint64_t local_blk) {
    return ((uint64_t)vdev_idx << 48) | (local_blk & 0x0000FFFFFFFFFFFFULL);
}

void ZPool::decode_block(uint64_t global_blk, uint16_t& vdev_idx, uint64_t& local_blk) {
    vdev_idx = (uint16_t)(global_blk >> 48);
    local_blk = global_blk & 0x0000FFFFFFFFFFFFULL;
}

// --- Constructor / Destructor ---

ZPool::ZPool() : next_vdev_idx(0), opened(false) {}

ZPool::~ZPool() {
    // unique_ptrs handle cleanup
}

// --- Pool construction ---

void ZPool::add_vdev(const std::string& img_path, uint64_t size_bytes) {
    PoolMember pm;
    pm.vdev = std::make_unique<VDev>(img_path);
    pm.alloc = nullptr; // Created during format/open
    pm.img_path = img_path;
    pm.size_bytes = size_bytes;
    members.push_back(std::move(pm));
}

bool ZPool::format() {
    for (auto& pm : members) {
        if (!pm.vdev->format(pm.size_bytes)) {
            std::cerr << "[zpool] Failed to format VDev: " << pm.img_path << std::endl;
            return false;
        }
        if (!pm.vdev->open()) {
            std::cerr << "[zpool] Failed to open VDev after format: " << pm.img_path << std::endl;
            return false;
        }
        uint64_t total = pm.size_bytes / VDEV_BLOCK_SIZE;
        pm.alloc = std::make_unique<BlockAllocator>(pm.vdev.get(), total);
    }
    opened = true;
    return true;
}

bool ZPool::open() {
    for (auto& pm : members) {
        if (!pm.vdev->open()) {
            return false;
        }
        uint64_t total = pm.size_bytes / VDEV_BLOCK_SIZE;
        pm.alloc = std::make_unique<BlockAllocator>(pm.vdev.get(), total);
    }
    opened = true;
    return true;
}

bool ZPool::is_open() const {
    return opened;
}

// --- Block I/O ---

bool ZPool::read_block(uint64_t global_blk, void* buffer) {
    uint16_t vdev_idx;
    uint64_t local_blk;
    decode_block(global_blk, vdev_idx, local_blk);
    if (vdev_idx >= members.size()) {
        std::cerr << "[zpool] read_block: invalid vdev_idx=" << vdev_idx << std::endl;
        return false;
    }
    return members[vdev_idx].vdev->read_block(local_blk, buffer);
}

bool ZPool::write_block(uint64_t global_blk, const void* buffer) {
    uint16_t vdev_idx;
    uint64_t local_blk;
    decode_block(global_blk, vdev_idx, local_blk);
    if (vdev_idx >= members.size()) {
        std::cerr << "[zpool] write_block: invalid vdev_idx=" << vdev_idx << std::endl;
        return false;
    }
    return members[vdev_idx].vdev->write_block(local_blk, buffer);
}

void ZPool::sync() {
    for (auto& pm : members) {
        pm.vdev->sync();
    }
}

// --- Allocator operations ---

bool ZPool::init_allocators() {
    for (auto& pm : members) {
        if (!pm.alloc) return false;
        if (!pm.alloc->init_empty()) return false;
    }
    return true;
}

bool ZPool::load_allocators() {
    for (auto& pm : members) {
        if (!pm.alloc) return false;
        if (!pm.alloc->load()) return false;
    }
    return true;
}

bool ZPool::save_allocators() {
    for (auto& pm : members) {
        if (!pm.alloc) return false;
        if (!pm.alloc->save()) return false;
    }
    return true;
}

uint64_t ZPool::alloc_block() {
    if (members.empty()) return 0;

    // Round-robin across VDevs to stripe allocations
    for (size_t attempt = 0; attempt < members.size(); ++attempt) {
        size_t idx = (next_vdev_idx + attempt) % members.size();
        uint64_t local_blk = members[idx].alloc->alloc_block();
        if (local_blk != 0) {
            next_vdev_idx = (idx + 1) % members.size();
            return encode_block((uint16_t)idx, local_blk);
        }
    }
    return 0; // All VDevs are full
}

void ZPool::inc_ref(uint64_t global_blk) {
    uint16_t vdev_idx;
    uint64_t local_blk;
    decode_block(global_blk, vdev_idx, local_blk);
    if (vdev_idx < members.size() && members[vdev_idx].alloc) {
        members[vdev_idx].alloc->inc_ref(local_blk);
    }
}

void ZPool::dec_ref(uint64_t global_blk) {
    uint16_t vdev_idx;
    uint64_t local_blk;
    decode_block(global_blk, vdev_idx, local_blk);
    if (vdev_idx < members.size() && members[vdev_idx].alloc) {
        members[vdev_idx].alloc->dec_ref(local_blk);
    }
}

// --- Stats ---

void ZPool::get_stats(uint64_t* total, uint64_t* free) const {
    uint64_t t = 0, f = 0;
    for (const auto& pm : members) {
        if (pm.alloc) {
            uint64_t mt = 0, mf = 0;
            pm.alloc->get_stats(&mt, &mf);
            t += mt;
            f += mf;
        }
    }
    if (total) *total = t;
    if (free) *free = f;
}

uint64_t ZPool::get_total_blocks() const {
    uint64_t total = 0;
    for (const auto& pm : members) {
        total += pm.size_bytes / VDEV_BLOCK_SIZE;
    }
    return total;
}

size_t ZPool::get_vdev_count() const {
    return members.size();
}
