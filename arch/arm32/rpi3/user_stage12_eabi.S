.syntax unified
.arm
.text
.align 2

.global __aeabi_uidiv
.type __aeabi_uidiv, %function
__aeabi_uidiv:
    mov r2, #0
    b rpi3_user_udivmod32

.global __aeabi_uidivmod
.type __aeabi_uidivmod, %function
__aeabi_uidivmod:
    push {r4, lr}
    sub sp, sp, #8
    mov r2, sp
    bl rpi3_user_udivmod32
    ldr r1, [sp]
    add sp, sp, #8
    pop {r4, pc}

.global __aeabi_idiv
.type __aeabi_idiv, %function
__aeabi_idiv:
    mov r2, #0
    b rpi3_user_sdivmod32

.global __aeabi_idivmod
.type __aeabi_idivmod, %function
__aeabi_idivmod:
    push {r4, lr}
    sub sp, sp, #8
    mov r2, sp
    bl rpi3_user_sdivmod32
    ldr r1, [sp]
    add sp, sp, #8
    pop {r4, pc}

.global __aeabi_uldivmod
.type __aeabi_uldivmod, %function
__aeabi_uldivmod:
    push {r4, lr}
    sub sp, sp, #24
    add r4, sp, #4
    str r4, [sp]
    bl rpi3_user_udivmod64_words
    ldr r0, [sp, #4]
    ldr r1, [sp, #8]
    ldr r2, [sp, #12]
    ldr r3, [sp, #16]
    add sp, sp, #24
    pop {r4, pc}
