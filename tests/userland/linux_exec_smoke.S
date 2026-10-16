.section .text
.global _start
.type _start,@function
_start:
    call 1f
1:
    popl %esi
    cmpl $0x08048000, %esi
    jb exec_fail
    cmpl $0x08100000, %esi
    jae exec_fail
    movl $20, %eax
    int $0x80
    testl %eax, %eax
    jz exec_fail
    movl %eax, %edi
    movl %eax, private_marker
    movl $64, %ecx
2:
    movl $158, %eax
    int $0x80
    loop 2b
    cmpl %edi, private_marker
    jne exec_fail
    movl $4, %eax
    movl $1, %ebx
    movl $exec_message, %ecx
    movl $32, %edx
    int $0x80
    movl $252, %eax
    xorl %ebx, %ebx
    int $0x80
exec_fail:
    movl $252, %eax
    movl $71, %ebx
    int $0x80

.section .data
.align 4
private_marker:
    .long 0
exec_message:
    .ascii "LINUX ET_EXEC 0x08048000 CR3 OK\n"
