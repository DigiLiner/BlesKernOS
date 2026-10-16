#ifndef BLESKERNOS_POSIX_STDDEF_H
#define BLESKERNOS_POSIX_STDDEF_H

typedef unsigned int size_t;
typedef signed int ptrdiff_t;
#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
