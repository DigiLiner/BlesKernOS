#include "task_arm.h"
#include "timer.h"
#include "uart.h"
#include "mmu.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/libc/include/stdio.h"
#include "../../../gui/gui.h"

extern void rpi3_syscall_cleanup_process(uint32_t process_id);

#define ARM_TASK_MAX            32U
#define ARM_TASK_STACK_BYTES    (64U * 1024U)
#define ARM_TASK_STACK_GUARD    16U
#define ARM_TASK_STACK_CANARY   0xA53C4E59U
#define ARM_TASK_QUANTUM_TICKS  5U
#define ARM_CPSR_USER           0x10U
#define ARM_CPSR_SYSTEM         0x1FU
#define ARM_USER_ALLOC_MAX      128U
#define ARM_TASK_CWD_LEN        260U
#define ARM_TASK_LAUNCH_ARG_LEN 2048U

typedef struct arm_task_upcall {
    uint32_t entry;
    uint32_t arguments[TASK_UPCALL_ARGS];
    uint8_t argument_count;
    int8_t payload_argument;
    uint8_t payload_size;
    uint8_t payload[TASK_UPCALL_PAYLOAD];
    struct gui_window *window_target;
    bool invalidate_valid;
    int32_t invalidate_x, invalidate_y, invalidate_w, invalidate_h;
} arm_task_upcall_t;

typedef enum arm_task_state {
    ARM_TASK_UNUSED = 0,
    ARM_TASK_READY,
    ARM_TASK_RUNNING,
    ARM_TASK_SLEEPING,
    ARM_TASK_ZOMBIE,
    ARM_TASK_REAPING
} arm_task_state_t;

typedef struct arm_user_allocation {
    uint32_t address;
    uint32_t size;
    bool used;
} arm_user_allocation_t;

typedef struct arm_kernel_task {
    uint32_t pid;
    char name[TASK_NAME_LEN];
    arm_task_state_t state;
    bool idle;
    bool user;
    uint16_t preempt_depth;
    uint32_t wake_tick;
    uint32_t cpu_ticks;
    uint32_t switches;
    uint32_t process_id;
    uint32_t parent_pid;
    int32_t exit_code;
    uint32_t console_route;
    uint32_t memory_hint;
    uint32_t user_base;
    uint32_t user_bytes;
    uint32_t user_heap_next;
    uint32_t user_heap_limit;
    uint8_t user_slot;
    arm_user_allocation_t user_allocations[ARM_USER_ALLOC_MAX];
    void (*entry)(void *);
    void *argument;
    void *stack_allocation;
    uint32_t stack_bytes;
    char cwd[ARM_TASK_CWD_LEN];
    char launch_arg[ARM_TASK_LAUNCH_ARG_LEN];
    arm_task_upcall_t upcalls[TASK_UPCALL_QUEUE];
    uint8_t upcall_head;
    uint8_t upcall_tail;
    bool upcall_active;
    arm_task_upcall_t active_upcall;
    arm_task_frame_t upcall_saved_frame;
    uint32_t upcall_return_pc;
    arm_task_frame_t frame;
} arm_kernel_task_t;

static arm_kernel_task_t g_tasks[ARM_TASK_MAX];
static int32_t g_current = -1;
static uint32_t g_next_pid = 1U;
static uint32_t g_last_timer_tick;
static uint32_t g_quantum_start;
static uint32_t g_switches;
static uint32_t g_rr_cursor;
static bool g_initialized;
static bool g_started;
static volatile bool g_reschedule;
static bool g_preemption_reported;
static int32_t g_preempt_watch_slot = -1;
static uint32_t g_preempt_watch_since;
static bool g_preempt_watch_reported;
static bool g_user_slots[RPI3_USER_SLOT_COUNT];
static char g_early_cwd[ARM_TASK_CWD_LEN] = "/";

static void copy_name(char *dst, const char *src, uint32_t capacity) {
    uint32_t i = 0U;
    if (!dst || !capacity) return;
    if (!src) src = "task";
    while (src[i] && i + 1U < capacity) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void copy_cwd(char *dst, const char *src) {
    uint32_t i = 0U;
    if (!dst) return;
    if (!src || !src[0]) src = "/";
    while (src[i] && i + 1U < ARM_TASK_CWD_LEN) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool tick_reached(uint32_t now, uint32_t target) {
    return (int32_t)(now - target) >= 0;
}

static arm_kernel_task_t *current_task(void) {
    if (g_current < 0 || (uint32_t)g_current >= ARM_TASK_MAX) return NULL;
    return &g_tasks[g_current];
}

static void task_return_trap(void) NORETURN;

static void task_bootstrap(arm_kernel_task_t *task) NORETURN;
static void task_bootstrap(arm_kernel_task_t *task) {
    if (task && task->entry) task->entry(task->argument);
    task_exit();
}

static void task_return_trap(void) {
    task_exit();
}

static void release_task_resources(arm_kernel_task_t *task) {
    size_t released;
    if (!task || task->state == ARM_TASK_UNUSED) return;
    if (task->user) rpi3_task_user_slot_release(task->user_slot);
    if (task->process_id) {
        rpi3_syscall_cleanup_process(task->process_id);
        gui_desktop_cleanup_process(gui_get_desktop(), task->process_id);
        released = mm_release_process(task->process_id);
        if (released)
            kprintf("[MM:ARM] process %u reclaimed %u KiB\n",
                    task->process_id, (uint32_t)((released + 1023U) / 1024U));
    }
    if (task->stack_allocation) {
        kfree(task->stack_allocation);
        task->stack_allocation = NULL;
    }
}

static int allocate_slot(void) {
    uint32_t i;
    /* Prefer genuinely unused slots.  Older code immediately recycled the
       first zombie it encountered, which could erase a child's PID/exit_code
       before its parent had reached waitpid(). */
    for (i = 0U; i < ARM_TASK_MAX; i++)
        if (g_tasks[i].state == ARM_TASK_UNUSED) return (int)i;

    /* Detached kernel helpers still need eventual recycling.  Preserve user
       children until waitpid consumes their status. */
    for (i = 0U; i < ARM_TASK_MAX; i++) {
        if (g_tasks[i].state != ARM_TASK_ZOMBIE || g_tasks[i].parent_pid) continue;
        g_tasks[i].state = ARM_TASK_REAPING;
        /* GUI teardown may wait for the compositor mutex.  Do not wait while
           preemption is disabled or the task owning that mutex can never run. */
        if (g_started) task_preempt_enable();
        release_task_resources(&g_tasks[i]);
        if (g_started) task_preempt_disable();
        return (int)i;
    }
    return -1;
}

void rpi3_task_init(void) {
    uint32_t i;
    if (g_initialized) return;
    kmemset(g_tasks, 0, sizeof(g_tasks));
    kmemset(g_user_slots, 0, sizeof(g_user_slots));
    for (i = 0U; i < ARM_TASK_MAX; i++) g_tasks[i].state = ARM_TASK_UNUSED;
    g_current = -1;
    g_next_pid = 1U;
    g_last_timer_tick = rpi3_timer_ticks();
    g_quantum_start = g_last_timer_tick;
    g_switches = 0U;
    g_rr_cursor = 0U;
    g_reschedule = false;
    g_preemption_reported = false;
    g_preempt_watch_slot = -1;
    g_preempt_watch_since = 0U;
    g_preempt_watch_reported = false;
    g_initialized = true;
    kprintf("[TASK:ARM] scheduler core initialized quantum=%u ticks stack=%u KiB\n",
            ARM_TASK_QUANTUM_TICKS, ARM_TASK_STACK_BYTES / 1024U);
}

int rpi3_task_create(const char *name, void (*entry)(void *), void *argument,
                     bool idle_task) {
    arm_kernel_task_t *task;
    uintptr_t stack_top;
    int slot;
    int result;
    bool locked = false;
    if (!g_initialized) rpi3_task_init();
    if (!entry) return -1;

    /* Task creation can happen while the preemptive scheduler is already
       running (Terminal, Calculator, command workers, ...).  A READY state is
       a publication barrier from the scheduler's point of view: once visible,
       choose_next() is allowed to restore this task immediately.  Older
       Stage 10-12 code published READY before SP/PC/LR/CPSR were initialized,
       so a timer tick in the middle of task_create_kernel() could jump into a
       half-built frame.  Keep the current task non-preemptible and publish
       READY only after the complete context exists. */
    if (g_started) {
        task_preempt_disable();
        locked = true;
    }

    slot = allocate_slot();
    if (slot < 0) {
        result = -2;
        goto out;
    }
    task = &g_tasks[(uint32_t)slot];
    kmemset(task, 0, sizeof(*task));
    /* Remain ARM_TASK_UNUSED until the frame is completely initialized. */
    task->stack_allocation = kmalloc(ARM_TASK_STACK_BYTES + ARM_TASK_STACK_GUARD);
    if (!task->stack_allocation) {
        task->state = ARM_TASK_UNUSED;
        result = -3;
        goto out;
    }
    (void)mm_set_allocation_owner(task->stack_allocation, 0U);
    for (uint32_t guard = 0U; guard < ARM_TASK_STACK_GUARD / 4U; ++guard)
        ((uint32_t *)task->stack_allocation)[guard] = ARM_TASK_STACK_CANARY;
    task->pid = g_next_pid++;
    task->process_id = task->pid;
    task->idle = idle_task;
    task->entry = entry;
    task->argument = argument;
    task->stack_bytes = ARM_TASK_STACK_BYTES;
    copy_name(task->name, name, TASK_NAME_LEN);
    {
        arm_kernel_task_t *parent = current_task();
        copy_cwd(task->cwd, (g_started && parent) ? parent->cwd : "/");
        task->console_route = (g_started && parent) ? parent->console_route : 0U;
    }

    stack_top = (uintptr_t)task->stack_allocation + ARM_TASK_STACK_GUARD +
                ARM_TASK_STACK_BYTES;
    stack_top &= ~(uintptr_t)7U;
    task->frame.r0 = (uint32_t)(uintptr_t)task;
    task->frame.sp = (uint32_t)stack_top;
    task->frame.lr = (uint32_t)(uintptr_t)task_return_trap;
    task->frame.pc = (uint32_t)(uintptr_t)task_bootstrap;
    task->frame.cpsr = ARM_CPSR_SYSTEM;

    /* Publish last. */
    task->state = ARM_TASK_READY;
    kprintf("[TASK:ARM] create pid=%u slot=%u name=%s%s stack=%p\n",
            task->pid, (uint32_t)slot, task->name,
            idle_task ? " idle" : "", (void *)stack_top);
    result = (int)task->pid;

out:
    if (locked) task_preempt_enable();
    return result;
}

static void activate_task_address_space(const arm_kernel_task_t *task) {
    if (task && task->user && task->user_bytes) {
        if (!rpi3_mmu_activate_user_window(task->user_base, task->user_bytes)) {
            kprintf("[TASK:ARM] user MMU window FAILED pid=%u base=%p bytes=%u\n",
                    task->pid, (void *)(uintptr_t)task->user_base, task->user_bytes);
        }
    } else {
        rpi3_mmu_deactivate_user_window();
    }
}

static void wake_sleepers(uint32_t now) {
    uint32_t i;
    for (i = 0U; i < ARM_TASK_MAX; i++) {
        arm_kernel_task_t *task = &g_tasks[i];
        if (task->state == ARM_TASK_SLEEPING && tick_reached(now, task->wake_tick))
            task->state = ARM_TASK_READY;
    }
}

static bool ready_non_idle_exists(void) {
    uint32_t i;
    for (i = 0U; i < ARM_TASK_MAX; i++)
        if (g_tasks[i].state == ARM_TASK_READY && !g_tasks[i].idle) return true;
    return false;
}

static int choose_next(void) {
    uint32_t pass;
    int idle_slot = -1;
    for (pass = 0U; pass < ARM_TASK_MAX; pass++) {
        uint32_t i = (g_rr_cursor + 1U + pass) % ARM_TASK_MAX;
        arm_kernel_task_t *task = &g_tasks[i];
        if (task->state != ARM_TASK_READY) continue;
        if (task->idle) {
            if (idle_slot < 0) idle_slot = (int)i;
            continue;
        }
        g_rr_cursor = i;
        return (int)i;
    }
    if (idle_slot >= 0) {
        g_rr_cursor = (uint32_t)idle_slot;
        return idle_slot;
    }
    if (g_current >= 0 && (uint32_t)g_current < ARM_TASK_MAX &&
        g_tasks[g_current].state == ARM_TASK_RUNNING)
        return (int)g_current;
    return -1;
}

static arm_task_frame_t *schedule(arm_task_frame_t *incoming, bool force,
                                  bool timer_advanced) {
    arm_kernel_task_t *current;
    arm_kernel_task_t *next_task;
    uint32_t now = rpi3_timer_ticks();
    bool quantum_expired;
    int next;

    if (!g_started || !incoming) return incoming;
    wake_sleepers(now);
    current = current_task();
    if (!current) return incoming;

    if (current->stack_allocation && !current->user) {
        uint32_t *guard = (uint32_t *)current->stack_allocation;
        bool intact = true;
        for (uint32_t i = 0U; i < ARM_TASK_STACK_GUARD / 4U; ++i)
            if (guard[i] != ARM_TASK_STACK_CANARY) { intact = false; break; }
        if (!intact) {
            rpi3_uart_puts("[TASK:ARM] STACK OVERFLOW pid=");
            rpi3_uart_puthex(current->pid);
            rpi3_uart_puts(" task=");
            rpi3_uart_puts(current->name);
            rpi3_uart_puts("\n");
            if (!current->idle) {
                current->exit_code = -200;
                current->state = ARM_TASK_ZOMBIE;
                force = true;
            }
        }
    }

    current->frame = *incoming;
    if (timer_advanced) current->cpu_ticks++;
    quantum_expired = (uint32_t)(now - g_quantum_start) >= ARM_TASK_QUANTUM_TICKS;
    if (timer_advanced && current->idle && ready_non_idle_exists()) force = true;

    if (current->preempt_depth != 0U) {
        if (timer_advanced) {
            int32_t slot = (int32_t)(current - g_tasks);
            if (g_preempt_watch_slot != slot) {
                g_preempt_watch_slot = slot;
                g_preempt_watch_since = now;
                g_preempt_watch_reported = false;
            } else if (!g_preempt_watch_reported &&
                       (uint32_t)(now - g_preempt_watch_since) >= 100U) {
                /* Direct UART only: the console sink can itself belong to the
                   task that is stuck in the critical section. */
                rpi3_uart_puts("[TASK:ARM] WARN preemption held >1s task=");
                rpi3_uart_puts(current->name);
                rpi3_uart_puts(" pid=");
                rpi3_uart_puthex(current->pid);
                rpi3_uart_puts(" depth=");
                rpi3_uart_puthex(current->preempt_depth);
                rpi3_uart_puts(" tick=");
                rpi3_uart_puthex(now);
                rpi3_uart_puts("\n");
                g_preempt_watch_reported = true;
            }
        }
        if (force || quantum_expired || current->state != ARM_TASK_RUNNING)
            g_reschedule = true;
        return incoming;
    }
    g_preempt_watch_slot = -1;
    g_preempt_watch_reported = false;
    if (!force && !g_reschedule && current->state == ARM_TASK_RUNNING &&
        !quantum_expired)
        return incoming;

    if (current->state == ARM_TASK_RUNNING)
        current->state = ARM_TASK_READY;

    next = choose_next();
    if (next < 0) {
        /* This should be unreachable because Stage 10 always installs idle. */
        current->state = ARM_TASK_RUNNING;
        g_reschedule = false;
        g_quantum_start = now;
        return &current->frame;
    }

    next_task = &g_tasks[(uint32_t)next];
    next_task->state = ARM_TASK_RUNNING;
    if (next != g_current) {
        current->switches++;
        next_task->switches++;
        g_switches++;
    }
    g_current = next;
    activate_task_address_space(next_task);
    g_reschedule = false;
    g_quantum_start = now;
    if (timer_advanced && next != (int)(current - g_tasks) && !g_preemption_reported) {
        g_preemption_reported = true;
        kprintf("[TASK:ARM] timer preemption verified %s -> %s tick=%u\n",
                current->name, next_task->name, now);
    }
    return &next_task->frame;
}

arm_task_frame_t *rpi3_task_irq_schedule(arm_task_frame_t *frame) {
    uint32_t now;
    bool advanced;
    arm_task_frame_t *result;
    if (!g_started) return frame;
    now = rpi3_timer_ticks();
    advanced = now != g_last_timer_tick;
    if (advanced) g_last_timer_tick = now;
    result = schedule(frame, false, advanced);
    (void)rpi3_task_prepare_user_upcall(result);
    return result;
}

arm_task_frame_t *rpi3_task_svc_schedule(arm_task_frame_t *frame) {
    arm_task_frame_t *result;
    if (!g_started) return frame;
    result = schedule(frame, true, false);
    (void)rpi3_task_prepare_user_upcall(result);
    return result;
}

void rpi3_task_start(void) {
    int first;
    uint32_t now;
    if (!g_initialized) rpi3_task_init();
    if (g_started) {
        for (;;) __asm__ volatile("wfi");
    }
    first = choose_next();
    if (first < 0) {
        kprintf("[TASK:ARM] FATAL: no runnable tasks\n");
        for (;;) __asm__ volatile("wfi");
    }
    now = rpi3_timer_ticks();
    g_current = first;
    g_tasks[(uint32_t)first].state = ARM_TASK_RUNNING;
    g_last_timer_tick = now;
    g_quantum_start = now;
    activate_task_address_space(&g_tasks[(uint32_t)first]);
    g_started = true;
    kprintf("[TASK:ARM] preemptive scheduler START first=%s pid=%u mode=SYS\n",
            g_tasks[(uint32_t)first].name, g_tasks[(uint32_t)first].pid);
    rpi3_task_enter_first(&g_tasks[(uint32_t)first].frame);
}

bool rpi3_task_scheduler_started(void) { return g_started; }
uint32_t rpi3_task_switch_count(void) { return g_switches; }

uint32_t rpi3_task_live_count(void) {
    uint32_t count = 0U, i;
    for (i = 0U; i < ARM_TASK_MAX; i++)
        if (g_tasks[i].state != ARM_TASK_UNUSED && g_tasks[i].state != ARM_TASK_ZOMBIE)
            count++;
    return count;
}

uint32_t rpi3_task_ready_count(void) {
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < ARM_TASK_MAX; ++i)
        if (g_tasks[i].state == ARM_TASK_READY ||
            g_tasks[i].state == ARM_TASK_RUNNING) count++;
    return count;
}

uint32_t rpi3_task_snapshot(rpi3_task_public_info_t *infos, uint32_t capacity) {
    uint32_t count = 0U;
    if (!infos || !capacity) return 0U;
    task_preempt_disable();
    for (uint32_t i = 0U; i < ARM_TASK_MAX && count < capacity; ++i) {
        arm_kernel_task_t *task = &g_tasks[i];
        rpi3_task_public_info_t *out;
        if (task->state == ARM_TASK_UNUSED) continue;
        out = &infos[count++];
        kmemset(out, 0, sizeof(*out));
        out->pid = task->pid;
        out->process_id = task->process_id ? task->process_id : task->pid;
        copy_name(out->name, task->name, sizeof(out->name));
        out->state = (uint32_t)task->state;
        out->cpu_ticks = task->cpu_ticks;
        out->memory_hint = task->memory_hint;
        out->idle = task->idle;
        out->user = task->user;
    }
    task_preempt_enable();
    return count;
}

/* -------------------------------------------------------------------------
 * Stage 11 user-mode slots.  Each process receives a fixed 4 MiB identity-
 * mapped physical window.  The MMU flips AP permissions on context switch,
 * so USR mode can access only the currently scheduled slot; kernel memory and
 * MMIO remain privileged.  This is intentionally a bootstrap allocator, not
 * the final per-process page-table design. */
int rpi3_task_user_slot_acquire(uint32_t *slot_out, uint32_t *base_out,
                                uint32_t *bytes_out) {
    uint32_t i;
    int result = -1;
    /* The slot bitmap became shared scheduler state in Stage 11.  Keep the
       test+set atomic on the current UP ARM scheduler. */
    task_preempt_disable();
    for (i = 0U; i < RPI3_USER_SLOT_COUNT; ++i) {
        if (g_user_slots[i]) continue;
        g_user_slots[i] = true;
        if (slot_out) *slot_out = i;
        if (base_out) *base_out = RPI3_USER_SLOT_BASE + i * RPI3_USER_SLOT_BYTES;
        if (bytes_out) *bytes_out = RPI3_USER_SLOT_BYTES;
        result = 0;
        break;
    }
    task_preempt_enable();
    return result;
}

void rpi3_task_user_slot_release(uint32_t slot) {
    /* A release is a single aligned byte store.  This function is also called
       from the SVC exit path with IRQ masked, so do not invoke the scheduler
       preemption API here (it could recursively SVC while already in SVC). */
    if (slot < RPI3_USER_SLOT_COUNT) g_user_slots[slot] = false;
}

int rpi3_task_create_user_image(const char *name, uint32_t entry,
                                uint32_t return_pc, uint32_t upcall_return_pc,
                                uint32_t user_slot,
                                uint32_t image_end, const char *launch_arg) {
    arm_kernel_task_t *task;
    uint32_t base, limit, heap_start;
    int slot;
    int result;
    bool locked = false;
    if (!g_initialized) rpi3_task_init();
    if (user_slot >= RPI3_USER_SLOT_COUNT || !g_user_slots[user_slot]) return -1;
    base = RPI3_USER_SLOT_BASE + user_slot * RPI3_USER_SLOT_BYTES;
    limit = base + RPI3_USER_SLOT_BYTES;
    if (entry < base || entry >= limit || return_pc < base || return_pc >= limit ||
        upcall_return_pc < base || upcall_return_pc >= limit)
        return -2;
    if (image_end < base || image_end >= base + RPI3_USER_HEAP_END_OFF) return -3;

    if (g_started) {
        task_preempt_disable();
        locked = true;
    }
    slot = allocate_slot();
    if (slot < 0) { result = -4; goto out; }
    task = &g_tasks[(uint32_t)slot];
    kmemset(task, 0, sizeof(*task));
    task->pid = g_next_pid++;
    task->process_id = task->pid;
    {
        arm_kernel_task_t *parent = current_task();
        task->parent_pid = (g_started && parent) ?
            (parent->process_id ? parent->process_id : parent->pid) : 0U;
        task->console_route = (g_started && parent) ? parent->console_route : 0U;
        copy_cwd(task->cwd, (g_started && parent) ? parent->cwd : "/");
    }
    task->user = true;
    task->idle = false;
    task->user_slot = (uint8_t)user_slot;
    task->user_base = base;
    task->user_bytes = RPI3_USER_SLOT_BYTES;
    heap_start = (image_end + 15U) & ~15U;
    task->user_heap_next = heap_start;
    task->user_heap_limit = base + RPI3_USER_HEAP_END_OFF;
    copy_name(task->name, name, TASK_NAME_LEN);
    copy_name(task->launch_arg, launch_arg ? launch_arg : "", ARM_TASK_LAUNCH_ARG_LEN);
    task->frame.sp = (limit - 16U) & ~7U;
    task->frame.lr = return_pc;
    task->frame.pc = entry;
    task->frame.cpsr = ARM_CPSR_USER;
    task->upcall_return_pc = upcall_return_pc;

    /* Same publication rule as kernel threads: READY is visible last. */
    task->state = ARM_TASK_READY;
    kprintf("[TASK:ARM] create USER pid=%u slot=%u name=%s pc=%p sp=%p window=%p..%p\n",
            task->pid, (uint32_t)slot, task->name,
            (void *)(uintptr_t)entry, (void *)(uintptr_t)task->frame.sp,
            (void *)(uintptr_t)base, (void *)(uintptr_t)limit);
    result = (int)task->pid;

out:
    if (locked) task_preempt_enable();
    return result;
}

bool rpi3_task_current_launch_arg_copy(char *buffer, uint32_t capacity) {
    arm_kernel_task_t *task = current_task();
    uint32_t i = 0U;
    if (!buffer || !capacity || !g_started || !task || !task->user) return false;
    while (task->launch_arg[i] && i + 1U < capacity) {
        buffer[i] = task->launch_arg[i];
        ++i;
    }
    buffer[i] = '\0';
    return task->launch_arg[i] == '\0';
}

bool rpi3_task_current_user_range_ok(uint32_t address, uint32_t bytes,
                                     bool write) {
    arm_kernel_task_t *task = current_task();
    uint32_t end;
    (void)write; /* Stage 11 maps a combined RWX user window. */
    if (!g_started || !task || !task->user) return false;
    if (!bytes) return address >= task->user_base &&
                       address <= task->user_base + task->user_bytes;
    end = address + bytes - 1U;
    if (end < address) return false;
    return address >= task->user_base && end < task->user_base + task->user_bytes;
}

static arm_user_allocation_t *find_user_allocation(arm_kernel_task_t *task,
                                                    uint32_t address) {
    uint32_t i;
    if (!task) return NULL;
    for (i = 0U; i < ARM_USER_ALLOC_MAX; ++i)
        if (task->user_allocations[i].used &&
            task->user_allocations[i].address == address)
            return &task->user_allocations[i];
    return NULL;
}

void *rpi3_task_user_alloc(uint32_t bytes) {
    arm_kernel_task_t *task = current_task();
    arm_user_allocation_t *free_meta = NULL;
    arm_user_allocation_t *reuse = NULL;
    uint32_t i, wanted, address;
    if (!task || !task->user || !bytes) return NULL;
    wanted = (bytes + 7U) & ~7U;
    if (wanted < bytes) return NULL;
    for (i = 0U; i < ARM_USER_ALLOC_MAX; ++i) {
        arm_user_allocation_t *a = &task->user_allocations[i];
        if (a->used) continue;
        if (a->address && a->size >= wanted &&
            (!reuse || a->size < reuse->size)) reuse = a;
        if (!a->address && !free_meta) free_meta = a;
    }
    if (reuse) {
        reuse->used = true;
        kmemset((void *)(uintptr_t)reuse->address, 0, reuse->size);
        return (void *)(uintptr_t)reuse->address;
    }
    if (!free_meta || task->user_heap_next + wanted < task->user_heap_next ||
        task->user_heap_next + wanted > task->user_heap_limit) return NULL;
    address = task->user_heap_next;
    task->user_heap_next += wanted;
    free_meta->address = address;
    free_meta->size = wanted;
    free_meta->used = true;
    kmemset((void *)(uintptr_t)address, 0, wanted);
    return (void *)(uintptr_t)address;
}

bool rpi3_task_user_free(void *pointer) {
    arm_kernel_task_t *task = current_task();
    arm_user_allocation_t *a;
    if (!pointer) return true;
    a = find_user_allocation(task, (uint32_t)(uintptr_t)pointer);
    if (!a) return false;
    a->used = false;
    /* Return contiguous blocks at the top to the bump frontier.  Interior
       holes remain reusable through the best-fit path above. */
    for (;;) {
        arm_user_allocation_t *top = NULL;
        for (uint32_t i = 0U; i < ARM_USER_ALLOC_MAX; ++i) {
            arm_user_allocation_t *candidate = &task->user_allocations[i];
            if (!candidate->used && candidate->address &&
                candidate->address + candidate->size == task->user_heap_next) {
                top = candidate;
                break;
            }
        }
        if (!top) break;
        task->user_heap_next = top->address;
        top->address = 0U;
        top->size = 0U;
    }
    return true;
}

void *rpi3_task_user_realloc(void *pointer, uint32_t bytes) {
    arm_kernel_task_t *task = current_task();
    arm_user_allocation_t *old;
    void *next;
    uint32_t copy;
    if (!pointer) return rpi3_task_user_alloc(bytes);
    if (!bytes) { (void)rpi3_task_user_free(pointer); return NULL; }
    old = find_user_allocation(task, (uint32_t)(uintptr_t)pointer);
    if (!old) return NULL;
    if (bytes <= old->size) return pointer;
    next = rpi3_task_user_alloc(bytes);
    if (!next) return NULL;
    copy = old->size < bytes ? old->size : bytes;
    kmemcpy(next, pointer, copy);
    (void)rpi3_task_user_free(pointer);
    return next;
}

void rpi3_task_svc_exit(int32_t status) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task || !task->user) return;
    task->exit_code = status;
    task->state = ARM_TASK_ZOMBIE;
    g_reschedule = true;
    rpi3_task_user_slot_release(task->user_slot);
    kprintf("[TASK:ARM] USER exit pid=%u status=%d\n", task->pid, status);
}

void rpi3_task_svc_sleep(uint32_t ticks) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task || !task->user) return;
    if (!ticks) { g_reschedule = true; return; }
    task->wake_tick = rpi3_timer_ticks() + ticks;
    task->state = ARM_TASK_SLEEPING;
    g_reschedule = true;
}

arm_task_frame_t *rpi3_task_user_fault(uint32_t kind, arm_task_frame_t *frame) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task || !task->user || !frame) return frame;
    kprintf("[TASK:ARM] USER fault pid=%u kind=%u pc=%p -> terminate\n",
            task->pid, kind, (void *)(uintptr_t)frame->pc);
    task->exit_code = -(int32_t)(128U + kind);
    task->state = ARM_TASK_ZOMBIE;
    g_reschedule = true;
    rpi3_task_user_slot_release(task->user_slot);
    return schedule(frame, true, false);
}

/* -------------------------------------------------------------------------
 * BlesKernOS task ABI implemented natively for the ARM32 kernel-task stage.
 * Stage 11 adds native ARM USR tasks; Win16/upcalls remain deferred. */
int task_create(const char *name, task_entry_t entry, void *argument) {
    return rpi3_task_create(name, entry, argument, false);
}
int task_create_user(const char *name, void (*entry)(void)) {
    (void)name; (void)entry;
    /* Raw kernel pointers are intentionally not accepted as USR entrypoints.
       User processes must be created by the ARM ELF loader. */
    return -1;
}
int task_create_user_program(const char *name, task_entry_t entry,
                             void *argument, const char *launch_arg) {
    (void)name; (void)entry; (void)argument; (void)launch_arg; return -1;
}
int task_create_user_program_suspended(const char *name, task_entry_t entry,
                                       void *argument, const char *launch_arg) {
    (void)name; (void)entry; (void)argument; (void)launch_arg; return -1;
}
bool task_start_suspended(uint32_t pid) { (void)pid; return false; }
int task_create_kernel(const char *name, task_entry_t entry, void *argument) {
    return rpi3_task_create(name, entry, argument, false);
}
int task_create_kernel_suspended(const char *name, task_entry_t entry,
                                 void *argument) {
    (void)name; (void)entry; (void)argument; return -1;
}

void task_preempt_disable(void) {
    arm_kernel_task_t *task = current_task();
    if (g_started && task && task->preempt_depth != 0xFFFFU)
        task->preempt_depth++;
}

void task_preempt_enable(void) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task) return;
    if (task->preempt_depth) task->preempt_depth--;
    if (!task->preempt_depth && g_reschedule)
        __asm__ volatile("svc #0" : : : "memory");
}

void task_yield(void) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task) {
        __asm__ volatile("yield" : : : "memory");
        return;
    }
    g_reschedule = true;
    if (task->preempt_depth == 0U)
        __asm__ volatile("svc #0" : : : "memory");
}

void task_sleep(uint32_t ticks) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task) {
        if (ticks) {
            uint32_t until = rpi3_timer_ticks() + ticks;
            while (!tick_reached(rpi3_timer_ticks(), until)) __asm__ volatile("wfi");
        }
        return;
    }
    if (!ticks) { task_yield(); return; }
    task->wake_tick = rpi3_timer_ticks() + ticks;
    task->state = ARM_TASK_SLEEPING;
    g_reschedule = true;
    __asm__ volatile("svc #0" : : : "memory");
}

void task_sleep_from_interrupt(uint32_t ticks) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task) return;
    task->wake_tick = rpi3_timer_ticks() + (ticks ? ticks : 1U);
    task->state = ARM_TASK_SLEEPING;
    g_reschedule = true;
}

void task_exit(void) {
    arm_kernel_task_t *task = current_task();
    if (!g_started || !task) for (;;) __asm__ volatile("wfi");
    task->state = ARM_TASK_ZOMBIE;
    g_reschedule = true;
    __asm__ volatile("svc #0" : : : "memory");
    for (;;) __asm__ volatile("wfi");
}

void task_exit_status(int32_t status) {
    (void)status;
    task_exit();
}

uint32_t task_current_pid(void) {
    arm_kernel_task_t *task = current_task();
    return (g_started && task) ? task->pid : 0U;
}
uint32_t task_current_tid(void) { return task_current_pid(); }
uint32_t task_current_process_id(void) {
    arm_kernel_task_t *task = current_task();
    return (g_started && task) ? (task->process_id ? task->process_id : task->pid) : 0U;
}
uint32_t task_current_parent_pid(void) {
    arm_kernel_task_t *task = current_task();
    return (g_started && task) ? task->parent_pid : 0U;
}
uint32_t task_current_console_route(void) {
    arm_kernel_task_t *task = current_task();
    return (g_started && task) ? task->console_route : 0U;
}
void task_set_current_console_route(uint32_t route) {
    arm_kernel_task_t *task = current_task();
    if (g_started && task) task->console_route = route;
}
uint32_t task_current_cpu(void) {
    uint32_t mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 3U;
}
bool task_current_is_idle(void) {
    arm_kernel_task_t *task = current_task();
    return g_started && task && task->idle;
}
bool task_current_is_user(void) {
    arm_kernel_task_t *task = current_task();
    return g_started && task && task->user;
}

const char *task_current_cwd(void) {
    arm_kernel_task_t *task = current_task();
    return (g_started && task) ? task->cwd : g_early_cwd;
}

bool task_set_current_cwd(const char *path) {
    arm_kernel_task_t *task = current_task();
    char *dst = (g_started && task) ? task->cwd : g_early_cwd;
    uint32_t i = 0U;
    if (!path || !path[0]) return false;
    while (path[i] && i + 1U < ARM_TASK_CWD_LEN) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    return path[i] == '\0';
}

bool task_set_affinity_mask(uint32_t pid, uint32_t mask) {
    (void)pid; (void)mask; return true; /* Stage 10 is intentionally UP. */
}

bool task_queue_user_upcall(uint32_t pid, uint32_t entry,
                            const uint32_t *arguments, uint8_t argument_count,
                            const void *payload, uint8_t payload_size,
                            int8_t payload_argument) {
    return task_queue_window_upcall(NULL, pid, entry, arguments,
                                    argument_count, payload, payload_size,
                                    payload_argument);
}

static bool arm_upcall_is_mouse_move(const arm_task_upcall_t *upcall) {
    const gui_event_t *event;
    if (!upcall || (upcall->payload_argument != 1 &&
                    upcall->payload_argument != 2) ||
        upcall->payload_size != sizeof(gui_event_t)) return false;
    event = (const gui_event_t *)(const void *)upcall->payload;
    return event->type == GUI_EVENT_MOUSE_MOVE;
}

static uint8_t arm_upcall_count(const arm_kernel_task_t *task) {
    return (uint8_t)((task->upcall_head + TASK_UPCALL_QUEUE -
                      task->upcall_tail) % TASK_UPCALL_QUEUE);
}

static bool arm_upcall_drop_oldest_move(arm_kernel_task_t *task) {
    uint8_t index;
    if (!task) return false;
    index = task->upcall_tail;
    while (index != task->upcall_head) {
        if (arm_upcall_is_mouse_move(&task->upcalls[index])) {
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

bool task_queue_window_upcall(struct gui_window *window, uint32_t pid,
                              uint32_t entry, const uint32_t *arguments,
                              uint8_t argument_count, const void *payload,
                              uint8_t payload_size, int8_t payload_argument) {
    arm_kernel_task_t *target = NULL;
    arm_task_upcall_t *upcall;
    uint8_t next;
    uint32_t i;
    bool mouse_move = false;
    if (!pid || !entry || argument_count > TASK_UPCALL_ARGS ||
        payload_size > TASK_UPCALL_PAYLOAD ||
        (payload_size && (!payload || payload_argument < 0 ||
                          payload_argument >= (int8_t)argument_count)))
        return false;
    if ((payload_argument == 1 || payload_argument == 2) &&
        payload_size == sizeof(gui_event_t))
        mouse_move = ((const gui_event_t *)payload)->type == GUI_EVENT_MOUSE_MOVE;

    task_preempt_disable();
    for (i = 0U; i < ARM_TASK_MAX; ++i) {
        arm_kernel_task_t *candidate = &g_tasks[i];
        if (!candidate->user || candidate->state == ARM_TASK_UNUSED ||
            candidate->state == ARM_TASK_ZOMBIE) continue;
        if (candidate->pid == pid || candidate->process_id == pid) {
            target = candidate;
            break;
        }
    }
    if (!target || entry < target->user_base ||
        entry >= target->user_base + target->user_bytes ||
        (window && ((gui_window_t *)window)->destroy_state != 0U)) {
        task_preempt_enable();
        return false;
    }
    if (mouse_move) {
        uint8_t index = target->upcall_tail;
        while (index != target->upcall_head) {
            arm_task_upcall_t *queued = &target->upcalls[index];
            if (arm_upcall_is_mouse_move(queued) && queued->entry == entry &&
                queued->payload_argument == payload_argument &&
                queued->argument_count == argument_count &&
                (!argument_count || !arguments ||
                 queued->arguments[0] == arguments[0])) {
                for (i = 0U; i < argument_count; ++i)
                    queued->arguments[i] = arguments ? arguments[i] : 0U;
                kmemcpy(queued->payload, payload, payload_size);
                task_preempt_enable();
                return true;
            }
            index = (uint8_t)((index + 1U) % TASK_UPCALL_QUEUE);
        }
        if (arm_upcall_count(target) >= TASK_UPCALL_QUEUE - 4U) {
            task_preempt_enable();
            return true;
        }
    }
    next = (uint8_t)((target->upcall_head + 1U) % TASK_UPCALL_QUEUE);
    if (next == target->upcall_tail && !mouse_move &&
        arm_upcall_drop_oldest_move(target))
        next = (uint8_t)((target->upcall_head + 1U) % TASK_UPCALL_QUEUE);
    if (next == target->upcall_tail) {
        task_preempt_enable();
        return false;
    }
    upcall = &target->upcalls[target->upcall_head];
    kmemset(upcall, 0, sizeof(*upcall));
    upcall->entry = entry;
    upcall->argument_count = argument_count;
    upcall->payload_argument = payload_argument;
    upcall->payload_size = payload_size;
    upcall->window_target = window;
    for (i = 0U; i < argument_count; ++i)
        upcall->arguments[i] = arguments ? arguments[i] : 0U;
    if (payload_size) kmemcpy(upcall->payload, payload, payload_size);
    if (window && (payload_argument == -1 || payload_argument == 1)) {
        gui_window_t *gui_window = (gui_window_t *)window;
        upcall->invalidate_valid = true;
        upcall->invalidate_x = gui_window->bounds.x;
        upcall->invalidate_y = gui_window->bounds.y;
        upcall->invalidate_w = gui_window->bounds.w;
        upcall->invalidate_h = gui_window->bounds.h;
    }
    target->upcall_head = next;
    if (target->state == ARM_TASK_SLEEPING) target->state = ARM_TASK_READY;
    g_reschedule = true;
    task_preempt_enable();
    return true;
}

bool task_cancel_window_upcalls(struct gui_window *window) {
    uint32_t slot;
    bool active = false;
    if (!window) return true;
    task_preempt_disable();
    for (slot = 0U; slot < ARM_TASK_MAX; ++slot) {
        arm_kernel_task_t *task = &g_tasks[slot];
        arm_task_upcall_t kept[TASK_UPCALL_QUEUE];
        uint8_t count = 0U, index;
        if (task->state == ARM_TASK_UNUSED) continue;
        if (task->upcall_active && task->active_upcall.window_target == window)
            active = true;
        index = task->upcall_tail;
        while (index != task->upcall_head) {
            if (task->upcalls[index].window_target != window &&
                count < TASK_UPCALL_QUEUE - 1U)
                kept[count++] = task->upcalls[index];
            index = (uint8_t)((index + 1U) % TASK_UPCALL_QUEUE);
        }
        kmemset(task->upcalls, 0, sizeof(task->upcalls));
        for (index = 0U; index < count; ++index) task->upcalls[index] = kept[index];
        task->upcall_tail = 0U;
        task->upcall_head = count;
    }
    task_preempt_enable();
    return !active;
}

bool rpi3_task_prepare_user_upcall(arm_task_frame_t *frame) {
    arm_kernel_task_t *task = current_task();
    arm_task_upcall_t *upcall;
    uint32_t payload_bytes, payload_address = 0U;
    uint32_t i;
    if (!frame || !task || !task->user || task->upcall_active ||
        task->upcall_head == task->upcall_tail ||
        (frame->cpsr & 0x1FU) != ARM_CPSR_USER) return false;
    upcall = &task->upcalls[task->upcall_tail];
    payload_bytes = ((uint32_t)upcall->payload_size + 7U) & ~7U;
    if (frame->sp < task->user_base + payload_bytes + 16U ||
        frame->sp > task->user_base + task->user_bytes) return false;
    task->active_upcall = *upcall;
    task->upcall_tail = (uint8_t)((task->upcall_tail + 1U) % TASK_UPCALL_QUEUE);
    task->upcall_saved_frame = *frame;
    task->upcall_active = true;
    if (task->active_upcall.payload_size) {
        frame->sp -= payload_bytes;
        payload_address = frame->sp;
        kmemset((void *)(uintptr_t)payload_address, 0, payload_bytes);
        kmemcpy((void *)(uintptr_t)payload_address,
                task->active_upcall.payload,
                task->active_upcall.payload_size);
        task->active_upcall.arguments[
            (uint8_t)task->active_upcall.payload_argument] = payload_address;
    }
    frame->r0 = frame->r1 = frame->r2 = frame->r3 = 0U;
    for (i = 0U; i < task->active_upcall.argument_count; ++i) {
        if (i == 0U) frame->r0 = task->active_upcall.arguments[i];
        else if (i == 1U) frame->r1 = task->active_upcall.arguments[i];
        else if (i == 2U) frame->r2 = task->active_upcall.arguments[i];
        else frame->r3 = task->active_upcall.arguments[i];
    }
    frame->lr = task->upcall_return_pc;
    frame->pc = task->active_upcall.entry;
    return true;
}

bool rpi3_task_finish_user_upcall(arm_task_frame_t *frame) {
    arm_kernel_task_t *task = current_task();
    arm_task_upcall_t completed;
    if (!frame || !task || !task->user || !task->upcall_active) return false;
    completed = task->active_upcall;
    *frame = task->upcall_saved_frame;
    task->upcall_active = false;
    kmemset(&task->active_upcall, 0, sizeof(task->active_upcall));

    if (completed.payload_argument == -2 && completed.argument_count > 0U) {
        gui_window_t *window = (gui_window_t *)(uintptr_t)completed.arguments[0];
        gui_surface_t *surface = completed.argument_count > 1U
            ? (gui_surface_t *)(uintptr_t)completed.arguments[1] : NULL;
        if (window && window->destroy_state == 0U) {
            bool captured;
            window->content_pending = false;
            captured = gui_window_capture_content(window, surface);
            gui_window_end_content_paint(window);
            if (!captured || window->content_repaint) window->dirty = true;
            gui_desktop_invalidate_rect(gui_get_desktop(), window->bounds);
            gui_request_paint();
        }
    } else if (completed.payload_argument == -1 ||
               completed.payload_argument == 1) {
        if (completed.invalidate_valid) {
            gui_desktop_invalidate_rect(gui_get_desktop(), (gui_rect_t){
                completed.invalidate_x, completed.invalidate_y,
                completed.invalidate_w, completed.invalidate_h
            });
        } else {
            gui_desktop_invalidate_all(gui_get_desktop());
        }
        gui_request_paint();
    }
    return true;
}

bool rpi3_task_current_upcall_argument(uint32_t index, uint32_t value) {
    arm_kernel_task_t *task = current_task();
    return task && task->user && task->upcall_active &&
           index < task->active_upcall.argument_count &&
           task->active_upcall.arguments[index] == value;
}


bool task_query_pid(uint32_t pid, bool *active, int32_t *exit_code,
                    uint32_t *process_id) {
    uint32_t i;
    bool found = false;
    task_preempt_disable();
    for (i = 0U; i < ARM_TASK_MAX; ++i) {
        arm_kernel_task_t *task = &g_tasks[i];
        if (task->state == ARM_TASK_UNUSED || task->pid != pid) continue;
        if (active) *active = task->state != ARM_TASK_ZOMBIE;
        if (exit_code) *exit_code = task->exit_code;
        if (process_id) *process_id = task->process_id ? task->process_id : task->pid;
        found = true;
        break;
    }
    task_preempt_enable();
    return found;
}

bool task_request_exit(uint32_t pid) {
    uint32_t i;
    bool result = false;
    task_preempt_disable();
    for (i = 0U; i < ARM_TASK_MAX; ++i) {
        arm_kernel_task_t *task = &g_tasks[i];
        if (task->state == ARM_TASK_UNUSED || task->pid != pid || task->idle ||
            !task->user) continue;
        task->exit_code = -1;
        task->state = ARM_TASK_ZOMBIE;
        if (task->user) rpi3_task_user_slot_release(task->user_slot);
        g_reschedule = true;
        result = true;
        break;
    }
    task_preempt_enable();
    return result;
}

int32_t task_waitpid(uint32_t pid, int32_t *status) {
    uint32_t i;
    int32_t result = -1;
    arm_kernel_task_t *self = current_task();

    /* Reaping modifies both the scheduler table and the heap (kernel stacks).
       Keep the slot published as one coherent state until it is completely
       UNUSED; otherwise a timer switch can let a concurrent spawn recycle a
       half-cleared descriptor. */
    task_preempt_disable();
    for (i = 0U; i < ARM_TASK_MAX; ++i) {
        arm_kernel_task_t *task = &g_tasks[i];
        int32_t code;
        if (task->state == ARM_TASK_UNUSED || task->pid != pid) continue;
        if (task->state != ARM_TASK_ZOMBIE) { result = 0; break; }
        if (self == task) { result = -1; break; }

        code = task->exit_code;
        if (status) *status = code;
        task->state = ARM_TASK_REAPING;
        task_preempt_enable();
        release_task_resources(task);
        task_preempt_disable();
        kprintf("[TASK:ARM] reap pid=%u slot=%u status=%d\n", pid, i, code);
        kmemset(task, 0, sizeof(*task));
        task->state = ARM_TASK_UNUSED;
        result = (int32_t)pid;
        break;
    }
    task_preempt_enable();
    return result;
}

void task_set_memory_hint(uint32_t bytes) {
    arm_kernel_task_t *task = current_task();
    if (g_started && task) task->memory_hint = bytes;
}
