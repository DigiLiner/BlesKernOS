.section .text
.global _start
_start:
    movl $0x7fff,%eax
    movl $0x11111111,%ebx
    movl $0x22222222,%ecx
    movl $0x33333333,%edx
    movl $0x44444444,%esi
    movl $0x55555555,%edi
    movl $0x66666666,%ebp
    int $0x80
    cmpl $-38,%eax
    jne fail1
    cmpl $0x11111111,%ebx
    jne fail1
    cmpl $0x22222222,%ecx
    jne fail1
    cmpl $0x33333333,%edx
    jne fail1
    cmpl $0x44444444,%esi
    jne fail1
    cmpl $0x55555555,%edi
    jne fail1
    cmpl $0x66666666,%ebp
    jne fail1
    /* dup modifica ECX internamente. No debe modificar el frame Linux. */
    movl $41,%eax
    movl $-1,%ebx
    int $0x80
    cmpl $-9,%eax
    jne fail2
    cmpl $0x22222222,%ecx
    jne fail2
    /* old_mmap debe conservar incluso EBP al expandir sus seis argumentos. */
    movl $90,%eax
    movl $mmap_args,%ebx
    int $0x80
    testl %eax,%eax
    js fail3
    movl %eax,esp_saved
    cmpl $mmap_args,%ebx
    jne fail3
    cmpl $0x22222222,%ecx
    jne fail3
    cmpl $0x66666666,%ebp
    jne fail3
    movl $91,%eax
    movl esp_saved,%ebx
    movl $4096,%ecx
    int $0x80
    testl %eax,%eax
    jnz fail3
    movl $4,%eax
    movl $1,%ebx
    movl $message,%ecx
    movl $message_end-message,%edx
    int $0x80
    xorl %ebx,%ebx
exit:
    movl $252,%eax
    int $0x80
    jmp exit
fail1: movl $81,%ebx; jmp exit
fail2: movl $82,%ebx; jmp exit
fail3: movl $83,%ebx; jmp exit
.section .rodata
message: .ascii "[GATE:A] registros/ENOSYS/old_mmap PASS\n"
message_end:
.balign 4
mmap_args: .long 0,4096,3,0x22,-1,0
.section .bss
.balign 4
esp_saved: .skip 4
.section .note.GNU-stack,"",@progbits
