#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

/* Runtime deliberadamente pequeño para la primera ABI Linux/i386. No llama a
 * ninguna funcion bk_*: despues de personality(), todos los int 0x80 deben ser
 * compatibles con la numeracion y el convenio de registros de Linux. */
static int linux_errno;
int *__errno_location(void) { return &linux_errno; }

static long linux_call0(unsigned int number) {
    long result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number) : "memory");
    return result;
}
static long linux_call1(unsigned int number, unsigned int a) {
    long result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(a) : "memory");
    return result;
}
static long linux_call2(unsigned int number, unsigned int a, unsigned int b) {
    long result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(a), "c"(b) : "memory");
    return result;
}
static long linux_call3(unsigned int number, unsigned int a, unsigned int b,
                        unsigned int c) {
    long result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
    return result;
}
static long linux_call6(unsigned int number, unsigned int a, unsigned int b,
                        unsigned int c, unsigned int d, unsigned int e,
                        unsigned int f) {
    register unsigned int result __asm__("eax")=number;
    register unsigned int arg1 __asm__("ebx")=a;
    register unsigned int arg2 __asm__("ecx")=b;
    register unsigned int arg3 __asm__("edx")=c;
    register unsigned int arg4 __asm__("esi")=d;
    register unsigned int arg5 __asm__("edi")=e;
    register unsigned int arg6 __asm__("ebp")=f;
    __asm__ volatile ("int $0x80" : "+r"(result)
                      : "r"(arg1),"r"(arg2),"r"(arg3),"r"(arg4),
                        "r"(arg5),"r"(arg6) : "memory");
    return (long)result;
}
static int checked(long result) {
    if (result < 0 && result >= -4095) {
        errno = (int)-result;
        return -1;
    }
    return (int)result;
}

ssize_t read(int fd, void *buffer, unsigned int count) {
    return checked(linux_call3(3U, (unsigned int)fd,
                               (unsigned int)buffer, count));
}
ssize_t write(int fd, const void *buffer, unsigned int count) {
    return checked(linux_call3(4U, (unsigned int)fd,
                               (unsigned int)buffer, count));
}
int open(const char *path, int flags, ...) {
    return checked(linux_call3(5U, (unsigned int)path,
                               (unsigned int)flags, 0U));
}
int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}
int close(int fd) { return checked(linux_call1(6U, (unsigned int)fd)); }
off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)checked(linux_call3(19U, (unsigned int)fd,
                                     (unsigned int)offset,
                                     (unsigned int)whence));
}
pid_t getpid(void) { return (pid_t)checked(linux_call0(20U)); }
pid_t getppid(void) { return (pid_t)checked(linux_call0(64U)); }
int fsync(int fd) { return checked(linux_call1(118U, (unsigned int)fd)); }
int dup(int fd) { return checked(linux_call1(41U, (unsigned int)fd)); }
int dup2(int old_fd, int new_fd) {
    return checked(linux_call2(63U, (unsigned int)old_fd,
                               (unsigned int)new_fd));
}
int unlink(const char *path) {
    return checked(linux_call1(10U, (unsigned int)path));
}
int chdir(const char *path) {
    return checked(linux_call1(12U, (unsigned int)path));
}
int mkdir(const char *path, mode_t mode) {
    return checked(linux_call2(39U, (unsigned int)path, mode));
}
int rename(const char *old_path, const char *new_path) {
    return checked(linux_call2(38U, (unsigned int)old_path,
                               (unsigned int)new_path));
}
int kill(pid_t pid, int signal_number) {
    return checked(linux_call2(37U, (unsigned int)pid,
                               (unsigned int)signal_number));
}
char *getcwd(char *buffer, unsigned int size) {
    int result = checked(linux_call2(183U, (unsigned int)buffer, size));
    return result < 0 ? (char *)0 : buffer;
}

static unsigned int linux_program_break;
int brk(void *address) {
    unsigned int result = (unsigned int)linux_call1(45U,(unsigned int)address);
    if (result != (unsigned int)address) { errno=ENOMEM; return -1; }
    linux_program_break=result;
    return 0;
}
void *sbrk(int increment) {
    unsigned int previous, requested, result;
    if (!linux_program_break)
        linux_program_break=(unsigned int)linux_call1(45U,0U);
    previous=linux_program_break;
    requested=previous+(unsigned int)increment;
    if ((increment>0&&requested<previous)||(increment<0&&requested>previous)) {
        errno=ENOMEM; return (void*)-1;
    }
    result=(unsigned int)linux_call1(45U,requested);
    if(result!=requested){errno=ENOMEM;return(void*)-1;}
    linux_program_break=result;return(void*)previous;
}
void *mmap(void *address,size_t length,int protection,int flags,int fd,
           off_t offset) {
    long result;
    if(offset<0||(offset&4095)){errno=EINVAL;return MAP_FAILED;}
    result=linux_call6(192U,(unsigned int)address,length,
        (unsigned int)protection,(unsigned int)flags,(unsigned int)fd,
        (unsigned int)offset/4096U);
    if(result<0&&result>=-4095){errno=(int)-result;return MAP_FAILED;}
    return(void*)result;
}
int munmap(void *address,size_t length){return checked(linux_call2(91U,(unsigned int)address,length));}
int mprotect(void *address,size_t length,int protection){return checked(linux_call3(125U,(unsigned int)address,length,(unsigned int)protection));}
int msync(void *address,size_t length,int flags){(void)address;(void)length;(void)flags;errno=ENOSYS;return-1;}

void _exit(int status) {
    (void)linux_call1(252U, (unsigned int)status);
    for (;;) (void)linux_call0(158U);
}
void exit(int status) { _exit(status); }
void abort(void) { _exit(134); }

/* Arena inicial: suficiente para portar utilidades pequeñas sin mezclar el
 * allocator nativo en la personalidad Linux. Mantiene metadatos para realloc;
 * free valida el bloque pero la recuperacion se incorporara con brk/mmap. */
typedef struct linux_heap_block {
    unsigned int size;
    unsigned int magic;
} linux_heap_block_t;
#define LINUX_HEAP_MAGIC 0x4C4E5848U
#define LINUX_HEAP_SIZE (512U * 1024U)
static unsigned char linux_heap[LINUX_HEAP_SIZE];
static unsigned int linux_heap_used;

void *malloc(size_t size) {
    linux_heap_block_t *block;
    unsigned int aligned;
    if (!size) size = 1U;
    aligned = ((unsigned int)size + 7U) & ~7U;
    if (aligned < size || linux_heap_used > LINUX_HEAP_SIZE - sizeof(*block) ||
        aligned > LINUX_HEAP_SIZE - linux_heap_used - sizeof(*block)) {
        errno = ENOMEM;
        return (void *)0;
    }
    block = (linux_heap_block_t *)(void *)(linux_heap + linux_heap_used);
    block->size = aligned;
    block->magic = LINUX_HEAP_MAGIC;
    linux_heap_used += sizeof(*block) + aligned;
    return block + 1;
}
void free(void *pointer) {
    linux_heap_block_t *block;
    if (!pointer) return;
    block = (linux_heap_block_t *)pointer - 1;
    if (block->magic == LINUX_HEAP_MAGIC) block->magic = 0U;
}
void *calloc(size_t count, size_t size) {
    size_t total;
    void *result;
    if (size && count > (size_t)-1 / size) { errno = ENOMEM; return 0; }
    total = count * size;
    result = malloc(total);
    if (result) memset(result, 0, total);
    return result;
}
void *realloc(void *pointer, size_t size) {
    linux_heap_block_t *block;
    void *replacement;
    size_t copied;
    if (!pointer) return malloc(size);
    if (!size) { free(pointer); return 0; }
    block = (linux_heap_block_t *)pointer - 1;
    if (block->magic != LINUX_HEAP_MAGIC) { errno = EINVAL; return 0; }
    if (size <= block->size) return pointer;
    replacement = malloc(size);
    if (!replacement) return 0;
    copied = block->size < size ? block->size : size;
    memcpy(replacement, pointer, copied);
    free(pointer);
    return replacement;
}
void *reallocarray(void *pointer, size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) { errno = ENOMEM; return 0; }
    return realloc(pointer, count * size);
}
