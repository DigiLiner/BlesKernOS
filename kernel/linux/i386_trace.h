#ifndef BLESK_LINUX_I386_TRACE_H
#define BLESK_LINUX_I386_TRACE_H
#include "../include/klock.h"

/* 0: ENOSYS con deduplicacion acotada; 1: errores; 2: todas; 3: apagado.
 * Los diagnósticos se activan explícitamente para pruebas. */
#ifndef BLESK_LINUX_TRACE
#define BLESK_LINUX_TRACE 3
#endif
static volatile uint32_t linux_trace_mode=BLESK_LINUX_TRACE;
static char linux_trace_process[TASK_NAME_LEN];
static char linux_trace_module[TASK_NAME_LEN];
static volatile uint32_t linux_trace_module_pid;
static kspinlock_t linux_trace_target_lock=KSPINLOCK_INITIALIZER;

void syscall_set_linux_trace(uint32_t level) {
    linux_trace_mode=level>2U ? 2U : level;
}

/* W3-55: trace=2 is useful for Wine only if COM1 is not flooded by every
 * loader/helper process.  Filter by the task name captured by syscall_handler
 * instead of by PID: Wine forks/execs make PIDs intentionally non-stable
 * between runs, while services.exe is the bootstrap milestone we need. */
void syscall_set_linux_trace_process(const char *name) {
    uint32_t i = 0U;
    linux_trace_process[0] = 0;
    if (!name) return;
    while (i + 1U < sizeof(linux_trace_process) && name[i]) {
        linux_trace_process[i] = name[i];
        ++i;
    }
    linux_trace_process[i] = 0;
}

/* Wine PE programs are loaded inside wine-preloader instead of becoming the
 * Linux task name.  W3-56 can therefore arm tracing when a distinctive Wine
 * module is mmap()ed, then follow that process id (and all of its threads). */
void syscall_set_linux_trace_module(const char *name) {
    uint32_t irq, i = 0U;
    irq = kspin_lock_irqsave(&linux_trace_target_lock);
    linux_trace_module[0] = 0;
    linux_trace_module_pid = 0U;
    if (name) {
        while (i + 1U < sizeof(linux_trace_module) && name[i]) {
            linux_trace_module[i] = name[i];
            ++i;
        }
        linux_trace_module[i] = 0;
    }
    kspin_unlock_irqrestore(&linux_trace_target_lock, irq);
}

static uint8_t linux_trace_ascii_lower(uint8_t c) {
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + ('a' - 'A')) : c;
}

static bool linux_trace_contains_ci(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) return false;
    for (; *text; ++text) {
        const char *a = text, *b = needle;
        while (*a && *b && linux_trace_ascii_lower((uint8_t)*a) ==
                           linux_trace_ascii_lower((uint8_t)*b)) {
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

void syscall_linux_trace_observe_filemap(uint32_t pid, const char *path) {
    uint32_t irq;
    char target[TASK_NAME_LEN];
    bool arm = false;
    uint32_t i = 0U;

    irq = kspin_lock_irqsave(&linux_trace_target_lock);
    while (i + 1U < sizeof(target) && linux_trace_module[i]) {
        target[i] = linux_trace_module[i];
        ++i;
    }
    target[i] = 0;
    if (target[0] && !linux_trace_module_pid &&
        linux_trace_contains_ci(path, target)) {
        linux_trace_module_pid = pid;
        arm = true;
    }
    kspin_unlock_irqrestore(&linux_trace_target_lock, irq);
    if (arm)
        kprintf("[LINUX:TRACE] module=%s armed pid=%u path=%s\n",
                target, pid, path ? path : "?");
}

static bool linux_trace_process_matches(uint32_t pid, const char *name) {
    const char *a = linux_trace_process;
    const char *b = name ? name : "";
    uint32_t module_pid = linux_trace_module_pid;
    if (linux_trace_module[0]) return module_pid && pid == module_pid;
    if (!a[0]) return true;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

static const char *linux_i386_syscall_name(uint32_t number) {
    static const struct {
        char unknown[8];
#define LINUX_SYSCALL(number, name) char n##number[sizeof(#name)];
#include "i386_syscalls.inc"
#undef LINUX_SYSCALL
    } names={"unknown",
#define LINUX_SYSCALL(number, name) #name,
#include "i386_syscalls.inc"
#undef LINUX_SYSCALL
    };
    static const uint16_t offsets[]={
#define LINUX_SYSCALL(number, name) [number]=__builtin_offsetof(__typeof__(names),n##number),
#include "i386_syscalls.inc"
#undef LINUX_SYSCALL
    };
    return (const char *)&names+(number<sizeof(offsets)/sizeof(offsets[0])?offsets[number]:0);
}

static void linux_i386_trace(const registers_t *input, int32_t result,
                             uint32_t pid, uint32_t tid, const char *name) {
    /* Ventana acotada: conserva la primera falta sin inundar COM1 si libc
     * reintenta. PID monotono; colisiones reemplazan, nunca ocultan otro PID. */
    static struct { uint32_t pid, number; } seen[8];
    static uint32_t cursor;
    static kspinlock_t lock=KSPINLOCK_INITIALIZER;
    uint32_t number = input->eax;
    if (linux_trace_mode > 2U) return;
    if (!linux_trace_process_matches(pid, name)) return;
    if (linux_trace_mode < 2 && result >= 0) return;
    if (linux_trace_mode == 0) {
        if (result != -BK_ENOSYS) return;
        uint32_t irq=kspin_lock_irqsave(&lock);
        for (uint32_t i=0; i<8U; ++i)
            if (seen[i].pid == pid && seen[i].number == number) {
                kspin_unlock_irqrestore(&lock,irq);return;
            }
        uint32_t slot = cursor++ % 8U;
        seen[slot].pid = pid;
        seen[slot].number = number;
        kspin_unlock_irqrestore(&lock,irq);
    }
    kprintf("[LINUX:SYSCALL] pid=%u tid=%u proc=%s nr=%u name=%s "
            "ebx=%x ecx=%x edx=%x esi=%x edi=%x ebp=%x eip=%x result=%d\n",
            pid, tid, name, number, linux_i386_syscall_name(number),
            input->ebx, input->ecx, input->edx, input->esi, input->edi,
            input->ebp, input->eip, result);
}
#endif
