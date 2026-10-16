#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    printf("[AHCI:TEST] line=%d errno=%d FAIL\n", __LINE__, errno); \
    return 1; } } while (0)

static unsigned char data[131072];
static unsigned char got[131072];

int main(void) {
    const char *name = "/ahci0/ahci-verify.dat";
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (unsigned char)((i * 37U + (i >> 9U)) & 255U);
    struct statfs fs;
    CHECK(statfs("/ahci0", &fs) == 0 && (unsigned)fs.f_type == 0x4d44U);
#ifndef AHCI_REUSE
    int fd = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    for (unsigned offset = 0; offset < sizeof(data); offset += 4096U)
        CHECK(write(fd, data + offset, 4096U) == 4096);
    CHECK(fsync(fd) == 0 && close(fd) == 0);
#endif
#ifdef AHCI_REUSE
    int fd = open(name, O_RDONLY);
#else
    fd = open(name, O_RDONLY);
#endif
    CHECK(fd >= 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == sizeof(data));
    for (unsigned offset = 0; offset < sizeof(got); offset += 4096U)
        CHECK(read(fd, got + offset, 4096U) == 4096);
    CHECK(memcmp(data, got, sizeof(data)) == 0 && close(fd) == 0);
#ifdef AHCI_REUSE
    puts("[AHCI:TEST] cold reboot, persisted bytes PASS");
#else
    puts("[AHCI:TEST] DMA read, write, flush and reopen PASS");
#endif
    return 0;
}
