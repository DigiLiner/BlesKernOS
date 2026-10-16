#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BULK_BYTES (4U * 1024U * 1024U)
static unsigned char data[BULK_BYTES], copied[BULK_BYTES];

static unsigned long long ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (unsigned long long)t.tv_sec * 1000ULL + t.tv_nsec / 1000000ULL;
}
#define CHECK(x) do { if (!(x)) { \
    printf("[BENCH:FAIL] line=%u errno=%d\n", __LINE__, errno); return 1; \
} } while (0)

int main(void) {
    unsigned long long start, end;
    volatile uint32_t sum = 2166136261U;
    for (unsigned i = 0; i < BULK_BYTES; i++) data[i] = (unsigned char)(i * 13U + (i >> 7U));
    start = ms();
    for (unsigned round = 0; round < 1024; round++)
        for (unsigned i = 0; i < BULK_BYTES; i += 64U)
            sum = (sum ^ data[i]) * 16777619U;
    end = ms();
    printf("[BENCH:CPU] bytes=%llu ms=%llu checksum=%u\n",
           (unsigned long long)BULK_BYTES * 1024ULL, end-start, sum);

    CHECK(mkdir("/WINEGATE/storage-bench", 0700) == 0);
    start = ms();
    for (unsigned i = 0; i < 64; i++) {
        char path[96];
        snprintf(path, sizeof path, "/WINEGATE/storage-bench/file-%03u.dat", i);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        CHECK(write(fd, data + i * 1024U, 1024U) == 1024);
        CHECK(fsync(fd) == 0 && close(fd) == 0);
    }
    end = ms();
    printf("[BENCH:SMALL] files=64 bytes=65536 ms=%llu\n", end-start);

    struct stat st;
    start = ms();
    for (unsigned i = 0; i < 512; i++)
        CHECK(stat("/WINEGATE/storage-bench/file-000.dat", &st) == 0 &&
              st.st_size == 1024);
    end = ms();
    printf("[BENCH:STAT_HIT] calls=512 ms=%llu\n", end-start);
    start = ms();
    for (unsigned i = 0; i < 512; i++)
        CHECK(stat("/WINEGATE/storage-bench/absent.dat", &st) == -1 &&
              errno == ENOENT);
    end = ms();
    printf("[BENCH:STAT_MISS] calls=512 ms=%llu\n", end-start);

    /* Same 256 KiB of data, with one durable commit versus one per chunk.
       Wine's registry temporaries repeatedly flush a growing file. */
    const unsigned chunk = 8192U, chunks = 32U;
    int sync_fd = open("/WINEGATE/storage-bench/sync-once.dat",
                       O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(sync_fd >= 0);
    start = ms();
    for (unsigned i = 0; i < chunks; i++)
        CHECK(write(sync_fd, data + i * chunk, chunk) == (int)chunk);
    CHECK(fsync(sync_fd) == 0 && close(sync_fd) == 0);
    end = ms();
    printf("[BENCH:SYNC_ONCE] bytes=%u commits=1 ms=%llu\n", chunk*chunks, end-start);

    sync_fd = open("/WINEGATE/storage-bench/sync-each.dat",
                   O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(sync_fd >= 0);
    start = ms();
    for (unsigned i = 0; i < chunks; i++) {
        CHECK(write(sync_fd, data + i * chunk, chunk) == (int)chunk);
        CHECK(fsync(sync_fd) == 0);
    }
    CHECK(close(sync_fd) == 0);
    end = ms();
    printf("[BENCH:SYNC_EACH] bytes=%u commits=%u ms=%llu\n", chunk*chunks, chunks, end-start);

    sync_fd = open("/WINEGATE/storage-bench/sync-each.dat", O_RDONLY);
    CHECK(sync_fd >= 0);
    CHECK(read(sync_fd, copied, chunk*chunks) == (int)(chunk*chunks));
    CHECK(memcmp(data, copied, chunk*chunks) == 0 && close(sync_fd) == 0);

    /* Cross partial sectors, then force the full-rewrite fallback with an
       overwrite and a truncate. Reopen to verify the durable result. */
    unsigned char edge_expected[3664];
    memcpy(edge_expected, data, sizeof(edge_expected));
    sync_fd = open("/WINEGATE/storage-bench/edge.dat",
                   O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(sync_fd >= 0);
    CHECK(write(sync_fd, edge_expected, 701) == 701 && fsync(sync_fd) == 0);
    CHECK(write(sync_fd, edge_expected + 701, 913) == 913 && fsync(sync_fd) == 0);
    CHECK(write(sync_fd, edge_expected + 1614, 2000) == 2000 && fsync(sync_fd) == 0);
    CHECK(lseek(sync_fd, 100, SEEK_SET) == 100);
    CHECK(write(sync_fd, "ABCDE", 5) == 5 && fsync(sync_fd) == 0);
    memcpy(edge_expected + 100, "ABCDE", 5);
    CHECK(ftruncate(sync_fd, 1500) == 0 && fsync(sync_fd) == 0);
    CHECK(lseek(sync_fd, 0, SEEK_END) == 1500);
    CHECK(write(sync_fd, data + 1500, 47) == 47 && fsync(sync_fd) == 0);
    CHECK(close(sync_fd) == 0);
    sync_fd = open("/WINEGATE/storage-bench/edge.dat", O_RDONLY);
    CHECK(sync_fd >= 0 && fstat(sync_fd, &st) == 0 && st.st_size == 1547);
    CHECK(read(sync_fd, copied, 1547) == 1547 &&
          memcmp(copied, edge_expected, 1547) == 0 && close(sync_fd) == 0);
    puts("[BENCH:EDGE] append/overwrite/truncate/reopen verified");

    const char *bulk = "/WINEGATE/storage-bench/bulk.dat";
    int fd = open(bulk, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    start = ms();
    CHECK(write(fd, data, BULK_BYTES) == BULK_BYTES);
    CHECK(fsync(fd) == 0 && close(fd) == 0);
    end = ms();
    printf("[BENCH:BULK_WRITE] bytes=%u ms=%llu\n", BULK_BYTES, end-start);

    fd = open(bulk, O_RDONLY);
    CHECK(fd >= 0);
    start = ms();
    CHECK(read(fd, copied, BULK_BYTES) == BULK_BYTES);
    end = ms();
    CHECK(memcmp(data, copied, BULK_BYTES) == 0 && close(fd) == 0);
    printf("[BENCH:BULK_READ] bytes=%u ms=%llu\n", BULK_BYTES, end-start);
    puts("[BENCH:PASS] all data verified");
    return 0;
}
