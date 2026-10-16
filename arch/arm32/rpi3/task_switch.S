.syntax unified
.cpu cortex-a7
.arm

/* arm_task_frame_t offsets. */
.equ TF_SP,   52
.equ TF_LR,   56
.equ TF_PC,   60
.equ TF_CPSR, 64

.section .text,"ax"
.global rpi3_task_enter_first
.type rpi3_task_enter_first, %function
rpi3_task_enter_first:
    /* Called in SVC mode after boot. Load the System/User banked SP/LR,
     * install the target CPSR into SPSR_svc, then exception-return into the
     * synthetic task PC. */
    mov r3, r0
    add r1, r3, #TF_SP
    ldmia r1, {sp, lr}^
    nop
    ldr r1, [r3, #TF_CPSR]
    msr spsr_cxsf, r1
    ldr lr, [r3, #TF_PC]
    ldmia r3, {r0-r12}
    movs pc, lr
1:  wfe
    b 1b
.size rpi3_task_enter_first, . - rpi3_task_enter_first
