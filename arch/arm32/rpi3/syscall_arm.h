#ifndef BLES_ARCH_ARM32_RPI3_SYSCALL_ARM_H
#define BLES_ARCH_ARM32_RPI3_SYSCALL_ARM_H

#include "task_arm.h"

/* Dispatch one SVC raised from ARM USR mode. Kernel/System-mode SVC remains
 * the scheduler's yield trap and is handled separately in irq.c. */
arm_task_frame_t *rpi3_syscall_dispatch(arm_task_frame_t *frame);

#endif
