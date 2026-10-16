#include "../../../kernel/include/syscall.h"

#define USER_VFS_O_RDONLY 0x0001U

static uint32_t text_len(const char *text) {
    uint32_t n = 0U;
    while (text && text[n]) n++;
    return n;
}

static void write_text(const char *text) {
    (void)syscall3(SYS_WRITE, 1U, (uint32_t)(uintptr_t)text, text_len(text));
}

/* This object is intentionally self-contained: no kernel symbol is linked
 * into USR mode. Every transition goes through the public SVC syscall ABI. */
void _start(void) {
    static const char hello[] =
        "[USER:ARM] USR mode + SVC ABI online\n";
    static const char file_prefix[] =
        "[USER:ARM] /README.TXT first bytes: ";
    static const char ok[] =
        "\n[USER:ARM] alloc + VFS + sleep + preemption OK; exiting status=0\n";
    static const char fail[] =
        "[USER:ARM] Stage 11 smoke FAILED\n";
    char buffer[96];
    int32_t fd, got;
    int32_t memory;

    write_text(hello);
    if (syscall0(SYS_ABI_VERSION) != SYSCALL_ABI_VERSION ||
        syscall0(SYS_GETPID) <= 0) {
        write_text(fail);
        (void)syscall1(SYS_EXIT, 101U);
    }

    memory = syscall1(SYS_ALLOC, 128U);
    if (memory <= 0 || syscall1(SYS_FREE, (uint32_t)memory) != 0) {
        write_text(fail);
        (void)syscall1(SYS_EXIT, 102U);
    }

    fd = syscall2(SYS_OPEN, (uint32_t)(uintptr_t)"/README.TXT",
                  USER_VFS_O_RDONLY);
    if (fd < 0) {
        write_text(fail);
        (void)syscall1(SYS_EXIT, 103U);
    }
    got = syscall3(SYS_READ, (uint32_t)fd,
                   (uint32_t)(uintptr_t)buffer, sizeof(buffer) - 1U);
    if (got < 0) {
        (void)syscall1(SYS_CLOSE, (uint32_t)fd);
        write_text(fail);
        (void)syscall1(SYS_EXIT, 104U);
    }
    buffer[(uint32_t)got] = '\0';
    write_text(file_prefix);
    (void)syscall3(SYS_WRITE, 1U, (uint32_t)(uintptr_t)buffer, (uint32_t)got);
    (void)syscall1(SYS_CLOSE, (uint32_t)fd);

    (void)syscall1(SYS_SLEEP, 20U);
    write_text(ok);
    (void)syscall1(SYS_EXIT, 0U);
    for (;;) (void)syscall0(SYS_YIELD);
}
