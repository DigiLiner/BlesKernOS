.syntax unified
.cpu cortex-a7
.arm

.section .rodata.user_stage11_blob,"a"
.align 4
.global rpi3_stage11_user_elf_start
.global rpi3_stage11_user_elf_end
rpi3_stage11_user_elf_start:
    .incbin "build/rpi3/ARMTEST.BEX"
rpi3_stage11_user_elf_end:
