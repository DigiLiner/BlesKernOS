#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void *mmap(void *address, size_t length, int protection, int flags,
           int fd, off_t offset) {
    void *memory;
    off_t saved;
    ssize_t received;
    size_t done = 0U;
    (void)address;
    if (!length || offset < 0 || (flags & MAP_FIXED) ||
        !(flags & (MAP_PRIVATE | MAP_SHARED)) ||
        (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (!(flags & MAP_ANONYMOUS) && (flags & MAP_SHARED)) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    memory = malloc(length);
    if (!memory) return MAP_FAILED;
    memset(memory, 0, length);
    if (flags & MAP_ANONYMOUS) return memory;
    saved = lseek(fd, 0, SEEK_CUR);
    if (saved < 0 || lseek(fd, offset, SEEK_SET) < 0) {
        free(memory);
        return MAP_FAILED;
    }
    while (done < length) {
        received = read(fd, (unsigned char *)memory + done, length - done);
        if (received < 0) { free(memory); (void)lseek(fd, saved, SEEK_SET); return MAP_FAILED; }
        if (!received) break;
        done += (size_t)received;
    }
    (void)lseek(fd, saved, SEEK_SET);
    return memory;
}

int munmap(void *address, size_t length) {
    if (!address || address == MAP_FAILED || !length) { errno = EINVAL; return -1; }
    free(address);
    return 0;
}

int mprotect(void *address, size_t length, int protection) {
    if (!address || !length || (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
        errno = EINVAL;
        return -1;
    }
    /* The current i486 process heap is already user R/W. Per-range page
       permissions require a future VM syscall; accept compatible requests. */
    return 0;
}

int msync(void *address, size_t length, int flags) {
    (void)address; (void)length; (void)flags;
    errno = ENOSYS;
    return -1;
}
