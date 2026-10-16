#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#define COUNT 80
#define CHECK(x) do { if (!(x)) { \
    printf("[MAP-CAPACITY] line=%d errno=%d FAIL\n", __LINE__, errno); \
    return 1; } } while (0)

/* Exercise mappings above the old 32-object boundary, including VFS rename,
 * unlink and truncate guards. Each mapping retains its inode after close. */
int main(int argc, char **argv) {
    (void)argv;
    const int count = argc > 1 ? 16 : COUNT;
    const int renamed = count - 10, removed = count - 9;
    char paths[COUNT][64];
    unsigned char *views[COUNT];
    unsigned char page[4096];
    for (int i = 0; i < count; ++i) {
        snprintf(paths[i], sizeof(paths[i]), "/WINEGATE/map-capacity-%d.dat", i);
        unlink(paths[i]);
        int fd = open(paths[i], O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(fd >= 0);
        memset(page, i, sizeof(page));
        CHECK(write(fd, page, sizeof(page)) == sizeof(page));
        views[i] = mmap(NULL, sizeof(page), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
        CHECK(views[i] != MAP_FAILED && close(fd) == 0);
    }
    printf("[MAP-CAPACITY] %d simultaneous file mappings PASS\n", count);
    CHECK(rename(paths[renamed], "/WINEGATE/map-capacity-renamed.dat") == 0);
    strcpy(paths[renamed], "/WINEGATE/map-capacity-renamed.dat");
    int fd = open(paths[renamed], O_RDONLY);
    CHECK(fd >= 0);
    unsigned char *private = mmap(NULL, sizeof(page), PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE, fd, 0);
    CHECK(private != MAP_FAILED);
    views[renamed][0] = 123;
    CHECK(private[0] == 123);
    private[0] = 42;
    CHECK(views[renamed][0] == 123);
    CHECK(munmap(private, sizeof(page)) == 0 && close(fd) == 0);
    CHECK(open(paths[renamed], O_RDWR | O_TRUNC) == -1);
    CHECK(unlink(paths[removed]) == 0);
    views[removed][0] = 99;
    CHECK(msync(views[removed], sizeof(page), MS_SYNC) == 0);
    CHECK(access(paths[removed], F_OK) == -1 && errno == ENOENT);
    for (int i = 0; i < count; ++i) {
        CHECK(views[i][4095] == i);
        CHECK(munmap(views[i], sizeof(page)) == 0);
        if (i != removed) CHECK(unlink(paths[i]) == 0);
    }
    puts("[MAP-CAPACITY] rename/unlink/truncate/COW/release PASS");
    struct timespec start, end, mono_start, mono_end;
    CHECK(clock_gettime(CLOCK_REALTIME, &start) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &mono_start) == 0);
    for (int i = 0; i < 10000; ++i) {
        CHECK(clock_gettime(CLOCK_REALTIME, &end) == 0);
        CHECK(end.tv_sec >= start.tv_sec);
        CHECK(end.tv_nsec >= 0 && end.tv_nsec < 1000000000L);
    }
    struct timespec delay = {1, 100000000L};
    CHECK(nanosleep(&delay, NULL) == 0);
    CHECK(clock_gettime(CLOCK_REALTIME, &end) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &mono_end) == 0);
    CHECK(end.tv_sec >= start.tv_sec + 1);
    CHECK(mono_end.tv_sec >= mono_start.tv_sec + 1);
    puts("[MAP-CAPACITY] realtime refresh and monotonic clock PASS");
    return 0;
}
