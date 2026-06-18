# ZFS-Lite File System Handoff

## Overview
ZFS-Lite is a proof-of-concept userspace file system (FUSE) inspired by ZFS/Btrfs. It demonstrates Copy-on-Write (CoW), Merkle Tree integrity (SHA-256), pooled storage, and snapshots over a virtual block device.

**Supported platforms:** Linux (libfuse), Windows (WinFSP), macOS (macFUSE).

## Project Structure

```
zfs/
├── CMakeLists.txt          # Build config (FUSE app + tests)
├── .gitignore
├── HANDOFF.md              # This file
├── src/
│   ├── fuse_layer.cpp      # FUSE entry point with mutex, all callbacks (requires FUSE headers)
│   ├── zfs.h / zfs.cpp     # Core FS engine: CoW, Merkle, snapshots, CRUD
│   ├── zfs_structures.h    # On-disk packed structs (uberblock, dnode, dirent, blkptr)
│   ├── vdev.h / vdev.cpp   # Virtual block device (4KB block I/O over .img file)
│   ├── allocator.h / .cpp  # Persistent refcount-based block allocator
│   └── sha256.h / sha256.cpp  # Standalone SHA-256 implementation
└── tests/
    ├── eval.cpp             # Comprehensive evaluation (no FUSE needed)
    ├── test_vdev.cpp        # Unit test: VDev read/write
    └── test_allocator.cpp   # Unit test: bitmap allocator
```

## How to Build & Run

### Eval script (standalone, no FUSE needed)
```powershell
cd c:\Git\zfs
g++ tests/eval.cpp src/zfs.cpp src/allocator.cpp src/vdev.cpp src/sha256.cpp -o tests/eval_zfs.exe -std=c++17
.\tests\eval_zfs.exe
```

### Unit tests
```powershell
cd c:\Git\zfs\tests
g++ test_vdev.cpp ../src/vdev.cpp -o test_vdev.exe -std=c++17
.\test_vdev.exe

g++ test_allocator.cpp ../src/allocator.cpp ../src/vdev.cpp -o test_allocator.exe -std=c++17
.\test_allocator.exe
```

### Full FUSE build (requires WinFSP/macFUSE/libfuse)
```powershell
mkdir build; cd build
cmake -G "MinGW Makefiles" ..
mingw32-make
```

## Current Status

### Recent Fixes (Bug Resolved!)
The multi-block CoW propagation bug has been successfully resolved. 
- **Deferred Freeing:** Block freeing during `cow_propagate` is now deferred until all new block allocations for that CoW pass are complete. This prevents the allocator from recycling a block that was just freed for a different dnode/dirent in the same propagation path.
- **Windows Binary I/O:** Added `O_BINARY` to `::open` in `vdev.cpp` to prevent text-mode translation (e.g. `\n` to `\r\n`) from corrupting binary block reads/writes on Windows platforms.
- **Rename Overwrite Fix:** `rename` now correctly re-resolves both source and destination parent directories after an `unlink` overwrite, preventing stale CoW pointers.
- **ENOSPC Safety:** All `alloc_block()` callers now check for a 0 return and propagate `-ENOSPC` instead of silently corrupting the uberblock.
- **VDev lseek Checks:** `read_block` and `write_block` now validate `lseek()` return values.
- **FUSE Cleanup:** The FUSE driver now properly cleans up the ZFS instance on exit.

### What works (verified by eval)
- ✅ VDev block I/O (format, open, read_block, write_block)
- ✅ Refcount-based block allocator (alloc, free, persist)
- ✅ File create with duplicate name rejection
- ✅ Single-block and **Multi-block** write/read with Merkle hash verification
- ✅ Corruption detection (deliberate bit-flip caught by SHA-256 hash mismatch)
- ✅ Snapshot take/list/restore API
- ✅ CoW throughput benchmark (performance varies by machine; eval reports ~5,000 IOPS and ~20 MB/s with a 512 MB virtual device on Windows)
- ✅ Packed on-disk structs with timestamps (mtime, ctime, atime)
- ✅ File deletion (unlink) and Directory removal (rmdir)
- ✅ File truncation
- ✅ FUSE callbacks for: getattr, readdir, mkdir, create, read, write, unlink, rmdir, truncate, rename, chmod, chown, fsync, statfs, open, release, utimens
- ✅ **Indirect block pointers** — files can now span beyond 40KB by utilizing `zfsl_dnode.indirect` block.
- ✅ **Directory overflow handling** — directories can now hold up to 160 entries by spanning across all 10 direct blocks.
- ✅ **Phase 7: FUSE Completeness (Implemented)**
- **Snapshot GC (Block Reference Counting):** The block allocator now uses 8-bit reference counting instead of a 1-bit bitmap. Blocks are only freed when their refcount drops to 0, correctly tracking blocks shared across multiple snapshots. `take_snapshot` increments tree refcounts, `delete_snapshot` decrements tree refcounts.
- **Cross-Directory Renames:** Fully implemented in `ZFS::rename` with transactional safety for moving files between directories and overwriting existing files.
- **Permissions/Ownership:** `zfsl_dnode` now stores `uid`, `gid`, and `mode`. `ZFS::chmod` and `ZFS::chown` persist these changes, and `getattr` exposes them.
- **`fsync` / `flush` Support:** Added via `VDev::sync()` on Windows using `_commit()`.

## Current State & Known Issues
- **Fully Functional Prototype:** All planned features for ZFS-Lite are implemented and verified by `tests/eval.cpp`.
- **Performance:** `tests/eval.cpp` reports around ~5,000 IOPS and ~20 MB/s CoW throughput with the 512 MB virtual device format on Windows.
- **Stack Overflow Fix:** A critical stack buffer overflow was discovered and fixed in `traverse_tree_refcounts` (reading 4KB block data into a 512-byte dnode stack buffer).
- **FUSE Integration:** The FUSE layer (`fuse_layer.cpp`) provides a complete mapping of all filesystem operations, protected by a global mutex for thread safety.

## Architecture Notes

### On-disk layout (512MB default)
| Block(s) | Content |
|---|---|
| 0 | Uberblock (magic, txg, root pointer, root hash, snapshots) |
| 1–4 | Refcount allocator (131072 blocks × 1 byte per block / 4096 bytes per block = 32 blocks, but capped at `ceil(total_blocks / VDEV_BLOCK_SIZE)`) |
| 5+ | Data & metadata blocks |

### CoW write flow
1. Allocate new data block(s), write data, compute SHA-256 hash per block
2. Update file dnode's `direct[]` with new block pointers and hashes
3. `cow_propagate`: walk trace from file → root, CoW'ing each dnode and its parent's dirent block
4. Update uberblock atomically (increment txg, set new root pointer and hash)
5. Persist allocator refcounts

### Integrity check on read
- For each data block read, recompute SHA-256 and compare against stored hash in parent dnode's `direct[].hash`
- Mismatch → return `-EIO` (detected silent data corruption / bit rot)

### Snapshot mechanism
- `take_snapshot()`: saves `{txg, root_blk, root_hash}` into `uberblock.snapshots[]` (max 16), then increments refcounts for the entire snapshot tree via `inc_tree_refcounts`
- `restore_snapshot(txg)`: sets `uberblock.root_blk` to snapshot's root, effectively time-traveling the entire tree
- `delete_snapshot(txg)`: decrements refcounts for the snapshot's tree via `dec_tree_refcounts`; blocks with refcount 0 are freed by the allocator
- `cow_free_block()` calls `alloc->dec_ref()`, which only truly frees a block when its refcount reaches 0 — ensuring blocks shared across snapshots are preserved
