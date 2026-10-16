#ifndef BLES_ARCH_ARM32_RPI3_EXCEPTIONS_H
#define BLES_ARCH_ARM32_RPI3_EXCEPTIONS_H

#include "../../../kernel/include/types.h"

typedef enum arm_exception_kind {
    ARM_EXC_RESET = 0,
    ARM_EXC_UNDEFINED = 1,
    ARM_EXC_SVC = 2,
    ARM_EXC_PREFETCH_ABORT = 3,
    ARM_EXC_DATA_ABORT = 4,
    ARM_EXC_RESERVED = 5,
    ARM_EXC_IRQ = 6,
    ARM_EXC_FIQ = 7
} arm_exception_kind_t;

typedef struct arm_exception_frame {
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
    uint32_t lr;
    uint32_t spsr;
} arm_exception_frame_t;

void arm_exceptions_init(void);
void arm_exception_panic(uint32_t kind, const arm_exception_frame_t *frame);
struct arm_task_frame;
struct arm_task_frame *arm_user_fault_dispatch(uint32_t kind, struct arm_task_frame *frame);

#endif
