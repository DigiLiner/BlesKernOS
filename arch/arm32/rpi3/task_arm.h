#ifndef BLES_ARCH_ARM32_RPI3_TASK_ARM_H
#define BLES_ARCH_ARM32_RPI3_TASK_ARM_H

#include "../../../kernel/include/types.h"

/* Full AArch32 kernel-task context used at IRQ/SVC boundaries. All Stage 10
 * tasks execute in System mode, so the IRQ/SVC vectors can access their
 * unbanked SP/LR through the ARM user-register transfer form (the ^ suffix). */
typedef struct arm_task_frame {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t r12;
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
} arm_task_frame_t;

#define ARM_TASK_FRAME_BYTES 68U

void rpi3_task_init(void);
int rpi3_task_create(const char *name, void (*entry)(void *), void *argument,
                     bool idle_task);
void rpi3_task_start(void) NORETURN;
arm_task_frame_t *rpi3_task_irq_schedule(arm_task_frame_t *frame);
arm_task_frame_t *rpi3_task_svc_schedule(arm_task_frame_t *frame);
bool rpi3_task_scheduler_started(void);
uint32_t rpi3_task_switch_count(void);
uint32_t rpi3_task_live_count(void);
typedef struct rpi3_task_public_info {
    uint32_t pid;
    uint32_t process_id;
    char name[24];
    uint32_t state;
    uint32_t cpu_ticks;
    uint32_t memory_hint;
    bool idle;
    bool user;
} rpi3_task_public_info_t;
uint32_t rpi3_task_snapshot(rpi3_task_public_info_t *infos, uint32_t capacity);
uint32_t rpi3_task_ready_count(void);

/* Assembly restores this synthetic context and enters System mode. */
void rpi3_task_enter_first(arm_task_frame_t *frame) NORETURN;

#define RPI3_USER_SLOT_BASE  0x10000000U
#define RPI3_USER_SLOT_BYTES 0x00400000U
#define RPI3_USER_SLOT_COUNT 8U
#define RPI3_USER_HEAP_END_OFF 0x00300000U

int rpi3_task_user_slot_acquire(uint32_t *slot_out, uint32_t *base_out,
                                uint32_t *bytes_out);
void rpi3_task_user_slot_release(uint32_t slot);
int rpi3_task_create_user_image(const char *name, uint32_t entry,
                                uint32_t return_pc, uint32_t upcall_return_pc,
                                uint32_t user_slot,
                                uint32_t image_end, const char *launch_arg);
bool rpi3_task_current_launch_arg_copy(char *buffer, uint32_t capacity);
bool rpi3_task_current_user_range_ok(uint32_t address, uint32_t bytes,
                                     bool write);
void *rpi3_task_user_alloc(uint32_t bytes);
void *rpi3_task_user_realloc(void *pointer, uint32_t bytes);
bool rpi3_task_user_free(void *pointer);
void rpi3_task_svc_exit(int32_t status);
void rpi3_task_svc_sleep(uint32_t ticks);
arm_task_frame_t *rpi3_task_user_fault(uint32_t kind, arm_task_frame_t *frame);
bool rpi3_task_prepare_user_upcall(arm_task_frame_t *frame);
bool rpi3_task_finish_user_upcall(arm_task_frame_t *frame);
bool rpi3_task_current_upcall_argument(uint32_t index, uint32_t value);

#endif
