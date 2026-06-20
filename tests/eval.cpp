#include "../src/zfs.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---------- helpers ----------

static void check(bool ok, const char *msg) {
    std::cout << "   " << (ok ? "[PASS]" : "[FAIL]") << " " << msg << std::endl;
    if (!ok) std::cerr << "   *** FAILURE: " << msg << " ***" << std::endl;
}

// ---------- main ----------

int main() {
    const std::string IMG = "test_eval.img";
    std::remove(IMG.c_str());

    std::cout << "=== ZFS-Lite Comprehensive Evaluation ===" << std::endl;

    ZFS zfs(IMG);
    zfs.mount();

    // ---------------------------------------------------------------
    // 1. Create files
    // ---------------------------------------------------------------
    std::cout << "\n1. Creating files..." << std::endl;
    int rc = zfs.create("/hello.txt", 0644);
    check(rc == 0, "create /hello.txt");

    rc = zfs.create("/world.txt", 0644);
    check(rc == 0, "create /world.txt");

    // Duplicate check
    rc = zfs.create("/hello.txt", 0644);
    check(rc != 0, "duplicate /hello.txt rejected");

    // ---------------------------------------------------------------
    // 2. Write & read (single block)
    // ---------------------------------------------------------------
    std::cout << "\n2. Single-block write & read..." << std::endl;
    {
        char wbuf[VDEV_BLOCK_SIZE];
        memset(wbuf, 'A', VDEV_BLOCK_SIZE);
        rc = zfs.write("/hello.txt", wbuf, VDEV_BLOCK_SIZE, 0);
        check(rc == VDEV_BLOCK_SIZE, "write 4 KB");

        char rbuf[VDEV_BLOCK_SIZE] = {0};
        rc = zfs.read("/hello.txt", rbuf, VDEV_BLOCK_SIZE, 0);
        check(rc == VDEV_BLOCK_SIZE, "read 4 KB");
        check(rbuf[0] == 'A' && rbuf[4095] == 'A', "data integrity");
    }

    // ---------------------------------------------------------------
    // 3. Multi-block write & read (>4 KB)
    // ---------------------------------------------------------------
    std::cout << "\n3. Multi-block write & read (20 KB)..." << std::endl;
    {
        const size_t SZ = 20 * 1024; // 5 blocks
        char wbuf[SZ];
        for (size_t i = 0; i < SZ; ++i) wbuf[i] = 'B' + (i / VDEV_BLOCK_SIZE);

        rc = zfs.write("/world.txt", wbuf, SZ, 0);
        check(rc == (int)SZ, "write 20 KB");

        char rbuf[SZ] = {0};
        rc = zfs.read("/world.txt", rbuf, SZ, 0);
        check(rc == (int)SZ, "read 20 KB");
        check(memcmp(wbuf, rbuf, SZ) == 0, "multi-block data matches");
    }

    // ---------------------------------------------------------------
    // 4. mkdir / readdir
    // ---------------------------------------------------------------
    std::cout << "\n4. mkdir / readdir..." << std::endl;
    rc = zfs.mkdir("/subdir", 0755);
    check(rc == 0, "mkdir /subdir");

    rc = zfs.mkdir("/subdir", 0755);
    check(rc != 0, "duplicate /subdir rejected");

    rc = zfs.create("/subdir/nested.txt", 0644);
    check(rc == 0, "create /subdir/nested.txt");

    // ---------------------------------------------------------------
    // 5. Snapshots
    // ---------------------------------------------------------------
    std::cout << "\n5. Snapshots..." << std::endl;
    rc = zfs.take_snapshot();
    check(rc == 0, "take snapshot");

    std::vector<uint64_t> snaps;
    zfs.list_snapshots(snaps);
    check(snaps.size() == 1, "one snapshot listed");
    uint64_t snap_txg = snaps[0];
    std::cout << "   Snapshot txg = " << snap_txg << std::endl;

    // Overwrite /hello.txt with different data
    {
        char wbuf[VDEV_BLOCK_SIZE];
        memset(wbuf, 'Z', VDEV_BLOCK_SIZE);
        zfs.write("/hello.txt", wbuf, VDEV_BLOCK_SIZE, 0);

        char rbuf[VDEV_BLOCK_SIZE] = {0};
        zfs.read("/hello.txt", rbuf, VDEV_BLOCK_SIZE, 0);
        check(rbuf[0] == 'Z', "post-snapshot write is 'Z'");
    }

    // Restore snapshot
    rc = zfs.restore_snapshot(snap_txg);
    check(rc == 0, "restore snapshot");

    {
        char rbuf[VDEV_BLOCK_SIZE] = {0};
        rc = zfs.read("/hello.txt", rbuf, VDEV_BLOCK_SIZE, 0);
        check(rc == VDEV_BLOCK_SIZE && rbuf[0] == 'A', "restored data is 'A'");
    }

    // ---------------------------------------------------------------
    // 5.5 Indirect Blocks (Large File)
    // ---------------------------------------------------------------
    std::cout << "\n5.5 Indirect Blocks (Large File)..." << std::endl;
    {
        rc = zfs.create("/large.bin", 0644);
        check(rc == 0, "create /large.bin");

        // Write 60KB (15 blocks). Direct blocks handle 10, indirect handles 5.
        size_t large_sz = 60 * 1024;
        char* wbuf = new char[large_sz];
        for (size_t i = 0; i < large_sz; ++i) {
            wbuf[i] = (char)(i % 256);
        }

        rc = zfs.write("/large.bin", wbuf, large_sz, 0);
        check(rc == (int)large_sz, "write 60KB to /large.bin");

        char* rbuf = new char[large_sz];
        rc = zfs.read("/large.bin", rbuf, large_sz, 0);
        check(rc == (int)large_sz, "read 60KB from /large.bin");

        bool match = true;
        for (size_t i = 0; i < large_sz; ++i) {
            if (rbuf[i] != wbuf[i]) { match = false; break; }
        }
        check(match, "large file data matches exactly (indirect blocks work)");

        // Truncate to 30KB
        rc = zfs.truncate("/large.bin", 30 * 1024);
        check(rc == 0, "truncate /large.bin to 30KB");
        
        struct stat st;
        zfs.getattr("/large.bin", &st);
        check(st.st_size == 30 * 1024, "large file size is 30KB");

        rc = zfs.unlink("/large.bin");
        check(rc == 0, "unlink /large.bin");

        delete[] wbuf;
        delete[] rbuf;
    }

    // ---------------------------------------------------------------
    // 5.6 Directory Overflow Handling
    // ---------------------------------------------------------------
    std::cout << "\n5.6 Directory Overflow..." << std::endl;
    {
        rc = zfs.mkdir("/manyfiles", 0755);
        check(rc == 0, "mkdir /manyfiles");

        // 1 block holds 16 entries. Write 20 to overflow into 2nd block.
        for (int i = 0; i < 20; ++i) {
            std::string fname = "/manyfiles/file_" + std::to_string(i);
            rc = zfs.create(fname.c_str(), 0644);
            if (rc != 0) {
                std::string err = "failed to create " + fname;
                check(false, err.c_str());
            }
        }
        check(true, "created 20 files in /manyfiles (overflows 1st block)");

        // Read all entries
        std::vector<std::string> listed;
        typedef int (*filler_func_t)(void *, const char *, const struct stat *, off_t);
        filler_func_t filler = [](void *buf, const char *name, const struct stat *stbuf, off_t off) -> int {
            std::vector<std::string>* lst = (std::vector<std::string>*)buf;
            std::string s(name);
            if (s != "." && s != "..") lst->push_back(s);
            return 0;
        };
        rc = zfs.readdir("/manyfiles", &listed, (void*)filler);
        check(rc == 0 && listed.size() == 20, "readdir returned exactly 20 files");

        // Unlink a file in the first block (e.g. file_0), which should pull the last entry (file_19) from the second block into the first block.
        rc = zfs.unlink("/manyfiles/file_0");
        check(rc == 0, "unlink /manyfiles/file_0");

        listed.clear();
        rc = zfs.readdir("/manyfiles", &listed, (void*)filler);
        check(rc == 0 && listed.size() == 19, "readdir returned 19 files after unlink");
        
        bool has_file_0 = false;
        bool has_file_19 = false;
        for (const auto& s : listed) {
            if (s == "file_0") has_file_0 = true;
            if (s == "file_19") has_file_19 = true;
        }
        check(!has_file_0, "file_0 is correctly removed");
        check(has_file_19, "file_19 is still present (swapped successfully)");
    }

    // ---------------------------------------------------------------
    // 6. unlink / rmdir
    // ---------------------------------------------------------------
    std::cout << "\n6. unlink / rmdir..." << std::endl;

    // unlink a file inside /subdir first
    rc = zfs.unlink("/subdir/nested.txt");
    check(rc == 0, "unlink /subdir/nested.txt");

    // Verify it's gone
    {
        zfsl_dnode dn;
        uint64_t blk = zfs.resolve_path_public("/subdir/nested.txt", &dn);
        check(blk == 0, "/subdir/nested.txt no longer exists");
    }

    rc = zfs.rmdir("/subdir");
    check(rc == 0, "rmdir /subdir (now empty)");

    // ---------------------------------------------------------------
    // 6.5 Phase 7: Rename, Permissions, Snapshot GC
    // ---------------------------------------------------------------
    std::cout << "\n6.5 Phase 7: Rename, Permissions, GC..." << std::endl;
    {
        // 1. Rename (Cross-directory)
        rc = zfs.mkdir("/dir_a", 0755);
        rc = zfs.mkdir("/dir_b", 0755);
        rc = zfs.create("/dir_a/move_me.txt", 0644);
        check(rc == 0, "create /dir_a/move_me.txt");
        
        rc = zfs.rename("/dir_a/move_me.txt", "/dir_b/moved.txt");
        check(rc == 0, "rename (cross-dir) /dir_a/move_me.txt -> /dir_b/moved.txt");
        
        struct stat st;
        rc = zfs.getattr("/dir_a/move_me.txt", &st);
        check(rc == -ENOENT, "old file path no longer exists");
        
        rc = zfs.getattr("/dir_b/moved.txt", &st);
        check(rc == 0, "new file path exists");

        // 2. Permissions (chmod/chown)
        zfs.chmod("/dir_b/moved.txt", 0777);
        zfs.chown("/dir_b/moved.txt", 1234, 5678);
        zfs.getattr("/dir_b/moved.txt", &st);
        check((st.st_mode & 0777) == 0777, "chmod correctly updated mode to 0777");
        check(st.st_uid == 1234 && st.st_gid == 5678, "chown correctly updated uid/gid");

        // 3. Fsync
        rc = zfs.fsync("/dir_b/moved.txt");
        check(rc == 0, "fsync returned success");

        // 4. Snapshot Garbage Collection
        // We have a snapshot from earlier (txg = 7)
        std::vector<uint64_t> snaps;
        zfs.list_snapshots(snaps);
        check(snaps.size() == 1, "one snapshot currently exists");
        
        uint64_t snap_txg = snaps[0];
        rc = zfs.delete_snapshot(snap_txg);
        check(rc == 0, "delete_snapshot succeeded");
        
        snaps.clear();
        zfs.list_snapshots(snaps);
        check(snaps.empty(), "snapshot list is now empty (GC freed blocks)");
    }

    // ---------------------------------------------------------------
    // 7. truncate
    // ---------------------------------------------------------------
    std::cout << "\n7. truncate..." << std::endl;
    {
        struct stat st;
        zfs.getattr("/hello.txt", &st);
        std::cout << "   Size before truncate: " << st.st_size << std::endl;

        rc = zfs.truncate("/hello.txt", 100);
        check(rc == 0, "truncate to 100 bytes");

        zfs.getattr("/hello.txt", &st);
        check(st.st_size == 100, "size is now 100");
    }

    // ---------------------------------------------------------------
    // 8. Merkle integrity (corruption detection)
    // ---------------------------------------------------------------
    std::cout << "\n8. Corruption detection..." << std::endl;
    {
        // Write known data
        char wbuf[VDEV_BLOCK_SIZE];
        memset(wbuf, 'X', VDEV_BLOCK_SIZE);
        zfs.write("/hello.txt", wbuf, VDEV_BLOCK_SIZE, 0);

        // Verify read works before corruption
        char rbuf[VDEV_BLOCK_SIZE] = {0};
        rc = zfs.read("/hello.txt", rbuf, VDEV_BLOCK_SIZE, 0);
        check(rc == VDEV_BLOCK_SIZE && rbuf[0] == 'X', "pre-corruption read OK");

        // Find the actual data block number
        zfsl_dnode dn;
        zfs.resolve_path_public("/hello.txt", &dn);
        uint64_t data_blk = dn.direct[0].blk_no;
        std::cout << "   Data block number: " << data_blk << std::endl;

        // Corrupt the block directly on disk
        int fd = ::open(IMG.c_str(), O_RDWR);
        if (fd >= 0) {
            char bad = '!';
            lseek(fd, data_blk * VDEV_BLOCK_SIZE, SEEK_SET);
            ::write(fd, &bad, 1);
            close(fd);
            std::cout << "   Corrupted 1 byte in block " << data_blk << std::endl;
        }

        // Attempt read — should fail with EIO
        rc = zfs.read("/hello.txt", rbuf, VDEV_BLOCK_SIZE, 0);
        check(rc < 0, "corrupted read rejected (hash mismatch)");
    }

    // ---------------------------------------------------------------
    // 9. CoW throughput benchmark
    // ---------------------------------------------------------------
    std::cout << "\n9. CoW throughput benchmark (100 writes)..." << std::endl;
    {
        // Re-create a fresh file (old one is corrupted)
        zfs.create("/bench.txt", 0644);
        char wbuf[VDEV_BLOCK_SIZE];
        memset(wbuf, 'B', VDEV_BLOCK_SIZE);

        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 100; ++i)
            zfs.write("/bench.txt", wbuf, VDEV_BLOCK_SIZE, 0);
        auto t1 = std::chrono::high_resolution_clock::now();

        double secs = std::chrono::duration<double>(t1 - t0).count();
        double mb   = (100.0 * VDEV_BLOCK_SIZE) / (1024.0 * 1024.0);
        std::cout << "   Time:       " << secs << " s" << std::endl;
        std::cout << "   Throughput: " << (mb / secs) << " MB/s" << std::endl;
        std::cout << "   IOPS:       " << (int)(100.0 / secs) << std::endl;
    }

    // ---------------------------------------------------------------
    // 10. Multi-VDev Pooled Storage
    // ---------------------------------------------------------------
    std::cout << "\n10. Multi-VDev Pooled Storage (2 devices)..." << std::endl;
    {
        const std::string IMG_A = "test_pool_a.img";
        const std::string IMG_B = "test_pool_b.img";
        std::remove(IMG_A.c_str());
        std::remove(IMG_B.c_str());

        // Create a ZFS instance backed by TWO virtual devices
        std::vector<std::string> pool_imgs = {IMG_A, IMG_B};
        uint64_t per_dev_size = 64ULL * 1024 * 1024; // 64 MB each = 128 MB total
        ZFS pool_zfs(pool_imgs, per_dev_size);
        pool_zfs.mount();

        // Create files and write data
        int prc = pool_zfs.create("/pooled_file_1.txt", 0644);
        check(prc == 0, "create /pooled_file_1.txt on pool");

        prc = pool_zfs.create("/pooled_file_2.txt", 0644);
        check(prc == 0, "create /pooled_file_2.txt on pool");

        // Write distinct data to both files
        char wbuf1[VDEV_BLOCK_SIZE], wbuf2[VDEV_BLOCK_SIZE];
        memset(wbuf1, 'P', VDEV_BLOCK_SIZE);
        memset(wbuf2, 'Q', VDEV_BLOCK_SIZE);
        prc = pool_zfs.write("/pooled_file_1.txt", wbuf1, VDEV_BLOCK_SIZE, 0);
        check(prc == VDEV_BLOCK_SIZE, "write to pooled file 1");

        prc = pool_zfs.write("/pooled_file_2.txt", wbuf2, VDEV_BLOCK_SIZE, 0);
        check(prc == VDEV_BLOCK_SIZE, "write to pooled file 2");

        // Read back and verify integrity
        char rbuf1[VDEV_BLOCK_SIZE] = {0}, rbuf2[VDEV_BLOCK_SIZE] = {0};
        prc = pool_zfs.read("/pooled_file_1.txt", rbuf1, VDEV_BLOCK_SIZE, 0);
        check(prc == VDEV_BLOCK_SIZE, "read pooled file 1");
        check(rbuf1[0] == 'P' && rbuf1[4095] == 'P', "pooled file 1 data integrity");

        prc = pool_zfs.read("/pooled_file_2.txt", rbuf2, VDEV_BLOCK_SIZE, 0);
        check(prc == VDEV_BLOCK_SIZE, "read pooled file 2");
        check(rbuf2[0] == 'Q' && rbuf2[4095] == 'Q', "pooled file 2 data integrity");

        // Verify both image files exist (blocks spread across devices)
        struct stat sa, sb;
        bool a_exists = (::stat(IMG_A.c_str(), &sa) == 0 && sa.st_size > 0);
        bool b_exists = (::stat(IMG_B.c_str(), &sb) == 0 && sb.st_size > 0);
        check(a_exists && b_exists, "both VDev image files created");

        // mkdir + snapshot on pool
        prc = pool_zfs.mkdir("/pool_dir", 0755);
        check(prc == 0, "mkdir on pooled storage");

        prc = pool_zfs.take_snapshot();
        check(prc == 0, "snapshot on pooled storage");

        std::cout << "   Pool total capacity: "
                  << (2 * per_dev_size / (1024*1024)) << " MB across 2 devices" << std::endl;

        // Cleanup
        std::remove(IMG_A.c_str());
        std::remove(IMG_B.c_str());
    }

    std::cout << "\n=== Evaluation complete ===" << std::endl;
    return 0;
}
