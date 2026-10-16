.section .text

.global __libc_start_main
.type __libc_start_main,@function
__libc_start_main:
    pushl %ebp
    movl %esp,%ebp
    movl 20(%ebp),%eax             /* init */
    testl %eax,%eax
    jz 1f
    call *%eax
1:
    movl 12(%ebp),%ecx             /* argc */
    movl 16(%ebp),%edx             /* argv */
    leal 4(%edx,%ecx,4),%eax       /* envp */
    pushl %eax
    pushl %edx
    pushl %ecx
    call *8(%ebp)                  /* main(argc,argv,envp) */
    addl $12,%esp
    movl %eax,%ebx
    movl $252,%eax                 /* exit_group */
    int $0x80
    hlt

.global exit
.global _exit
.type exit,@function
.type _exit,@function
exit:
_exit:
    movl 4(%esp),%ebx
    movl $252,%eax
    int $0x80
    hlt

.global write
.type write,@function
write:
    pushl %ebx
    movl 8(%esp),%ebx
    movl 12(%esp),%ecx
    movl 16(%esp),%edx
    movl $4,%eax
    int $0x80
    popl %ebx
    ret

.global read
.type read,@function
read:
    pushl %ebx
    movl 8(%esp),%ebx
    movl 12(%esp),%ecx
    movl 16(%esp),%edx
    movl $3,%eax
    int $0x80
    popl %ebx
    ret

.global strlen
.type strlen,@function
strlen:
    movl 4(%esp),%edx
    xorl %eax,%eax
1:
    cmpb $0,(%edx,%eax)
    je 2f
    incl %eax
    jmp 1b
2:
    ret

.global puts
.type puts,@function
puts:
    pushl %ebx
    pushl %esi
    movl 12(%esp),%esi
    xorl %edx,%edx
1:
    cmpb $0,(%esi,%edx)
    je 2f
    incl %edx
    jmp 1b
2:
    movl $4,%eax
    movl $1,%ebx
    movl %esi,%ecx
    int $0x80
    testl %eax,%eax
    js 3f
    /* Keep this compatibility DSO genuinely position independent. Putting
       the byte on the stack avoids a text relocation for one constant. */
    pushl $10
    movl $4,%eax
    movl $1,%ebx
    movl %esp,%ecx
    movl $1,%edx
    int $0x80
    addl $4,%esp
    testl %eax,%eax
    js 3f
    xorl %eax,%eax
3:
    popl %esi
    popl %ebx
    ret
