#include <setjmp.h>

/* i386 SysV context: ebx, esi, edi, ebp, caller esp, return eip. */
__attribute__((naked)) int setjmp(jmp_buf environment)
{
    (void)environment;
    __asm__ volatile(
        "movl 4(%esp), %eax\n\t"
        "movl %ebx, 0(%eax)\n\t"
        "movl %esi, 4(%eax)\n\t"
        "movl %edi, 8(%eax)\n\t"
        "movl %ebp, 12(%eax)\n\t"
        "leal 4(%esp), %edx\n\t"
        "movl %edx, 16(%eax)\n\t"
        "movl 0(%esp), %edx\n\t"
        "movl %edx, 20(%eax)\n\t"
        "xorl %eax, %eax\n\t"
        "ret\n\t");
}

__attribute__((naked, noreturn)) void longjmp(jmp_buf environment, int value)
{
    (void)environment;
    (void)value;
    __asm__ volatile(
        "movl 4(%esp), %edx\n\t"
        "movl 8(%esp), %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "incl %eax\n"
        "1:\n\t"
        "movl 0(%edx), %ebx\n\t"
        "movl 4(%edx), %esi\n\t"
        "movl 8(%edx), %edi\n\t"
        "movl 12(%edx), %ebp\n\t"
        "movl 16(%edx), %esp\n\t"
        "jmp *20(%edx)\n\t");
}
