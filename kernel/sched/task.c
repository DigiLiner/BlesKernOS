#include "../include/file_dialog.h"
#include "../include/vision_resources.h"
#include "../include/vision_ipc.h"
#include "../include/gfx3d.h"
#include "../include/pager.h"
#include "../include/task.h"
#include "../include/memory.h"
#include "../include/pic.h"
#include "../include/pit.h"
#include "../include/gdt.h"
#include "../include/syscall.h"
#include "../include/vga.h"
#include "../include/elf_loader.h"
#include "../include/perfmon.h"
#include "../include/compat_mode.h"
#include "../include/smp.h"
#include "../include/klock.h"
#include "../include/kernel_domains.h"
#include "../include/paging.h"
#include "../include/smp_watchdog.h"
#include "../include/scheduler_smp.h"
#include "../include/usercopy.h"
#include "../include/linux_exec.h"
#include "../../gui/gui.h"

/* El detalle de cada DllMain es útil al depurar Wine, pero escribir tres
 * líneas por módulo en COM1 penaliza perceptiblemente la apertura normal. */
#define TASK_TRACE_DLLMAIN 0

/*
 * Scheduler tuning:
 *
 * TASK_QUANTUM_MILLISECONDS controls the real duration of a normal quantum.
 * Keeping it in milliseconds allows the Pentium II/III profile to lower PIT
 * and LAPIC timers to 100 Hz without turning a two-tick quantum into 20 ms.
 *
 * TASK_YIELD_BUDGET_PER_TICK controls how many voluntary yields may return
 * inside the same PIT tick before the task is throttled with HLT. This keeps
 * animation/app loops from going completely uncapped, but avoids the old
 * behavior where every single yield waited a whole hardware tick.
 *
 * You can override both from the Makefile with:
 *   -DTASK_QUANTUM_MILLISECONDS=10
 *   -DTASK_YIELD_BUDGET_PER_TICK=1
 */
#ifndef TASK_QUANTUM_MILLISECONDS
#define TASK_QUANTUM_MILLISECONDS 10U
#endif

#ifndef TASK_YIELD_BUDGET_PER_TICK
#define TASK_YIELD_BUDGET_PER_TICK 8U
#endif

static task_t tasks[TASK_MAX];
typedef struct {
    /* -1 means the private idle context for this CPU. Idle contexts are not
     * process-table entries and therefore do not consume TASK_MAX slots. */
    int current_slot;
    uint32_t preempt_nesting;
    uint32_t quantum_ticks;
    int rr_cursor; /* used only by the pre-SMP compatibility scheduler */
    int deferred_ready_slot; /* published after ESP has left the old stack */
    uint32_t watchdog_ticks;
} task_cpu_state_t;

static task_cpu_state_t cpu_states[SMP_MAX_CPUS];
static task_t *g_idle_tasks;
/* Kept out of task_t: the fixed table is part of the stringent 4 MiB profile.
 * Linux-only metadata is allocated after the memory manager is online. */
static uint32_t *g_linux_clear_child_tid;
static uint32_t next_pid;
static uint32_t sample_ticks;
static uint32_t sample_busy;
static uint8_t cpu_usage;
static uint32_t cpu_sample_ticks[SMP_MAX_CPUS];
static uint32_t cpu_sample_busy[SMP_MAX_CPUS];
static uint8_t cpu_usage_per_cpu[SMP_MAX_CPUS];
static uint32_t g_quantum_limit_ticks;
static uint32_t yield_tick[TASK_MAX];
static uint8_t yield_budget[TASK_MAX];
static uint8_t g_initial_fpu_state[TASK_FPU_STATE_SIZE] __attribute__((aligned(16)));
static kspinlock_t g_task_table_lock = KSPINLOCK_INITIALIZER;
/* A slot is reserved while its stacks are allocated outside the scheduler
 * spinlock. This keeps the hot scheduling lock short even for 128 KiB stacks. */
static uint8_t g_task_slot_reserved[TASK_MAX];
static uint8_t g_task_reap_claim[TASK_MAX];
/* task_get() historically returns a pointer. Return a per-CPU immutable copy
 * instead of exposing a live task-table slot to Process Manager readers. */
/* Consultas legacy: reservar despues de mm_init en paginas supervisor.
 * El array fijo consumia 36 KiB de la BSS limitada del perfil de 4 MiB. */
static task_t *g_task_query_snapshot;

/* Completed joinable threads are copied here before their task slot is
 * recycled. This prevents the asynchronous reaper from racing a later
 * bk_thread_join() while keeping zombie stacks out of TASK_MAX. */
typedef struct {
    uint32_t tid;
    uint32_t process_id;
    int32_t result;
    uint32_t sequence;
    bool valid;
} task_thread_completion_t;

static task_thread_completion_t g_thread_completions[TASK_MAX];
static uint32_t g_thread_completion_sequence;

/* Process children can finish between the caller observing TASK_ZOMBIE and
 * task_waitpid() claiming the slot.  The asynchronous GUI reaper must not
 * erase their exit status: synchronous BEX/PE launchers depend on it. */
typedef struct {
    uint32_t pid;
    uint32_t process_id;
    uint32_t parent_pid;
    int32_t result;
    uint32_t sequence;
    bool valid;
} task_process_completion_t;

static task_process_completion_t g_process_completions[TASK_MAX];
static uint32_t g_process_completion_sequence;

#define TASK_STACK_GUARD_BYTE 0xA5U
/* Leave enough room for the timer/exception frame and the containment path.
 * If a user API drives ESP below this reserve, discard only that process
 * before the stack reaches heap metadata or another allocation. */
#define TASK_KERNEL_STACK_EMERGENCY_RESERVE 8192U
#define TASK_STACK_GUARD_PROBE_BYTES 32U

/* Small CPL3-only gates. Publish only their dedicated page to userspace;
 * ordinary kernel .text and .bss remain supervisor-only. */
#define TASK_USER_GATE_SIZE 32U
/* Dedicated immutable code page: heap-root metadata remains supervisor-only
 * in private CR3s. The gates must never share that guarded allocator page. */
static uint8_t g_user_gate_page[PAGING_PAGE_SIZE] __attribute__((aligned(PAGING_PAGE_SIZE)));
static uint8_t *g_user_process_exit_gate;
static uint8_t *g_user_upcall_return_gate;

/* A large part of the historical public ABI returns const char pointers.
 * Those pointers used to refer directly to kernel .rodata/.bss, which is no
 * longer user-accessible after Phase 1. Cache mirrors by source identity so
 * stable strings remain stable without allocating on every frame. */
#define TASK_USER_STRING_EXPORT_LIMIT 64U
typedef struct task_user_string_export {
    const char *source;
    char *copy;
    uint32_t capacity;
    struct task_user_string_export *next;
} task_user_string_export_t;

static task_cpu_state_t *task_cpu_local(void) {
    uint32_t cpu = smp_cpu_index();
    if (cpu >= SMP_MAX_CPUS) cpu = 0U;
    return &cpu_states[cpu];
}

#define current_index        (task_cpu_local()->current_slot)
#define preempt_depth        (task_cpu_local()->preempt_nesting)
#define current_quantum      (task_cpu_local()->quantum_ticks)
#define round_robin_cursor   (task_cpu_local()->rr_cursor)

static task_t *task_idle_for_cpu(uint32_t cpu) {
    if (!g_idle_tasks || cpu >= SMP_MAX_CPUS) return NULL;
    return &g_idle_tasks[cpu];
}

static task_t *task_from_slot_cpu(int slot, uint32_t cpu) {
    if (slot >= 0 && slot < TASK_MAX) return &tasks[slot];
    return task_idle_for_cpu(cpu);
}

static task_t *task_current_entry(void) {
    uint32_t cpu = smp_cpu_index();
    if (cpu >= SMP_MAX_CPUS) cpu = 0U;
    return task_from_slot_cpu(cpu_states[cpu].current_slot, cpu);
}

#define X86_EFLAGS_IF 0x200U

/*
 * Context-switch handoff rule
 * ---------------------------
 *
 * task_schedule() is called only from an interrupt/trap frame.  Once it has
 * changed cpu_states[cpu].current_slot, the CPU must not accept another
 * interrupt until the assembly stub has installed the returned stack pointer.
 *
 * This matters especially for int 0x80: it is a trap gate, so IF is normally
 * still set.  Restoring the scheduler spinlock's saved IF before the caller
 * executes `mov esp, eax` creates a short but fatal split-brain window:
 * current_slot already names task B while ESP still points into task A's
 * kernel stack.  A LAPIC tick in that window stores B's context on A's stack,
 * and the later syscall IRET tries to load a random CS (observed as #GP with
 * selectors such as 0x009C and 0x3004).
 *
 * Release the table lock while keeping local IRQs masked.  IRET restores IF
 * from the selected task's saved EFLAGS after ESP, CS and SS are coherent.
 */
static void task_scheduler_unlock_for_iret(uint32_t scheduler_flags) {
    kspin_unlock_irqrestore(&g_task_table_lock,
                            scheduler_flags & ~X86_EFLAGS_IF);
}


/* Native Ring-3 programs use the flat data selector as part of their ABI.
 * Normalize frames before they are persisted: otherwise a frame captured
 * during an API/upcall transition can preserve null selectors and replay
 * them on every later context switch. */
static void task_normalize_user_segments(task_t *task, registers_t *regs) {
    if (!task || !regs || !task->user || task->linux_abi ||
        (regs->cs & 3U) != 3U)
        return;
    regs->eflags &= ~0x3000U; /* never restore user IOPL */
    regs->ds = GDT_USER_DATA;
    regs->es = GDT_USER_DATA;
    regs->gs = task->linux_abi && task->user_fs_base
        ? GDT_USER_FS : GDT_USER_DATA;
    regs->fs = task->user_fs_base ? GDT_USER_FS : GDT_USER_DATA;
}

/* BLES_TASK_FX_CONTEXT_20260902
 * The Pentium-III baseline allows Mesa/Tux to use SSE, therefore every task
 * switch and asynchronous Ring-3 upcall must preserve XMM/MXCSR as well as
 * x87 state.  Prefer FXSAVE/FXRSTOR when CPUID advertises FXSR and keep the
 * old FSAVE path as a defensive fallback for diagnostic/legacy boots. */
static bool g_fpu_fxsr;
static bool g_fpu_sse;

static bool task_fpu_cpu_features(uint32_t *features_edx) {
    uint32_t before, after, toggled;
    uint32_t eax = 0U, ebx, ecx, edx;

    __asm__ volatile ("pushfl; popl %0" : "=r"(before));
    toggled = before ^ (1U << 21);
    __asm__ volatile ("pushl %0; popfl" : : "r"(toggled) : "cc", "memory");
    __asm__ volatile ("pushfl; popl %0" : "=r"(after));
    __asm__ volatile ("pushl %0; popfl" : : "r"(before) : "cc", "memory");
    if (((before ^ after) & (1U << 21)) == 0U) return false;

    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (eax < 1U) return false;
    eax = 1U;
    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (features_edx) *features_edx = edx;
    return true;
}

static void task_fpu_prepare_cpu(void) {
    uint32_t cr0;
    uint32_t cr4;
    uint32_t features = 0U;

    g_fpu_fxsr = task_fpu_cpu_features(&features) &&
                 (features & (1U << 24)) != 0U;
    g_fpu_sse = g_fpu_fxsr && (features & (1U << 25)) != 0U;

    __asm__ volatile ("movl %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1U << 2) | (1U << 3)); /* clear EM and TS */
    cr0 |= (1U << 1) | (1U << 5);    /* MP + NE */
    __asm__ volatile ("movl %0, %%cr0" : : "r"(cr0) : "memory");

    if (g_fpu_fxsr) {
        __asm__ volatile ("movl %%cr4, %0" : "=r"(cr4));
        cr4 |= (1U << 9);             /* OSFXSR: FXSAVE + SSE state */
        if (g_fpu_sse) cr4 |= (1U << 10); /* OSXMMEXCPT */
        __asm__ volatile ("movl %0, %%cr4" : : "r"(cr4) : "memory");
    }
    __asm__ volatile ("fninit" : : : "memory");
}

static void task_fpu_build_initial_state(void) {
    kmemset(g_initial_fpu_state, 0, sizeof(g_initial_fpu_state));
    __asm__ volatile ("fninit" : : : "memory");
    if (g_fpu_fxsr) {
        uint32_t mxcsr = 0x1F80U;
        __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr) : "memory");
        __asm__ volatile ("fxsave (%0)" : : "r"(g_initial_fpu_state) : "memory");
    } else {
        __asm__ volatile ("fnsave (%0); fwait; frstor (%0)"
                          : : "r"(g_initial_fpu_state) : "memory");
    }
    kprintf("[TASK] FPU context=%s SSE=%u\n",
            g_fpu_fxsr ? "FXSAVE" : "FSAVE", g_fpu_sse ? 1U : 0U);
}

static bool task_fpu_alloc_buffer(void **allocation_out, uint8_t **state_out) {
    uint8_t *raw;
    uintptr_t aligned;
    if (allocation_out) *allocation_out = NULL;
    if (state_out) *state_out = NULL;
    raw = (uint8_t *)kmalloc(TASK_FPU_STATE_SIZE + 15U);
    if (!raw) return false;
    (void)mm_set_allocation_owner(raw, 0U);
    aligned = ((uintptr_t)raw + 15U) & ~(uintptr_t)15U;
    if (allocation_out) *allocation_out = raw;
    if (state_out) *state_out = (uint8_t *)aligned;
    return true;
}

static bool task_fpu_assign_initial(task_t *task) {
    if (!task) return false;
    if (!task->fpu_state &&
        !task_fpu_alloc_buffer(&task->fpu_allocation, &task->fpu_state))
        return false;
    if (!task->upcall_fpu_state &&
        !task_fpu_alloc_buffer(&task->upcall_fpu_allocation,
                               &task->upcall_fpu_state)) {
        kfree(task->fpu_allocation);
        task->fpu_allocation = NULL;
        task->fpu_state = NULL;
        return false;
    }
    kmemcpy(task->fpu_state, g_initial_fpu_state, TASK_FPU_STATE_SIZE);
    kmemcpy(task->upcall_fpu_state, g_initial_fpu_state, TASK_FPU_STATE_SIZE);
    task->fpu_state_valid = true;
    return true;
}

static void task_fpu_release(task_t *task) {
    if (!task) return;
    if (task->fpu_allocation) kfree(task->fpu_allocation);
    if (task->upcall_fpu_allocation) kfree(task->upcall_fpu_allocation);
    task->fpu_allocation = NULL;
    task->fpu_state = NULL;
    task->upcall_fpu_allocation = NULL;
    task->upcall_fpu_state = NULL;
    task->fpu_state_valid = false;
    task->upcall_fpu_saved = false;
}

static void task_fpu_save(task_t *task) {
    if (!task) return;
    if (g_fpu_fxsr)
        __asm__ volatile ("fxsave (%0)" : : "r"(task->fpu_state) : "memory");
    else
        __asm__ volatile ("fnsave (%0); fwait"
                          : : "r"(task->fpu_state) : "memory");
    task->fpu_state_valid = true;
}

static void task_fpu_restore(task_t *task) {
    if (!task) return;
    if (!task->fpu_state_valid && !task_fpu_assign_initial(task)) {
        __asm__ volatile ("fninit" : : : "memory");
        return;
    }
    if (g_fpu_fxsr)
        __asm__ volatile ("fxrstor (%0)" : : "r"(task->fpu_state) : "memory");
    else
        __asm__ volatile ("frstor (%0)" : : "r"(task->fpu_state) : "memory");
}

/* Ring-3 GUI callbacks are asynchronous. Give the upcall a clean floating
 * point/SIMD image and restore the interrupted task byte-for-byte afterwards. */
static void task_fpu_begin_upcall(task_t *task) {
    if (!task || task->upcall_fpu_saved) return;
    if (g_fpu_fxsr)
        __asm__ volatile ("fxsave (%0)" : : "r"(task->upcall_fpu_state) : "memory");
    else
        __asm__ volatile ("fnsave (%0); fwait"
                          : : "r"(task->upcall_fpu_state) : "memory");
    task->upcall_fpu_saved = true;
    if (g_fpu_fxsr)
        __asm__ volatile ("fxrstor (%0)" : : "r"(g_initial_fpu_state) : "memory");
    else
        __asm__ volatile ("frstor (%0)" : : "r"(g_initial_fpu_state) : "memory");
}

static void task_fpu_end_upcall(task_t *task) {
    if (!task || !task->upcall_fpu_saved) return;
    if (g_fpu_fxsr)
        __asm__ volatile ("fxrstor (%0)" : : "r"(task->upcall_fpu_state) : "memory");
    else
        __asm__ volatile ("fninit; frstor (%0)"
                          : : "r"(task->upcall_fpu_state) : "memory");
    task->upcall_fpu_saved = false;
}

static uint32_t *task_stack_allocate(uint32_t requested_size, bool user_stack,
                                     void **allocation_out,
                                     uint32_t *actual_size_out) {
    uint32_t actual_size=0;
    uint32_t *stack=(uint32_t *)mm_alloc_guarded_stack(requested_size,user_stack,
                                                     allocation_out,&actual_size);
    /* Stack profiling is opt-in; normal launches need no second full fill. */
#ifdef TASK_STACK_WATERMARK
    if(stack && !user_stack)kmemset(stack,0xA5,actual_size);
#endif
    if(actual_size_out)*actual_size_out=actual_size;
    return stack;
}

static bool task_stack_guard_quick_ok(const task_t *task) {
    uint32_t lower;
    uint32_t upper;
    if (!task || !task->stack || !task->stack_size) return true;
    lower = (uint32_t)(uintptr_t)task->stack - PAGING_PAGE_SIZE;
    upper = (uint32_t)(uintptr_t)task->stack + task->stack_size;
    return !paging_page_present(lower) && !paging_page_present(upper);
}

static uint32_t task_frame_stack_remaining(const task_t *task,
                                           const registers_t *frame) {
    uintptr_t low, high, position;
    if (!task || !task->stack || !task->stack_size || !frame)
        return 0xFFFFFFFFU;
    low = (uintptr_t)task->stack;
    high = low + task->stack_size;
    position = (uintptr_t)frame;
    if (position < low || position > high) return 0U;
    return (uint32_t)(position - low);
}

static void task_stack_release(task_t *task, void *allocation,
                               uint32_t *stack, uint32_t stack_size,
                               const char *kind) {
    uint32_t lower;
    uint32_t upper;
    if (!stack) return;
    lower = (uint32_t)(uintptr_t)stack - PAGING_PAGE_SIZE;
    upper = (uint32_t)(uintptr_t)stack + stack_size;
    if (paging_page_present(lower) || paging_page_present(upper))
        kprintf("[TASK] guard de pagina %s alterado pid=%u proceso=%u nombre=%s\n",
                kind, task ? task->pid : 0U,
                task ? task->process_id : 0U,
                task ? task->name : "?");
    mm_free_guarded_stack(allocation, stack, stack_size);
}

static bool task_process_can_release(uint32_t process_id) {
    bool can_release = true;
    uint32_t flags;
    if (!process_id) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].process_id != process_id) continue;
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING ||
            tasks[i].state == TASK_SLEEPING ||
            (tasks[i].state == TASK_ZOMBIE && !tasks[i].resources_released)) {
            can_release = false;
            break;
        }
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return can_release;
}

static void task_process_record_completion(const task_t *task) {
    uint32_t flags;
    uint32_t chosen = TASK_MAX;
    uint32_t oldest = 0xFFFFFFFFU;
    if (!task || task->thread || !task->pid || !task->parent_pid) return;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (uint32_t i = 0U; i < TASK_MAX; i++) {
        if (g_process_completions[i].valid &&
            g_process_completions[i].pid == task->pid &&
            g_process_completions[i].parent_pid == task->parent_pid) {
            chosen = i;
            break;
        }
        if (!g_process_completions[i].valid) {
            chosen = i;
            break;
        }
        if (g_process_completions[i].sequence < oldest) {
            oldest = g_process_completions[i].sequence;
            chosen = i;
        }
    }
    if (chosen < TASK_MAX) {
        task_process_completion_t *completion = &g_process_completions[chosen];
        completion->pid = task->pid;
        completion->process_id = task->process_id;
        completion->parent_pid = task->parent_pid;
        completion->result = task->exit_code;
        completion->sequence = ++g_process_completion_sequence;
        completion->valid = true;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
}

static void task_thread_record_completion(const task_t *task) {
    uint32_t flags;
    uint32_t chosen = TASK_MAX;
    uint32_t oldest = 0xFFFFFFFFU;
    if (!task || !task->thread || !task->thread_joinable) return;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (uint32_t i = 0U; i < TASK_MAX; i++) {
        if (g_thread_completions[i].valid &&
            g_thread_completions[i].tid == task->pid) {
            chosen = i;
            break;
        }
        if (!g_thread_completions[i].valid) {
            chosen = i;
            break;
        }
        if (g_thread_completions[i].sequence < oldest) {
            oldest = g_thread_completions[i].sequence;
            chosen = i;
        }
    }
    if (chosen < TASK_MAX) {
        task_thread_completion_t *completion = &g_thread_completions[chosen];
        completion->tid = task->pid;
        completion->process_id = task->process_id;
        completion->result = task->exit_code;
        completion->sequence = ++g_thread_completion_sequence;
        completion->valid = true;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
}

static void task_release_upcall_buffers(task_t *task) {
    gui_window_t *windows[TASK_UPCALL_QUEUE + 1U];
    bool paints[TASK_UPCALL_QUEUE + 1U];
    void *payloads[TASK_UPCALL_QUEUE + 1U];
    uint32_t count=0, payload_count=0;
    uint8_t index;
    if (!task || (!task->upcall_active && task->upcall_tail==task->upcall_head)) return;
    /* The dying thread cannot return through its callback gate. Retire its
     * references before GUI destruction, including windows already unlinked
     * by the application. Otherwise an active paint keeps the staging slot
     * borrowed forever (especially visible with the single low-RAM slot). */
    gui_desktop_paint_lock();
    uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
    index=task->upcall_tail;
    for (uint32_t n=0;n<TASK_UPCALL_QUEUE+1U;n++) {
        task_upcall_t *call;
        if (!n) {
            if (!task->upcall_active) continue;
            call=&task->active_upcall;
        } else {
            if (index==task->upcall_head) break;
            call=&task->upcalls[index];
            index=(uint8_t)((index+1U)%TASK_UPCALL_QUEUE);
        }
        if(call->release_after_upcall) {
            payloads[payload_count++]=call->release_after_upcall;
            call->release_after_upcall=NULL;
        }
        gui_window_t *window=(gui_window_t *)call->window_target;
        if(window) {
            uint32_t at=0;
            while(at<count && windows[at]!=window) at++;
            if(at==count) {windows[count]=window;paints[count++]=false;}
            if(call->payload_argument==-2) paints[at]=true;
        }
    }
    task->upcall_active=false;
    task->upcall_head=task->upcall_tail=0;
    kmemset(&task->active_upcall,0,sizeof(task->active_upcall));
    kmemset(task->upcalls,0,sizeof(task->upcalls));
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    for(uint32_t i=0;i<payload_count;i++) kfree(payloads[i]);
    for(uint32_t i=0;i<count;i++) {
        gui_window_t *window=windows[i];
        if(paints[i]) {
            window->content_pending=false;
            window->content_repaint=false;
            gui_window_end_content_paint(window);
        }
        if(window->destroy_state==1U) gui_window_destroy(window);
    }
    gui_desktop_paint_unlock();
}

#include "../linux/i386_signal.inc"
#include "../linux/i386_futex.inc"

static const char *task_debug_state_name(task_state_t state) {
    switch (state) {
        case TASK_UNUSED: return "UNUSED";
        case TASK_READY: return "READY";
        case TASK_RUNNING: return "RUNNING";
        case TASK_SLEEPING: return "SLEEPING";
        case TASK_STARTING: return "STARTING";
        case TASK_ZOMBIE: return "ZOMBIE";
        default: return "?";
    }
}

typedef struct {
    bool valid;
    int slot;
    uint32_t pid;
    uint32_t process;
    uint32_t parent;
    uint32_t lender;
    task_state_t state;
    int32_t running;
    int32_t queued;
    int32_t affinity_cpu;
    uint32_t affinity_mask;
    uint32_t wake;
    uint32_t ticks;
    bool exit_requested;
    bool thread;
    bool linux_abi;
    uint32_t eip;
    uint32_t esp;
    uint32_t eax;
    uint32_t context_address;
    uint32_t stack_address;
    uint32_t stack_size;
    uint32_t cs;
    uint32_t futex_key;
    uint32_t futex_process;
    uint32_t futex_mask;
    uint32_t clear_tid;
    char name[TASK_NAME_LEN];
} task_debug_snapshot_t;

static task_debug_snapshot_t g_task_debug_snapshot[TASK_MAX];
static int16_t g_task_debug_rq[SMP_MAX_CPUS][TASK_MAX];
static uint32_t g_task_debug_rq_count[SMP_MAX_CPUS];
static int g_task_debug_current[SMP_MAX_CPUS];
static int g_task_debug_deferred[SMP_MAX_CPUS];

void task_debug_dump_all(const char *reason) {
    uint32_t now;
    uint32_t cpu_count;
    uint32_t irq_flags;

    /* W3-50: W3-49 copied one task, re-enabled IRQs, printed it, then moved
     * to the next task.  The timer therefore scheduled Wine in the middle of
     * the dump and produced a timeline collage instead of one scheduler
     * state.  Freeze this diagnostic on the local CPU until every task and
     * runqueue has been copied (and keep IRQs masked while printing so serial
     * lines cannot be spliced by syscall tracing).  Gate-A runs this probe
     * with one CPU, so this is a coherent system-wide snapshot for the Wine
     * reproducer without changing normal scheduler behaviour. */
    __asm__ volatile ("pushfl; popl %0; cli"
                      : "=r"(irq_flags) : : "memory", "cc");
    now = pit_get_ticks();
    cpu_count = smp_cpu_count();
    if (!cpu_count) cpu_count = 1U;
    if (cpu_count > SMP_MAX_CPUS) cpu_count = SMP_MAX_CPUS;

    kmemset(g_task_debug_snapshot, 0, sizeof(g_task_debug_snapshot));
    kmemset(g_task_debug_rq_count, 0, sizeof(g_task_debug_rq_count));
    for (uint32_t cpu = 0U; cpu < cpu_count; cpu++) {
        g_task_debug_current[cpu] = cpu_states[cpu].current_slot;
        g_task_debug_deferred[cpu] = cpu_states[cpu].deferred_ready_slot;
        g_task_debug_rq_count[cpu] = scheduler_smp_debug_copy_queue(
            cpu, g_task_debug_rq[cpu], TASK_MAX);
    }

    for (int i = 1; i < TASK_MAX; i++) {
        task_t *t = &tasks[i];
        task_debug_snapshot_t *d = &g_task_debug_snapshot[i];
        if (t->state == TASK_UNUSED) continue;
        d->valid = true;
        d->slot = i;
        d->pid = t->pid;
        d->process = t->process_id;
        d->parent = t->parent_pid;
        d->lender = t->linux_vm_lender;
        d->state = t->state;
        d->running = t->running_cpu;
        d->queued = t->queued_cpu;
        d->affinity_cpu = t->affinity_cpu;
        d->affinity_mask = t->affinity_mask;
        d->wake = t->wake_tick;
        d->ticks = t->cpu_ticks;
        d->exit_requested = t->exit_requested;
        d->thread = t->thread;
        d->linux_abi = t->linux_abi;
        d->eip = t->context ? t->context->eip : 0U;
        d->esp = t->context ? t->context->useresp : 0U;
        d->eax = t->context ? t->context->eax : 0U;
        d->context_address = (uint32_t)(uintptr_t)t->context;
        d->stack_address = (uint32_t)(uintptr_t)t->stack;
        d->stack_size = t->stack_size;
        d->cs = t->context ? t->context->cs : 0U;
        d->futex_key = t->linux_signals ? t->linux_signals->futex_key : 0U;
        d->futex_process = t->linux_signals ? t->linux_signals->futex_process : 0U;
        d->futex_mask = t->linux_signals ? t->linux_signals->futex_mask : 0U;
        d->clear_tid = (g_linux_clear_child_tid && i < TASK_MAX)
                     ? g_linux_clear_child_tid[i] : 0U;
        kstrncpy(d->name, t->name, sizeof(d->name));
        d->name[sizeof(d->name)-1U] = '\0';
    }

    kprintf("[TASK:DUMP2] begin reason=%s tick=%u cpus=%u\n",
            reason ? reason : "?", now, cpu_count);
    for (uint32_t cpu = 0U; cpu < cpu_count; cpu++) {
        kprintf("[TASK:DUMP2] cpu=%u current_slot=%d deferred=%d rq_count=%u rq=",
                cpu, g_task_debug_current[cpu], g_task_debug_deferred[cpu],
                g_task_debug_rq_count[cpu]);
        for (uint32_t q = 0U; q < g_task_debug_rq_count[cpu]; q++)
            kprintf("%s%d", q ? "," : "", (int)g_task_debug_rq[cpu][q]);
        kprintf("\n");
    }

    for (int i = 1; i < TASK_MAX; i++) {
        task_debug_snapshot_t *d = &g_task_debug_snapshot[i];
        if (!d->valid) continue;
        kprintf("[TASK:DUMP2] slot=%d pid=%u proc=%u ppid=%u name=%s state=%s(%u) run=%d queue=%d affinity=%d mask=%x ticks=%u wake=%u delta=%d exit=%u thread=%u linux=%u lender=%u eip=%x esp=%x eax=%x ctx=%x stack=%x size=%x cs=%x futex=%x fproc=%u fmask=%x clear_tid=%x\n",
                d->slot, d->pid, d->process, d->parent, d->name,
                task_debug_state_name(d->state), (uint32_t)d->state,
                d->running, d->queued, d->affinity_cpu, d->affinity_mask,
                d->ticks, d->wake, (int32_t)(d->wake-now),
                d->exit_requested ? 1U : 0U, d->thread ? 1U : 0U,
                d->linux_abi ? 1U : 0U, d->lender, d->eip, d->esp, d->eax,
                d->context_address, d->stack_address, d->stack_size, d->cs,
                d->futex_key, d->futex_process, d->futex_mask, d->clear_tid);
    }
    kprintf("[TASK:DUMP2] end\n");
    __asm__ volatile ("pushl %0; popfl" : : "r"(irq_flags) : "memory", "cc");
}

static void task_release_resources(task_t *task) {
    uint32_t process_id;
    size_t released;

    if (!task || task->resources_released) return;
    /* Keep the lender's heap, images and stacks until every borrowed CR3 is
     * detached. Check before releasing ANY process-owned storage. */
    if(paging_process_has_borrowers(task->process_id))return;
    bool last=true;
    uint32_t release_flags=kspin_lock_irqsave(&g_task_table_lock);
    for(int i=0;i<TASK_MAX;i++)
        if(&tasks[i]!=task && tasks[i].process_id==task->process_id &&
           tasks[i].state!=TASK_UNUSED && !tasks[i].resources_released){last=false;break;}
    kspin_unlock_irqrestore(&g_task_table_lock,release_flags);
    if(last && !paging_process_retire(task->process_id))return;
    if(task->pid==gui_vision_owner_pid()){vision_ipc_server_offline(task->pid);vision_file_dialog_offline(task->pid);vision_resources_recover(gui_get_desktop());}
    gui_vision_abort_frame(task->pid);
    kernel_domains_abandon_owner(task->pid);
    task_release_upcall_buffers(task);
    task_fpu_release(task);
    linux_signal_release(task);
    process_id = task->process_id;
    /* A TID and its process ID are equal for the main thread. Historically
     * that made the reaper destroy process-owned windows as soon as the main
     * entry returned, even when a worker thread was still alive and about to
     * display the application. Tear down only callbacks owned by this TID;
     * process-owned windows are released once the final sibling exits. */
    gui_desktop_cleanup_thread(gui_get_desktop(), task->pid);
    task_stack_release(task, task->stack_allocation, task->stack,
                       task->stack_size, "kernel");
    task_stack_release(task, task->user_stack_allocation, task->user_stack,
                       task->user_stack_size, "usuario");
    if(task->linux_stack_pin) {
        mm_unpin_address_allocation(task->linux_stack_pin);task->linux_stack_pin=NULL;
    }
    if (task->launch_arg) {
        kfree(task->launch_arg);
        task->launch_arg = NULL;
    }
    if (task->cwd) {
        kfree(task->cwd);
        task->cwd = NULL;
    }
    {
        uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
        task->stack = NULL;
        task->user_stack = NULL;
        task->stack_allocation = NULL;
        task->user_stack_allocation = NULL;
        task->context = NULL;
        task->resources_released = true;
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
    }
    if (!task_process_can_release(process_id)) return;
    gui_vision_process_cleanup(process_id);
    gui_desktop_cleanup_process(gui_get_desktop(), process_id);
    /* BLES_WINE_REAPER_CLEANUP_CALL_20260723
     * El entrypoint puede retornar o el task puede morir sin llamar
     * ExitProcess. Libere primero la imagen PE y su vista fija; además esto
     * consume el handoff pendiente antes de que mm_release_process() borre
     * las allocations restantes del proceso. */
    vision_file_dialog_process_cleanup(process_id);
    vision_ipc_process_cleanup(process_id);
    gfx3d_process_cleanup(process_id);
    syscall_process_cleanup(process_id);
    elf_process_cleanup(process_id);
    paging_process_destroy(process_id);
#ifdef BLESK_WINE_VERBOSE_TRACE
    if(task->linux_vm_lender) kprintf("[VFORK] release child=%u lender=%u\n",process_id,task->linux_vm_lender);
#endif
    task->linux_vm_lender=0;
    released = mm_release_process(process_id);
    if (released)
        kprintf("[MM] proceso %u: %u KiB recuperados\n", process_id,
                (uint32_t)((released + 1023U) / 1024U));
}

static void task_reap_claimed_index(int index) {
    task_t *task;
    uint32_t flags;
    if (index <= 0 || index >= TASK_MAX) return;
    task = &tasks[index];
    scheduler_smp_remove(index);

    task_thread_record_completion(task);
    kernel_domains_enter(KDOMAIN_VFS | KDOMAIN_GUI | KDOMAIN_GFX | KDOMAIN_WINE | KDOMAIN_LEGACY);
    task_release_resources(task);
    kernel_domains_exit(KDOMAIN_VFS | KDOMAIN_GUI | KDOMAIN_GFX | KDOMAIN_WINE | KDOMAIN_LEGACY);

    flags = kspin_lock_irqsave(&g_task_table_lock);
    if (tasks[index].state == TASK_ZOMBIE &&
        tasks[index].resources_released) {
        kmemset(&tasks[index], 0, sizeof(tasks[index]));
        g_task_slot_reserved[index] = 0U;
    }
    g_task_reap_claim[index] = 0U;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
}

/* Resource destruction may call GUI, Wine and heap code and therefore must
 * never run under the scheduler spinlock. Claim a zombie briefly, release the
 * table, perform the slow cleanup in task context, then recycle the slot. */
void task_reap_deferred(void) {
    /* A launch burst can leave several short-lived ELF entry tasks waiting
     * behind the GUI loop. Reaping only one per frame made the 32-slot table
     * appear exhausted even though most entries were already zombies. Keep
     * the batch deliberately small so cleanup cannot monopolize CPU0. */
    for (uint32_t pass = 0U; pass < 4U; pass++) {
        int index = -1;
        uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
        for (int i = 1; i < TASK_MAX; i++) {
            if (tasks[i].state != TASK_ZOMBIE || tasks[i].running_cpu != -1 ||
                tasks[i].queued_cpu != -1 || tasks[i].resources_released ||
                g_task_reap_claim[i] ||
                paging_process_has_borrowers(tasks[i].process_id)) continue;
            g_task_reap_claim[i] = 1U;
            index = i;
            break;
        }
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        if (index < 0) break;
        /* Preserve waitpid semantics even if this background reaper wins the
         * race against a synchronous parent. */
        task_process_record_completion(&tasks[index]);
        task_reap_claimed_index(index);
    }
}

static bool task_running_in_user_cpl(void) {
    uint16_t cs;

    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return (cs & 0x3U) == 0x3U;
}

static void task_copy_launch_arg(task_t *task, const char *launch_arg) {
    uint32_t length = 0U;
    if (!task || !launch_arg || !launch_arg[0]) return;
    while (launch_arg[length] && length + 1U < TASK_LAUNCH_ARG_LEN) length++;
    task->launch_arg = (char *)kmalloc(length + 1U);
    if (!task->launch_arg) return;
    kmemcpy(task->launch_arg, launch_arg, length);
    task->launch_arg[length] = '\0';
    /* task_create_internal() allocates this while the parent is current.
     * Keep the launch string attached to the child process just like cwd;
     * otherwise an early parent teardown can reclaim it underneath the child. */
    if (!mm_set_allocation_owner(task->launch_arg, task->process_id)) {
        kfree(task->launch_arg);
        task->launch_arg = NULL;
    }
}

static int task_pick_next_up(void) {
    int next = -1;

    /* Preserve the proven uniprocessor scheduler until SMP is explicitly
     * released. Idle is represented by slot -1, outside TASK_MAX. */
    if (current_index != 0 && tasks[0].state == TASK_READY)
        return 0;

    for (int checked = 0; checked < TASK_MAX; checked++) {
        int candidate = (round_robin_cursor + checked + 1) % TASK_MAX;
        if (tasks[candidate].state == TASK_READY) {
            next = candidate;
            break;
        }
    }

    if (next >= 0) {
        round_robin_cursor = next;
        return next;
    }
    return -1;
}

static void task_name_idle_cpu(char *name, uint32_t cpu);

static void idle_main(void *argument UNUSED) {
    for (;;) __asm__ volatile ("hlt");
}

static void task_bootstrap(void) {
    task_t *task = task_current_entry();
    sti();
    if (task && task->entry) task->entry(task->argument);
    task_exit();
}

static void task_user_bootstrap(void) {
    task_t *task = &tasks[current_index];

    if (task->entry) task->entry(task->argument);
    (void)syscall1(SYS_EXIT, 0);
    for (;;) (void)syscall0(SYS_YIELD);
}

static bool task_idle_initialize(uint32_t cpu, void *stack_allocation,
                                 uint32_t *stack, uint32_t stack_size) {
    task_t *idle;
    if (!g_idle_tasks || cpu >= SMP_MAX_CPUS || !stack ||
        stack_size < 4096U) return false;
    idle = &g_idle_tasks[cpu];
    kmemset(idle, 0, sizeof(*idle));
    idle->running_cpu = -1;
    idle->queued_cpu = -1;
    idle->affinity_cpu = (int8_t)cpu;
    idle->affinity_mask = cpu < 32U ? (1U << cpu) : 0U;
    idle->preferred_cpu = (uint8_t)cpu;
    idle->last_cpu = (uint8_t)cpu;
    idle->stack = stack;
    idle->stack_allocation = stack_allocation;
    idle->stack_size = stack_size;
    idle->memory_bytes = stack_size;
    idle->idle = true;
    idle->system = true;
    idle->state = TASK_READY;
    task_name_idle_cpu(idle->name, cpu);
    idle->entry = idle_main;
    idle->context = (registers_t *)((uint8_t *)stack + stack_size -
                                    sizeof(registers_t));
    kmemset(idle->context, 0, sizeof(registers_t));
    idle->context->ds = GDT_KERNEL_DATA;
    idle->context->es = GDT_KERNEL_DATA;
    idle->context->fs = GDT_KERNEL_DATA;
    idle->context->gs = GDT_KERNEL_DATA;
    idle->context->int_no = 32U;
    idle->context->eip = (uint32_t)(uintptr_t)task_bootstrap;
    idle->context->cs = GDT_KERNEL_CODE;
    idle->context->eflags = 0x202U;
    idle->context->useresp = (uint32_t)(uintptr_t)task_exit;
    if (!task_fpu_assign_initial(idle)) return false;
    return true;
}

static bool task_user_gates_init(void) {
    uint8_t *exit_gate;
    uint8_t *upcall_gate;

    if (g_user_process_exit_gate && g_user_upcall_return_gate) return true;
    exit_gate = g_user_gate_page;
    upcall_gate = g_user_gate_page + TASK_USER_GATE_SIZE;
    kmemset(exit_gate, 0x90, TASK_USER_GATE_SIZE);
    kmemset(upcall_gate, 0x90, TASK_USER_GATE_SIZE);

    /* A normal user entry returns here: exit(0), then yield forever if a
     * malformed syscall path unexpectedly returns. */
    exit_gate[0] = 0x31; exit_gate[1] = 0xDB;             /* xor ebx,ebx */
    exit_gate[2] = 0xB8;                                 /* mov eax,SYS_EXIT */
    *(uint32_t *)(void *)(exit_gate + 3) = SYS_EXIT;
    exit_gate[7] = 0xCD; exit_gate[8] = 0x80;             /* int 0x80 */
    exit_gate[9] = 0xB8;                                 /* mov eax,SYS_YIELD */
    *(uint32_t *)(void *)(exit_gate + 10) = SYS_YIELD;
    exit_gate[14] = 0xCD; exit_gate[15] = 0x80;
    exit_gate[16] = 0xEB; exit_gate[17] = 0xF7;           /* jmp 9 */

    /* Preserve callback EAX in EBX, exactly like the old kernel-text gate. */
    upcall_gate[0] = 0x89; upcall_gate[1] = 0xC3;         /* mov ebx,eax */
    upcall_gate[2] = 0xB8;                               /* mov eax,UPCALL */
    *(uint32_t *)(void *)(upcall_gate + 3) = SYS_UPCALL_RETURN;
    upcall_gate[7] = 0xCD; upcall_gate[8] = 0x80;
    upcall_gate[9] = 0x0F; upcall_gate[10] = 0x0B;        /* ud2 */
    upcall_gate[11] = 0xEB; upcall_gate[12] = 0xFE;

    __asm__ volatile ("" : : : "memory");
    if (!paging_set_range((uint32_t)(uintptr_t)g_user_gate_page,
                          PAGING_PAGE_SIZE, true, true, false)) return false;
    g_user_process_exit_gate = exit_gate;
    g_user_upcall_return_gate = upcall_gate;
    return true;
}

void task_init(void) {
    uint32_t *idle_stack;
    void *idle_allocation = NULL;
    uint32_t idle_stack_size;

    kspin_init(&g_task_table_lock);
    kmemset(tasks, 0, sizeof(tasks));
    kmemset(g_task_slot_reserved, 0, sizeof(g_task_slot_reserved));
    kmemset(g_task_reap_claim, 0, sizeof(g_task_reap_claim));
    kmemset(g_thread_completions, 0, sizeof(g_thread_completions));
    g_thread_completion_sequence = 0U;
    kmemset(g_process_completions, 0, sizeof(g_process_completions));
    g_process_completion_sequence = 0U;
    kmemset(cpu_states, 0, sizeof(cpu_states));
    kmemset(cpu_sample_ticks, 0, sizeof(cpu_sample_ticks));
    kmemset(cpu_sample_busy, 0, sizeof(cpu_sample_busy));
    kmemset(cpu_usage_per_cpu, 0, sizeof(cpu_usage_per_cpu));
    sample_ticks = 0U;
    sample_busy = 0U;
    cpu_usage = 0U;
    g_quantum_limit_ticks = 0U;

    if (!task_user_gates_init()) {
        kprintf("[TASK] no hay memoria para gates Ring 3 seguros\n");
        cli();
        for (;;) __asm__ volatile ("hlt");
    }

    g_idle_tasks = (task_t *)kzalloc(sizeof(task_t) * SMP_MAX_CPUS);
    g_task_query_snapshot = (task_t *)mm_alloc_guarded_stack(
        sizeof(task_t) * SMP_MAX_CPUS, false, NULL, NULL);
    g_linux_clear_child_tid=(uint32_t *)kzalloc(
        sizeof(uint32_t)*TASK_MAX);
    if (!g_idle_tasks || !g_task_query_snapshot || !g_linux_clear_child_tid ||
        !scheduler_smp_init(tasks, TASK_MAX)) {
        kprintf("[TASK] no hay memoria para estructuras SMP del scheduler\n");
        cli();
        for (;;) __asm__ volatile ("hlt");
    }

    for (uint32_t cpu = 0U; cpu < SMP_MAX_CPUS; cpu++) {
        cpu_states[cpu].current_slot = -1;
        cpu_states[cpu].rr_cursor = -1;
        cpu_states[cpu].deferred_ready_slot = -1;
    }
    for (int i = 0; i < TASK_MAX; i++) {
        tasks[i].running_cpu = -1;
        tasks[i].queued_cpu = -1;
        tasks[i].affinity_cpu = -1;
        tasks[i].preferred_cpu = 0U;
        tasks[i].last_cpu = 0xFFU;
    }

    cpu_states[0].current_slot = 0;
    next_pid = 1;
    current_quantum = 0;
    round_robin_cursor = 0;
    kmemset(yield_tick, 0, sizeof(yield_tick));
    kmemset(yield_budget, 0, sizeof(yield_budget));

    tasks[0].pid = next_pid++;
    tasks[0].process_id = tasks[0].pid;
    kstrcpy(tasks[0].name, "kernel-core");
    tasks[0].state = TASK_RUNNING;
    tasks[0].running_cpu = 0;
    tasks[0].queued_cpu = -1;
    tasks[0].affinity_cpu = 0;
    tasks[0].affinity_mask = 1U;
    tasks[0].preferred_cpu = 0U;
    tasks[0].last_cpu = 0U;
    tasks[0].system = true;
    tasks[0].stack_size = compat_mode_stack_size("kernel-core", false);
    tasks[0].memory_bytes = tasks[0].stack_size;

    task_fpu_prepare_cpu();
    task_fpu_build_initial_state();
    if (!task_fpu_assign_initial(&tasks[0])) {
        kprintf("[TASK] no hay memoria para estado FPU de kernel-core\n");
        cli();
        for (;;) __asm__ volatile ("hlt");
    }

    idle_stack_size = compat_mode_stack_size("idle", false);
    idle_stack = task_stack_allocate(idle_stack_size, false,
                                     &idle_allocation, &idle_stack_size);
    if (!idle_stack ||
        !task_idle_initialize(0U, idle_allocation, idle_stack,
                              idle_stack_size)) {
        kprintf("[TASK] no se pudo crear el contexto idle de CPU0\n");
        cli();
        for (;;) __asm__ volatile ("hlt");
    }
}

static int task_create_internal(const char *name, task_entry_t entry,
                                void *argument, bool user,
                                const char *launch_arg,
                                uint32_t process_id,
                                bool force_cpu0,
                                uint32_t requested_user_stack,
                                bool is_thread,
                                bool thread_joinable,
                                bool start_suspended) {
    int index = -1;
    uint32_t *kernel_stack = NULL;
    uint32_t *user_stack = NULL;
    void *kernel_stack_allocation = NULL;
    void *user_stack_allocation = NULL;
    uint32_t kernel_stack_size;
    uint32_t user_stack_size;
    uint32_t pid = 0U;
    uint32_t parent_pid = 0U;
    uint32_t parent_console = 0U;
    char parent_cwd[TASK_CWD_LEN] = "/";
    int8_t affinity_cpu;
    task_t *task;
    uint32_t active_tasks = 0U;
    uint32_t table_flags;

    if (!entry) return -1;
    /* Flat Ring-3 tasks must start in a user-mapped image. The one exception
     * is the temporary Win16 placeholder, whose segmented frame is installed
     * before the outer preemption guard is released. Rejecting a kernel-text
     * entry turns old PE compatibility paths into a clean launch failure
     * instead of a supervisor-page #PF. */
    if (user && !start_suspended &&
        entry != (task_entry_t)task_user_bootstrap &&
        !user_access_ok((const void *)(uintptr_t)entry, 1U, false)) {
        kprintf("[TASK] entrada Ring 3 no accesible: %x (%s)\n",
                (uint32_t)(uintptr_t)entry, name ? name : "?");
        return -1;
    }
    if (user && !compat_mode_allow_user_programs()) {
        kprintf("[COMPAT] programa rechazado: 4 MiB reservados para escritorio\n");
        return -1;
    }

    /* Recycle completed launcher/worker slots before looking for a new one.
     * Without this, a burst could hit 32 occupied table entries even though
     * several of them were already zombies waiting for the GUI reaper. */
    task_reap_deferred();

    /* Keep the caller's per-CPU current slot stable, but do not hold the
     * cross-CPU task-table lock while allocating and clearing large stacks. */
    task_preempt_disable();
    table_flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++)
        if ((tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_ZOMBIE &&
             !tasks[i].idle) || g_task_slot_reserved[i]) active_tasks++;
    if (active_tasks >= compat_mode_task_limit()) {
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        task_preempt_enable();
        kprintf("[COMPAT] limite de tareas alcanzado (%u)\n", active_tasks);
        return -1;
    }
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED && !g_task_slot_reserved[i]) {
            index = i;
            g_task_slot_reserved[i] = 1U;
            break;
        }
    }
    if (index < 0) {
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        task_preempt_enable();
        return -1;
    }
    pid = next_pid++;
    {
        task_t *parent = task_current_entry();
        parent_pid = parent && parent->process_id
                   ? parent->process_id : (parent ? parent->pid : 0U);
        parent_console = parent ? parent->console_route : 0U;
        if (parent && task_current_cwd()[0]) {
            kstrncpy(parent_cwd, task_current_cwd(), sizeof(parent_cwd) - 1U);
            parent_cwd[sizeof(parent_cwd) - 1U] = '\0';
        }
    }
    affinity_cpu = (user && smp_scheduler_started() && !force_cpu0) ? -1 : 0;
    kspin_unlock_irqrestore(&g_task_table_lock, table_flags);

    kernel_stack_size = compat_mode_stack_size(name, false);
    user_stack_size = user ? compat_mode_stack_size(name, true) : 0U;
    if (user && requested_user_stack) {
        if (requested_user_stack < 16384U) requested_user_stack = 16384U;
        if (requested_user_stack > TASK_STACK_MAX_SIZE)
            requested_user_stack = TASK_STACK_MAX_SIZE;
        requested_user_stack = (requested_user_stack + 4095U) & ~4095U;
        user_stack_size = requested_user_stack;
    }
    kernel_stack = task_stack_allocate(kernel_stack_size, false,
                                       &kernel_stack_allocation,
                                       &kernel_stack_size);
    if (kernel_stack && user)
        user_stack = task_stack_allocate(user_stack_size, true,
                                         &user_stack_allocation,
                                         &user_stack_size);
    if (!kernel_stack || (user && !user_stack)) {
        if (kernel_stack)
            task_stack_release(NULL, kernel_stack_allocation, kernel_stack,
                               kernel_stack_size, "kernel");
        if (user_stack)
            task_stack_release(NULL, user_stack_allocation, user_stack,
                               user_stack_size, "usuario");
        table_flags = kspin_lock_irqsave(&g_task_table_lock);
        g_task_slot_reserved[index] = 0U;
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        task_preempt_enable();
        return -1;
    }

    task = &tasks[index];
    kmemset(task, 0, sizeof(*task));
    g_linux_clear_child_tid[index]=0U;
    task->running_cpu = -1;
    task->queued_cpu = -1;
    task->affinity_cpu = affinity_cpu;
    task->affinity_mask = affinity_cpu >= 0
        ? (1U << (uint32_t)(uint8_t)affinity_cpu)
        : scheduler_smp_all_mask();
    task->preferred_cpu = affinity_cpu >= 0
        ? (uint8_t)affinity_cpu : (uint8_t)smp_cpu_index();
    task->last_cpu = 0xFFU;
    task->stack = kernel_stack;
    task->user_stack = user_stack;
    task->stack_allocation = kernel_stack_allocation;
    task->user_stack_allocation = user_stack_allocation;
    task->stack_size = kernel_stack_size;
    task->user_stack_size = user_stack_size;
    task->pid = pid;
    task->process_id = process_id ? process_id : task->pid;
    task->thread = is_thread;
    task->thread_joinable = is_thread && thread_joinable;
    task->parent_pid = parent_pid;
    task->console_route = parent_console;
    {
        size_t cwd_len = kstrlen(parent_cwd);
        task->cwd = (char *)kmalloc(cwd_len + 1U);
        if (task->cwd) {
            kmemcpy(task->cwd, parent_cwd, cwd_len);
            task->cwd[cwd_len] = '\0';
            /* kmalloc() runs in the parent while the child task is being
             * constructed, so the block initially belongs to the parent
             * process.  Re-own it before the parent can exit and have
             * mm_release_process() reclaim the child's cwd underneath it. */
            if (!mm_set_allocation_owner(task->cwd, task->process_id)) {
                kfree(task->cwd);
                task->cwd = NULL;
            }
        }
    }
    kstrncpy(task->name, name ? name : (user ? "user" : "task"),
             TASK_NAME_LEN - 1);
    task->context = (registers_t *)((uint8_t *)kernel_stack + kernel_stack_size -
                                    sizeof(registers_t));
    kmemset(task->context, 0, sizeof(registers_t));
    task->entry = entry;
    task->argument = argument;
    if (!task_fpu_assign_initial(task)) {
        if (task->cwd) { kfree(task->cwd); task->cwd = NULL; }
        task_stack_release(task, task->stack_allocation, task->stack,
                           task->stack_size, "kernel");
        task_stack_release(task, task->user_stack_allocation, task->user_stack,
                           task->user_stack_size, "usuario");
        table_flags = kspin_lock_irqsave(&g_task_table_lock);
        kmemset(task, 0, sizeof(*task));
        g_task_slot_reserved[index] = 0U;
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        task_preempt_enable();
        return -1;
    }
    task->context->int_no = 32;

    if (user) {
        task->context->ds = GDT_USER_DATA;
        task->context->es = GDT_USER_DATA;
        task->context->fs = GDT_USER_DATA;
        task->context->gs = GDT_USER_DATA;
        task->context->cs = GDT_USER_CODE;
        task->context->ss = GDT_USER_DATA;
        {
            uint32_t aligned_top =
                ((uint32_t)(uintptr_t)user_stack + user_stack_size) & ~15U;
            uint32_t *initial_user_stack =
                (uint32_t *)(uintptr_t)(aligned_top - 20U);
            /* Normal cdecl entry: [return address][argument]. Native thread
             * trampolines are noreturn, while process entries may return to
             * the shared user exit gate. */
            initial_user_stack[0] = is_thread && thread_joinable
                ? 0U : (uint32_t)(uintptr_t)g_user_process_exit_gate;
            initial_user_stack[1] = (uint32_t)(uintptr_t)argument;
            initial_user_stack[2] = 0U;
            initial_user_stack[3] = 0U;
            initial_user_stack[4] = 0U;
            task->context->eip = (uint32_t)(uintptr_t)entry;
            /* Ring 3 must not program ports or disable interrupts. Drivers
             * execute their privileged operations through kernel entry points. */
            task->context->eflags = 0x202U;
            task->context->useresp = (uint32_t)(uintptr_t)initial_user_stack;
        }
        task->user = true;
        task->memory_bytes = kernel_stack_size + user_stack_size;
    } else {
        task->context->ds = GDT_KERNEL_DATA;
        task->context->es = GDT_KERNEL_DATA;
        task->context->fs = GDT_KERNEL_DATA;
        task->context->gs = GDT_KERNEL_DATA;
        task->context->eip = (uint32_t)(uintptr_t)task_bootstrap;
        task->context->cs = GDT_KERNEL_CODE;
        task->context->eflags = 0x202;
        task->context->useresp = (uint32_t)(uintptr_t)task_exit;
        task->memory_bytes = kernel_stack_size;
    }

    task_copy_launch_arg(task, launch_arg);
    table_flags = kspin_lock_irqsave(&g_task_table_lock);
    /* A cooperative process stop may arrive while this thread's stacks are
       being allocated. Publish its inherited stop flag under the same lock
       as TASK_READY so newly created threads cannot escape that request. */
    if(is_thread && user) {
        task_t *creator=task_current_entry();
        if(creator && creator->process_id==process_id) {
            task->priority=creator->priority;
            task->exit_requested=creator->exit_requested;
            if(creator->exit_requested)task->exit_code=creator->exit_code;
        }
    }
    yield_tick[index] = 0;
    yield_budget[index] = TASK_YIELD_BUDGET_PER_TICK;
    __asm__ volatile ("" : : : "memory");
    task->state = start_suspended ? TASK_STARTING : TASK_READY;
    g_task_slot_reserved[index] = 0U;
    kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
    if (!start_suspended && smp_scheduler_started())
        (void)scheduler_smp_enqueue(index, -1, true);
    task_preempt_enable();
    return (int)task->pid;
}

int task_create(const char *name, task_entry_t entry, void *argument) {
    task_t *current = task_current_entry();
    return task_create_internal(name, entry, argument,
                                current ? current->user : false,
                                NULL, 0U, false, 0U, false, false, false);
#if 0
    if (!entry) return -1;
    task_preempt_disable();
    int index = -1;
    for (int i = 1; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_ZOMBIE) {
            index = i;
            break;
        }
    if (index < 0) {
        task_preempt_enable();
        return -1;
    }

    uint32_t *stack = (uint32_t *)kzalloc(TASK_STACK_SIZE);
    if (!stack) {
        task_preempt_enable();
        return -1;
    }
    task_t *task = &tasks[index];
    kmemset(task, 0, sizeof(*task));
    task->stack = stack;
    task->pid = next_pid++;
    kstrncpy(task->name, name ? name : "task", TASK_NAME_LEN - 1);
    task->context = (registers_t *)((uint8_t *)stack + TASK_STACK_SIZE -
                                    sizeof(registers_t));
    kmemset(task->context, 0, sizeof(registers_t));

    /*
     * Los stubs de interrupción restauran segmentos desde registers_t:
     *
     *   pop gs
     *   pop fs
     *   pop es
     *   pop ds
     *
     * Antes sólo inicializábamos DS. Entonces las tareas kernel nuevas
     * arrancaban con ES/FS/GS en 0. En i386, el compilador usa REP MOVSL
     * para copiar structs, y REP MOVSL escribe usando ES:EDI.
     *
     * Resultado en hardware real: GENERAL PROTECTION FAULT (#13) al abrir
     * apps como Calendar, File Browser, Process Manager, etc.
     */
    task->context->ds = GDT_KERNEL_DATA;
    task->context->es = GDT_KERNEL_DATA;
    task->context->fs = GDT_KERNEL_DATA;
    task->context->gs = GDT_KERNEL_DATA;

    task->entry = entry;
    task->argument = argument;
    /* Copying the template does not disturb the currently running task's
     * live x87 stack. */
    task_fpu_assign_initial(task);
    task->context->int_no = 32;
    task->context->eip = (uint32_t)task_bootstrap;
    task->context->cs = GDT_KERNEL_CODE;
    task->context->eflags = 0x202;
    task->context->useresp = (uint32_t)task_exit;
    task->state = TASK_READY;
    task->memory_bytes = TASK_STACK_SIZE;
    yield_tick[index] = 0;
    yield_budget[index] = TASK_YIELD_BUDGET_PER_TICK;
    task_preempt_enable();
    return (int)task->pid;
#endif
}

int task_create_user(const char *name, void (*entry)(void)) {
    return task_create_internal(name, (task_entry_t)(uintptr_t)entry, NULL,
                                true, NULL, 0U, false, 0U, false, false,
                                false);
#if 0
    int index = -1;
    uint32_t *kernel_stack;
    uint32_t *user_stack;
    task_t *task;

    if (!entry) return -1;
    task_preempt_disable();
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_ZOMBIE) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        task_preempt_enable();
        return -1;
    }

    kernel_stack = (uint32_t *)kzalloc(TASK_STACK_SIZE);
    user_stack = (uint32_t *)kzalloc(TASK_STACK_SIZE);
    if (!kernel_stack || !user_stack) {
        if (kernel_stack) kfree(kernel_stack);
        if (user_stack) kfree(user_stack);
        task_preempt_enable();
        return -1;
    }

    task = &tasks[index];
    kmemset(task, 0, sizeof(*task));
    task->stack = kernel_stack;
    task->user_stack = user_stack;
    task->pid = next_pid++;
    kstrncpy(task->name, name ? name : "user", TASK_NAME_LEN - 1);
    task->context = (registers_t *)((uint8_t *)kernel_stack + TASK_STACK_SIZE -
                                    sizeof(registers_t));
    kmemset(task->context, 0, sizeof(registers_t));
    task->context->ds = GDT_USER_DATA;
    task->context->es = GDT_USER_DATA;
    task->context->fs = GDT_USER_DATA;
    task->context->gs = GDT_USER_DATA;
    task->context->eip = (uint32_t)(uintptr_t)entry;
    task->context->cs = GDT_USER_CODE;
    task->context->eflags = 0x202;
    task->context->useresp = (uint32_t)(uintptr_t)
        ((uint8_t *)user_stack + TASK_STACK_SIZE);
    task->context->ss = GDT_USER_DATA;
    task->state = TASK_READY;
    task->user = true;
    task->memory_bytes = TASK_STACK_SIZE * 2U;
    yield_tick[index] = 0;
    yield_budget[index] = TASK_YIELD_BUDGET_PER_TICK;
    task_preempt_enable();
    return (int)task->pid;
#endif
}

int task_create_user_program(const char *name, task_entry_t entry,
                             void *argument, const char *launch_arg) {
    return task_create_internal(name, entry, argument, true, launch_arg, 0U,
                                false, 0U, false, false, false);
}

int task_create_user_program_suspended(const char *name, task_entry_t entry,
                                       void *argument,
                                       const char *launch_arg) {
    return task_create_internal(name, entry, argument, true, launch_arg, 0U,
                                false, 0U, false, false, true);
}

bool task_start_suspended(uint32_t pid) {
    int index = -1;
    uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].pid != pid || tasks[i].state != TASK_STARTING) continue;
        tasks[i].state = TASK_READY;
        index = i;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    if (index < 0) return false;
    if (smp_scheduler_started())
        (void)scheduler_smp_enqueue(index, -1, true);
    return true;
}


/* BLES_WINE_KERNEL_WORKER_API_20260723 */
int task_create_kernel(const char *name, task_entry_t entry,
                       void *argument) {
    return task_create_internal(name, entry, argument, false,
                                NULL, 0U, false, 0U, false, false, false);
}
int task_create_kernel_suspended(const char *name, task_entry_t entry,
                                 void *argument) {
    return task_create_internal(name, entry, argument, false,
                                NULL, 0U, false, 0U, false, false, true);
}
int task_create_user_thread_ex(const char *name, task_entry_t entry,
                               void *argument, uint32_t process_id,
                               uint32_t user_stack_size, bool joinable) {
    task_t *current = task_current_entry();
    if (!process_id && current) process_id = current->process_id;
    if (!process_id) return -1;
    return task_create_internal(name, entry, argument, true, NULL, process_id,
                                false, user_stack_size, true, joinable, false);
}

int task_create_user_thread(const char *name, task_entry_t entry,
                            void *argument, uint32_t process_id) {
    /* Preserve the historical fire-and-forget API used by Wine. */
    return task_create_user_thread_ex(name, entry, argument, process_id,
                                      0U, false);
}

int32_t task_clone_linux_thread(const registers_t *parent_frame,
                                uint32_t child_stack, uint32_t tls_base,
                                int32_t tls_entry, uint32_t clone_flags,
                                uint32_t parent_tid_address,
                                uint32_t child_tid_address) {
    enum { CLONE_SETTLS=0x00080000U,
           CLONE_PARENT_SETTID=0x00100000U,
           CLONE_CHILD_CLEARTID=0x00200000U,
           CLONE_CHILD_SETTID=0x01000000U };
    task_t *parent = task_current_entry();
    int pid;
    int slot = -1;
    uint32_t flags;
    uint32_t tls_slot = 0U;

    if (!parent_frame || !parent || !parent->user || !parent->linux_abi ||
        !child_stack || (child_stack & 3U)) return -1;
    if (clone_flags & CLONE_SETTLS) {
        if (tls_entry < (int32_t)GDT_USER_TLS_MIN_ENTRY ||
            tls_entry >= (int32_t)(GDT_USER_TLS_MIN_ENTRY +
                                    GDT_USER_TLS_COUNT))
            return -1;
        tls_slot = (uint32_t)tls_entry - GDT_USER_TLS_MIN_ENTRY;
    }

    /* The task stays TASK_STARTING until its complete Linux return frame is
     * installed.  Its private fallback user stack is retained as guarded
     * emergency backing, although Linux resumes on child_stack exactly. */
    pid = task_create_internal("linux-thread",
            (task_entry_t)(uintptr_t)parent_frame->eip, NULL, true, NULL,
            parent->process_id, false, 16384U, true, false, true);
    if (pid < 0) return -1;

    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 1; i < TASK_MAX; i++) {
        task_t *child = &tasks[i];
        if (child->pid != (uint32_t)pid || child->state != TASK_STARTING)
            continue;
        kmemcpy(child->context, parent_frame, sizeof(*parent_frame));
        child->context->eax = 0U;
        child->context->useresp = child_stack;
        child->context->fs = parent_frame->fs;
        child->context->gs = parent_frame->gs;
        child->user_fs_base = parent->user_fs_base;
        kmemcpy(child->linux_tls_base, parent->linux_tls_base,
                sizeof(child->linux_tls_base));
        child->linux_tls_used_mask = parent->linux_tls_used_mask;
        if (clone_flags & CLONE_SETTLS) {
            child->linux_tls_base[tls_slot] = tls_base;
            child->linux_tls_used_mask |= (uint8_t)(1U << tls_slot);
            if (tls_slot == 0U) child->user_fs_base = tls_base;
            child->context->gs = GDT_USER_TLS_SELECTOR((uint32_t)tls_entry);
        }
        g_linux_clear_child_tid[i]=(clone_flags & CLONE_CHILD_CLEARTID)
            ? child_tid_address : 0U;
        child->linux_abi = true;
        child->thread = true;
        child->thread_joinable = false;
        slot = i;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    if (slot < 0) return -1;
    if (!linux_signal_init(&tasks[slot],parent)) {
        task_request_exit((uint32_t)pid);
        return -1;
    }
    task_fpu_save(parent);
    kmemcpy(tasks[slot].fpu_state,parent->fpu_state,TASK_FPU_STATE_SIZE);
    /* Publish both TID words while the child is still suspended.  Publishing
     * them after enqueueing permits a fast child to exit/clear first and the
     * parent to overwrite that zero with a stale TID. */
    if (((clone_flags & CLONE_PARENT_SETTID) &&
         !copy_to_user((void *)(uintptr_t)parent_tid_address,
                       &pid,sizeof(pid))) ||
        ((clone_flags & CLONE_CHILD_SETTID) &&
         !copy_to_user((void *)(uintptr_t)child_tid_address,
                       &pid,sizeof(pid)))) {
        flags=kspin_lock_irqsave(&g_task_table_lock);
        tasks[slot].state=TASK_ZOMBIE;
        kspin_unlock_irqrestore(&g_task_table_lock,flags);
        return -1;
    }
    if (!task_start_suspended((uint32_t)pid)) return -1;
    return pid;
}

int32_t task_linux_fork(registers_t *r,uint32_t tls_base) {
    task_t *parent=task_current_entry(),*child=NULL;
    const char *fail_stage="precheck";
    uint32_t vm_before=mm_vm_frame_free_pages();
    if (!parent || !parent->linux_abi) return -BK_ENOSYS;
    if (!parent->linux_signals && !linux_signal_init(parent,NULL)) {
        kprintf("[LINUX:FORKMEM] parent=%u fail=signal-init free=%u\n",
                parent->process_id,mm_vm_frame_free_pages());
        return -BK_ENOMEM;
    }
    bool vfork=(r->ebx&0x4100U)==0x4100U;
    if(vfork && (parent->linux_vm_lender || task_thread_count(parent->process_id))) return -BK_ENOSYS;
    /* fork copies the process image but resumes only its calling thread.
     * pthread stacks/TLS live in VMAs cloned below; the original exec stack
     * is separate and must still be copied from the process leader, not the
     * calling thread's small emergency stack. */
    task_t *stack_owner=parent;
    uint32_t stack_lock=kspin_lock_irqsave(&g_task_table_lock);
    if(parent->thread) {
        stack_owner=NULL;
        for(int i=1;i<TASK_MAX;i++) {
            if(tasks[i].pid==parent->process_id && tasks[i].user_stack &&
               tasks[i].linux_signals && tasks[i].state!=TASK_ZOMBIE) {
                stack_owner=&tasks[i];break;
            }
        }
    }
    uint32_t stack=stack_owner?stack_owner->linux_signals->user_stack_address:0U;
    uint32_t stack_size=stack_owner?stack_owner->user_stack_size:0U;
    void *stack_pin=stack_owner?(stack_owner->linux_stack_pin?
        stack_owner->linux_stack_pin:stack_owner->user_stack_allocation):NULL;
    bool stack_ready=stack_owner && (vfork || mm_pin_address_allocation(stack_pin));
    kspin_unlock_irqrestore(&g_task_table_lock,stack_lock);
    if(!stack_ready)return -BK_ENOMEM;
    int pid=task_create_internal(parent->name,(task_entry_t)r->eip,NULL,true,NULL,0,
        false,stack_size,false,false,true);
    if (pid<0) {
        if(!vfork)mm_unpin_address_allocation(stack_pin);
        kprintf("[LINUX:FORKMEM] parent=%u fail=task-create flags=%x free_before=%u free_now=%u\n",
                parent->process_id,r->ebx,vm_before,mm_vm_frame_free_pages());
        return -BK_EAGAIN;
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[LINUX:FORKMEM] parent=%u child=%u begin flags=%x vfork=%u free=%u\n",
            parent->process_id,(uint32_t)pid,r->ebx,vfork?1U:0U,
            mm_vm_frame_free_pages());
#endif
    for (int i=1;i<TASK_MAX;i++) if(tasks[i].pid==(uint32_t)pid) {child=&tasks[i];break;}
    if (!child) {
        if(!vfork)mm_unpin_address_allocation(stack_pin);
        return -BK_EAGAIN;
    }
    if(!vfork)child->linux_stack_pin=stack_pin;
    child->linux_abi=true;
    kmemcpy(child->linux_tls_base, parent->linux_tls_base,
            sizeof(child->linux_tls_base));
    child->linux_tls_used_mask = parent->linux_tls_used_mask;
    child->user_fs_base = parent->user_fs_base;
    if (!linux_signal_init(child,NULL)) goto failed;
    child->linux_signals->user_stack_address=stack;
    child->linux_signals->mask=parent->linux_signals->mask;
    child->linux_signals->stack=parent->linux_signals->stack;
    uint32_t lock=kspin_lock_irqsave(&g_task_table_lock);
    kmemcpy(child->linux_signals->shared->actions,parent->linux_signals->shared->actions,
        sizeof(child->linux_signals->shared->actions));
    kspin_unlock_irqrestore(&g_task_table_lock,lock);
    if(vfork) {
        /* CLONE_VFORK lends the parent's address space only until the child
         * completes execve() or exits.  The zombie/reaper may retain the
         * borrowed paging/resource bookkeeping a little longer, but Linux
         * must let the parent return from clone() as soon as the child can no
         * longer execute in that shared image. */
        child->linux_vm_lender=parent->process_id;
        fail_stage="vfork-paging-borrow";
        if(!paging_process_borrow(parent->process_id,pid)) goto failed;
        fail_stage="vfork-resource-borrow";
        if(!syscall_linux_borrow_resources(parent->process_id,pid)) goto failed;
    } else {
        fail_stage="stack-copy";
        if (!paging_process_copy(parent->process_id,pid,stack,
                                 (uint32_t)child->user_stack,
                                 stack_size)) goto failed;
        fail_stage="elf-images";
        if (!elf_linux_clone_images(parent->process_id,pid)) goto failed;
        fail_stage="vm-resources";
        if (!syscall_linux_clone_resources(parent->process_id,pid)) goto failed;
    }
    *child->context=*r;child->context->eax=0;
    if (r->ecx) child->context->useresp=r->ecx;
    if (r->ebx&0x80000) {
        child->user_fs_base=tls_base;
        child->linux_tls_base[0]=tls_base;
        child->linux_tls_used_mask|=1U;
        child->context->gs=tls_base?GDT_USER_FS:GDT_USER_DATA;
    }
    task_fpu_save(parent);kmemcpy(child->fpu_state,parent->fpu_state,TASK_FPU_STATE_SIZE);
    fail_stage="child-settid";
    if ((r->ebx&0x1000000) && !paging_process_write(pid,r->edi,&pid,4)) goto failed;
    fail_stage="parent-settid";
    if ((r->ebx&0x100000) && !copy_to_user((void *)r->edx,&pid,4)) goto failed;
    g_linux_clear_child_tid[child-tasks]=(r->ebx&0x200000)?r->edi:0;
    fail_stage="start-child";
    if (!task_start_suspended(pid)) goto failed;
    kernel_domain_snapshot_t vfork_domains;
    if(vfork) kernel_domains_drop_current(&vfork_domains);
#ifdef BLESK_WINE_VERBOSE_TRACE
    if(vfork) kprintf("[VFORK] parent=%u child=%u wait\n",parent->process_id,pid);
#endif
    if(vfork) for(;;) {
        bool child_still_using_parent_vm=false;
        uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
        for(int i=1;i<TASK_MAX;i++) {
            task_t *candidate=&tasks[i];
            if(candidate->linux_vm_lender!=parent->process_id) continue;
            /* execve() atomically retires the old vfork task as ZOMBIE while
             * publishing the replacement under this same table lock.  _exit
             * also marks it ZOMBIE before scheduling away.  Waiting for the
             * reaper here is both stronger than Linux requires and can
             * deadlock: the normal waiter itself is what will reap an exited
             * child after clone() finally returns. */
            if(candidate->state!=TASK_ZOMBIE && candidate->state!=TASK_UNUSED) {
                child_still_using_parent_vm=true;
                break;
            }
        }
        kspin_unlock_irqrestore(&g_task_table_lock,flags);
        if(!child_still_using_parent_vm) break;
        task_yield();
    }
    if(vfork) {
#ifdef BLESK_WINE_VERBOSE_TRACE
        kprintf("[VFORK] parent=%u child=%u resume\n",parent->process_id,pid);
#endif
        kernel_domains_restore(&vfork_domains);
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[LINUX:FORKMEM] parent=%u child=%u ok free_before=%u free_after=%u consumed=%u\n",
            parent->process_id,(uint32_t)pid,vm_before,mm_vm_frame_free_pages(),
            vm_before>=mm_vm_frame_free_pages()?vm_before-mm_vm_frame_free_pages():0U);
#endif
    return pid;
failed:
    kprintf("[LINUX:FORKMEM] parent=%u child=%u fail=%s flags=%x free_before=%u free_now=%u\n",
            parent->process_id,(uint32_t)pid,fail_stage,r->ebx,vm_before,
            mm_vm_frame_free_pages());
    lock=kspin_lock_irqsave(&g_task_table_lock);
    child->exit_code=-BK_ENOMEM;child->state=TASK_ZOMBIE;
    child->parent_pid=0;child->thread=true;
    kspin_unlock_irqrestore(&g_task_table_lock,lock);return -BK_ENOMEM;
}

static void task_wake_sleepers_smp(uint32_t now) {
    int wake_slots[TASK_MAX];
    uint32_t wake_count = 0U;
    uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int slot = 0; slot < TASK_MAX; slot++) {
        task_t *task = &tasks[slot];
        if (task->state != TASK_SLEEPING ||
            (int32_t)(now - task->wake_tick) < 0) continue;
        /* SYS_SLEEP publishes TASK_SLEEPING before the originating CPU has
         * left the task's kernel stack.  Waking it during that short window
         * would make the same stack runnable elsewhere.  A later BSP tick
         * will observe running_cpu == -1 after the context switch completes. */
        if (task->running_cpu != -1) continue;
        /* A timed-out futex must not remain registered as a waiter: a later
         * unrelated sleep could otherwise be mistaken for a FUTEX_WAKE and
         * have its saved syscall return value overwritten. */
        if (task->linux_signals) task->linux_signals->futex_key = 0U;
        task->state = TASK_READY;
        if (task->queued_cpu < 0 && wake_count < TASK_MAX)
            wake_slots[wake_count++] = slot;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    for (uint32_t i = 0U; i < wake_count; i++)
        (void)scheduler_smp_enqueue(wake_slots[i], -1, true);
}

#define TASK_CPU_HANDOFF (-2)

static void task_publish_deferred_ready(uint32_t cpu) {
    int slot;
    task_cpu_state_t *state;
    task_t *task;
    if (cpu >= SMP_MAX_CPUS) return;
    state = &cpu_states[cpu];
    slot = state->deferred_ready_slot;
    if (slot < 0 || slot >= TASK_MAX) return;
    state->deferred_ready_slot = -1;
    task = &tasks[slot];
    uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
    bool ready=false;
    if(task->running_cpu==TASK_CPU_HANDOFF) {
        task->running_cpu=-1;
        /* A producer may have signalled while this stack was in handoff.
         * It could only advance wake_tick then, not enqueue a live stack.
         * We are on the next stack now: publish the completed wake here
         * instead of waiting for CPU0's next timer scan on every IPC round.
         * The same signed deadline check preserves timed sleeps and wrap. */
        if(task->state==TASK_SLEEPING &&
           (int32_t)(pit_get_ticks()-task->wake_tick)>=0) {
            if(task->linux_signals)task->linux_signals->futex_key=0U;
            task->state=TASK_READY;
        }
        ready=task->state==TASK_READY;
    }
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    if(ready)(void)scheduler_smp_enqueue(slot, (int32_t)cpu, false);
}

static uint32_t task_quantum_limit(void) {
    if (!g_quantum_limit_ticks) {
        uint32_t hz = pit_get_frequency_hz();
        uint32_t ticks = (hz * TASK_QUANTUM_MILLISECONDS + 999U) / 1000U;
        g_quantum_limit_ticks = ticks ? ticks : 1U;
    }
    task_t *t=task_current_entry();
    if(t && t->priority==3U)return g_quantum_limit_ticks*2U;
    if(t && t->priority==1U && g_quantum_limit_ticks>1U)return g_quantum_limit_ticks/2U;
    return g_quantum_limit_ticks;
}

static void task_watchdog_tick(uint32_t cpu) {
    task_cpu_state_t *state;
    uint32_t hz;
    uint32_t interval;

    if (cpu >= SMP_MAX_CPUS) cpu = 0U;
    state = &cpu_states[cpu];
    hz = pit_get_frequency_hz();
    if (!hz) hz = 100U;
    interval = hz / 10U;
    if (!interval) interval = 1U;
    if (++state->watchdog_ticks < interval) return;
    state->watchdog_ticks = 0U;
    smp_watchdog_heartbeat();
    /* Un solo CPU inspecciona la tabla. Antes cada LAPIC entraba al lock de
       polling en cada tick, aun cuando el informe sólo puede cambiar 1 vez/s. */
    if (cpu == 0U) smp_watchdog_poll();
}

static void task_account_tick(task_t *current, uint32_t cpu) {
    uint32_t online = smp_online_cpu_count();
    bool busy;
    if (cpu >= SMP_MAX_CPUS) cpu = 0U;
    if (!online) online = 1U;
    busy = current && current->state == TASK_RUNNING && !current->idle;
    perfmon_scheduler_tick(busy);
    if (current) current->cpu_ticks++;
    if (busy) cpu_sample_busy[cpu]++;
    if (++cpu_sample_ticks[cpu] >=
        (pit_get_frequency_hz() ? pit_get_frequency_hz() : 100U)) {
        uint32_t total = 0U;
        uint32_t usage = (cpu_sample_busy[cpu] * 100U) /
                         cpu_sample_ticks[cpu];
        cpu_usage_per_cpu[cpu] = (uint8_t)(usage > 100U ? 100U : usage);
        cpu_sample_ticks[cpu] = 0U;
        cpu_sample_busy[cpu] = 0U;
        if (online > SMP_MAX_CPUS) online = SMP_MAX_CPUS;
        for (uint32_t i = 0U; i < online; i++) total += cpu_usage_per_cpu[i];
        cpu_usage = (uint8_t)(total / online);
    }
}

static registers_t *task_schedule_internal(registers_t *frame,
                                             bool nonblocking_ap_timer,
                                             bool force_reschedule,
                                             bool account_tick) {
    uint32_t scheduler_flags = 0U;
    uint64_t perf_scheduler_started;
    uint32_t cpu;
    bool smp_mode;
    task_t *current;
    task_t *next_task;
    int old_slot;
    int next;

    (void)nonblocking_ap_timer;
    cpu = smp_cpu_index();
    if (cpu >= SMP_MAX_CPUS) cpu = 0U;
    paging_sync_cpu();
    if (account_tick) task_watchdog_tick(cpu);
    smp_mode = smp_scheduler_started() && scheduler_smp_ready();

    /* int 0x80 is a trap gate. Mask local IRQs before publishing any change
     * to current_slot; IRET restores the selected task's IF atomically. */
    if (smp_mode) {
        __asm__ volatile ("pushfl; popl %0; cli"
                          : "=r"(scheduler_flags) : : "memory", "cc");
        task_publish_deferred_ready(cpu);
        if (cpu == 0U) task_wake_sleepers_smp(pit_get_ticks());
    } else {
        scheduler_flags = kspin_lock_irqsave(&g_task_table_lock);
    }

    perf_scheduler_started = perfmon_scope_begin();
#define TASK_SCHEDULE_FINISH(value) do { \
        perfmon_scope_end(PERF_SCOPE_SCHEDULER, perf_scheduler_started); \
        if (!smp_mode) task_scheduler_unlock_for_iret(scheduler_flags); \
        return (value); \
    } while (0)

    current = task_current_entry();
    if (!current || !frame) {
        perfmon_scheduler_preempt_blocked();
        TASK_SCHEDULE_FINISH(frame);
    }
    if (account_tick) {
        task_account_tick(current, cpu);
        if (!current->idle) perfmon_sample_pc(current->pid, frame->eip);
    }

    if (!smp_mode) {
        uint32_t now = pit_get_ticks();
        for (int i = 0; i < TASK_MAX; i++) {
            if (tasks[i].state == TASK_SLEEPING &&
                (int32_t)(now - tasks[i].wake_tick) >= 0) {
                if (tasks[i].linux_signals)
                    tasks[i].linux_signals->futex_key = 0U;
                tasks[i].state = TASK_READY;
            }
        }
    }

    if (preempt_depth) {
        perfmon_scheduler_preempt_blocked();
        TASK_SCHEDULE_FINISH(frame);
    }

    {
        bool kernel_frame = (frame->cs & 3U) == 0U;
        if (smp_mode && current->user && kernel_frame &&
            !current->kernel_switch_safe) {
            uint32_t remaining = task_frame_stack_remaining(current, frame);
            bool guard_ok = task_stack_guard_quick_ok(current);
            if (guard_ok && (remaining == 0xFFFFFFFFU ||
                             remaining >= TASK_KERNEL_STACK_EMERGENCY_RESERVE)) {
                perfmon_scheduler_preempt_blocked();
                TASK_SCHEDULE_FINISH(frame);
            }
            kprintf("[TASK] carga pesada contenida pid=%u cpu=%u nombre=%s "
                    "stack=%u guard=%s; proceso terminado\n",
                    current->pid, cpu, current->name, remaining,
                    guard_ok ? "OK" : "DANADO");
            current->exit_code = 0xA502;
            current->state = TASK_ZOMBIE;
            current->kernel_switch_safe = true;
        }
        if (kernel_frame) current->kernel_switch_safe = false;
    }

    task_normalize_user_segments(current, frame);
    current->context = frame;

    if (!force_reschedule && current->state == TASK_RUNNING &&
        !current->idle) {
        bool gui_waiting = cpu == 0U && current_index != 0 &&
                           tasks[0].state == TASK_READY;
        if (!gui_waiting && ++current_quantum < task_quantum_limit()) {
            perfmon_scheduler_fast_quantum();
            TASK_SCHEDULE_FINISH(current->context);
        }
    }

    old_slot = current_index;
    if (smp_mode) {
        /* Reserve the next task first, but do not publish the old task until
         * this CPU has actually left its kernel stack. Publishing it now
         * would let a thief restore the same frame on another CPU while the
         * original CPU is still returning through it. */
        bool must_migrate = old_slot >= 0 &&
            current->state == TASK_RUNNING && !current->idle &&
            !scheduler_smp_cpu_allowed(current, cpu);
        next = scheduler_smp_dequeue(cpu);
        if (next < 0) next = scheduler_smp_steal(cpu);

        if (current->state == TASK_RUNNING && !current->idle && next < 0 &&
            !must_migrate) {
            current_quantum = 0U;
            TASK_SCHEDULE_FINISH(current->context);
        }
        if (current->idle && next < 0) {
            current_quantum = 0U;
            TASK_SCHEDULE_FINISH(current->context);
        }

        {
            uint64_t perf_fpu_started = perfmon_scope_begin();
            task_fpu_save(current);
            perfmon_scope_end(PERF_SCOPE_FPU_SWITCH, perf_fpu_started);
        }

        if (old_slot >= 0 && !current->idle) {
            if (cpu_states[cpu].deferred_ready_slot >= 0) {
                /* A second deferred slot would lose a runnable task. Keep the
                 * current task and return the reserved task to its queue. */
                if (next >= 0) {
                    task_t *reserved = &tasks[next];
                    reserved->state = TASK_READY;
                    reserved->running_cpu = -1;
                    (void)scheduler_smp_enqueue(next, (int32_t)cpu, false);
                }
                task_fpu_restore(current);
                TASK_SCHEDULE_FINISH(current->context);
            }
            if(current->state==TASK_RUNNING)current->state=TASK_READY;
            /* Sleeping and zombie tasks still execute on this stack until
             * the interrupt stub installs next_task->context. Publishing -1
             * here lets another CPU wake a futex or reap/free the stack while
             * syscall_handler is still restoring its register frame. */
            current->running_cpu = TASK_CPU_HANDOFF;
            cpu_states[cpu].deferred_ready_slot = old_slot;
        } else {
            current->running_cpu = -1;
            if (current->idle) current->state = TASK_READY;
        }

        if (next < 0) {
            next = -1;
            next_task = task_idle_for_cpu(cpu);
            if (!next_task) {
                task_fpu_restore(current);
                TASK_SCHEDULE_FINISH(current->context);
            }
            next_task->state = TASK_RUNNING;
            next_task->running_cpu = (int32_t)cpu;
            next_task->last_cpu = (uint8_t)cpu;
        } else {
            next_task = &tasks[next];
        }
    } else {
        {
            uint64_t perf_fpu_started = perfmon_scope_begin();
            task_fpu_save(current);
            perfmon_scope_end(PERF_SCOPE_FPU_SWITCH, perf_fpu_started);
        }
        if (old_slot >= 0 && current->state == TASK_RUNNING) {
            current->state = TASK_READY;
            current->running_cpu = -1;
        } else if (current->idle) {
            current->state = TASK_READY;
            current->running_cpu = -1;
        } else if (old_slot >= 0) {
            /* Sleeping and zombie tasks also leave this CPU at the switch.
             * Keeping running_cpu=0 here made single-CPU join/wait/reaping
             * wait forever even though another task was already executing. */
            current->running_cpu = -1;
        }
        next = task_pick_next_up();
        next_task = task_from_slot_cpu(next, 0U);
        if (!next_task) {
            task_fpu_restore(current);
            TASK_SCHEDULE_FINISH(current->context);
        }
        next_task->state = TASK_RUNNING;
        next_task->running_cpu = 0;
        next_task->last_cpu = 0U;
    }

    {
        uint64_t perf_fpu_started = perfmon_scope_begin();
        if (next != old_slot) {
            current_index = next;
            perfmon_scheduler_switch();
        }
        task_fpu_restore(next_task);
        perfmon_scope_end(PERF_SCOPE_FPU_SWITCH, perf_fpu_started);
    }
    current_quantum = 0U;

    if (next_task->stack) {
        tss_set_kernel_stack((uint32_t)(uintptr_t)
            ((uint8_t *)next_task->stack + next_task->stack_size));
    }
    if (next_task->user) {
        if (next_task->linux_abi) {
            for (uint32_t tls_slot = 0; tls_slot < GDT_USER_TLS_COUNT;
                 tls_slot++) {
                uint32_t base = (next_task->linux_tls_used_mask &
                                 (1U << tls_slot))
                    ? next_task->linux_tls_base[tls_slot] : 0U;
                gdt_set_user_tls_base(tls_slot, base);
            }
        } else {
            gdt_set_user_tls_base(0U, next_task->user_fs_base);
            gdt_set_user_tls_base(1U, 0U);
            gdt_set_user_tls_base(2U, 0U);
            if (next_task->user_fs_base) next_task->context->fs = GDT_USER_FS;
        }
    }
    if (next_task->user && next_task->linux_abi)
        syscall_linux_activate_ldt(next_task->process_id);
    else
        gdt_disable_user_ldt();
    if (next_task->user)
        (void)task_prepare_user_upcall(next_task->context);
    task_normalize_user_segments(next_task, next_task->context);
    paging_switch_process(next_task->idle ? 0U : next_task->process_id);
    TASK_SCHEDULE_FINISH(next_task->context);
#undef TASK_SCHEDULE_FINISH
}

registers_t *task_schedule(registers_t *frame) {
    return task_schedule_internal(frame, false, false, true);
}

registers_t *task_schedule_lapic(registers_t *frame) {
    return task_schedule_internal(frame, smp_cpu_index() != 0U, false, true);
}

registers_t *task_schedule_reschedule_ipi(registers_t *frame) {
    return task_schedule_internal(frame, true, true, false);
}

registers_t *task_schedule_voluntary(registers_t *frame) {
    return task_schedule_internal(frame, false, true, false);
}

void task_yield(void) {
    uint32_t flags;
    int index;
    task_t *current;
    kernel_domain_snapshot_t domain_snapshot;

    if (task_running_in_user_cpl()) {
        (void)syscall0(SYS_YIELD);
        return;
    }

    index = current_index;
    current = task_current_entry();
    if (!current) return;

    __asm__ volatile ("pushfl; popl %0" : "=r"(flags) : : "memory", "cc");
    if (!current->idle && !preempt_depth &&
        (flags & X86_EFLAGS_IF)) {
        /* Native API calls, mutex waiters and Linux IPC must give runnable
         * peers the vCPU immediately, including on UP. HLT adds a timer
         * quantum to each exchange despite runnable peers.
         * Kernel-origin int80 uses native numbering for Linux tasks too. */
        if (current->user) current->kernel_switch_safe = true;
        kernel_domains_drop_current(&domain_snapshot);
        (void)syscall0(SYS_YIELD);
        if (current->user) current->kernel_switch_safe = false;
        kernel_domains_restore(&domain_snapshot);
        return;
    }

    if (!smp_scheduler_started()) {
        if (current->idle || current->state != TASK_RUNNING || index < 0) {
            __asm__ volatile ("sti; hlt");
            return;
        }
        {
            uint32_t now = pit_get_ticks();
            if (yield_tick[index] != now) {
                yield_tick[index] = now;
                yield_budget[index] = TASK_YIELD_BUDGET_PER_TICK;
            }
            if (yield_budget[index] > 0U) {
                yield_budget[index]--;
                __asm__ volatile ("pause");
                return;
            }
        }
        __asm__ volatile ("sti; hlt");
        return;
    }

    __asm__ volatile ("pushfl; popl %0" : "=r"(flags) : : "memory", "cc");
    if (current->idle || current->state != TASK_RUNNING || index < 0) {
        if (current->user) current->kernel_switch_safe = true;
        kernel_domains_drop_current(&domain_snapshot);
        if (flags & X86_EFLAGS_IF)
            __asm__ volatile ("sti; hlt; cli" : : : "memory");
        else
            __asm__ volatile ("pause" : : : "memory");
        if (current->user) current->kernel_switch_safe = false;
        if (flags & X86_EFLAGS_IF) sti(); else cli();
        if (!current->idle) kernel_domains_restore(&domain_snapshot);
        return;
    }

    {
        uint32_t now = pit_get_ticks();
        if (yield_tick[index] != now) {
            yield_tick[index] = now;
            yield_budget[index] = TASK_YIELD_BUDGET_PER_TICK;
        }
    }
    if (current->user) current->kernel_switch_safe = true;
    kernel_domains_drop_current(&domain_snapshot);
    if (yield_budget[index] > 0U) {
        yield_budget[index]--;
        __asm__ volatile ("pause" : : : "memory");
    } else if (flags & X86_EFLAGS_IF) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    } else {
        __asm__ volatile ("pause" : : : "memory");
    }
    if (current->user) current->kernel_switch_safe = false;
    /* HLT returns through CLI above. Restore the caller's interrupt state
     * before reacquiring sleeping domain mutexes: their owner may have been
     * preempted on this CPU while we yielded. With IF clear kmutex_lock spins
     * forever instead of allowing that owner to run. */
    if (flags & X86_EFLAGS_IF) sti(); else cli();
    kernel_domains_restore(&domain_snapshot);
}

void task_sleep(uint32_t ticks) {
    task_t *current;
    if (task_running_in_user_cpl()) {
        (void)syscall1(SYS_SLEEP, ticks ? ticks : 1U);
        return;
    }
    current = task_current_entry();
    if (!current || current->idle) return;
    if (ticks == 0U) ticks = 1U;
    /* Native kernel workers must hand off immediately. HLT here stalls the
     * entire vCPU even when an application is runnable, and an unrelated IRQ
     * can return before the requested deadline. Use the saved interrupt frame
     * path already used by user SYS_SLEEP. Retain the non-switchable fallback
     * for preemption-disabled / IRQ-off callers. */
    uint32_t sleep_flags;
    __asm__ volatile ("pushfl; popl %0" : "=r"(sleep_flags));
    if (!preempt_depth && (sleep_flags & X86_EFLAGS_IF)) {
        kernel_domain_snapshot_t snapshot;
        if (current->user) current->kernel_switch_safe = true;
        kernel_domains_drop_current(&snapshot);
        (void)syscall1(SYS_SLEEP, ticks);
        if (current->user) current->kernel_switch_safe = false;
        kernel_domains_restore(&snapshot);
        return;
    }
    {
        uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
        current->wake_tick = pit_get_ticks() + ticks;
        current->state = TASK_SLEEPING;
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
    }
    task_yield();
}

void task_sleep_from_interrupt(uint32_t ticks) {
    task_t *current = task_current_entry();
    uint32_t flags;
    if (!current || current->idle) return;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    current->wake_tick = pit_get_ticks() + (ticks ? ticks : 1U);
    current->state = TASK_SLEEPING;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
}

/* Compatibility subsystems (notably Wine's AF_UNIX transport) need to
 * sleep until another kernel endpoint changes state. Polling with
 * task_sleep(1) adds a full 10 ms at the default 100 Hz PIT to every unlucky
 * RPC handoff. These helpers provide a tiny generic event primitive without
 * exposing scheduler internals to syscall.c.
 *
 * prepare/commit is deliberately two-phase. A producer may signal after the
 * caller has observed its condition but before it actually sleeps; fired is
 * latched in that window so the wake cannot be lost. */
bool task_event_wait_prepare(uint32_t key) {
    task_t *current = task_current_entry();
    uint32_t flags;
    if (!key || !current || current->idle) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    current->event_wait_key = key;
    current->event_wait_fired = false;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return true;
}

bool task_event_wait_commit(uint32_t key, uint32_t timeout_ticks) {
    task_t *current = task_current_entry();
    uint32_t flags;
    bool fired;
    if (!key || !current || current->idle) return false;
    if (!timeout_ticks) timeout_ticks = 1U;

    flags = kspin_lock_irqsave(&g_task_table_lock);
    if (current->event_wait_key != key) {
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        return false;
    }
    if (current->event_wait_fired) {
        current->event_wait_key = 0U;
        current->event_wait_fired = false;
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        return true;
    }
    current->wake_tick = pit_get_ticks() + timeout_ticks;
    current->state = TASK_SLEEPING;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);

    /* task_yield() switches away because state is no longer RUNNING. A
     * producer on either CPU can now turn this task READY immediately. */
    task_yield();

    flags = kspin_lock_irqsave(&g_task_table_lock);
    fired = current->event_wait_fired;
    if (current->event_wait_key == key) current->event_wait_key = 0U;
    current->event_wait_fired = false;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return fired;
}

void task_event_wait_cancel(uint32_t key) {
    task_t *current = task_current_entry();
    uint32_t flags;
    if (!current || !key) return;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    if (current->event_wait_key == key) {
        current->event_wait_key = 0U;
        current->event_wait_fired = false;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
}

uint32_t task_event_signal(uint32_t key) {
    int wake_slots[TASK_MAX];
    uint32_t wake_count = 0U;
    uint32_t running_mask = 0U;
    uint32_t matched = 0U;
    uint32_t flags;
    if (!key) return 0U;

    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; ++i) {
        task_t *task = &tasks[i];
        if (task->event_wait_key != key || task->state == TASK_UNUSED ||
            task->state == TASK_ZOMBIE) continue;
        task->event_wait_fired = true;
        matched++;
        if (task->state != TASK_SLEEPING) continue;
        if (task->running_cpu == -1) {
            task->state = TASK_READY;
            if (task->queued_cpu < 0 && wake_count < TASK_MAX)
                wake_slots[wake_count++] = i;
        } else {
            /* The waiter may still own its kernel stack between commit() and
             * task_yield(). Let it leave that stack first; the normal sleeper
             * wake path will publish READY on the next safe scheduler pass. */
            task->wake_tick = pit_get_ticks();
            if (task->running_cpu >= 0 && task->running_cpu < 32)
                running_mask |= 1U << (uint32_t)task->running_cpu;
        }
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);

    if (smp_scheduler_started()) {
        for (uint32_t i = 0U; i < wake_count; ++i)
            (void)scheduler_smp_enqueue(wake_slots[i], -1, true);
        smp_reschedule_mask(running_mask);
    }
    return matched;
}

void task_exit_status(int32_t status) {
    task_t *current;
    if (task_running_in_user_cpl()) {
        (void)syscall1(SYS_EXIT, (uint32_t)status);
        for (;;) (void)syscall0(SYS_YIELD);
    }
    current = task_current_entry();
    if (!current || current->idle) {
        for (;;) __asm__ volatile ("sti; hlt");
    }
    kernel_domains_abandon_current();
    {
        uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
        current->exit_code = status;
        current->state = TASK_ZOMBIE;
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
    }
    for (;;) task_yield();
}

void task_exit(void) {
    task_exit_status(0);
}

void task_exit_from_interrupt(int32_t status) {
    task_t *current = task_current_entry();
    uint32_t flags;
    if (!current || current->idle) return;
    if (current->linux_abi && status != 0) {
        kprintf("[LINUX:EXIT] pid=%u proc=%u ppid=%u name=%s status=%d thread=%u\n",
                current->pid, current->process_id, current->parent_pid,
                current->name, status, current->thread ? 1U : 0U);
    }
    kernel_domains_abandon_current();
    if(current->linux_abi)linux_robust_exit(current);
    if (g_linux_clear_child_tid && current>=tasks &&
        current<tasks+TASK_MAX &&
        g_linux_clear_child_tid[(uint32_t)(current-tasks)]) {
        uint32_t zero = 0U;
        uint32_t slot=(uint32_t)(current-tasks);
        (void)copy_to_user((void *)(uintptr_t)g_linux_clear_child_tid[slot],
                           &zero, sizeof(zero));
        (void)task_linux_futex_wake(g_linux_clear_child_tid[slot],1,~0U,false);
        g_linux_clear_child_tid[slot]=0U;
    }
    flags = kspin_lock_irqsave(&g_task_table_lock);
    current->exit_code = status;
    current->state = TASK_ZOMBIE;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
}

void task_allow_kernel_switch_once(void) {
    if (current_index < 0 || current_index >= TASK_MAX) return;
    if (tasks[current_index].user)
        tasks[current_index].kernel_switch_safe = true;
}

uint32_t task_kernel_stack_remaining(void) {
    uint32_t stack_pointer;
    uintptr_t low, high;
    int index = current_index;

    if (index < 0 || index >= TASK_MAX || !tasks[index].stack ||
        tasks[index].stack_size == 0U)
        return 0xFFFFFFFFU;

    __asm__ volatile ("movl %%esp, %0" : "=r"(stack_pointer));
    low = (uintptr_t)tasks[index].stack;
    high = low + tasks[index].stack_size;
    if ((uintptr_t)stack_pointer < low || (uintptr_t)stack_pointer > high)
        return 0U;
    return (uint32_t)((uintptr_t)stack_pointer - low);
}

void task_user_api_guard_enter(const char *name, uint32_t target,
                               uint32_t token) {
    task_t *task;
    if (current_index < 0 || current_index >= TASK_MAX) return;
    task = &tasks[current_index];
    task->api_guard_name = name ? name : "(API sin nombre)";
    task->api_guard_target = target;
    task->api_guard_token = token;
    task->api_guard_depth++;
}

void task_user_api_guard_leave(void) {
    task_t *task;
    if (current_index < 0 || current_index >= TASK_MAX) return;
    task = &tasks[current_index];
    if (task->api_guard_depth) task->api_guard_depth--;
}

bool task_user_api_guard_info(const char **name_out, uint32_t *target_out,
                              uint32_t *token_out) {
    task_t *task;
    if (current_index < 0 || current_index >= TASK_MAX) return false;
    task = &tasks[current_index];
    if (!task->api_guard_depth) return false;
    if (name_out) *name_out = task->api_guard_name
        ? task->api_guard_name : "(API sin nombre)";
    if (target_out) *target_out = task->api_guard_target;
    if (token_out) *token_out = task->api_guard_token;
    return true;
}

void task_user_api_guard_clear(void) {
    task_t *task;
    if (current_index < 0 || current_index >= TASK_MAX) return;
    task = &tasks[current_index];
    task->api_guard_depth = 0U;
    task->api_guard_target = 0U;
    task->api_guard_token = 0U;
}

void task_preempt_disable(void) {
    preempt_depth++;
}

void task_preempt_enable(void) {
    if (preempt_depth) preempt_depth--;
}

bool task_request_exit(uint32_t pid) {
    bool result = false;
    int wake_slot = -1;
    int32_t running_cpu = -1;
    uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid != pid || tasks[i].state == TASK_UNUSED) continue;
        if (tasks[i].system) break;
        if(tasks[i].state==TASK_STARTING) {
            tasks[i].state=TASK_ZOMBIE;tasks[i].parent_pid=0;tasks[i].thread=true;
        }
        tasks[i].exit_requested = true;
        running_cpu = tasks[i].running_cpu;
        if (tasks[i].state == TASK_SLEEPING) {
            if (running_cpu == -1) {
                tasks[i].state = TASK_READY;
                wake_slot = i;
            } else {
                tasks[i].wake_tick = pit_get_ticks();
            }
        }
        result = true;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    if (wake_slot >= 0 && smp_scheduler_started())
        (void)scheduler_smp_enqueue(wake_slot, -1, true);
    else if (running_cpu >= 0 && smp_scheduler_started())
        smp_reschedule_cpu((uint32_t)running_cpu);
    return result;
}

bool task_request_exit_process(uint32_t process_id, int32_t exit_code) {
    int wake_slots[TASK_MAX];
    uint32_t wake_count = 0U;
    uint32_t running_mask = 0U;
    bool result = false;
    uint32_t flags;
    if (!process_id) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].process_id != process_id ||
            tasks[i].state == TASK_UNUSED || tasks[i].system)
            continue;
        tasks[i].exit_code = exit_code;
        tasks[i].exit_requested = true;
        if (tasks[i].running_cpu >= 0 && tasks[i].running_cpu < 32)
            running_mask |= 1U << (uint32_t)tasks[i].running_cpu;
        if (tasks[i].state == TASK_SLEEPING) {
            if (tasks[i].running_cpu == -1) {
                tasks[i].state = TASK_READY;
                if (wake_count < TASK_MAX) wake_slots[wake_count++] = i;
            } else {
                tasks[i].wake_tick = pit_get_ticks();
            }
        }
        result = true;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    if (smp_scheduler_started()) {
        for (uint32_t i = 0U; i < wake_count; i++)
            (void)scheduler_smp_enqueue(wake_slots[i], -1, true);
        smp_reschedule_mask(running_mask);
    }
    return result;
}

bool task_query_pid(uint32_t pid, bool *active, int32_t *exit_code,
                    uint32_t *process_id) {
    bool found = false;
    uint32_t flags;
    if (active) *active = false;
    if (exit_code) *exit_code = 0;
    if (process_id) *process_id = 0U;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid != pid || tasks[i].state == TASK_UNUSED) continue;
        found = true;
        if (active) *active = tasks[i].state == TASK_READY ||
            tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_SLEEPING ||
            tasks[i].state == TASK_STARTING;
        if (exit_code) *exit_code = tasks[i].exit_code;
        if (process_id) *process_id = tasks[i].process_id;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return found;
}

bool task_query_process(uint32_t process_id, bool *active,
                        int32_t *exit_code, uint32_t *main_pid) {
    bool found = false;
    bool any_active = false;
    int32_t status = 0;
    uint32_t main = 0U;
    uint32_t flags;
    if (active) *active = false;
    if (exit_code) *exit_code = 0;
    if (main_pid) *main_pid = 0U;
    if (!process_id) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].process_id != process_id || tasks[i].state == TASK_UNUSED)
            continue;
        found = true;
        if (!main || tasks[i].pid == process_id) {
            main = tasks[i].pid;
            status = tasks[i].exit_code;
        }
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING ||
            tasks[i].state == TASK_SLEEPING || tasks[i].state == TASK_STARTING)
            any_active = true;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    if (active) *active = any_active;
    if (exit_code) *exit_code = status;
    if (main_pid) *main_pid = main ? main : process_id;
    return found;
}

bool task_exit_requested(void) {
    task_t *task = task_current_entry();
    return task ? task->exit_requested : false;
}

void task_set_memory_hint(uint32_t bytes) {
    task_t *task = task_current_entry();
    if (!task || task->idle) return;
    task->memory_bytes = task->stack_size + task->user_stack_size + bytes;
}

uint32_t task_process_memory_bytes(uint32_t process_id) {
    uint64_t stacks = 0U;
    uint64_t hints = 0U;
    uint64_t heap;
    uint64_t total;
    uint32_t flags;

    if (!process_id) return 0U;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        uint32_t base;
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_ZOMBIE ||
            tasks[i].process_id != process_id) continue;
        base = tasks[i].stack_size + tasks[i].user_stack_size;
        stacks += base;
        if (tasks[i].memory_bytes > base)
            hints += tasks[i].memory_bytes - base;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    heap = mm_get_process_usage(process_id);
    total = heap > stacks + hints ? heap : stacks + hints;
    return total > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)total;
}

void task_bind_window(struct gui_window *window) {
    task_t *task = task_current_entry();
    if (task && !task->idle) task->window = window;
}

void task_fpu_prepare_secondary_cpu(void) {
    task_fpu_prepare_cpu();
    if (g_fpu_fxsr)
        __asm__ volatile ("fxrstor (%0)" : : "r"(g_initial_fpu_state) : "memory");
    else
        __asm__ volatile ("frstor (%0)" : : "r"(g_initial_fpu_state) : "memory");
}

static void task_name_idle_cpu(char *name, uint32_t cpu) {
    kstrcpy(name, "idle-cpu");
    if (cpu >= 10U) {
        name[8] = (char)('0' + ((cpu / 10U) % 10U));
        name[9] = (char)('0' + (cpu % 10U));
        name[10] = '\0';
    } else {
        name[8] = (char)('0' + cpu);
        name[9] = '\0';
    }
}

bool task_smp_prepare_cpu(uint32_t cpu_index, void *stack_allocation,
                          uint32_t *stack, uint32_t stack_size) {
    if (cpu_index == 0U || cpu_index >= SMP_MAX_CPUS) return false;
    return task_idle_initialize(cpu_index, stack_allocation, stack,
                                stack_size);
}

void task_smp_ap_online(uint32_t cpu_index) {
    task_cpu_state_t *state;
    task_t *idle;
    if (cpu_index == 0U || cpu_index >= SMP_MAX_CPUS) return;
    state = &cpu_states[cpu_index];
    idle = task_idle_for_cpu(cpu_index);
    if (!idle || !idle->stack) return;
    state->current_slot = -1;
    state->preempt_nesting = 0U;
    state->quantum_ticks = 0U;
    state->deferred_ready_slot = -1;
    idle->state = TASK_RUNNING;
    idle->running_cpu = (int32_t)cpu_index;
    idle->last_cpu = (uint8_t)cpu_index;
    tss_set_kernel_stack((uint32_t)(uintptr_t)
        ((uint8_t *)idle->stack + idle->stack_size));
}

bool task_smp_scheduler_start(void) {
    uint32_t online = smp_online_cpu_count();
    uint32_t all_mask;
    if (!scheduler_smp_ready()) return false;
    if (!online) online = 1U;
    if (online > SMP_MAX_CPUS) online = SMP_MAX_CPUS;
    scheduler_smp_reset(online);
    all_mask = scheduler_smp_all_mask();

    for (uint32_t cpu = 0U; cpu < online; cpu++) {
        task_t *idle = task_idle_for_cpu(cpu);
        if (idle) {
            idle->affinity_mask = cpu < 32U ? (1U << cpu) : 0U;
            idle->queued_cpu = -1;
            if (cpu != 0U) idle->state = TASK_READY;
        }
    }

    /* Seed only READY tasks. The GUI task is currently RUNNING on CPU0;
     * sleepers are inserted when their wake tick expires. */
    for (int slot = 0; slot < TASK_MAX; slot++) {
        task_t *task = &tasks[slot];
        task->queued_cpu = -1;
        if (task->state == TASK_UNUSED || task->state == TASK_ZOMBIE)
            continue;
        if (task->affinity_cpu >= 0)
            task->affinity_mask = 1U << (uint32_t)(uint8_t)task->affinity_cpu;
        else if (!task->affinity_mask)
            task->affinity_mask = all_mask;
        if (task->state == TASK_READY)
            (void)scheduler_smp_enqueue(slot, -1, false);
    }
    kprintf("[SCHED] runqueues por CPU activas; idle fuera de TASK_MAX (%u slots libres)\n",
            (uint32_t)TASK_MAX - task_count());
    return true;
}

bool task_current_is_idle(void) {
    task_t *task = task_current_entry();
    return task ? task->idle : true;
}

uint32_t task_current_cpu(void) { return smp_cpu_index(); }

uint32_t task_count(void) {
    uint32_t count = 0;
    uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_ZOMBIE) count++;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return count;
}

uint32_t task_snapshot_public(task_public_snapshot_t *out,
                              uint32_t capacity) {
    uint32_t count = 0U;
    uint32_t flags;
    if (!out || !capacity) return 0U;
    if (capacity > TASK_MAX) capacity = TASK_MAX;

    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX && count < capacity; i++) {
        task_public_snapshot_t *snapshot;
        uint32_t base;
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_ZOMBIE)
            continue;
        snapshot = &out[count++];
        snapshot->pid = tasks[i].pid;
        snapshot->process_id = tasks[i].process_id;
        kstrncpy(snapshot->name, tasks[i].name, sizeof(snapshot->name));
        snapshot->name[sizeof(snapshot->name) - 1U] = '\0';
        snapshot->state = tasks[i].state;
        snapshot->cpu_ticks = tasks[i].cpu_ticks;
        base = tasks[i].stack_size + tasks[i].user_stack_size;
        snapshot->stack_bytes = base;
        snapshot->memory_hint_bytes = tasks[i].memory_bytes > base
            ? tasks[i].memory_bytes - base : 0U;
        snapshot->system = tasks[i].system;
        snapshot->user = tasks[i].user;
        snapshot->exit_requested = tasks[i].exit_requested;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return count;
}

const task_t *task_get(uint32_t index) {
    uint32_t found = 0;
    uint32_t cpu = smp_cpu_index();
    const task_t *result = NULL;
    uint32_t flags;
    if (cpu >= SMP_MAX_CPUS) cpu = 0U;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_ZOMBIE) {
            if (found++ == index) {
                g_task_query_snapshot[cpu] = tasks[i];
                result = &g_task_query_snapshot[cpu];
                break;
            }
        }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    /* g_task_query_snapshot es memoria supervisor. Legacy Process Manager builds run
     * in Ring 3 and must receive the existing per-task user-heap mirror. */
    return task_user_export_snapshot(result);
}

uint32_t task_current_pid(void) {
    task_t *task = task_current_entry();
    return task && !task->idle ? task->pid : 0U;
}
uint32_t task_current_process_id(void) {
    task_t *task = task_current_entry();
    if (!task || task->idle) return 0U;
    return task->process_id ? task->process_id : task->pid;
}

bool task_process_user_range_occupied(uint32_t process_id,
                                      uint32_t address, uint32_t length) {
    uint32_t end, flags;
    bool occupied = false;
    if (!process_id || !length || address + length < address) return true;
    end = address + length;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 1; i < TASK_MAX; i++) {
        task_t *task = &tasks[i];
        uint32_t start, stack_end;
        if (task->process_id != process_id || !task->user || !task->user_stack ||
            task->state == TASK_UNUSED || task->state == TASK_ZOMBIE) continue;
        start = (uint32_t)(uintptr_t)task->user_stack;
        stack_end = start + task->user_stack_size;
        if (stack_end < start || (address < stack_end && start < end)) {
            occupied = true;
            break;
        }
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return occupied;
}

uint32_t task_current_parent_pid(void) {
    task_t *task = task_current_entry();
    return task && !task->idle ? task->parent_pid : 0U;
}
uint32_t task_current_console_route(void) {
    task_t *task = task_current_entry();
    return task && !task->idle ? task->console_route : 0U;
}
void task_set_current_console_route(uint32_t route) {
    task_t *task = task_current_entry();
    if (task && !task->idle) task->console_route = route;
}
const char *task_current_cwd(void) {
    task_t *task = task_current_entry();
    if (task && task->linux_abi && task->linux_signals) return task->linux_signals->shared->cwd;
    if (!task || task->idle || !task->cwd || !task->cwd[0]) return "/";
    return task->cwd;
}
bool task_set_current_cwd(const char *path) {
    task_t *task = task_current_entry();
    char *copy;
    size_t length;
    if (!task || task->idle || !path || !path[0]) return false;
    length = kstrlen(path);
    if (length >= TASK_CWD_LEN) return false;
    if(task->linux_abi) {
        if(!task->linux_signals && !linux_signal_init(task,NULL))return false;
        kmemcpy(task->linux_signals->shared->cwd,path,length+1);return true;
    }
    copy = (char *)kmalloc(length + 1U);
    if (!copy) return false;
    kmemcpy(copy, path, length);
    copy[length] = '\0';
    if (task->cwd) kfree(task->cwd);
    task->cwd = copy;
    return true;
}
bool task_current_is_user(void) {
    task_t *task = task_current_entry();
    return task ? task->user : false;
}
bool task_current_is_linux_abi(void) {
    task_t *task = task_current_entry();
    return task ? task->linux_abi : false;
}
bool task_set_current_linux_abi(bool enabled) {
    task_t *task = task_current_entry();
    if (!task || task->idle || !task->user) return false;
    task->linux_abi = enabled;
    return true;
}
bool task_set_process_linux_abi(uint32_t process_id, bool enabled) {
    bool found = false;
    uint32_t flags;
    if (!process_id) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *task = &tasks[i];
        if (task->process_id != process_id || !task->user ||
            task->state == TASK_UNUSED || task->state == TASK_ZOMBIE) continue;
        task->linux_abi = enabled;
        found = true;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return found;
}

/* Replace the native cdecl bootstrap frame of a suspended ELF process with
 * the initial i386 Linux process stack.  Keeping this operation in task.c
 * means the loader never needs to know how guarded user stacks are laid out.
 *
 *   argc, argv[], NULL, envp[], NULL, auxv(type,value)..., AT_NULL
 */
bool task_configure_linux_start(uint32_t process_id, const char *argv0,
                                uint32_t start_entry, uint32_t program_entry,
                                uint32_t phdr,
                                uint32_t phent, uint32_t phnum,
                                uint32_t interpreter_base,
                                uint32_t init_list) {
    enum { AT_NULL=0, AT_PHDR=3, AT_PHENT=4, AT_PHNUM=5, AT_PAGESZ=6,
           AT_BASE=7, AT_ENTRY=9, AT_UID=11, AT_EUID=12,
           AT_GID=13, AT_EGID=14, AT_FLAGS=8, AT_CLKTCK=17,
           AT_SECURE=23, AT_RANDOM=25, AT_EXECFN=31,
           AT_BLESK_INIT_LIST=0x6B01, LINUX_STACK_WORDS=40 };
    task_t *task = NULL;
    uint32_t flags;
    uint32_t top, bottom, string_address, argument_address, stack_address;
    uint32_t words[LINUX_STACK_WORDS];
    uint32_t random_words[4], random_address;
    uint32_t n = 0U;
    size_t path_length, argument_length = 0U;

    if (!process_id || !argv0 || !argv0[0] || !start_entry ||
        !program_entry) return false;
    path_length = kstrlen(argv0) + 1U;
    if (path_length > TASK_LAUNCH_ARG_LEN) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].process_id == process_id &&
            tasks[i].state == TASK_STARTING && tasks[i].user &&
            tasks[i].linux_abi && !tasks[i].thread) {
            task = &tasks[i];
            break;
        }
    }
    if (!task || !task->user_stack || !task->context) {
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        return false;
    }
    bottom = (uint32_t)(uintptr_t)task->user_stack;
    top = bottom + task->user_stack_size;
    if (task->launch_arg && task->launch_arg[0])
        argument_length = kstrlen(task->launch_arg) + 1U;
    string_address = top - (uint32_t)path_length;
    string_address &= ~3U;
    argument_address = (string_address - (uint32_t)argument_length) & ~3U;
    random_address=(argument_address-sizeof(random_words))&~15U;
    for (uint32_t i=0;i<4U;++i) random_words[i]=syscall_linux_random_word();

    words[n++] = argument_length ? 2U : 1U; /* argc */
    words[n++] = string_address;     /* argv[0] */
    if (argument_length) words[n++] = argument_address;
    words[n++] = 0U;                 /* argv terminator */
    words[n++] = 0U;                 /* empty envp */
    words[n++] = AT_PHDR;   words[n++] = phdr;
    words[n++] = AT_PHENT;  words[n++] = phent;
    words[n++] = AT_PHNUM;  words[n++] = phnum;
    words[n++] = AT_PAGESZ; words[n++] = 4096U;
    words[n++] = AT_BASE;   words[n++] = interpreter_base;
    words[n++] = AT_ENTRY;  words[n++] = program_entry;
    words[n++] = AT_UID;    words[n++] = 0U;
    words[n++] = AT_EUID;   words[n++] = 0U;
    words[n++] = AT_GID;    words[n++] = 0U;
    words[n++] = AT_EGID;   words[n++] = 0U;
    words[n++] = AT_FLAGS;  words[n++] = 0U;
    words[n++] = AT_CLKTCK; words[n++] = 100U;
    words[n++] = AT_SECURE; words[n++] = 0U;
    words[n++] = AT_RANDOM; words[n++] = random_address;
    words[n++] = AT_EXECFN; words[n++] = string_address;
    words[n++] = AT_BLESK_INIT_LIST; words[n++] = init_list;
    words[n++] = AT_NULL;   words[n++] = 0U;
    /* Preserve the ABI-required 16-byte alignment at process entry. */
    stack_address = (random_address - n * sizeof(uint32_t)) & ~15U;
    if (stack_address < bottom + 256U) {
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        return false;
    }
    kmemcpy((void *)(uintptr_t)string_address, argv0, path_length);
    if (argument_length)
        kmemcpy((void *)(uintptr_t)argument_address, task->launch_arg,
                argument_length);
    kmemcpy((void *)(uintptr_t)random_address, random_words, sizeof(random_words));
    kmemcpy((void *)(uintptr_t)stack_address, words,
            n * sizeof(uint32_t));
    task->context->eip = start_entry;
    task->context->useresp = stack_address;
    task->context->eax = task->context->ebx = task->context->ecx = 0U;
    task->context->edx = task->context->esi = task->context->edi = 0U;
    task->context->ebp = 0U;
    if(task->user_fs_base) {
        task->context->fs=GDT_USER_FS;
        task->context->gs=GDT_USER_FS;
    }
    task->context->eflags = 0x202U;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return true;
}

static bool task_upcall_is_mouse_move(const task_upcall_t *upcall) {
    const gui_event_t *event;
    if (!upcall || (upcall->payload_argument != 1 &&
                    upcall->payload_argument != 2) ||
        upcall->payload_size != sizeof(gui_event_t)) return false;
    event = (const gui_event_t *)(const void *)upcall->payload;
    return event->type == GUI_EVENT_MOUSE_MOVE;
}

static gui_window_t *task_upcall_window(const task_upcall_t *upcall) {
    return upcall ? (gui_window_t *)upcall->window_target : NULL;
}

static uint8_t task_upcall_queue_count(const task_t *task) {
    if (!task) return 0U;
    return (uint8_t)((task->upcall_head + TASK_UPCALL_QUEUE -
                      task->upcall_tail) % TASK_UPCALL_QUEUE);
}

/* Remove one queued mouse move while preserving FIFO order for every other
 * callback.  Mouse positions are state samples, not transactions; keeping an
 * old sample is less important than admitting a click, key or paint callback. */
static bool task_upcall_remove_oldest_mouse_move(task_t *task) {
    uint8_t index;
    if (!task) return false;
    index = task->upcall_tail;
    while (index != task->upcall_head) {
        if (task_upcall_is_mouse_move(&task->upcalls[index])) {
            uint8_t current = index;
            uint8_t next = (uint8_t)((current + 1U) % TASK_UPCALL_QUEUE);
            while (next != task->upcall_head) {
                task->upcalls[current] = task->upcalls[next];
                current = next;
                next = (uint8_t)((next + 1U) % TASK_UPCALL_QUEUE);
            }
            task->upcall_head = (uint8_t)((task->upcall_head +
                TASK_UPCALL_QUEUE - 1U) % TASK_UPCALL_QUEUE);
            return true;
        }
        index = (uint8_t)((index + 1U) % TASK_UPCALL_QUEUE);
    }
    return false;
}

static bool task_queue_user_upcall_internal(
                            uint32_t pid, uint32_t entry,
                            const uint32_t *arguments, uint8_t argument_count,
                            const void *payload, uint8_t payload_size,
                            int8_t payload_argument,
                            void *release_after_upcall,
                            gui_window_t *window_target) {
    task_t *task = NULL;
    int task_slot = -1;
    bool woke = false;
    int32_t running_cpu = -1;
    uint8_t next;
    uint32_t table_flags;
    bool mouse_move = false;

    if (!pid || !entry || argument_count > TASK_UPCALL_ARGS ||
        payload_size > TASK_UPCALL_PAYLOAD ||
        (payload_size && (!payload || payload_argument < 0 ||
                          payload_argument >= argument_count))) return false;
    if ((payload_argument == 1 || payload_argument == 2) &&
        payload_size == sizeof(gui_event_t) && payload)
        mouse_move = ((const gui_event_t *)payload)->type ==
                     GUI_EVENT_MOUSE_MOVE;
    table_flags = kspin_lock_irqsave(&g_task_table_lock);
    /* A GUI callback is owned by a thread ID, not by an arbitrary sibling of
     * the same process.  Prefer the exact TID first.  The process-ID fallback
     * is retained for legacy Win32/process-wide callbacks whose original
     * owner thread has already disappeared. */
    for (int pass = 0; pass < 2 && !task; pass++) {
        for (int i = 1; i < TASK_MAX; i++) {
            bool matches = pass == 0 ? tasks[i].pid == pid
                                     : tasks[i].process_id == pid;
            if (!matches || !tasks[i].user ||
                tasks[i].state == TASK_UNUSED ||
                tasks[i].state == TASK_ZOMBIE) continue;
            task = &tasks[i];
            task_slot = i;
            break;
        }
    }
    if (!task) {
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        return false;
    }
    if (window_target && window_target->destroy_state != 0U) {
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        return false;
    }
    if (mouse_move) {
        uint8_t index = task->upcall_tail;
        while (index != task->upcall_head) {
            task_upcall_t *queued = &task->upcalls[index];
            if (task_upcall_is_mouse_move(queued) &&
                queued->entry == entry &&
                queued->payload_argument == payload_argument &&
                queued->argument_count == argument_count &&
                (!argument_count || !arguments ||
                 queued->arguments[0] == arguments[0])) {
                for (uint8_t i = 0; i < argument_count; i++)
                    queued->arguments[i] = arguments ? arguments[i] : 0U;
                kmemcpy(queued->payload, payload, payload_size);
                kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
                return true;
            }
            index = (uint8_t)((index + 1U) % TASK_UPCALL_QUEUE);
        }
        /* Reserve a few entries for release/click/key/paint callbacks. A
         * dropped intermediate move is harmless because the desktop already
         * stores the newest absolute cursor coordinates. */
        if (task_upcall_queue_count(task) >= TASK_UPCALL_QUEUE - 4U) {
            kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
            return true;
        }
    }

    next = (uint8_t)((task->upcall_head + 1U) % TASK_UPCALL_QUEUE);
    if (next == task->upcall_tail && !mouse_move &&
        task_upcall_remove_oldest_mouse_move(task))
        next = (uint8_t)((task->upcall_head + 1U) % TASK_UPCALL_QUEUE);
    if (next == task->upcall_tail) {
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        return mouse_move;
    }
    task_upcall_t *upcall = &task->upcalls[task->upcall_head];
    kmemset(upcall, 0, sizeof(*upcall));
    upcall->entry = entry;
    upcall->argument_count = argument_count;
    upcall->payload_argument = payload_argument;
    upcall->payload_size = payload_size;
    upcall->release_after_upcall = release_after_upcall;
    upcall->window_target = window_target;
    for (uint8_t i = 0; i < argument_count; i++)
        upcall->arguments[i] = arguments ? arguments[i] : 0U;
    if (payload_size) kmemcpy(upcall->payload, payload, payload_size);
    /*
     * Los callbacks GUI Ring 3 pueden cerrar su propia ventana, así que al
     * volver ya no es seguro desreferenciar arguments[0]. Guarde ahora una
     * copia de sus límites. Esto reemplaza la invalidación histórica de toda
     * la pantalla por el rectángulo afectado, sin usar un puntero posiblemente
     * destruido después del callback.
     */
    /* BLES_GRAPHICS_GUI_PERF_V1: mouse motion is input, not damage. Kernel widgets
     * already invalidate their exact hover geometry; a Ring-3 client that
     * changes pixels on motion must explicitly request invalidation. */
    if (window_target &&
        (payload_argument == -1 || payload_argument == 1) &&
        !mouse_move &&
        !(window_target->streaming_content &&
          payload_argument == 1 &&
          payload_size == sizeof(gui_event_t))) {
        /* BLESKERNOS_STREAM_INPUT_NO_IMPLICIT_DAMAGE_V9_1
         * Conserva la politica de v1: mover el mouse nunca implica dano.
         * Ademas, en una ventana streaming, click/tecla/focus solo entregan
         * input; el cliente publica su siguiente frame explicitamente. */
        upcall->invalidate_valid = true;
        upcall->invalidate_x = window_target->bounds.x;
        upcall->invalidate_y = window_target->bounds.y;
        upcall->invalidate_w = window_target->bounds.w;
        upcall->invalidate_h = window_target->bounds.h;
    }
    task->upcall_head = next;
    running_cpu = task->running_cpu;
    if (task->state == TASK_SLEEPING) {
        if (running_cpu == -1) {
            task->state = TASK_READY;
            woke = true;
        } else {
            /* The callback arrived while SYS_SLEEP still owns this task's
             * kernel stack. Ask that CPU to reschedule, then let the BSP wake
             * it only after running_cpu becomes -1. */
            task->wake_tick = pit_get_ticks();
        }
    }
    if (TASK_TRACE_DLLMAIN && payload_argument == -5) {
        kprintf("[UPCALL:DLLMAIN] QUEUE pid=%u entry=%x module=%x reason=%u reserved=%x head=%u tail=%u\n",
                task->pid, entry,
                argument_count > 0U ? upcall->arguments[0] : 0U,
                argument_count > 1U ? upcall->arguments[1] : 0U,
                argument_count > 2U ? upcall->arguments[2] : 0U,
                (uint32_t)task->upcall_head,
                (uint32_t)task->upcall_tail);
    }
    kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
    if (smp_scheduler_started()) {
        if (woke && task_slot >= 0)
            (void)scheduler_smp_enqueue(task_slot, -1, true);
        else if (running_cpu >= 0)
            smp_reschedule_cpu((uint32_t)running_cpu);
    }
    return true;
}

bool task_queue_user_upcall(uint32_t pid, uint32_t entry,
                            const uint32_t *arguments, uint8_t argument_count,
                            const void *payload, uint8_t payload_size,
                            int8_t payload_argument) {
    return task_queue_user_upcall_internal(pid, entry, arguments,
                                           argument_count, payload,
                                           payload_size, payload_argument,
                                           NULL, NULL);
}

bool task_queue_window_upcall(struct gui_window *raw_window,
                              uint32_t pid, uint32_t entry,
                              const uint32_t *arguments,
                              uint8_t argument_count,
                              const void *payload, uint8_t payload_size,
                              int8_t payload_argument) {
    gui_window_t *window = (gui_window_t *)raw_window;
    if (!window) return false;
    return task_queue_user_upcall_internal(pid, entry, arguments,
                                           argument_count, payload,
                                           payload_size, payload_argument,
                                           NULL, window);
}

bool task_queue_user_upcall_owned(uint32_t pid, uint32_t entry,
                                  const uint32_t *arguments,
                                  uint8_t argument_count,
                                  void *release_after_upcall) {
    if (!release_after_upcall) return false;
    return task_queue_user_upcall_internal(pid, entry, arguments,
                                           argument_count, NULL, 0U, -6,
                                           release_after_upcall, NULL);
}

bool task_cancel_window_upcalls(struct gui_window *raw_window) {
    gui_window_t *window = (gui_window_t *)raw_window;
    bool active = false;
    bool cancelled_paint = false;

    if (!window) return true;
    for (int slot = 1; slot < TASK_MAX; slot++) {
        task_upcall_t kept[TASK_UPCALL_QUEUE];
        void *release[TASK_UPCALL_QUEUE];
        uint8_t kept_count = 0U;
        uint8_t release_count = 0U;
        uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
        task_t *task = &tasks[slot];
        uint8_t index;

        if (task->state == TASK_UNUSED) {
            kspin_unlock_irqrestore(&g_task_table_lock, flags);
            continue;
        }
        if (task->upcall_active &&
            task_upcall_window(&task->active_upcall) == window)
            active = true;

        index = task->upcall_tail;
        while (index != task->upcall_head) {
            task_upcall_t *upcall = &task->upcalls[index];
            bool matches = task_upcall_window(upcall) == window;
            if (matches) {
                if (upcall->payload_argument == -2)
                    cancelled_paint = true;
                if (upcall->release_after_upcall &&
                    release_count < TASK_UPCALL_QUEUE)
                    release[release_count++] = upcall->release_after_upcall;
            } else if (kept_count < TASK_UPCALL_QUEUE - 1U) {
                kept[kept_count++] = *upcall;
            }
            index = (uint8_t)((index + 1U) % TASK_UPCALL_QUEUE);
        }
        kmemset(task->upcalls, 0, sizeof(task->upcalls));
        for (uint8_t i = 0U; i < kept_count; i++)
            task->upcalls[i] = kept[i];
        task->upcall_tail = 0U;
        task->upcall_head = kept_count;
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        for (uint8_t i = 0U; i < release_count; i++)
            kfree(release[i]);
    }
    if (cancelled_paint) {
        window->content_pending = false;
        window->content_repaint = false;
        gui_window_end_content_paint(window);
    }
    return !active;
}

void task_block_user_upcalls(bool blocked) {
    tasks[current_index].user_upcalls_blocked=blocked;
}
bool task_prepare_user_upcall(registers_t *regs) {
    task_t *task = &tasks[current_index];
    uint32_t stack_limit;
    uint32_t payload_bytes;
    uint32_t needed;
    uint32_t stack;
    uint32_t user_payload = 0U;
    uint32_t table_flags;

    /*
     * Una tarea de usuario tambien puede ser interrumpida mientras ejecuta
     * una API proxy en Ring 0. Ese frame no contiene useresp/ss validos. No
     * retire el callback de la cola hasta volver a un frame Ring 3 real.
     *
     * CPU0 produce most GUI upcalls while the application can consume them on
     * another CPU.  head/tail and the selected entry therefore must be read as
     * one transaction; the old unlocked dequeue could observe a half-published
     * entry or race the producer into treating a live slot as free.
     */
    if (!regs || (regs->cs & 3U) != 3U || !task->user || !task->user_stack)
        return false;
    task_normalize_user_segments(task, regs);
    stack_limit = (uint32_t)(uintptr_t)task->user_stack;

    table_flags = kspin_lock_irqsave(&g_task_table_lock);
    if (task->user_upcalls_blocked || task->upcall_active || task->upcall_head == task->upcall_tail) {
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        return false;
    }
    payload_bytes =
        ((uint32_t)task->upcalls[task->upcall_tail].payload_size + 3U) & ~3U;
    needed = payload_bytes +
        ((uint32_t)task->upcalls[task->upcall_tail].argument_count + 1U) * 4U;
    if (regs->useresp < stack_limit ||
        regs->useresp - stack_limit < needed) {
        kspin_unlock_irqrestore(&g_task_table_lock, table_flags);
        return false;
    }
    task->active_upcall = task->upcalls[task->upcall_tail];
    task->upcall_tail = (uint8_t)((task->upcall_tail + 1U) % TASK_UPCALL_QUEUE);
    task->upcall_saved_context = *regs;
    task->upcall_active = true;
    kspin_unlock_irqrestore(&g_task_table_lock, table_flags);

    task_fpu_begin_upcall(task);
    stack = regs->useresp;
    if (task->active_upcall.payload_size &&
        task->active_upcall.payload_argument >= 0) {
        /* active_upcall lives in the supervisor-only task table. Passing its
         * payload address to Ring 3 made every GUI event fault as soon as the
         * application read event->type. Keep the callback ABI unchanged, but
         * materialize the payload in the interrupted task's own user stack. */
        payload_bytes =
            ((uint32_t)task->active_upcall.payload_size + 3U) & ~3U;
        stack -= payload_bytes;
        user_payload = stack;
        kmemset((void *)(uintptr_t)user_payload, 0, payload_bytes);
        kmemcpy((void *)(uintptr_t)user_payload,
                task->active_upcall.payload,
                task->active_upcall.payload_size);
        task->active_upcall.arguments[
            (uint8_t)task->active_upcall.payload_argument] = user_payload;
    }
    for (int i = (int)task->active_upcall.argument_count - 1; i >= 0; i--) {
        stack -= 4U;
        *(uint32_t *)(uintptr_t)stack = task->active_upcall.arguments[i];
    }
    stack -= 4U;
    *(uint32_t *)(uintptr_t)stack =
        (uint32_t)(uintptr_t)g_user_upcall_return_gate;
    regs->eip = task->active_upcall.entry;
    regs->useresp = stack;
    regs->eax = 0;
    if (TASK_TRACE_DLLMAIN &&
        task->active_upcall.payload_argument == -5) {
        kprintf("[UPCALL:DLLMAIN] ENTER pid=%u entry=%x module=%x reason=%u useresp=%x return=%x\n",
                task->pid,
                task->active_upcall.entry,
                task->active_upcall.arguments[0],
                task->active_upcall.arguments[1],
                regs->useresp,
                (uint32_t)(uintptr_t)g_user_upcall_return_gate);
    }
    return true;
}

bool task_finish_user_upcall(registers_t *regs) {
    task_t *task = &tasks[current_index];
    gui_window_t *deferred_window = NULL;
    uint32_t callback_eax;

    if (!regs || !task->upcall_active) return false;
    {
        gui_window_t *candidate = task_upcall_window(&task->active_upcall);
        if (candidate && candidate->destroy_state == 1U)
            deferred_window = candidate;
    }
    callback_eax = regs->eax;

    /* The return trampoline preserves callback EAX in EBX. */
    if (task->active_upcall.payload_argument == -5)
        callback_eax = regs->ebx;

    /* BLES_WINE_DLL_DIAGNOSTICS_TASK_20260723 */
    if (TASK_TRACE_DLLMAIN &&
        task->active_upcall.payload_argument == -5) {
        kprintf("[UPCALL:DLLMAIN] RETURN pid=%u entry=%x module=%x reason=%u eax=%x (%s)\n",
                task->pid,
                task->active_upcall.entry,
                task->active_upcall.arguments[0],
                task->active_upcall.arguments[1],
                callback_eax,
                callback_eax ? "TRUE" : "FALSE");
    }
    if (task->active_upcall.payload_argument == -2 &&
        task->active_upcall.argument_count > 0) {
        gui_window_t *window = (gui_window_t *)(uintptr_t)
            task->active_upcall.arguments[0];
        if (window) {
            gui_surface_t *surface = task->active_upcall.argument_count > 1
                ? (gui_surface_t *)(uintptr_t)
                    task->active_upcall.arguments[1]
                : NULL;
            bool captured;
            bool direct_low_memory;
            bool first_content_frame = !window->content_ready;
            window->content_pending = false;
            direct_low_memory = window->content_staging_slot == -3;
            captured = gui_window_capture_content(window, surface);
            gui_window_end_content_paint(window);
            if (captured && direct_low_memory) {
                gui_gfx_present_rect(&gui_get_desktop()->surface,
                                     gui_window_content_rect(window));
                window->dirty = false;
            } else if (!captured || window->content_repaint) {
                window->dirty = true;
            } else if (first_content_frame) {
                /* Vision v3.6 keeps a persistent shell layer underneath the
                 * window.  The first client frame therefore needs only the
                 * window rectangle.  Invalidating the entire front shadow here
                 * touched every framebuffer pixel and dominated window-open
                 * latency even for tiny tools. */
                gui_desktop_invalidate_rect(gui_get_desktop(),
                                            window->bounds);
            } else {
                gui_desktop_invalidate_rect(gui_get_desktop(),
                                            window->bounds);
            }
            /* A mode change can invalidate a direct surface while its old
               callback is still returning. Ask for the replacement frame
               even on the no-cache path; otherwise the cleared background
               could remain visible until unrelated input arrives. */
            if (!direct_low_memory || !captured) gui_request_paint();
        }
    } else if (task->active_upcall.payload_argument == -3 &&
               task->active_upcall.argument_count > 1) {
        gui_program_t *program = (gui_program_t *)(uintptr_t)
            task->active_upcall.arguments[0];
        gui_desktop_t *desktop = (gui_desktop_t *)(uintptr_t)
            task->active_upcall.arguments[1];
        if (program) {
            gui_rect_t finished_clip;
            gui_program_finish_paint(program);
            /* A Ring-3 gui_program may repaint only a small dirty region.
             * Re-invalidating the complete desktop here turned every Vision
             * frame into an 800x600 copy/composite cycle and made normal
             * applications crawl.  The finished cache preserves the staging
             * clip, so publish only that region. */
            finished_clip = gui_gfx_get_clip(&program->paint_cache);
            if (desktop && finished_clip.w > 0 && finished_clip.h > 0)
                gui_desktop_publish_program_rect(desktop, program,
                                                 finished_clip);
        }
        gui_request_paint();
    } else if (task->active_upcall.payload_argument == -4 &&
               task->active_upcall.argument_count > 0) {
        gui_program_t *program = (gui_program_t *)(uintptr_t)
            task->active_upcall.arguments[0];
        gui_program_release_paint(program);
        kfree(program);
    } else if (task->active_upcall.payload_argument == 2 &&
               task->active_upcall.argument_count > 0) {
        /* A Ring-3 gui_program event changes user-owned shell state.  Merely
         * bumping the global paint generation is insufficient: the cached
         * program image is only regenerated when paint_requested is set. */
        gui_program_t *program = (gui_program_t *)(uintptr_t)
            task->active_upcall.arguments[0];
        gui_desktop_t *desktop = gui_get_desktop();
        (void)program;
        /* Do not request a new Vision frame merely because an input callback
         * returned.  The callback already calls gui_desktop_invalidate_rect()
         * whenever pixels actually changed, and that routine marks the scene
         * owner paint_requested.  The old unconditional assignment meant every
         * mouse-move over a window scheduled another Ring-3 scene pass even
         * when hover/cursor state was unchanged. */
        if (desktop && desktop->dirty_valid) gui_request_paint();
    } else if (task->active_upcall.payload_argument == -1 ||
               task->active_upcall.payload_argument == 1) {
        /* Menu, widget o evento de una ventana Ring 3. El callback puede haber
           cambiado su estado mientras el compositor terminaba otro frame; una
           dirty booleana sola podia ser limpiada por esa carrera. Convierta el
           resultado en una invalidacion generacional que no puede perderse. */
        /* El callback tambien puede haber cerrado/destruido la ventana, por
           lo que no se vuelve a desreferenciar arguments[0] al retornar. */
        gui_desktop_t *desktop = gui_get_desktop();
        if (desktop && task->active_upcall.invalidate_valid) {
            gui_desktop_invalidate_rect(desktop, (gui_rect_t){
                task->active_upcall.invalidate_x,
                task->active_upcall.invalidate_y,
                task->active_upcall.invalidate_w,
                task->active_upcall.invalidate_h
            });
            gui_request_paint();
        } else if (!task->active_upcall.window_target) {
            /* Generic/legacy callbacks still retain the historical fallback.
             * Native streaming mouse moves deliberately arrive here with a
             * window target but without invalidate_valid: the GUI loop already
             * moves/presents the cursor, and forcing a repaint would erase the
             * cache-less streaming client to its gray background. */
            gui_desktop_invalidate_all(desktop);
            gui_request_paint();
        }
    } else if (task->active_upcall.payload_argument == 2 &&
               task->active_upcall.argument_count > 1) {
        /* Legacy fallback for external gui_program events.  Keep existing
         * damage instead of promoting every event to a full desktop frame. */
        gui_desktop_t *desktop = (gui_desktop_t *)(uintptr_t)
            task->active_upcall.arguments[1];
        if (desktop && desktop->dirty_valid) gui_request_paint();
    }
    task_fpu_end_upcall(task);
    *regs = task->upcall_saved_context;
    task_normalize_user_segments(task, regs);
    {
        void *release_after_upcall;
        uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
        release_after_upcall = task->active_upcall.release_after_upcall;
        task->active_upcall.release_after_upcall = NULL;
        task->upcall_active = false;
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        if (release_after_upcall) kfree(release_after_upcall);
    }
    if (deferred_window && deferred_window->destroy_state == 1U)
        gui_window_destroy(deferred_window);
    return true;
}

int32_t task_thread_join_try(uint32_t tid, int32_t *status) {
    uint32_t process_id = task_current_process_id();
    uint32_t self = task_current_pid();
    int reap_index = -1;
    int32_t result = -BK_ECHILD;
    uint32_t flags;

    if (!tid || !process_id || tid == self) return -BK_EINVAL;
    flags = kspin_lock_irqsave(&g_task_table_lock);

    for (uint32_t i = 0U; i < TASK_MAX; i++) {
        task_thread_completion_t *completion = &g_thread_completions[i];
        if (!completion->valid || completion->tid != tid) continue;
        if (completion->process_id != process_id) {
            result = -BK_EACCES;
        } else {
            if (status) *status = completion->result;
            completion->valid = false;
            result = (int32_t)tid;
        }
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        return result;
    }

    for (int i = 1; i < TASK_MAX; i++) {
        task_t *thread = &tasks[i];
        if (thread->pid != tid || thread->state == TASK_UNUSED) continue;
        if (!thread->thread || thread->process_id != process_id) {
            result = -BK_EACCES;
            break;
        }
        if (!thread->thread_joinable) {
            result = -BK_EINVAL;
            break;
        }
        if (thread->state != TASK_ZOMBIE || thread->running_cpu != -1 ||
            thread->queued_cpu != -1 || g_task_reap_claim[i]) {
            result = 0;
            break;
        }
        if (status) *status = thread->exit_code;
        thread->thread_joinable = false;
        g_task_reap_claim[i] = 1U;
        reap_index = i;
        result = (int32_t)tid;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);

    if (reap_index >= 0) task_reap_claimed_index(reap_index);
    return result ? result : 0;
}

bool task_thread_detach(uint32_t tid) {
    uint32_t process_id = task_current_process_id();
    uint32_t flags;
    bool found = false;
    if (!tid || !process_id) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (uint32_t i = 0U; i < TASK_MAX; i++) {
        if (g_thread_completions[i].valid &&
            g_thread_completions[i].tid == tid &&
            g_thread_completions[i].process_id == process_id) {
            g_thread_completions[i].valid = false;
            found = true;
            break;
        }
    }
    if (!found) {
        for (int i = 1; i < TASK_MAX; i++) {
            if (tasks[i].pid != tid || tasks[i].state == TASK_UNUSED) continue;
            if (!tasks[i].thread || tasks[i].process_id != process_id) break;
            tasks[i].thread_joinable = false;
            found = true;
            break;
        }
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return found;
}

uint32_t task_current_tid(void) {
    return task_current_pid();
}

bool task_set_current_tls_base(uint32_t base) {
    task_t *task;
    uint32_t flags;
    /* FS is a per-CPU descriptor. Keep this task on the current CPU until
     * both its saved base and the live GDT descriptor agree. */
    task_preempt_disable();
    task = task_current_entry();
    if (!task || !task->user || task->idle) {
        task_preempt_enable();
        return false;
    }
    flags = kspin_lock_irqsave(&g_task_table_lock);
    task->user_fs_base = base;
    if (task->linux_abi) {
        task->linux_tls_base[0] = base;
        task->linux_tls_used_mask |= 1U;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    gdt_set_user_fs_base(base);
    task_preempt_enable();
    return true;
}

uint32_t task_current_tls_base(void) {
    task_t *task = task_current_entry();
    return task && task->user ? task->user_fs_base : 0U;
}

bool task_linux_set_thread_area(int32_t requested_entry, uint32_t base,
                                int32_t *actual_entry) {
    task_t *task = task_current_entry();
    uint32_t flags, slot;
    if (!task || !task->user || !task->linux_abi ||
        !actual_entry) return false;
    if (requested_entry == -1) {
        for (slot = 0U; slot < GDT_USER_TLS_COUNT; slot++)
            if (!(task->linux_tls_used_mask & (1U << slot))) break;
        if (slot == GDT_USER_TLS_COUNT) return false;
    } else {
        if (requested_entry < (int32_t)GDT_USER_TLS_MIN_ENTRY ||
            requested_entry >= (int32_t)(GDT_USER_TLS_MIN_ENTRY +
                                          GDT_USER_TLS_COUNT))
            return false;
        slot = (uint32_t)requested_entry - GDT_USER_TLS_MIN_ENTRY;
    }
    flags = kspin_lock_irqsave(&g_task_table_lock);
    task->linux_tls_base[slot] = base;
    task->linux_tls_used_mask |= (uint8_t)(1U << slot);
    if (slot == 0U) task->user_fs_base = base;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    gdt_set_user_tls_base(slot, base);
    *actual_entry = (int32_t)(GDT_USER_TLS_MIN_ENTRY + slot);
    return true;
}

bool task_linux_get_thread_area(int32_t entry, uint32_t *base) {
    task_t *task = task_current_entry();
    uint32_t flags, slot;
    if (!task || !task->user || !task->linux_abi || !base ||
        entry < (int32_t)GDT_USER_TLS_MIN_ENTRY ||
        entry >= (int32_t)(GDT_USER_TLS_MIN_ENTRY + GDT_USER_TLS_COUNT))
        return false;
    slot = (uint32_t)entry - GDT_USER_TLS_MIN_ENTRY;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    *base = task->linux_tls_base[slot];
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return true;
}

bool task_set_current_clear_child_tid(uint32_t address) {
    task_t *task = task_current_entry();
    uint32_t flags;
    if (!task || !task->user || !task->linux_abi)
        return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    if (!g_linux_clear_child_tid || task<tasks || task>=tasks+TASK_MAX) {
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        return false;
    }
    g_linux_clear_child_tid[(uint32_t)(task-tasks)] = address;
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return true;
}

uint32_t task_thread_count(uint32_t process_id) {
    uint32_t count = 0U;
    uint32_t flags;
    if (!process_id) process_id = task_current_process_id();
    if (!process_id) return 0U;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].process_id == process_id && tasks[i].thread &&
            tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_ZOMBIE)
            count++;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return count;
}

int32_t task_waitpid(uint32_t pid, int32_t *status) {
    uint32_t parent = task_current_process_id();
    bool child_exists = false;
    int index = -1;
    int32_t result = -1;
    int32_t child_status = 0;
    uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);

    /* The deferred reaper may already have recycled a short-lived child.
     * Consume its recorded completion before consulting the live table. */
    for (uint32_t i = 0U; i < TASK_MAX; i++) {
        task_process_completion_t *completion = &g_process_completions[i];
        if (!completion->valid || completion->parent_pid != parent) continue;
        if (pid != 0U && completion->pid != pid &&
            completion->process_id != pid) continue;
        if (status) *status = completion->result;
        result = (int32_t)completion->pid;
        completion->valid = false;
        /* El reaper puede seguir liberando el zombie en otro CPU. Una vez
         * consumido el estado ya no debe aparecer como hijo esperable. */
        for(int j=1;j<TASK_MAX;j++)
            if(tasks[j].pid==(uint32_t)result && tasks[j].parent_pid==parent &&
                tasks[j].state==TASK_ZOMBIE)tasks[j].parent_pid=0;
        kspin_unlock_irqrestore(&g_task_table_lock, flags);
        return result;
    }

    for (int i = 1; i < TASK_MAX; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED || child->parent_pid != parent ||
            child->thread)
            continue;
        if (pid != 0U && child->pid != pid && child->process_id != pid)
            continue;
        child_exists = true;
        if (child->state != TASK_ZOMBIE || child->running_cpu != -1 ||
            child->queued_cpu != -1 || g_task_reap_claim[i]) continue;
        g_task_reap_claim[i] = 1U;
        index = i;
        result = (int32_t)child->pid;
        child_status = child->exit_code;
        child->parent_pid=0;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);

    if (index < 0) return child_exists ? 0 : -1;
    if (status) *status = child_status;
    task_reap_claimed_index(index);
    return result;
}

bool task_linux_exec_stack(uint32_t pid,const linux_exec_args_t *args) {
    task_t *t=NULL;
    for(int i=1;i<TASK_MAX;i++) if(tasks[i].pid==pid && tasks[i].state==TASK_STARTING) {t=&tasks[i];break;}
    if(!t || !t->linux_abi || !args || args->argc+args->envc>128 || args->used>sizeof(args->data)) return false;
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stack fresh=%u user_stack=%x size=%u eip=%x esp=%x\n",
            pid,(uint32_t)(uintptr_t)t->user_stack,t->user_stack_size,
            t->context->eip,t->context->useresp);
#endif
    uint32_t *old=(uint32_t *)t->context->useresp,aux[64],n=0;
    if(old[0]!=1 || old[2] || old[3]) return false;
    do {
        if(n+2>64)return false;
        aux[n]=old[4+n];aux[n+1]=old[5+n];n+=2;
    } while(aux[n-2]);
    uint32_t data=(t->context->useresp-args->used)&~15U;
    uint32_t words=3+args->argc+args->envc+n;
    uint32_t bottom=(data-words*4)&~15U;
    if(bottom<(uint32_t)t->user_stack+4096 || data>t->context->useresp) return false;
    kmemcpy((void *)data,args->data,args->used);
    uint32_t *out=(uint32_t *)bottom;
    *out++=args->argc;
    for(uint32_t i=0;i<args->argc;i++)*out++=data+args->offsets[i];
    *out++=0;
    for(uint32_t i=0;i<args->envc;i++)*out++=data+args->offsets[args->argc+i];
    *out++=0;kmemcpy(out,aux,n*4);t->context->useresp=bottom;return true;
}

/* Commit de exec: la tarea vieja conserva la pila del kernel hasta que el
 * reaper pueda liberarla. La tarea preparada toma PID/parent del proceso;
 * la imagen retirada queda bajo el identificador temporal, sin wait zombie. */
bool task_linux_exec_commit(uint32_t pid) {
    task_t *old=task_current_entry(),*fresh=NULL;
    if(!old || !old->linux_abi || old->thread || task_thread_count(old->process_id))return false;
    for(int i=1;i<TASK_MAX;i++)if(tasks[i].pid==pid && tasks[i].state==TASK_STARTING){fresh=&tasks[i];break;}
    if(!fresh || !fresh->linux_abi || !linux_signal_init(fresh,NULL))return false;
    syscall_linux_exec_locks();
    uint32_t process=old->process_id;
    /* Pager I/O can sleep: exchange ownership before the IRQ-off task lock. */
    pager_swap_ids(process,pid);
    uint32_t lock=kspin_lock_irqsave(&g_task_table_lock);
    if(old->linux_signals) {
        fresh->linux_signals->mask=old->linux_signals->mask;
        for(unsigned i=0;i<64;i++)if(old->linux_signals->shared->actions[i].handler==1)
            fresh->linux_signals->shared->actions[i].handler=1;
        kmemcpy(fresh->linux_signals->queue,old->linux_signals->queue,sizeof(fresh->linux_signals->queue));
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] commit process=%u temp=%u step=mm-swap\n",process,pid);
#endif
    mm_swap_process_owners(process,pid);
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] commit process=%u temp=%u step=paging-swap\n",process,pid);
#endif
    paging_swap_process_ids(process,pid);
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] commit process=%u temp=%u step=elf-swap\n",process,pid);
#endif
    elf_swap_process_ids(process,pid);
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] commit process=%u temp=%u step=fd-swap\n",process,pid);
#endif
    syscall_swap_process_ids(process,pid);
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] commit process=%u temp=%u step=domains\n",process,pid);
#endif
    kernel_domains_abandon_current();
    fresh->pid=process;fresh->process_id=process;fresh->parent_pid=old->parent_pid;
    old->pid=pid;old->process_id=pid;old->parent_pid=0;old->thread=true;
    old->exit_code=0;old->state=TASK_ZOMBIE;
    g_linux_clear_child_tid[old-tasks]=0;g_linux_clear_child_tid[fresh-tasks]=0;
    /* Publish the replacement before restoring interrupts. The current task
     * is already a zombie: a timer interrupt can retire it permanently, so
     * no operation needed to start fresh may remain after the unlock.
     * task_start_suspended takes this recursive per-CPU lock and preserves
     * the disabled interrupt state while publishing/enqueuing fresh. */
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] commit process=%u temp=%u step=start\n",process,pid);
#endif
    {
        bool started=task_start_suspended(process);
#ifdef BLESK_WINE_VERBOSE_TRACE
        kprintf("[WINE:EXEC] commit process=%u temp=%u step=start-done ok=%u\n",
                process,pid,started?1U:0U);
#endif
        kspin_unlock_irqrestore(&g_task_table_lock,lock);
        return started;
    }
}

int32_t task_linux_wait(registers_t *r) {
    if ((r->edx&~1U) || ((int32_t)r->ebx<=0 && r->ebx!=0xffffffffU)) return -BK_EINVAL;
    if (r->eax==114 && r->esi) return -BK_ENOSYS;
    if (r->ecx && !user_access_ok((void *)r->ecx,4,true)) return -BK_EFAULT;
    int32_t status,result;
    for (;;) {
        result=task_waitpid(r->ebx==0xffffffffU?0:r->ebx,&status);
        if (result || (r->edx&1)) break;
        task_t *t=task_current_entry();
        linux_signals *s=t->linux_signals;
        if(s) for(unsigned i=0;i<32;i++) if(s->queue[i].sig &&
            !(s->mask&(1ULL<<(s->queue[i].sig-1)))) return -4;
        task_yield();
    }
    if (result<0) return -BK_ECHILD;
    if (result>0 && r->ecx && !copy_to_user((void *)r->ecx,&status,4)) return -BK_EFAULT;
    return result;
}

uint8_t task_cpu_usage(void) { return cpu_usage; }

uint32_t task_cpu_count(void) {
    uint32_t count = smp_online_cpu_count();
    if (!count) count = 1U;
    return count > SMP_MAX_CPUS ? SMP_MAX_CPUS : count;
}

uint8_t task_cpu_usage_for(uint32_t cpu_index) {
    if (cpu_index >= task_cpu_count()) return 0U;
    return cpu_usage_per_cpu[cpu_index];
}

uint32_t task_runqueue_depth(uint32_t cpu_index) {
    return scheduler_smp_depth(cpu_index);
}

uint32_t task_scheduler_steals(uint32_t cpu_index) {
    return scheduler_smp_steals(cpu_index);
}

uint32_t task_scheduler_migrations(uint32_t cpu_index) {
    return scheduler_smp_migrations(cpu_index);
}

uint32_t task_scheduler_ipis(uint32_t cpu_index) {
    return scheduler_smp_ipis(cpu_index);
}

bool task_set_affinity_mask(uint32_t pid, uint32_t mask) {
    int slot = -1;
    int32_t running = -1;
    bool was_queued = false;
    uint32_t allowed = scheduler_smp_all_mask();
    uint32_t flags;
    if (!pid) pid = task_current_pid();
    mask &= allowed;
    if (!pid || !mask) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *task = &tasks[i];
        if (task->pid != pid || task->state == TASK_UNUSED ||
            task->state == TASK_ZOMBIE || task->system) continue;
        if (task->linux_abi && smp_cpu_count() >= 2U) {
            mask &= 1U << 1U;
            if (!mask) {
                kspin_unlock_irqrestore(&g_task_table_lock, flags);
                return false;
            }
        }
        {
            uint32_t current_mask = task->affinity_cpu >= 0
                ? (1U << (uint32_t)(uint8_t)task->affinity_cpu)
                : (task->affinity_mask ? task->affinity_mask : allowed);
            if (current_mask == mask) {
                kspin_unlock_irqrestore(&g_task_table_lock, flags);
                return true;
            }
        }
        task->affinity_cpu = -1;
        task->affinity_mask = mask;
        task->preferred_cpu = (uint8_t)scheduler_smp_choose_cpu(
            task, task->last_cpu < SMP_MAX_CPUS ? task->last_cpu : 0U);
        running = task->running_cpu;
        was_queued = task->queued_cpu >= 0;
        slot = i;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    if (slot < 0) return false;
    if (was_queued) {
        scheduler_smp_remove(slot);
        (void)scheduler_smp_enqueue(slot, -1, true);
    } else if (running >= 0 &&
               ((mask & (1U << (uint32_t)running)) == 0U)) {
        smp_reschedule_cpu((uint32_t)running);
    } else if (smp_scheduler_started()) {
        /* Covers a dequeue/affinity race: the task may have left its queue
         * between the snapshot and scheduler_smp_remove(). */
        smp_reschedule_mask(scheduler_smp_all_mask());
    }
    return true;
}

uint32_t task_get_affinity_mask(uint32_t pid) {
    uint32_t result = 0U;
    uint32_t flags;
    if (!pid) pid = task_current_pid();
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid != pid || tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_ZOMBIE) continue;
        result = tasks[i].affinity_cpu >= 0
            ? (1U << (uint32_t)(uint8_t)tasks[i].affinity_cpu)
            : (tasks[i].affinity_mask ? tasks[i].affinity_mask
                                      : scheduler_smp_all_mask());
        if (tasks[i].linux_abi && smp_cpu_count() >= 2U)
            result = 1U << 1U;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return result;
}

const char *task_state_name(task_state_t state) {
    if (state == TASK_READY) return "Listo";
    if (state == TASK_RUNNING) return "Ejecutando";
    if (state == TASK_SLEEPING) return "Durmiendo";
    if (state == TASK_ZOMBIE) return "Terminado";
    return "Libre";
}

const char *task_launch_arg(void) {
    task_t *task = task_current_entry();
    return task && !task->idle && task->launch_arg ? task->launch_arg : "";
}


bool task_set_user_fs_base(uint32_t pid, uint32_t base) {
    bool changed = false;
    uint32_t flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid != pid || tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_ZOMBIE || !tasks[i].user) continue;
        tasks[i].user_fs_base = base;
        if (tasks[i].linux_abi) {
            tasks[i].linux_tls_base[0] = base;
            tasks[i].linux_tls_used_mask |= 1U;
        }
        if (tasks[i].context) {
            tasks[i].context->fs = base ? GDT_USER_FS : GDT_USER_DATA;
            if(tasks[i].linux_abi)
                tasks[i].context->gs=base ? GDT_USER_FS : GDT_USER_DATA;
        }
        changed = true;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return changed;
}

const char *task_user_export_string(const char *source) {
    task_t *task = task_current_entry();
    task_user_string_export_t *node;
    uint32_t length;
    uint32_t capacity;

    if (!source) source = "";
    if (!task || !task->user || task->idle) return source;

    length = (uint32_t)kstrlen(source) + 1U;
    if (!length || length > 65536U) return NULL;
    capacity = (length + 63U) & ~63U;

    node = (task_user_string_export_t *)task->user_string_exports;
    while (node) {
        if (node->source == source) {
            if (node->capacity < capacity) {
                char *replacement = (char *)krealloc(node->copy, capacity);
                if (!replacement) return NULL;
                node->copy = replacement;
                node->capacity = capacity;
            }
            kmemcpy(node->copy, source, length);
            return node->copy;
        }
        node = node->next;
    }

    if (task->user_string_export_count < TASK_USER_STRING_EXPORT_LIMIT) {
        node = (task_user_string_export_t *)kzalloc(sizeof(*node));
        if (!node) return NULL;
        node->copy = (char *)kmalloc(capacity);
        if (!node->copy) {
            kfree(node);
            return NULL;
        }
        node->source = source;
        node->capacity = capacity;
        node->next = (task_user_string_export_t *)task->user_string_exports;
        task->user_string_exports = node;
        task->user_string_export_count++;
        kmemcpy(node->copy, source, length);
        return node->copy;
    }

    /* Pathological callers that translate an unbounded number of distinct
     * pointers share one overflow slot instead of leaking until process exit. */
    node = (task_user_string_export_t *)task->user_string_overflow;
    if (!node) {
        node = (task_user_string_export_t *)kzalloc(sizeof(*node));
        if (!node) return NULL;
        task->user_string_overflow = node;
    }
    if (node->capacity < capacity) {
        char *replacement = (char *)krealloc(node->copy, capacity);
        if (!replacement) return NULL;
        node->copy = replacement;
        node->capacity = capacity;
    }
    node->source = source;
    kmemcpy(node->copy, source, length);
    return node->copy;
}

const task_t *task_user_export_snapshot(const task_t *source) {
    task_t *task = task_current_entry();
    task_t *snapshot;

    if (!source) return NULL;
    if (!task || !task->user || task->idle) return source;
    snapshot = (task_t *)task->user_task_snapshot;
    if (!snapshot) {
        snapshot = (task_t *)kmalloc(sizeof(*snapshot));
        if (!snapshot) return NULL;
        task->user_task_snapshot = snapshot;
    }
    kmemcpy(snapshot, source, sizeof(*snapshot));
    /* Never publish live kernel stack/context pointers through the legacy
     * snapshot. Old Process Manager builds only need metadata. */
    snapshot->stack = NULL;
    snapshot->user_stack = NULL;
    snapshot->stack_allocation = NULL;
    snapshot->user_stack_allocation = NULL;
    snapshot->fpu_state = NULL;
    snapshot->fpu_allocation = NULL;
    snapshot->upcall_fpu_state = NULL;
    snapshot->upcall_fpu_allocation = NULL;
    snapshot->context = NULL;
    snapshot->user_string_exports = NULL;
    snapshot->user_string_overflow = NULL;
    snapshot->user_task_snapshot = NULL;
    snapshot->linux_signals = NULL;
    return snapshot;
}

bool task_get_user_stack_bounds(uint32_t pid, uint32_t *limit_out,
                                uint32_t *base_out) {
    bool found = false;
    uint32_t flags;

    if (!limit_out || !base_out) return false;
    flags = kspin_lock_irqsave(&g_task_table_lock);
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid != pid || tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_ZOMBIE || !tasks[i].user ||
            !tasks[i].user_stack) continue;
        *limit_out = (uint32_t)(uintptr_t)tasks[i].user_stack;
        *base_out = (uint32_t)(uintptr_t)
            ((uint8_t *)tasks[i].user_stack + tasks[i].user_stack_size);
        found = true;
        break;
    }
    kspin_unlock_irqrestore(&g_task_table_lock, flags);
    return found;
}

const char *task_current_name(void) {
    task_t *task = task_current_entry();
    return task ? task->name : "kernel";
}

uint32_t task_process_priority(uint32_t pid, uint32_t level) {
    if(!pid || level>3U)return 0;
    uint32_t result=0,flags=kspin_lock_irqsave(&g_task_table_lock);
    for(int i=0;i<TASK_MAX;i++) {
        if(tasks[i].process_id!=pid || tasks[i].state==TASK_UNUSED || tasks[i].state==TASK_ZOMBIE)continue;
        if(level && tasks[i].system){kspin_unlock_irqrestore(&g_task_table_lock,flags);return 0;}
        result=tasks[i].priority?tasks[i].priority:2U;
    }
    if(result && level) {
        for(int i=0;i<TASK_MAX;i++)if(tasks[i].process_id==pid && tasks[i].state!=TASK_UNUSED && tasks[i].state!=TASK_ZOMBIE)tasks[i].priority=level;
        result=level;
    }
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    return result;
}
