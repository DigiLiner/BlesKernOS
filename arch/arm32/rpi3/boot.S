.syntax unified
.cpu cortex-a7
.arm

/* ARM processor modes used by the exception vectors. */
.equ MODE_FIQ, 0x11
.equ MODE_IRQ, 0x12
.equ MODE_SVC, 0x13
.equ MODE_ABT, 0x17
.equ MODE_UND, 0x1b

.section .text.boot,"ax"
.global _start
.type _start, %function
_start:
    cpsid if

    /* Firmware starts all four cores. Keep only core 0 in the bootstrap. */
    mrc p15, 0, r3, c0, c0, 5
    and r3, r3, #3
    cmp r3, #0
    bne .Lpark

    /* Preserve the firmware-provided device-tree pointer (r2 in AArch32). */
    mov r4, r2

    /* Every exception mode has a banked SP. Set them before enabling IRQs. */
    cps #MODE_IRQ
    ldr sp, =__irq_stack_top
    cps #MODE_FIQ
    ldr sp, =__fiq_stack_top
    cps #MODE_ABT
    ldr sp, =__abt_stack_top
    cps #MODE_UND
    ldr sp, =__und_stack_top
    cps #MODE_SVC
    ldr sp, =__svc_stack_top

    /* Zero .bss before entering C. */
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
.Lzero_bss:
    cmp r0, r1
    strlo r2, [r0], #4
    blo .Lzero_bss

    mov r0, r4
    bl rpi3_boot_main

.Lhang:
    wfe
    b .Lhang

.Lpark:
    wfe
    b .Lpark
.size _start, . - _start
