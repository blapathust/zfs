# ZFS-Lite: A Prototype Copy-on-Write Filesystem

ZFS-Lite is a user-space filesystem (FUSE) implementation written in C++ that demonstrates the core, advanced architectural concepts of ZFS. It acts as a fully functional filesystem backed by a single loopback image file, featuring cryptographically secure data integrity, Copy-on-Write (CoW) transactions, and instant zero-cost snapshots.

## Features

- **Cross-Platform FUSE Wrapper**: Natively supports mounting as a real filesystem on Windows (via WinFSP), macOS (via macFUSE), and Linux (via libfuse) using a unified C++ codebase.
- **Copy-on-Write (CoW)**: Data is never overwritten in place. Modifications write to newly allocated blocks and propagate up the directory tree, ensuring transactional consistency.
- **Merkle Tree Integrity**: Every block in the filesystem is cryptographically hashed using SHA-256. The hashes are stored in the parent block's pointer, forming a Merkle tree up to the "Uberblock".
- **Instant Snapshots & Rollbacks**: Creating a snapshot simply duplicates the root Uberblock. Rolling back is an instant operation.
- **Self-Healing / Corruption Detection**: Automatically detects silent data corruption on disk by verifying block hashes on read.

## Dependencies

You need a C++17 compliant compiler and CMake (3.10+).

Depending on your operating system, you will need the corresponding FUSE libraries:
- **Windows**: [WinFSP](https://winfsp.dev/) and a MinGW-w64 toolchain (e.g., MSYS2 UCRT64).
- **macOS**: [macFUSE](https://osxfuse.github.io/) (`brew install --cask macfuse`). 
  > **Important**: macFUSE requires installing a system extension. After installation, open **System Settings > Privacy & Security**, allow the extension, and restart your Mac. On Apple Silicon Macs, you must first boot into Recovery Mode to change your security policy to "Reduced Security" and allow user management of kernel extensions.
- **Linux**: `libfuse-dev` (`sudo apt install libfuse-dev pkg-config`).

## Build Instructions

```bash
mkdir build
cd build
cmake ..
```
*Note: On Windows using MinGW, you may need to use `cmake -G "MinGW Makefiles" ..`*

Compile the project:
```bash
make  # On Windows, use: mingw32-make
```

This will produce the evaluation script `eval_zfs` and unit tests. If FUSE libraries are installed (WinFSP, macFUSE, or libfuse), the main FUSE driver `zfslite` will also be built.

## Tutorial / Walkthrough

### 1. Evaluating the Core Engine
Before mounting, you can run the comprehensive test suite to visualize how the filesystem handles CoW, snapshots, and corruption detection under the hood:
```bash
./eval_zfs
```
*(On Windows: `.\eval_zfs.exe`)*
This script will format a test image (`test_eval.img`) and output a live demonstration of snapshot rollbacks and Merkle tree corruption detection.

### 2. Mounting the Filesystem Live
To mount ZFS-Lite and use it as a native drive in your OS, run the FUSE driver. If the image file doesn't exist, it will format a new one automatically.

**On Windows:**
```powershell
.\zfslite.exe zfs.img -f B:
```
*(This mounts the filesystem to the `B:\` drive letter in the foreground)*

**On Linux / macOS:**
```bash
mkdir -p /tmp/zfs_mount
./zfslite zfs.img -f /tmp/zfs_mount
```

You can now open Windows Explorer or your file manager, navigate to the mount point, and create folders, copy images, or edit text files natively! 

> **Pro-Tip**: Replace `-f` with `-d` to run the FUSE driver in **Debug Mode**. This will print every single I/O system call (`Create`, `Write`, `GetAttr`, etc.) to your terminal in real-time as you interact with the filesystem.

## Limitations

As a prototype filesystem designed for educational and demonstration purposes, it has several limitations:
- **Not for Production**: Do not store critical data on this filesystem.
- **Concurrency**: The FUSE layer is currently protected by a single global mutex, meaning multi-threaded I/O operations are serialized.
- **Missing POSIX Features**: Advanced filesystem features like symbolic links, hard links, and extended attributes (xattrs) are not implemented.
- **Simplified Garbage Collection**: Snapshot deletion and block freeing are implemented with a basic reference counting mechanism.
