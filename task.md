# Task Outline: ZFS & ZFS-Lite Analysis Report

## 1. Body Part 1: Overview of ZFS
- **Introduction to ZFS:** Briefly introduce ZFS as a combined file system and logical volume manager.
- **Pooled Storage Paradigm:** Explain how ZFS eliminates traditional partitions by using storage pools, allowing file systems to share available space dynamically.
- **Core Features Overview:**
  - **Copy-on-Write (CoW):** Explain the principle of never overwriting live data in place to ensure crash consistency.
  - **End-to-End Data Integrity:** Describe the Merkle Tree structure and how it prevents silent data corruption (bit-rot).
  - **Snapshots:** Explain how CoW enables instantaneous, zero-space snapshots.

## 2. Body Part 2: Deep Dive & ZFS-Lite Implementation
*Instruction: Map theoretical ZFS concepts directly to the C++ implementation found in the ZFS-Lite prototype, utilizing specific code snippets as evidence.*

- **Feature A: Copy-on-Write (CoW) Transactional Engine**
  - **ZFS Concept:** Modifying data allocates new blocks; pointer updates recursively propagate up the tree to the root (Uberblock).
  - **ZFS-Lite Implementation:** Analyze the `ZFS::cow_propagate` function (found in `src/zfs.cpp`). Detail how it walks the path trace from the leaf node to the root, allocating new blocks. 
  - **Code Focus:** Highlight the `deferred_frees` vector. Explain how deferring the deallocation of old blocks until the entire CoW path is constructed prevents the allocator from prematurely recycling blocks during the same transaction.

- **Feature B: Merkle Tree Data Integrity**
  - **ZFS Concept:** Every block is cryptographically hashed, and the hash is stored in its parent pointer, creating a verifiable chain of trust up to the Uberblock.
  - **ZFS-Lite Implementation:** Analyze the `ZFS::read` function. 
  - **Code Focus:** Show the snippet where `compute_sha256` recalculates the 4KB block hash and compares it (`memcmp`) against the `target_hash` stored in the parent's `zfsl_blkptr`. Explain how returning `-EIO` upon a mismatch actively prevents corrupted data from being served to the user application.

- **Feature C: O(1) Snapshots & Reference-Counted Allocation**
  - **ZFS Concept:** Taking a snapshot is instantaneous because it simply preserves the current root pointer and locks the referenced blocks.
  - **ZFS-Lite Implementation:** Analyze `ZFS::take_snapshot` and the `BlockAllocator` (in `src/allocator.cpp`).
  - **Code Focus:** Detail how `take_snapshot` saves the current `uberblock` state and calls `inc_tree_refcounts`. Explain the allocator's 8-bit reference counting mechanism (`inc_ref` / `dec_ref`), which replaces a standard free-space bitmap, safely allowing data blocks to be shared between the live filesystem and multiple historical snapshots.

## 3. Conclusion
- **Synthesis:** Summarize how ZFS revolutionizes data safety and how ZFS-Lite effectively distills these enterprise storage concepts into an accessible user-space prototype.
- **Prototype Evaluation:** Acknowledge the prototype's success in achieving cryptographically secure file creation, directory overflow handling, and active corruption detection.
- **Limitations & Future Work:** Note the specific limitations of the ZFS-Lite architecture. Point out the performance bottleneck of the FUSE layer utilizing a single global mutex (`std::lock_guard<std::mutex> lock(zfs_mutex)` in `src/fuse_layer.cpp`), which serializes I/O. Mention the absence of RAID-Z parity and advanced POSIX features as areas for future development.
