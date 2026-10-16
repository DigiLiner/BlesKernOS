.section .text
.global _start
.type _start,@function
.extern blesk_dependency_value
.extern blesk_tls_value
_start:
    call __x86_get_pc_thunk_bx
    addl $_GLOBAL_OFFSET_TABLE_, %ebx
    call blesk_dependency_value@PLT
    cmpl $41, %eax
    jne dynamic_fail
    call blesk_tls_value@PLT
    cmpl $41, %eax
    jne dynamic_fail
    subl $32, %esp
    movl $355, %eax                /* getrandom(buffer,8,0) */
    movl %esp, %ebx
    movl $8, %ecx
    xorl %edx, %edx
    int $0x80
    cmpl $8, %eax
    jne dynamic_fail_stack
    movl $340, %eax                /* prlimit64(0,RLIMIT_NOFILE,NULL,out) */
    xorl %ebx, %ebx
    movl $7, %ecx
    xorl %edx, %edx
    leal 8(%esp), %esi
    int $0x80
    testl %eax, %eax
    jne dynamic_fail_stack
    movl $242, %eax                /* sched_getaffinity(0,4,&mask) */
    xorl %ebx, %ebx
    movl $4, %ecx
    leal 24(%esp), %edx
    int $0x80
    cmpl $4, %eax
    jne dynamic_fail_stack
    cmpl $0, 24(%esp)
    je dynamic_fail_stack
    movl $221, %eax                /* fcntl64(stdout,F_GETFD) */
    movl $1, %ebx
    movl $1, %ecx
    int $0x80
    testl %eax, %eax
    jne dynamic_fail_stack
    movl $199, %eax                /* getuid32 */
    int $0x80
    testl %eax, %eax
    jne dynamic_fail_stack
    addl $32, %esp

    /* pthread-style clone: shared VM/files/signals, parent+child TID words
       and CHILD_CLEARTID.  The child resumes after int 0x80 with eax=0. */
    call __x86_get_pc_thunk_bx
    addl $_GLOBAL_OFFSET_TABLE_, %ebx
    movl %ebx, %ebp                /* preserve GOT base across clone args */
    leal clone_parent_tid@GOTOFF(%ebp), %edx
    leal clone_child_tid@GOTOFF(%ebp), %edi
    movl $0, (%edx)
    movl $0, (%edi)
    leal clone_stack+4096@GOTOFF(%ebp), %ecx
    xorl %esi, %esi
    movl $0x01350f00, %ebx         /* pthread flags, no CLONE_SETTLS */
    movl $120, %eax                /* clone */
    int $0x80
    testl %eax, %eax
    jz clone_child
    js dynamic_fail
    cmpl %eax, clone_parent_tid@GOTOFF(%ebp)
    jne dynamic_fail
    movl $10000, %ecx
clone_wait:
    cmpl $0xC10E, clone_marker@GOTOFF(%ebp)
    jne clone_yield
    cmpl $0, clone_child_tid@GOTOFF(%ebp)
    je clone_done
clone_yield:
    movl $158, %eax                /* sched_yield */
    int $0x80
    loop clone_wait
    jmp dynamic_fail
clone_done:
    movl %ebp, %ebx
    call 1f
dynamic_message:
    .ascii "LINUX DT_NEEDED+PLT+TLSGD+CLONE OK\n"
1:
    popl %ecx
    movl $4, %eax
    movl $1, %ebx
    movl $35, %edx
    int $0x80
    movl $252, %eax                /* exit_group(0) */
    xorl %ebx, %ebx
    int $0x80
dynamic_fail:
    movl $252, %eax
    movl $42, %ebx
    int $0x80
dynamic_fail_stack:
    addl $32, %esp
    jmp dynamic_fail

clone_child:
    movl $0xC10E, clone_marker@GOTOFF(%ebp)
    movl $1, %eax                  /* exit only this thread */
    xorl %ebx, %ebx
    int $0x80

.section .bss
.align 16
clone_parent_tid:
    .long 0
clone_child_tid:
    .long 0
clone_marker:
    .long 0
.align 16
clone_stack:
    .skip 4096

.section .text
.hidden __x86_get_pc_thunk_bx
.type __x86_get_pc_thunk_bx,@function
__x86_get_pc_thunk_bx:
    movl (%esp), %ebx
    ret
