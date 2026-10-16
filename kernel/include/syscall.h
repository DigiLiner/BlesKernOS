#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "idt.h"

#define SYSCALL_VECTOR 0x80
void syscall_wine_boot_begin(uint32_t pid);
int32_t syscall_wine_boot_result(void);
bool syscall_linux_borrow_resources(uint32_t parent,uint32_t child);
bool syscall_linux_clone_resources(uint32_t parent,uint32_t child);
bool syscall_linux_cow_fault(uint32_t address);
bool syscall_linux_futex_key(uint32_t address,bool private_key,uint32_t *key,uint32_t *scope);
void syscall_linux_activate_ldt(uint32_t process_id);
#if defined(BLES_ARCH_ARM32_RPI3)
#define SYSCALL_ABI_VERSION 6
#else
#define SYSCALL_ABI_VERSION 17
#endif

#define BK_ENOENT  2
#define BK_ESRCH   3
#define BK_EIO     5
#define BK_EBADF   9
#define BK_ECHILD 10
#define BK_EAGAIN 11
#define BK_ENOMEM 12
#define BK_EACCES 13
#define BK_EFAULT 14
#define BK_EBUSY  16
#define BK_EEXIST 17
#define BK_EINVAL 22
#define BK_EMFILE 24
#define BK_ENOTTY 25
#define BK_ENOSYS 38

enum {
    SYS_EXIT = 0,
    SYS_WRITE,
    SYS_GETPID,
    SYS_YIELD,
    SYS_SLEEP,
    SYS_UPTIME_MS,
    SYS_ABI_VERSION,
    SYS_RESERVED_7,
    SYS_GETPPID,
    SYS_OPEN,
    SYS_READ,
    SYS_CLOSE,
    SYS_GETCWD,
    SYS_CHDIR,
    SYS_MKDIR,
    SYS_UNLINK,
    SYS_RENAME,
    SYS_GETDENTS,
    SYS_ALLOC,
    SYS_REALLOC,
    SYS_FREE,
    SYS_SPAWN,
    SYS_WAITPID,
    SYS_KILL,
    SYS_API_CALL,
    SYS_UPCALL_RETURN,
    SYS_NET_RESOLVE,
    SYS_NET_SOCKET,
    SYS_NET_CONNECT,
    SYS_NET_SEND,
    SYS_NET_RECV,
    SYS_NET_CLOSE,
    SYS_NET_HTTP_GET,
    SYS_RESERVED_33,
    SYS_THREAD_CREATE,
    SYS_THREAD_JOIN,
    SYS_THREAD_DETACH,
    SYS_THREAD_EXIT,
    SYS_THREAD_SELF,
    SYS_THREAD_TLS_SET,
    SYS_THREAD_TLS_GET,
    /* Stage 12 ARM-native SDK extension. Appended so the established x86
       syscall numbering remains byte-for-byte compatible. */
    SYS_FILE_WRITE,
    SYS_FILE_SEEK,
    SYS_FILE_TELL,
    SYS_FILE_SIZE,
    SYS_FILE_STAT,
    SYS_LAUNCH_ARG_COPY,
    SYS_TICKS,
    /* POSIX/runtime extension. Keep additions at the end of the public ABI. */
    SYS_FILE_SYNC,
    SYS_FILE_TRUNCATE,
    SYS_FILE_DUP,
    SYS_SIGNAL_REGISTER,
    SYS_SIGNAL_SEND,
    SYS_PERSONALITY,
    SYS_VM_BRK,
    SYS_VM_MMAP,
    SYS_VM_MUNMAP,
    SYS_VM_MPROTECT,
    SYS_LINUX_FUTEX,
    SYS_LINUX_SET_THREAD_AREA,
    SYS_LINUX_GET_THREAD_AREA,
    SYS_LINUX_EXECVE,
    SYS_LINUX_UNAME,
    SYS_LINUX_GETTIMEOFDAY,
    SYS_LINUX_CLOCK_GETTIME,
    SYS_LINUX_CLOCK_GETRES,
    SYS_LINUX_STAT64,
    SYS_LINUX_FSTAT64,
    SYS_LINUX_ACCESS,
    SYS_LINUX_IOCTL,
    SYS_LINUX_SET_TID_ADDRESS,
    SYS_LINUX_SET_ROBUST_LIST,
    SYS_LINUX_TIME,
    SYS_LINUX_NANOSLEEP,
    SYS_LINUX_SYSINFO,
    SYS_LINUX_GETRLIMIT,
    SYS_LINUX_MADVISE,
    SYS_LINUX_PRLIMIT64,
    SYS_LINUX_GETRANDOM,
    SYS_LINUX_SCHED_GETAFFINITY,
    SYS_LINUX_SCHED_SETAFFINITY,
    SYS_LINUX_FCNTL64,
    SYS_LINUX_CLONE,
    SYS_COUNT
};

registers_t *syscall_handler(registers_t *regs);
void syscall_process_cleanup(uint32_t process_id);
void syscall_linux_exec_locks(void);
void syscall_inherit_files(uint32_t parent_process, uint32_t child_process);
/* Comparte el generador existente de getrandom con AT_RANDOM del cargador. */
uint32_t syscall_linux_random_word(void);
void syscall_set_linux_trace(uint32_t level);
void syscall_set_linux_trace_process(const char *name);
void syscall_set_linux_trace_module(const char *name);
void syscall_linux_trace_observe_filemap(uint32_t pid, const char *path);

#if defined(BLES_ARCH_ARM32_RPI3)
/* ARM32 transport keeps the public syscall numbers but uses the conventional
 * AArch32 register layout: r7=number, r0-r3=arguments/result, SVC #0.  x86
 * continues to use int 0x80 below. */
static inline int32_t syscall0(uint32_t number) {
    register uint32_t r0 __asm__("r0") = 0U;
    register uint32_t r7 __asm__("r7") = number;
    __asm__ volatile ("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t syscall1(uint32_t number, uint32_t arg1) {
    register uint32_t r0 __asm__("r0") = arg1;
    register uint32_t r7 __asm__("r7") = number;
    __asm__ volatile ("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t syscall2(uint32_t number, uint32_t arg1,
                               uint32_t arg2) {
    register uint32_t r0 __asm__("r0") = arg1;
    register uint32_t r1 __asm__("r1") = arg2;
    register uint32_t r7 __asm__("r7") = number;
    __asm__ volatile ("svc #0" : "+r"(r0) : "r"(r1), "r"(r7)
                      : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t syscall3(uint32_t number, uint32_t arg1,
                               uint32_t arg2, uint32_t arg3) {
    register uint32_t r0 __asm__("r0") = arg1;
    register uint32_t r1 __asm__("r1") = arg2;
    register uint32_t r2 __asm__("r2") = arg3;
    register uint32_t r7 __asm__("r7") = number;
    __asm__ volatile ("svc #0" : "+r"(r0)
                      : "r"(r1), "r"(r2), "r"(r7) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t syscall4(uint32_t number, uint32_t arg1,
                               uint32_t arg2, uint32_t arg3,
                               uint32_t arg4) {
    register uint32_t r0 __asm__("r0") = arg1;
    register uint32_t r1 __asm__("r1") = arg2;
    register uint32_t r2 __asm__("r2") = arg3;
    register uint32_t r3 __asm__("r3") = arg4;
    register uint32_t r7 __asm__("r7") = number;
    __asm__ volatile ("svc #0" : "+r"(r0)
                      : "r"(r1), "r"(r2), "r"(r3), "r"(r7)
                      : "memory", "cc");
    return (int32_t)r0;
}
#else
static inline int32_t syscall0(uint32_t number) {
    int32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number) : "memory");
    return result;
}

static inline int32_t syscall1(uint32_t number, uint32_t arg1) {
    int32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(arg1) : "memory");
    return result;
}

static inline int32_t syscall2(uint32_t number, uint32_t arg1,
                               uint32_t arg2) {
    int32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(arg1), "c"(arg2) : "memory");
    return result;
}

static inline int32_t syscall3(uint32_t number, uint32_t arg1,
                               uint32_t arg2, uint32_t arg3) {
    int32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(arg1), "c"(arg2), "d"(arg3)
                      : "memory");
    return result;
}

static inline int32_t syscall4(uint32_t number, uint32_t arg1,
                               uint32_t arg2, uint32_t arg3,
                               uint32_t arg4) {
    int32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(arg1), "c"(arg2), "d"(arg3),
                        "S"(arg4) : "memory");
    return result;
}
#endif

#endif
