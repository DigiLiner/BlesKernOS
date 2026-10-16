.section .text
.global _start
.type _start,@function
_start:
    movl %esp, %ebp
    movl (%esp), %eax
    testl %eax, %eax
    jle loader_fail
    leal 8(%esp,%eax,4), %edi
1:  cmpl $0, (%edi)
    je 2f
    addl $4, %edi
    jmp 1b
2:  addl $4, %edi
    xorl %esi, %esi
    xorl %ebx, %ebx
3:  movl (%edi), %eax
    movl 4(%edi), %edx
    testl %eax, %eax
    jz 5f
    cmpl $9, %eax                  /* AT_ENTRY */
    jne 7f
    movl %edx, %esi
    jmp 4f
7:  cmpl $0x6b01, %eax             /* AT_BLESK_INIT_LIST */
    jne 4f
    movl %edx, %ebx
4:  addl $8, %edi
    jmp 3b
5:  testl %esi, %esi
    jz loader_fail
    testl %ebx, %ebx
    jz 8f
    movl (%ebx), %edi
    addl $4, %ebx
9:  testl %edi, %edi
    jz 8f
    call *(%ebx)
    addl $4, %ebx
    decl %edi
    jmp 9b
8:
    movl %ebp, %esp
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %edi, %edi
    xorl %ebp, %ebp
    jmp *%esi
loader_fail:
    movl $252, %eax                /* exit_group(127) */
    movl $127, %ebx
    int $0x80
6:  jmp 6b

/* i386 TLS general-dynamic ABI. GCC passes Elf32_TLS_Index* in EAX.
 * gs:4 points at the loader-created DTV whose entries are module TLS bases. */
.global ___tls_get_addr
.type ___tls_get_addr,@function
___tls_get_addr:
    movl %gs:4, %edx
    testl %edx, %edx
    jz tls_fail
    movl (%eax), %ecx
    testl %ecx, %ecx
    jz tls_fail
    movl (%edx,%ecx,4), %edx
    testl %edx, %edx
    jz tls_fail
    movl 4(%eax), %eax
    addl %edx, %eax
    ret
tls_fail:
    xorl %eax, %eax
    ret

/* Opt-in explicito al enlazador didactico del kernel; no es glibc ld.so. */
.section .note.blesk,"a",@note
.balign 4
.long 6, 4, 0x6b01
.asciz "BLESK"
.balign 4
.long 1
.section .note.GNU-stack,"",@progbits
