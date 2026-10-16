.section .text
.global _start
.type _start,@function
_start:
    movl 4(%esp), %ebp
    movl (%esp), %eax
    testl %eax, %eax
    jle stack_bad
    leal 8(%esp,%eax,4), %edi       /* first envp after argv NULL */
env_loop:
    cmpl $0, (%edi)
    je env_done
    addl $4, %edi
    jmp env_loop
env_done:
    addl $4, %edi                   /* first auxv entry */
auxv_loop:
    movl (%edi), %eax
    testl %eax, %eax
    jz stack_bad
    cmpl $6, %eax
    je pagesz_found
    addl $8, %edi
    jmp auxv_loop
pagesz_found:
    cmpl $4096, 4(%edi)
    jne stack_bad
    subl $512, %esp
    movl $24, %eax                  /* getuid */
    int $0x80
    testl %eax, %eax
    js stack_bad
    movl $224, %eax                 /* gettid */
    int $0x80
    testl %eax, %eax
    jle stack_bad
    movl $122, %eax                 /* uname */
    movl %esp, %ebx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    movl $265, %eax                 /* clock_gettime(CLOCK_MONOTONIC) */
    movl $1, %ebx
    leal 400(%esp), %ecx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    cmpl $1000000000, 404(%esp)
    jae stack_bad
    movl $78, %eax                  /* gettimeofday */
    leal 416(%esp), %ebx
    xorl %ecx, %ecx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    movl $195, %eax                 /* stat64(argv[0]) */
    movl %ebp, %ebx
    leal 256(%esp), %ecx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    movl $0, 448(%esp)
    movl $258, %eax                 /* set_tid_address */
    leal 448(%esp), %ebx
    int $0x80
    testl %eax, %eax
    jle stack_bad
    movl $13, %eax                  /* time */
    leal 416(%esp), %ebx
    int $0x80
    testl %eax, %eax
    js stack_bad
    movl $116, %eax                 /* sysinfo */
    leal 424(%esp), %ebx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    movl $191, %eax                 /* ugetrlimit(RLIMIT_NOFILE) */
    movl $7, %ebx
    leal 496(%esp), %ecx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    movl %esp, %ebx
    andl $0xfffff000, %ebx
    movl $219, %eax                 /* madvise(MADV_NORMAL) */
    movl $4096, %ecx
    xorl %edx, %edx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    movl $0, 408(%esp)
    movl $1, 412(%esp)
    movl $162, %eax                 /* nanosleep(1 ns) */
    leal 408(%esp), %ebx
    xorl %ecx, %ecx
    int $0x80
    testl %eax, %eax
    jne stack_bad
    call 1f
message:
    .ascii "LINUX PIE ABI17 RUNTIME OK\n"
1:
    popl %ecx
    movl $4, %eax
    movl $1, %ebx
    movl $27, %edx
    int $0x80
    movl $252, %eax
    xorl %ebx, %ebx
    int $0x80
2:
    movl $158, %eax
    int $0x80
    jmp 2b
stack_bad:
    call 3f
bad_message:
    .ascii "LINUX PIE STACK FAIL\n"
3:
    popl %ecx
    movl $4, %eax
    movl $1, %ebx
    movl $21, %edx
    int $0x80
    movl $252, %eax
    movl $31, %ebx
    int $0x80
