#include "bleskernos.h"
#include "bleskernos_heap.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define POSIX_ERRNO_TLS_SLOT 31U
static int fallback_errno;
int *__errno_location(void) {
    int *value = (int *)bk_tls_get(POSIX_ERRNO_TLS_SLOT);
    if (!value) {
        value = (int *)bk_malloc(sizeof(*value));
        if (!value || (bk_i32)(bk_u32)value < 0) return &fallback_errno;
        *value = 0;
        if (bk_tls_set(POSIX_ERRNO_TLS_SLOT, value) < 0) {
            (void)bk_free(value);
            return &fallback_errno;
        }
    }
    return value;
}
#define ENVIRONMENT_CAPACITY 32U
static char *environment_storage[ENVIRONMENT_CAPACITY + 1U];
char **environ = environment_storage;
static unsigned int random_state = 1U;
static mode_t process_umask = 0022U;

extern int __blesk_posix_is_socket(int fd);
extern int __blesk_posix_socket_close(int fd);
extern ssize_t __blesk_posix_socket_read(int fd, void *buffer, size_t length);
extern ssize_t __blesk_posix_socket_write(int fd, const void *buffer,
                                          size_t length);
extern int __blesk_posix_socket_fcntl(int fd, int command, int value);
extern int __blesk_posix_is_pipe(int fd);
extern int __blesk_posix_pipe_close(int fd);
extern ssize_t __blesk_posix_pipe_read(int fd, void *buffer, size_t length);
extern ssize_t __blesk_posix_pipe_write(int fd, const void *buffer,
                                        size_t length);
extern int __blesk_posix_pipe_fcntl(int fd, int command, int value);

struct __blesk_posix_dir {
    bk_dirent_t entries[BK_DIRENT_MAX];
    bk_u32 count;
    bk_u32 position;
    struct dirent current;
};

static int posix_result(bk_i32 result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

static void copy_name(char *destination, const char *source,
                      unsigned int capacity) {
    unsigned int i = 0U;
    if (!capacity) return;
    while (source && source[i] && i + 1U < capacity) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
}

int open(const char *path, int flags, ...) {
    bk_u32 native_flags;
    bk_dirent_t existing;
    int access_mode = flags & 3;
    bk_i32 fd;

    if (!path) { errno = EFAULT; return -1; }
    if (access_mode == O_RDONLY) native_flags = BK_O_RDONLY;
    else if (access_mode == O_WRONLY) native_flags = BK_O_WRONLY;
    else if (access_mode == O_RDWR) native_flags = BK_O_RDWR;
    else { errno = EINVAL; return -1; }

    if ((flags & O_CREAT) && (flags & O_EXCL) && bk_stat(path, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (flags & O_CREAT) native_flags |= BK_O_CREATE;
    if (flags & O_TRUNC) native_flags |= BK_O_TRUNC;
    fd = bk_open(path, native_flags);
    if (fd < 0) return posix_result(fd);
    if (flags & O_APPEND) {
        bk_i32 seek_result = bk_lseek(fd, 0, SEEK_END);
        if (seek_result < 0) {
            (void)bk_close(fd);
            errno = -seek_result;
            return -1;
        }
    }
    return fd;
}

int creat(const char *path, mode_t mode) {
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

ssize_t read(int fd, void *buffer, unsigned int count) {
    if (__blesk_posix_is_pipe(fd))
        return __blesk_posix_pipe_read(fd, buffer, count);
    if (__blesk_posix_is_socket(fd))
        return __blesk_posix_socket_read(fd, buffer, count);
    return (ssize_t)posix_result(bk_read(fd, buffer, count));
}

ssize_t write(int fd, const void *buffer, unsigned int count) {
    if (__blesk_posix_is_pipe(fd))
        return __blesk_posix_pipe_write(fd, buffer, count);
    if (__blesk_posix_is_socket(fd))
        return __blesk_posix_socket_write(fd, buffer, count);
    return (ssize_t)posix_result(bk_write(fd, buffer, count));
}

int close(int fd) {
    if (__blesk_posix_is_pipe(fd)) return __blesk_posix_pipe_close(fd);
    if (__blesk_posix_is_socket(fd)) return __blesk_posix_socket_close(fd);
    return posix_result(bk_close(fd));
}

int dup(int fd) {
    if (__blesk_posix_is_pipe(fd) || __blesk_posix_is_socket(fd)) {
        errno = ENOSYS;
        return -1;
    }
    return posix_result(bk_dup(fd, -1));
}

int dup2(int old_fd, int new_fd) {
    if (new_fd < 0) { errno = EBADF; return -1; }
    if (__blesk_posix_is_pipe(old_fd) || __blesk_posix_is_socket(old_fd)) {
        errno = ENOSYS;
        return -1;
    }
    return posix_result(bk_dup(old_fd, new_fd));
}

int fcntl(int fd, int command, ...) {
    int value = 0;
    va_list arguments;
    if (command == F_SETFL) {
        va_start(arguments, command);
        value = va_arg(arguments, int);
        va_end(arguments);
    }
    if (__blesk_posix_is_socket(fd))
        return __blesk_posix_socket_fcntl(fd, command, value);
    if (__blesk_posix_is_pipe(fd))
        return __blesk_posix_pipe_fcntl(fd, command, value);
    if (command == F_GETFL) return 0;
    if (command == F_SETFL && !(value & ~O_NONBLOCK)) return 0;
    errno = EINVAL;
    return -1;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (whence < SEEK_SET || whence > SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)posix_result(bk_lseek(fd, offset, (bk_u32)whence));
}

int fsync(int fd) { return posix_result(bk_fsync(fd)); }

int ftruncate(int fd, off_t length) {
    if (length < 0) { errno = EINVAL; return -1; }
    return posix_result(bk_ftruncate(fd, (bk_u32)length));
}

int truncate(const char *path, off_t length) {
    int fd, result;
    if (length < 0) { errno = EINVAL; return -1; }
    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    result = ftruncate(fd, length);
    if (close(fd) < 0 && result == 0) result = -1;
    return result;
}

int unlink(const char *path) { return posix_result(bk_unlink(path)); }
int rmdir(const char *path) { return posix_result(bk_unlink(path)); }
int chdir(const char *path) { return posix_result(bk_chdir(path)); }

char *getcwd(char *buffer, unsigned int size) {
    bk_i32 result;
    if (!buffer || !size) { errno = EINVAL; return (char *)0; }
    result = bk_getcwd(buffer, size);
    if (result < 0) { errno = -result; return (char *)0; }
    return buffer;
}

int access(const char *path, int mode) {
    bk_dirent_t entry;
    bk_i32 result;
    if (mode & ~(R_OK | W_OK | X_OK)) { errno = EINVAL; return -1; }
    result = bk_stat(path, &entry);
    if (result < 0) return posix_result(result);
    if ((mode & X_OK) && entry.type != BK_NODE_FILE) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int isatty(int fd) {
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) return 1;
    errno = ENOTTY;
    return 0;
}

pid_t getpid(void) { return (pid_t)bk_getpid(); }
pid_t getppid(void) { return (pid_t)bk_getppid(); }
unsigned int getuid(void) { return 0U; }
unsigned int geteuid(void) { return 0U; }
unsigned int getgid(void) { return 0U; }
unsigned int getegid(void) { return 0U; }

static void sleep_milliseconds(bk_u32 milliseconds) {
    bk_u32 start = bk_uptime_ms();
    while ((bk_u32)(bk_uptime_ms() - start) < milliseconds) bk_sleep(1U);
}

unsigned int sleep(unsigned int seconds) {
    while (seconds > 4294967U) {
        sleep_milliseconds(4294967000U);
        seconds -= 4294967U;
    }
    sleep_milliseconds(seconds * 1000U);
    return 0U;
}

int usleep(useconds_t microseconds) {
    bk_u32 milliseconds = (microseconds + 999U) / 1000U;
    sleep_milliseconds(milliseconds);
    return 0;
}

void _exit(int status) { bk_exit(status); }

pid_t waitpid(pid_t pid, int *status, int options) {
    bk_i32 result;
    if (pid <= 0 || (options & ~WNOHANG)) { errno = EINVAL; return -1; }
    result = options & WNOHANG
        ? bk_waitpid_nohang((bk_u32)pid, (bk_i32 *)status)
        : bk_waitpid((bk_u32)pid, (bk_i32 *)status);
    return (pid_t)posix_result(result);
}

int kill(pid_t pid, int signal_number) {
    bk_i32 result;
    if (pid <= 0 || signal_number < 0 || signal_number >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    result = bk_signal_send((bk_u32)pid, signal_number);
    return posix_result(result);
}

int sched_yield(void) { bk_yield(); return 0; }

static void stat_from_native(struct stat *info, const bk_dirent_t *entry) {
    memset(info, 0, sizeof(*info));
    info->st_mode = entry->type == BK_NODE_DIR ? S_IFDIR : S_IFREG;
    info->st_mode |= S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    info->st_nlink = 1U;
    info->st_size = (off_t)entry->size;
}

int stat(const char *path, struct stat *info) {
    bk_dirent_t entry;
    bk_i32 result;
    if (!path || !info) { errno = EFAULT; return -1; }
    result = bk_stat(path, &entry);
    if (result < 0) return posix_result(result);
    stat_from_native(info, &entry);
    return 0;
}

int fstat(int fd, struct stat *info) {
    bk_i32 size;
    if (!info) { errno = EFAULT; return -1; }
    size = bk_file_size(fd);
    if (size < 0) return posix_result(size);
    memset(info, 0, sizeof(*info));
    info->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    info->st_nlink = 1U;
    info->st_size = size;
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    return posix_result(bk_mkdir(path));
}

int chmod(const char *path, mode_t mode) {
    (void)mode;
    return access(path, F_OK);
}

mode_t umask(mode_t mask) {
    mode_t old = process_umask;
    process_umask = mask & 0777U;
    return old;
}

DIR *opendir(const char *path) {
    struct __blesk_posix_dir *directory;
    bk_i32 result;
    if (!path) { errno = EFAULT; return (DIR *)0; }
    directory = (struct __blesk_posix_dir *)bk_malloc(sizeof(*directory));
    if (!directory || (bk_i32)(bk_u32)directory < 0) {
        errno = ENOMEM;
        return (DIR *)0;
    }
    memset(directory, 0, sizeof(*directory));
    result = bk_getdents(path, directory->entries, BK_DIRENT_MAX,
                         &directory->count);
    if (result < 0) {
        (void)bk_free(directory);
        errno = -result;
        return (DIR *)0;
    }
    return (DIR *)directory;
}

struct dirent *readdir(DIR *opaque) {
    struct __blesk_posix_dir *directory =
        (struct __blesk_posix_dir *)opaque;
    bk_dirent_t *source;
    if (!directory) { errno = EBADF; return (struct dirent *)0; }
    if (directory->position >= directory->count) return (struct dirent *)0;
    source = &directory->entries[directory->position++];
    directory->current.d_ino = directory->position;
    directory->current.d_type = source->type == BK_NODE_DIR ? DT_DIR : DT_REG;
    copy_name(directory->current.d_name, source->name,
              sizeof(directory->current.d_name));
    return &directory->current;
}

void rewinddir(DIR *opaque) {
    struct __blesk_posix_dir *directory =
        (struct __blesk_posix_dir *)opaque;
    if (directory) directory->position = 0U;
}

int closedir(DIR *opaque) {
    if (!opaque) { errno = EBADF; return -1; }
    return posix_result(bk_free(opaque));
}

/* BLES_POSIX_REALTIME_SUBSECOND_V4
 * The RTC exposed by time() has one-second resolution.  Returning tv_nsec=0
 * made gettimeofday() advance in whole seconds; games such as Tux Racer use
 * it as their frame clock and therefore simulated almost no motion between
 * RTC ticks, then jumped one second at a time.  Anchor wall time once to the
 * millisecond monotonic clock and preserve fractional time thereafter. */
int clock_gettime(clockid_t clock_id, struct timespec *value) {
    static int realtime_anchor_valid;
    static time_t realtime_anchor_sec;
    static bk_u32 realtime_anchor_ms;
    bk_u32 milliseconds;

    if (!value) { errno = EFAULT; return -1; }
    milliseconds = bk_uptime_ms();

    if (clock_id == CLOCK_REALTIME) {
        bk_u32 elapsed;
        if (!realtime_anchor_valid) {
            time_t wall = time((time_t *)0);
            if (wall == (time_t)-1) return -1;
            realtime_anchor_sec = wall;
            realtime_anchor_ms = milliseconds;
            realtime_anchor_valid = 1;
        }
        /* Unsigned subtraction intentionally remains correct across one
         * 32-bit uptime wrap, which is plenty for a process lifetime. */
        elapsed = (bk_u32)(milliseconds - realtime_anchor_ms);
        value->tv_sec = realtime_anchor_sec + (time_t)(elapsed / 1000U);
        value->tv_nsec = (signed long)((elapsed % 1000U) * 1000000U);
        return 0;
    }
    if (clock_id != CLOCK_MONOTONIC) { errno = EINVAL; return -1; }
    value->tv_sec = (time_t)(milliseconds / 1000U);
    value->tv_nsec = (signed long)((milliseconds % 1000U) * 1000000U);
    return 0;
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    bk_u32 milliseconds;
    if (!request || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L || request->tv_sec > 4294967) {
        errno = EINVAL;
        return -1;
    }
    milliseconds = (bk_u32)request->tv_sec * 1000U +
                   ((bk_u32)request->tv_nsec + 999999U) / 1000000U;
    sleep_milliseconds(milliseconds);
    if (remaining) { remaining->tv_sec = 0; remaining->tv_nsec = 0; }
    return 0;
}

/*
 * Native POSIX heap
 * -----------------
 *
 * bk_malloc()/bk_free() are kernel allocation syscalls.  Calling them for
 * every tiny libc allocation is both expensive and, more importantly, consumes
 * one entry in the kernel's per-process allocation tracker.  Real programs
 * such as Tux Racer/Jim Tcl can have many thousands of live 20-100 byte
 * objects while parsing course scripts, which used to exhaust the 8192 kernel
 * slots even though less than 1 MiB of memory was actually live.
 *
 * Keep the syscall ABI as the backing allocator, but sub-allocate normal
 * malloc() traffic from 256 KiB user-space arenas.  Free blocks are reused and
 * adjacent free blocks are coalesced.  Large requests get a dedicated arena.
 * This turns thousands of kernel allocations into a handful without changing
 * the public POSIX API or the ownership/cleanup rules in the kernel.
 */
#define POSIX_HEAP_ALIGNMENT       16U
#define POSIX_HEAP_ARENA_PAYLOAD   (256U * 1024U)
#define POSIX_HEAP_CHUNK_MAGIC     0x5048434BU /* "PHCK" */
#define POSIX_HEAP_BLOCK_MAGIC     0x5048424CU /* "PHBL" */

typedef struct posix_heap_chunk posix_heap_chunk_t;
typedef struct posix_heap_block posix_heap_block_t;

struct posix_heap_block {
    bk_u32 magic;
    bk_u32 size;
    bk_u32 is_free;
    bk_u32 reserved0;
    posix_heap_block_t *previous;
    posix_heap_block_t *next;
    posix_heap_chunk_t *chunk;
    bk_u32 reserved1;
};

struct posix_heap_chunk {
    bk_u32 magic;
    bk_u32 allocation_size;
    posix_heap_chunk_t *next;
    posix_heap_block_t *first;
};

static posix_heap_chunk_t *posix_heap_chunks;
static volatile bk_u32 posix_heap_spinlock;

static size_t posix_heap_align(size_t size) {
    if (size > 0xFFFFFFFFU - (POSIX_HEAP_ALIGNMENT - 1U)) return 0U;
    return (size + POSIX_HEAP_ALIGNMENT - 1U) &
           ~(size_t)(POSIX_HEAP_ALIGNMENT - 1U);
}

static void posix_heap_lock(void) {
    while (__sync_lock_test_and_set(&posix_heap_spinlock, 1U)) bk_yield();
    __sync_synchronize();
}

static void posix_heap_unlock(void) {
    __sync_synchronize();
    __sync_lock_release(&posix_heap_spinlock);
}

static int posix_heap_pointer_ok(const void *pointer) {
    return pointer && (bk_i32)(bk_u32)pointer >= 0;
}

static void posix_heap_split(posix_heap_block_t *block, size_t wanted) {
    size_t remaining;
    posix_heap_block_t *split;
    if (!block || wanted > block->size) return;
    remaining = block->size - wanted;
    if (remaining < sizeof(posix_heap_block_t) + POSIX_HEAP_ALIGNMENT)
        return;
    split = (posix_heap_block_t *)((unsigned char *)(block + 1) + wanted);
    split->magic = POSIX_HEAP_BLOCK_MAGIC;
    split->size = (bk_u32)(remaining - sizeof(posix_heap_block_t));
    split->is_free = 1U;
    split->reserved0 = 0U;
    split->previous = block;
    split->next = block->next;
    split->chunk = block->chunk;
    split->reserved1 = 0U;
    if (split->next) split->next->previous = split;
    block->next = split;
    block->size = (bk_u32)wanted;
}

static posix_heap_block_t *posix_heap_coalesce(posix_heap_block_t *block) {
    posix_heap_block_t *other;
    if (!block) return block;
    other = block->next;
    if (other && other->is_free && other->chunk == block->chunk &&
        other->magic == POSIX_HEAP_BLOCK_MAGIC) {
        block->size += (bk_u32)sizeof(posix_heap_block_t) + other->size;
        block->next = other->next;
        if (block->next) block->next->previous = block;
        other->magic = 0U;
    }
    other = block->previous;
    if (other && other->is_free && other->chunk == block->chunk &&
        other->magic == POSIX_HEAP_BLOCK_MAGIC) {
        other->size += (bk_u32)sizeof(posix_heap_block_t) + block->size;
        other->next = block->next;
        if (other->next) other->next->previous = other;
        block->magic = 0U;
        block = other;
    }
    return block;
}

static posix_heap_block_t *posix_heap_new_chunk(size_t wanted) {
    size_t payload = wanted > POSIX_HEAP_ARENA_PAYLOAD
        ? wanted : POSIX_HEAP_ARENA_PAYLOAD;
    size_t overhead = sizeof(posix_heap_chunk_t) + POSIX_HEAP_ALIGNMENT - 1U +
                      sizeof(posix_heap_block_t);
    size_t total;
    posix_heap_chunk_t *chunk;
    posix_heap_block_t *block;
    bk_u32 block_address;
    size_t block_capacity;

    if (payload > 0xFFFFFFFFU - overhead) return (posix_heap_block_t *)0;
    total = payload + overhead;
    chunk = (posix_heap_chunk_t *)bk_malloc((bk_u32)total);
    if (!posix_heap_pointer_ok(chunk)) return (posix_heap_block_t *)0;

    block_address = ((bk_u32)chunk + (bk_u32)sizeof(*chunk) +
                     POSIX_HEAP_ALIGNMENT - 1U) &
                    ~(POSIX_HEAP_ALIGNMENT - 1U);
    block = (posix_heap_block_t *)block_address;
    block_capacity = total - (size_t)(block_address - (bk_u32)chunk) -
                     sizeof(*block);
    block_capacity &= ~(size_t)(POSIX_HEAP_ALIGNMENT - 1U);

    chunk->magic = POSIX_HEAP_CHUNK_MAGIC;
    chunk->allocation_size = (bk_u32)total;
    chunk->next = posix_heap_chunks;
    chunk->first = block;
    posix_heap_chunks = chunk;

    block->magic = POSIX_HEAP_BLOCK_MAGIC;
    block->size = (bk_u32)block_capacity;
    block->is_free = 1U;
    block->reserved0 = 0U;
    block->previous = (posix_heap_block_t *)0;
    block->next = (posix_heap_block_t *)0;
    block->chunk = chunk;
    block->reserved1 = 0U;
    return block;
}

static posix_heap_block_t *posix_heap_find_free(size_t wanted) {
    posix_heap_chunk_t *chunk;
    posix_heap_block_t *block;
    for (chunk = posix_heap_chunks; chunk; chunk = chunk->next) {
        if (chunk->magic != POSIX_HEAP_CHUNK_MAGIC) continue;
        for (block = chunk->first; block; block = block->next) {
            if (block->magic == POSIX_HEAP_BLOCK_MAGIC && block->is_free &&
                block->size >= wanted)
                return block;
        }
    }
    return (posix_heap_block_t *)0;
}

void *malloc(size_t size) {
    size_t aligned;
    posix_heap_block_t *block;
    if (!size) size = 1U;
    aligned = posix_heap_align(size);
    if (!aligned) { errno = ENOMEM; return (void *)0; }

    posix_heap_lock();
    block = posix_heap_find_free(aligned);
    if (!block) block = posix_heap_new_chunk(aligned);
    if (!block) {
        posix_heap_unlock();
        errno = ENOMEM;
        return (void *)0;
    }
    posix_heap_split(block, aligned);
    block->is_free = 0U;
    posix_heap_unlock();
    return (void *)(block + 1);
}

void *calloc(size_t count, size_t size) {
    size_t total;
    void *result;
    if (size && count > 0xFFFFFFFFU / size) { errno = ENOMEM; return 0; }
    total = count * size;
    result = malloc(total);
    if (result) memset(result, 0, total);
    return result;
}

void *realloc(void *pointer, size_t size) {
    posix_heap_block_t *block;
    posix_heap_block_t *next;
    size_t aligned;
    size_t old_size;
    void *replacement;

    if (!pointer) return malloc(size);
    if (!size) { free(pointer); return (void *)0; }
    aligned = posix_heap_align(size);
    if (!aligned) { errno = ENOMEM; return (void *)0; }

    block = (posix_heap_block_t *)pointer - 1;
    posix_heap_lock();
    if (block->magic != POSIX_HEAP_BLOCK_MAGIC || !block->chunk ||
        block->chunk->magic != POSIX_HEAP_CHUNK_MAGIC || block->is_free) {
        posix_heap_unlock();
        errno = EINVAL;
        return (void *)0;
    }

    if (aligned <= block->size) {
        posix_heap_split(block, aligned);
        if (block->next && block->next->is_free)
            (void)posix_heap_coalesce(block->next);
        posix_heap_unlock();
        return pointer;
    }

    next = block->next;
    if (next && next->is_free && next->magic == POSIX_HEAP_BLOCK_MAGIC &&
        next->chunk == block->chunk &&
        block->size + sizeof(posix_heap_block_t) + next->size >= aligned) {
        block->size += (bk_u32)sizeof(posix_heap_block_t) + next->size;
        block->next = next->next;
        if (block->next) block->next->previous = block;
        next->magic = 0U;
        posix_heap_split(block, aligned);
        posix_heap_unlock();
        return pointer;
    }

    old_size = block->size;
    posix_heap_unlock();
    replacement = malloc(size);
    if (!replacement) return (void *)0;
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    free(pointer);
    return replacement;
}

void *reallocarray(void *pointer,size_t count,size_t size){if(size&&count>0xFFFFFFFFU/size){errno=ENOMEM;return(void*)0;}return realloc(pointer,count*size);}

void free(void *pointer) {
    posix_heap_block_t *block;
    posix_heap_chunk_t *chunk;
    posix_heap_chunk_t **link;
    int release_chunk = 0;

    if (!pointer) return;
    block = (posix_heap_block_t *)pointer - 1;
    posix_heap_lock();
    if (block->magic != POSIX_HEAP_BLOCK_MAGIC || !block->chunk ||
        block->chunk->magic != POSIX_HEAP_CHUNK_MAGIC || block->is_free) {
        posix_heap_unlock();
        return;
    }
    block->is_free = 1U;
    block = posix_heap_coalesce(block);
    chunk = block->chunk;

    /* Keep one empty arena warm, but return additional completely-empty
     * arenas to the kernel so long-running applications do not retain their
     * peak transient memory forever. */
    if (!block->previous && !block->next && chunk->first == block &&
        (posix_heap_chunks != chunk || chunk->next != (posix_heap_chunk_t *)0)) {
        link = &posix_heap_chunks;
        while (*link && *link != chunk) link = &(*link)->next;
        if (*link == chunk) {
            *link = chunk->next;
            chunk->magic = 0U;
            release_chunk = 1;
        }
    }
    posix_heap_unlock();
    if (release_chunk) (void)bk_free(chunk);
}
int bk_posix_heap_statistics(bk_posix_heap_statistics_t *out) {
    posix_heap_chunk_t *chunk;
    posix_heap_block_t *block;
    if (!out || out->size < sizeof(*out)) return -1;
    posix_heap_lock();
    out->live_bytes = out->reserved_bytes = out->allocations = out->free_bytes = 0;
    for (chunk = posix_heap_chunks; chunk; chunk = chunk->next) {
        out->reserved_bytes += chunk->allocation_size;
        for (block = chunk->first; block; block = block->next) {
            if (block->is_free) out->free_bytes += block->size;
            else { out->live_bytes += block->size; out->allocations++; }
        }
    }
    posix_heap_unlock();
    return 0;
}

void exit(int status) { bk_exit(status); }
void abort(void) { bk_exit(134); }
int abs(int value) { return value < 0 ? -value : value; }

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
    size_t low = 0U, high = count;
    const unsigned char *bytes = (const unsigned char *)base;
    if (!key || (!base && count) || !size || !compare) return (void *)0;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const void *item = bytes + middle * size;
        int relation = compare(key, item);
        if (relation < 0) high = middle;
        else if (relation > 0) low = middle + 1U;
        else return (void *)item;
    }
    return (void *)0;
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
    unsigned char *bytes=(unsigned char*)base;
    if(!base||!size||!compare)return;
    for(size_t i=1;i<count;i++)for(size_t j=i;j>0;j--){
        unsigned char *a=bytes+(j-1U)*size,*b=bytes+j*size;
        if(compare(a,b)<=0)break;
        for(size_t k=0;k<size;k++){unsigned char t=a[k];a[k]=b[k];b[k]=t;}
    }
}

void srand(unsigned int seed){random_state=seed?seed:1U;}int rand(void){random_state=random_state*1103515245U+12345U;return(int)((random_state>>16)&0x7FFFU);}

static int environment_name_match(const char*entry,const char*name){size_t n=strlen(name);return strncmp(entry,name,n)==0&&entry[n]=='=';}
char *getenv(const char*name){if(!name||!name[0]||strchr(name,'='))return 0;for(unsigned i=0;environment_storage[i];i++)if(environment_name_match(environment_storage[i],name))return strchr(environment_storage[i],'=')+1;return 0;}
int setenv(const char*name,const char*value,int overwrite){size_t nl,vl;char*entry;unsigned slot=ENVIRONMENT_CAPACITY;if(!name||!name[0]||strchr(name,'=')||!value){errno=EINVAL;return-1;}for(unsigned i=0;i<ENVIRONMENT_CAPACITY;i++){if(environment_storage[i]&&environment_name_match(environment_storage[i],name)){if(!overwrite)return 0;slot=i;break;}if(!environment_storage[i]){slot=i;break;}}if(slot==ENVIRONMENT_CAPACITY){errno=ENOMEM;return-1;}nl=strlen(name);vl=strlen(value);entry=(char*)malloc(nl+vl+2U);if(!entry)return-1;memcpy(entry,name,nl);entry[nl]='=';memcpy(entry+nl+1U,value,vl+1U);if(environment_storage[slot])free(environment_storage[slot]);environment_storage[slot]=entry;if(slot+1U<=ENVIRONMENT_CAPACITY&&!environment_storage[slot+1U])environment_storage[slot+1U]=0;return 0;}
int unsetenv(const char*name){if(!name||!name[0]||strchr(name,'=')){errno=EINVAL;return-1;}for(unsigned i=0;environment_storage[i];i++)if(environment_name_match(environment_storage[i],name)){free(environment_storage[i]);do{environment_storage[i]=environment_storage[i+1U];i++;}while(i<ENVIRONMENT_CAPACITY&&environment_storage[i]);break;}return 0;}

static int randomize_template(char *name) {
    static const char alphabet[]="abcdefghijklmnopqrstuvwxyz0123456789";
    size_t length;
    if(!name){errno=EINVAL;return-1;}
    length=strlen(name);
    if(length<6U||strcmp(name+length-6U,"XXXXXX")){errno=EINVAL;return-1;}
    for(unsigned int i=0U;i<6U;i++)name[length-6U+i]=alphabet[(unsigned int)rand()%36U];
    return 0;
}
int mkstemp(char *name){
    for(unsigned int attempt=0U;attempt<128U;attempt++){
        int fd;if(randomize_template(name)<0)return-1;
        fd=open(name,O_RDWR|O_CREAT|O_EXCL,0600);
        if(fd>=0)return fd;
        if(errno!=EEXIST)return-1;
        for(unsigned int i=0U;i<6U;i++)name[strlen(name)-6U+i]='X';
    }
    errno=EEXIST;return-1;
}
char *mkdtemp(char *name){
    for(unsigned int attempt=0U;attempt<128U;attempt++){
        if(randomize_template(name)<0)return(char*)0;
        if(mkdir(name,0700)==0)return name;
        if(errno!=EEXIST)return(char*)0;
        for(unsigned int i=0U;i<6U;i++)name[strlen(name)-6U+i]='X';
    }
    errno=EEXIST;return(char*)0;
}
