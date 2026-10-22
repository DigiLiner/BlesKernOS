#include <bleskernos_lbs.h>
#include <dlfcn.h>
#include <string.h>
extern void __bk_dl_set_error(const char *);
extern void *__bk_dl_own_symbol(void *, const char *);
void *bk_lbs_open(const char *name, uint32_t major) {
    char path[260];
    void *h;
    const bk_lbs_info_t *(*query)(void);
    const bk_lbs_info_t *info;
    if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\') || strlen(name) > 100) {
        __bk_dl_set_error("invalid native library name");
        return 0;
    }
    strcpy(path, BK_LBS_DIRECTORY);
    strcat(path, name);
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return 0;
    query = (const bk_lbs_info_t *(*)(void))__bk_dl_own_symbol(h, "bk_lbs_info");
    info = query ? query() : 0;
    if (!info || info->size < sizeof(*info)) {
        dlclose(h);
        __bk_dl_set_error("missing or invalid LBS ABI descriptor");
        return 0;
    }
    if (info->abi_major != major) {
        dlclose(h);
        __bk_dl_set_error("incompatible LBS ABI major");
        return 0;
    }
    if (info->syscall_abi > (uint32_t)bk_abi_version()) {
        dlclose(h);
        __bk_dl_set_error("LBS requires a newer syscall ABI");
        return 0;
    }
    return h;
}

/* Optional loader backend. These are the existing native VM syscalls, not the
 * POSIX mmap shim (which intentionally retains its historical heap semantics).
 * No Linux personality switch or new kernel ABI is required. */
/* A cdecl assembler leaf keeps EBP available even when the SDK enables frame
 * pointers. Save all callee-saved registers around the six-argument syscall. */
extern int lbs_vm_call(unsigned int, unsigned int, unsigned int, unsigned int,
                       unsigned int, unsigned int, unsigned int);
__asm__(".pushsection .text\n"
        ".type lbs_vm_call,@function\n"
        "lbs_vm_call:\n"
        "pushl %ebp\n pushl %edi\n pushl %esi\n pushl %ebx\n"
        "movl 20(%esp),%eax\n movl 24(%esp),%ebx\n"
        "movl 28(%esp),%ecx\n movl 32(%esp),%edx\n"
        "movl 36(%esp),%esi\n movl 40(%esp),%edi\n movl 44(%esp),%ebp\n"
        "int $0x80\n popl %ebx\n popl %esi\n popl %edi\n popl %ebp\n ret\n"
        ".size lbs_vm_call,.-lbs_vm_call\n .popsection\n");
int __bk_lbs_vm_available(void) { return bk_abi_version() >= 17; }
void *__bk_lbs_vm_map(void *address, unsigned int length, unsigned int protection,
                      unsigned int flags, int fd, unsigned int offset) {
    if (offset & 4095U) return 0;
    if (!address && (flags & 0x20U)) address = (void *)0x60000000U;
    int result = lbs_vm_call(55U, (unsigned int)address, length, protection,
                             flags, (unsigned int)fd, offset / 4096U);
    return result < 0 ? 0 : (void *)(unsigned int)result;
}
int __bk_lbs_vm_unmap(void *address, unsigned int length) {
    return lbs_vm_call(56U, (unsigned int)address, length, 0, 0, 0, 0);
}
int __bk_lbs_vm_protect(void *address, unsigned int length, unsigned int protection) {
    return lbs_vm_call(57U, (unsigned int)address, length, protection, 0, 0, 0);
}
