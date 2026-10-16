.syntax unified
.cpu cortex-a7
.arm

.section .vectors,"ax"
.align 5
.global arm_vector_table
.type arm_vector_table, %object
arm_vector_table:
    b arm_reset_vector
    b arm_undefined_vector
    b arm_svc_vector
    b arm_prefetch_abort_vector
    b arm_data_abort_vector
    b arm_reserved_vector
    b arm_irq_vector
    b arm_fiq_vector
.size arm_vector_table, . - arm_vector_table

/* Compact fatal frame used when an exception came from privileged code. */
.macro SAVE_PANIC_FRAME
    sub sp, sp, #64
    stmia sp, {r0-r12, lr}
    mrs r0, spsr
    str r0, [sp, #56]
.endm

.macro PANIC_VECTOR name, kind
.global \name
.type \name, %function
\name:
    SAVE_PANIC_FRAME
    mov r0, #\kind
    mov r1, sp
    bl arm_exception_panic
1:  wfe
    b 1b
.size \name, . - \name
.endm

PANIC_VECTOR arm_reset_vector,    0
PANIC_VECTOR arm_reserved_vector, 5
PANIC_VECTOR arm_fiq_vector,      7

/* Schedulable arm_task_frame_t.  The ^ form accesses the banked USR/System
 * SP/LR while vectors execute in IRQ/SVC/ABT/UND modes. */
.equ TF_SP,   52
.equ TF_LR,   56
.equ TF_PC,   60
.equ TF_CPSR, 64
.equ TF_SIZE, 72

.macro SAVE_TASK_FRAME pc_adjust
    sub sp, sp, #TF_SIZE
    stmia sp, {r0-r12}
    add r0, sp, #TF_SP
    stmia r0, {sp, lr}^
    nop
    .if \pc_adjust
        sub r0, lr, #\pc_adjust
    .else
        mov r0, lr
    .endif
    str r0, [sp, #TF_PC]
    mrs r0, spsr
    str r0, [sp, #TF_CPSR]
.endm

.macro RESTORE_TASK_FRAME
    mov r3, r0
    add r1, r3, #TF_SP
    ldmia r1, {sp, lr}^
    nop
    ldr r1, [r3, #TF_CPSR]
    msr spsr_cxsf, r1
    ldr lr, [r3, #TF_PC]
    add sp, sp, #TF_SIZE
    ldmia r3, {r0-r12}
    movs pc, lr
.endm

/* A bad USR program must not panic the whole OS.  If SPSR says the exception
 * originated in USR mode, construct a scheduler frame, terminate that task,
 * and resume another one. Privileged faults keep the detailed fatal path. */
.macro USER_FAULT_VECTOR name, kind, pc_adjust
.global \name
.type \name, %function
\name:
    mrs r0, spsr
    and r0, r0, #0x1f
    cmp r0, #0x10
    bne 9f
    cpsid i
    SAVE_TASK_FRAME \pc_adjust
    mov r0, #\kind
    mov r1, sp
    bl arm_user_fault_dispatch
    RESTORE_TASK_FRAME
9:
    SAVE_PANIC_FRAME
    mov r0, #\kind
    mov r1, sp
    bl arm_exception_panic
8:  wfe
    b 8b
.size \name, . - \name
.endm

USER_FAULT_VECTOR arm_undefined_vector,      1, 4
USER_FAULT_VECTOR arm_prefetch_abort_vector, 3, 4
USER_FAULT_VECTOR arm_data_abort_vector,     4, 8

.global arm_irq_vector
.type arm_irq_vector, %function
arm_irq_vector:
    SAVE_TASK_FRAME 4
    mov r0, sp
    bl arm_irq_dispatch
    RESTORE_TASK_FRAME
.size arm_irq_vector, . - arm_irq_vector

/* Kernel/System SVC #0 is the scheduler yield trap.  USR SVC #0 uses r7 as
 * the Stage 11 public syscall number and is routed by arm_svc_dispatch(). */
.global arm_svc_vector
.type arm_svc_vector, %function
arm_svc_vector:
    /* Do not allow the timer IRQ to preempt a banked SVC stack. The saved
     * task CPSR still contains the caller's IRQ state and restores it on the
     * exception return. */
    cpsid i
    SAVE_TASK_FRAME 0
    mov r0, sp
    bl arm_svc_dispatch
    RESTORE_TASK_FRAME
.size arm_svc_vector, . - arm_svc_vector
