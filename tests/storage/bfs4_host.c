/* Integration test for the real kernel BFS4 driver on a regular host file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../kernel/include/bfs4.h"
#include "../../kernel/include/memory.h"
#include "../../kernel/include/klock.h"
#include "../../kernel/include/rtc.h"

void kprintf(const char *format, ...) { (void)format; }

static FILE *disk;
static bool host_read(block_device_t *device, uint32_t lba, uint8_t count, void *data) {
    (void)device;
    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&
           fread(data, 512, count, disk) == count;
}
static bool host_write(block_device_t *device, uint32_t lba, uint8_t count,
                       const void *data) {
    (void)device;
    return fseek(disk, (long)lba * 512L, SEEK_SET) == 0 &&
           fwrite(data, 512, count, disk) == count && fflush(disk) == 0;
}
bool block_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buffer) {
    return dev->read(dev, lba, count, buffer);
}
bool block_write(block_device_t *dev, uint32_t lba, uint8_t count, const void *buffer) {
    return dev->write(dev, lba, count, buffer);
}
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
uint32_t mm_total_bytes(void) { return 512U * 1024U * 1024U; }
bool mm_set_allocation_owner(void *p, uint32_t owner) { (void)p; (void)owner; return true; }
uint32_t kspin_lock_irqsave(kspinlock_t *lock) { (void)lock; return 0; }
void kspin_unlock_irqrestore(kspinlock_t *lock, uint32_t flags) {
    (void)lock; (void)flags;
}
void kmutex_lock(kmutex_t *lock){(void)lock;}
void kmutex_unlock(kmutex_t *lock){(void)lock;}
void kfree(void *p) { free(p); }
void *kmemset(void *p, int c, size_t n) { return memset(p, c, n); }
void *kmemcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int kmemcmp(const void *a,const void *b,size_t n) {return memcmp(a,b,n);}
size_t kstrlen(const char *s) { return strlen(s); }
char *kstrncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
uint32_t pit_get_uptime_seconds(void) { return 1; }
bool rtc_get_datetime(rtc_datetime_t *now) { (void)now; return false; }

static void require(bool condition, const char *what) {
    if (!condition) { fprintf(stderr, "BFS4 test failed: %s\n", what); exit(1); }
}

int main(int argc, char **argv) {
    block_device_t dev = {0};
    fat_fs_t fs;
    fat_dir_entry_t entry, entries[8];
    fat_dir_metadata_t meta[8];
    uint32_t count, read_count;
    uint64_t inode, total, free_before, free_after;
    char *payload, *received;
    if ((argc < 2 || argc > 4) || !(disk = fopen(argv[1], "r+b"))) return 2;
    require(fseek(disk, 0, SEEK_END) == 0, "disk size seek");
    dev.sector_count = (uint32_t)ftell(disk) / 512U;
    dev.sector_size = 512U;
    dev.read = host_read;
    dev.write = host_write;
    if (argc == 3 && !strcmp(argv[2], "--mount-only")) {
        fat_check_report_t report;
        require(bfs4_mount(&fs, &dev, 0), "dirty/clean mount");
        require(bfs4_check(&fs, &report) && report.completed && !report.errors,
                "mounted volume consistency");
        require(bfs4_sync(&fs), "mount-only sync");
        fclose(disk);
        puts("BFS4 remount OK");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[2], "--format")) {
        require(bfs4_format(&dev, "HOSTTEST"), "format");
        require(bfs4_mount(&fs, &dev, 0), "mount formatted volume");
        require(bfs4_mkdir(&fs, "/a"), "mkdir after format");
        require(bfs4_write(&fs, "/a/test.txt", "hello", 5), "write after format");
    }
    require(bfs4_mount(&fs, &dev, 0), "mount");
    require(bfs4_resolve(&fs, "/A/TEST.TXT", &entry), "case-insensitive lookup");
    require(entry.size == 5, "original file size");
    require(bfs4_inode(&fs, "/a/test.txt", &inode) && inode == entry.first_cluster,
            "stable inode");
    require(bfs4_mkdir(&fs, "/a/new"), "mkdir");
    require(bfs4_space(&fs, &total, &free_before), "free space before write");
    payload = malloc(100000);
    received = malloc(100000);
    require(payload && received, "test buffers");
    for (unsigned i = 0; i < 100000; ++i) payload[i] = (char)(i * 37U);
    require(bfs4_write(&fs, "/a/new/data.bin", payload, 100000), "write 100K");
    require(bfs4_resolve(&fs, "/A/NEW/DATA.BIN", &entry), "resolve new file");
    require(!bfs4_rename(&fs, "/a", "/a/new/data.bin"),
            "reject moving a directory into itself");
    require(bfs4_resolve(&fs, "/a/new/data.bin", &entry),
            "failed directory move preserves destination");
    require(bfs4_read(&fs, &entry, 0, received, 100000, &read_count) &&
            read_count == 100000 && !memcmp(payload, received, 100000), "read 100K");
    payload[0] ^= 0x5a;
    require(bfs4_write(&fs, "/a/new/data.bin", payload, 8193), "shrink file");
    require(bfs4_read(&fs, &entry, 0, received, 8193, &read_count) &&
            read_count == 8193 && !memcmp(payload, received, 8193),
            "cache invalidation after overwrite");
    require(bfs4_append(&fs, "/a/new/data.bin", payload, 8193, 100000),
            "append only new blocks");
    require(bfs4_read(&fs, &entry, 0, received, 100000, &read_count) &&
            read_count == 100000 && !memcmp(payload, received, 100000),
            "read after append");
    require(bfs4_space(&fs, &total, &free_after) && free_after < free_before,
            "space accounting");
    require(bfs4_rename(&fs, "/a/new/data.bin", "/a/new/renamed.bin"), "rename");
    require(!bfs4_resolve(&fs, "/a/new/data.bin", &entry), "old name absent");
    require(bfs4_resolve(&fs, "/a/new/renamed.bin", &entry), "new name present");
    require(bfs4_set_mode(&fs, "/a/new/renamed.bin", 0600), "chmod");
    uint16_t mode;
    require(bfs4_get_mode(&fs, "/a/new/renamed.bin", &mode) &&
            (mode & 0777U) == 0600U, "get mode");
    require(bfs4_list(&fs, NULL, entries, meta, 8, &count) && count == 1,
            "root list");
    require(bfs4_remove(&fs, "/a/new/renamed.bin"), "unlink");
    require(bfs4_remove(&fs, "/a/new"), "rmdir");
    require(bfs4_space(&fs, &total, &free_after) && free_after == free_before,
            "free space restored");
    char *fragmented = malloc(13U * 4096U);
    char filler[4096], filler_path[32];
    require(fragmented != NULL, "fragmentation buffer");
    memset(fragmented, 0x47, 13U * 4096U);
    memset(filler, 0x29, sizeof(filler));
    require(bfs4_write(&fs, "/a/frag.bin", fragmented, 4096U),
            "initial fragmented file");
    for (unsigned i = 0; i < 12U; ++i) {
        snprintf(filler_path, sizeof(filler_path), "/a/filler%u", i);
        require(bfs4_write(&fs, filler_path, filler, sizeof(filler)),
                "interleaved filler");
        require(bfs4_append(&fs, "/a/frag.bin", fragmented,
                            (i + 1U) * 4096U, (i + 2U) * 4096U),
                "fragmented append");
    }
    require(bfs4_resolve(&fs, "/a/frag.bin", &entry), "fragmented resolve");
    fat_check_report_t report;
    require(bfs4_check(&fs, &report) && report.completed && !report.errors,
            "online consistency check");
    require(bfs4_read(&fs, &entry, 11U * 4096U, received, 4096U,
                      &read_count) && read_count == 4096U &&
            !memcmp(received, fragmented + 11U * 4096U, 4096U),
            "indirect extent read");
    if (argc == 4 && !strcmp(argv[3], "--keep-frag")) {
        require(bfs4_sync(&fs), "sync fragmented image");
        fclose(disk);
        puts("BFS4 fragmented image OK");
        return 0;
    }
    require(bfs4_remove(&fs, "/a/frag.bin"), "fragmented unlink");
    for (unsigned i = 0; i < 12U; ++i) {
        snprintf(filler_path, sizeof(filler_path), "/a/filler%u", i);
        require(bfs4_remove(&fs, filler_path), "remove filler");
    }
    require(bfs4_space(&fs, &total, &free_after) && free_after == free_before,
            "indirect extent space restored");
    free(fragmented);
    require(bfs4_sync(&fs), "clean sync");
    free(payload); free(received);
    fclose(disk);
    puts("BFS4 host integration OK");
    return 0;
}
