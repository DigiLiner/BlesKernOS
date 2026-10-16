#ifndef TASK_H
#define TASK_H

#include "types.h"
#include "gdt.h"
#include "idt.h"
int32_t task_linux_signal_syscall(registers_t *regs);
void task_linux_sigreturn(registers_t *regs, bool rt);
registers_t *task_linux_signal_return(registers_t *regs);
registers_t *task_linux_signal_fault(registers_t *regs, uint32_t address);
int32_t task_linux_futex_wait(registers_t *regs,uint32_t address,uint32_t value,uint32_t mask,
                             bool private_key,uint32_t ticks);
int32_t task_linux_futex_wake(uint32_t address,uint32_t count,uint32_t mask,
                             bool private_key);
int32_t task_linux_robust(registers_t *regs,bool get);
int32_t task_linux_fork(registers_t *regs,uint32_t tls_base);
int32_t task_linux_wait(registers_t *regs);
bool task_linux_signal_pending(void);
uint64_t task_linux_mask_swap(uint64_t mask);

struct gui_window;

/* Wine keeps services and worker threads alive across consecutive Win32
 * launches. 64 slots were exhausted by Regedit followed by Notepad; keep
 * enough headroom for several graphical applications in one session. */
#define TASK_MAX       128
/* Las pilas se dimensionan por perfil y tipo de tarea. Quake 1.09 necesita
 * casi 256 KiB durante SCR_UpdateScreen; el hilo y el adaptador agregan sus
 * propios marcos, así que el caso aislado dispone de 512 KiB. Las tareas
 * comunes y los perfiles de memoria baja no se agrandan. */
#define TASK_STACK_MAX_SIZE   524288U
#define TASK_STACK_SIZE       TASK_STACK_MAX_SIZE
#define TASK_STACK_GUARD_SIZE 1024U
#define TASK_NAME_LEN      24
#define TASK_CWD_LEN       260
/* Las recetas del toolchain necesitan conservar lineas de compilacion reales.
 * Los enlaces masivos usan response files, pero las invocaciones C superan
 * facilmente el antiguo limite Win32 de 255 caracteres. */
#define TASK_LAUNCH_ARG_LEN 2048
#define TASK_UPCALL_QUEUE 16
#define TASK_UPCALL_ARGS 4
#define TASK_UPCALL_PAYLOAD 64
#define TASK_FPU_STATE_SIZE 512U

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_STARTING,
    TASK_ZOMBIE
} task_state_t;

typedef void (*task_entry_t)(void *argument);

typedef struct {
    uint32_t entry;
    uint32_t arguments[TASK_UPCALL_ARGS];
    uint8_t argument_count;
    int8_t payload_argument;
    uint8_t payload_size;
    uint8_t payload[TASK_UPCALL_PAYLOAD];
    /* Optional user-visible heap block owned by the queued callback.  The
     * kernel releases it only after the Ring-3 callback returns (or when the
     * target task dies before delivery). */
    void *release_after_upcall;
    /* Set only by task_queue_window_upcall(). Generic PE/Win32 callbacks may
     * use the same payload_argument values, but their arguments[0] is not a
     * native gui_window pointer. */
    struct gui_window *window_target;
    bool invalidate_valid;
    int32_t invalidate_x;
    int32_t invalidate_y;
    int32_t invalidate_w;
    int32_t invalidate_h;
} task_upcall_t;

typedef struct {
    uint32_t pid;
    uint32_t process_id;
    uint32_t parent_pid;
    uint32_t linux_vm_lender;
    void *linux_stack_pin;
    char name[TASK_NAME_LEN];
    task_state_t state;
    /* -1 means not executing / no fixed affinity. Kernel and Win16 tasks are
     * pinned to CPU0 until those legacy subsystems become re-entrant. */
    /* Scheduler placement. running_cpu and queued_cpu are 32-bit so the
     * scalable scheduler can update them atomically without byte races. */
    volatile int32_t running_cpu;
    int8_t affinity_cpu;          /* legacy single-CPU pin, -1 = mask */
    uint8_t preferred_cpu;
    uint8_t last_cpu;
    uint8_t scheduler_reserved;
    volatile int32_t queued_cpu;  /* -1 none, -2 transient claim, >=0 runqueue */
    uint32_t affinity_mask;       /* bit per allowed CPU; zero = all online */
    uint32_t *stack;
    uint32_t *user_stack;
    /* Raw heap blocks backing the page-aligned guarded stacks. The exposed
     * stack pointers intentionally skip a non-present page at each edge. */
    void *stack_allocation;
    void *user_stack_allocation;
    uint32_t stack_size;
    uint32_t user_stack_size;
    registers_t *context;
    task_entry_t entry;
    void *argument;
    uint32_t cpu_ticks;
    uint32_t wake_tick;
    bool idle;
    bool system;
    bool user;
    /* Preserve task layout for existing native modules; always zero. */
    bool abi_reserved;
    /* Personalidad de syscalls Linux i386. El proceso sigue siendo Ring 3,
     * pero int 0x80 usa la numeracion y errno negativo de Linux. */
    bool linux_abi;
    struct linux_signals *linux_signals;
    /* Native user threads share process_id/resources but keep independent
     * stacks, register/FPU state, affinity and TLS. pid acts as the TID. */
    bool thread;
    bool thread_joinable;
    bool exit_requested;
    bool resources_released;
    int32_t exit_code;
    uint32_t memory_bytes;
    /* Generic kernel event wait used by compatibility shims that need to
     * block without polling a full PIT tick. The producer only stores an
     * opaque key and wakes the task through task_event_signal(). */
    uint32_t event_wait_key;
    bool event_wait_fired;
    /* Canal de consola heredado por procesos hijos. Cero significa que la
       tarea escribe solamente en la consola/COM1 del sistema. */
    uint32_t console_route;
    /* VFS cwd is task-local.  Keeping only a pointer here avoids growing the
     * fixed TASK_MAX table by ~8 KiB, important for the 4 MiB profile. */
    char *cwd;
    uint32_t user_fs_base;
    /* Linux i386 set_thread_area() owns GDT entries 6..8 independently.
     * Slot 0 mirrors user_fs_base for compatibility with existing native/
     * Win32 paths; slots 1/2 let Wine keep FS (TEB) separate from glibc GS. */
    uint32_t linux_tls_base[GDT_USER_TLS_COUNT];
    uint8_t linux_tls_used_mask;
    /* Phase 1 keeps kernel BSS supervisor-only. Public APIs that historically
     * returned const char pointers therefore need per-task user-heap mirrors
     * rather than exposing kernel strings directly. */
    void *user_string_exports;
    void *user_string_overflow;
    uint32_t user_string_export_count;
    void *user_task_snapshot;
    struct gui_window *window;
    char *launch_arg;
    task_upcall_t upcalls[TASK_UPCALL_QUEUE];
    uint8_t upcall_head;
    uint8_t upcall_tail;
    bool upcall_active;
    task_upcall_t active_upcall;
    registers_t upcall_saved_context;
    /* An asynchronous Ring-3 upcall may interrupt the application in the
       middle of an x87 expression. Keep that interrupted x87 image separate
       from the callback's normal per-task FPU image. */
    uint8_t *upcall_fpu_state;
    void *upcall_fpu_allocation;
    bool upcall_fpu_saved;
    /* Guard for public API proxy execution. It belongs to the task rather
     * than the CPU because an explicit task_yield() may migrate a suspended
     * kernel frame to another processor. */
    bool user_upcalls_blocked;
    uint32_t api_guard_depth;
    const char *api_guard_name;
    uint32_t api_guard_target;
    uint32_t api_guard_token;
    /* A user task may only be switched away from a CPL0 frame after it has
     * explicitly dropped its subsystem-domain locks in task_yield()/task_sleep().
     * This bit belongs to the task because the suspended kernel frame may
     * later resume on another CPU. */
    bool kernel_switch_safe;
    /* GCC i386 uses the x87 stack for float/double.  This state must be
     * private to each task; otherwise a timer switch can corrupt TinyGL and
     * any application doing floating-point work. */
    uint8_t *fpu_state;
    void *fpu_allocation;
    bool fpu_state_valid;
    uint32_t priority; /* 0/2 normal, 1 low, 3 high: quantum weighting */
} task_t;

typedef struct {
    uint32_t pid;
    uint32_t process_id;
    char name[TASK_NAME_LEN];
    task_state_t state;
    uint32_t cpu_ticks;
    uint32_t stack_bytes;
    uint32_t memory_hint_bytes;
    bool system;
    bool user;
    bool exit_requested;
} task_public_snapshot_t;

uint32_t task_process_priority(uint32_t process_id,uint32_t level);
void task_init(void);
int task_create(const char *name, task_entry_t entry, void *argument);
int task_create_user(const char *name, void (*entry)(void));
int task_create_user_program(const char *name, task_entry_t entry,
                             void *argument, const char *launch_arg);
int task_create_user_program_suspended(const char *name, task_entry_t entry,
                                       void *argument,
                                       const char *launch_arg);
bool task_start_suspended(uint32_t pid);

/* BLES_WINE_KERNEL_WORKER_API_20260723 */
int task_create_kernel(const char *name, task_entry_t entry,
                       void *argument);
int task_create_kernel_suspended(const char *name, task_entry_t entry,
                                 void *argument);
int task_create_user_thread(const char *name, task_entry_t entry,
                            void *argument, uint32_t process_id);
int task_create_user_thread_ex(const char *name, task_entry_t entry,
                               void *argument, uint32_t process_id,
                               uint32_t user_stack_size, bool joinable);
int32_t task_clone_linux_thread(const registers_t *parent_frame,
                                uint32_t child_stack, uint32_t tls_base,
                                int32_t tls_entry, uint32_t clone_flags,
                                uint32_t parent_tid_address,
                                uint32_t child_tid_address);
registers_t *task_schedule(registers_t *current_frame);
registers_t *task_schedule_voluntary(registers_t *current_frame);
/* Local-APIC timers on APs use a non-blocking scheduler admission path.
 * A missed quantum is harmless; spinning 15 CPUs inside an interrupt is not. */
registers_t *task_schedule_lapic(registers_t *current_frame);
registers_t *task_schedule_reschedule_ipi(registers_t *current_frame);
void task_yield(void);
void task_sleep(uint32_t ticks);
void task_sleep_from_interrupt(uint32_t ticks);
void task_exit(void) NORETURN;
void task_exit_status(int32_t status) NORETURN;
void task_exit_from_interrupt(int32_t status);
/* Non-blocking join primitive used by the user ABI: >0 completed TID,
 * 0 still running, negative on invalid ownership/state. */
int32_t task_thread_join_try(uint32_t tid, int32_t *status);
bool task_thread_detach(uint32_t tid);
uint32_t task_current_tid(void);
const char *task_current_name(void);
bool task_set_current_tls_base(uint32_t base);
uint32_t task_current_tls_base(void);
bool task_linux_set_thread_area(int32_t requested_entry, uint32_t base,
                                int32_t *actual_entry);
bool task_linux_get_thread_area(int32_t entry, uint32_t *base);
bool task_set_current_clear_child_tid(uint32_t address);
uint32_t task_thread_count(uint32_t process_id);
/* Slow zombie cleanup runs in process context, never in the timer spinlock. */
void task_reap_deferred(void);
void task_preempt_disable(void);
void task_preempt_enable(void);
/* Internal stability hooks. A user task running in CPL0 is non-preemptible
 * unless it reaches an explicit safe point after dropping domain locks. API thunks
 * can also reject calls before exhausting the task's kernel stack. */
void task_allow_kernel_switch_once(void);
uint32_t task_kernel_stack_remaining(void);
void task_user_api_guard_enter(const char *name, uint32_t target,
                               uint32_t token);
void task_user_api_guard_leave(void);
bool task_user_api_guard_info(const char **name_out, uint32_t *target_out,
                              uint32_t *token_out);
void task_user_api_guard_clear(void);
bool task_request_exit(uint32_t pid);
bool task_request_exit_process(uint32_t process_id, int32_t exit_code);
/* Two-phase event wait. prepare() may run before the producer changes its
 * state; commit() then sleeps atomically with respect to task_event_signal().
 * A one-tick timeout is normally used as a safety net, not as the wake path. */
bool task_event_wait_prepare(uint32_t key);
bool task_event_wait_commit(uint32_t key, uint32_t timeout_ticks);
void task_event_wait_cancel(uint32_t key);
uint32_t task_event_signal(uint32_t key);
bool task_query_pid(uint32_t pid, bool *active, int32_t *exit_code,
                    uint32_t *process_id);
bool task_query_process(uint32_t process_id, bool *active,
                        int32_t *exit_code, uint32_t *main_pid);
bool task_exit_requested(void);
void task_set_memory_hint(uint32_t bytes);
uint32_t task_process_memory_bytes(uint32_t process_id);
void task_bind_window(struct gui_window *window);
uint32_t task_count(void);
uint32_t task_snapshot_public(task_public_snapshot_t *out, uint32_t capacity);
/* W3 diagnostic: snapshot scheduler/task state immediately before a gate timeout. */
void task_debug_dump_all(const char *reason);
const task_t *task_get(uint32_t index);
uint32_t task_current_pid(void);
uint32_t task_current_process_id(void);
bool task_process_user_range_occupied(uint32_t process_id,
                                      uint32_t address, uint32_t length);
uint32_t task_current_parent_pid(void);
uint32_t task_current_console_route(void);
void task_set_current_console_route(uint32_t route);
const char *task_current_cwd(void);
bool task_set_current_cwd(const char *path);
bool task_current_is_user(void);
bool task_current_is_linux_abi(void);
bool task_set_current_linux_abi(bool enabled);
bool task_set_process_linux_abi(uint32_t process_id, bool enabled);
bool task_configure_linux_start(uint32_t process_id, const char *argv0,
                                uint32_t start_entry, uint32_t program_entry,
                                uint32_t phdr,
                                uint32_t phent, uint32_t phnum,
                                uint32_t interpreter_base,
                                uint32_t init_list);
bool task_current_is_idle(void);
uint32_t task_current_cpu(void);

/* Internal SMP bootstrap hooks used by smp.c. */
bool task_smp_prepare_cpu(uint32_t cpu_index, void *stack_allocation,
                          uint32_t *stack, uint32_t stack_size);
void task_smp_ap_online(uint32_t cpu_index);
/* Called by smp_start_scheduler() while interrupts are masked. It seeds the
 * per-CPU runqueues and removes idle contexts from the normal process table. */
bool task_smp_scheduler_start(void);
void task_fpu_prepare_secondary_cpu(void);
int32_t task_waitpid(uint32_t pid, int32_t *status);
bool task_queue_user_upcall(uint32_t pid, uint32_t entry,
                            const uint32_t *arguments, uint8_t argument_count,
                            const void *payload, uint8_t payload_size,
                            int8_t payload_argument);
bool task_queue_window_upcall(struct gui_window *window,
                              uint32_t pid, uint32_t entry,
                              const uint32_t *arguments,
                              uint8_t argument_count,
                              const void *payload, uint8_t payload_size,
                              int8_t payload_argument);
/* Queue a callback whose arguments contain one temporary user-visible heap
 * block. Ownership transfers only on success; the block is freed after the
 * callback returns or if the target task exits before consuming it. */
bool task_queue_user_upcall_owned(uint32_t pid, uint32_t entry,
                                  const uint32_t *arguments,
                                  uint8_t argument_count,
                                  void *release_after_upcall);
/* Cancel queued GUI callbacks that still reference a window before it is
 * freed. Returns false while a callback for that window is already active. */
bool task_cancel_window_upcalls(struct gui_window *window);
void task_block_user_upcalls(bool blocked);
bool task_prepare_user_upcall(registers_t *regs);
bool task_finish_user_upcall(registers_t *regs);
uint8_t task_cpu_usage(void);
uint32_t task_cpu_count(void);
uint8_t task_cpu_usage_for(uint32_t cpu_index);
uint32_t task_runqueue_depth(uint32_t cpu_index);
uint32_t task_scheduler_steals(uint32_t cpu_index);
uint32_t task_scheduler_migrations(uint32_t cpu_index);
uint32_t task_scheduler_ipis(uint32_t cpu_index);
bool task_set_affinity_mask(uint32_t pid, uint32_t mask);
uint32_t task_get_affinity_mask(uint32_t pid);
const char *task_state_name(task_state_t state);
const char *task_launch_arg(void);
bool task_set_user_fs_base(uint32_t pid, uint32_t base);
bool task_get_user_stack_bounds(uint32_t pid, uint32_t *limit_out,
                                uint32_t *base_out);
/* Export a kernel-owned string through a stable user-heap mirror for the
 * current Ring-3 task. Kernel callers receive the original pointer. */
const char *task_user_export_string(const char *source);
/* Legacy bk_proc_get() compatibility without exposing the supervisor-only
 * scheduler table. The returned copy lives in the current task's user heap. */
const task_t *task_user_export_snapshot(const task_t *source);

#endif
