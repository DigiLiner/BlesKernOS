#ifndef BLESKERNOS_POSIX_SETJMP_H
#define BLESKERNOS_POSIX_SETJMP_H

typedef unsigned int jmp_buf[6];

#ifdef __cplusplus
extern "C" {
#endif
int setjmp(jmp_buf environment) __attribute__((returns_twice));
void longjmp(jmp_buf environment, int value) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif

#endif
