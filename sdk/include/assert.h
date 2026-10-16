#ifndef BLESKERNOS_POSIX_ASSERT_H
#define BLESKERNOS_POSIX_ASSERT_H
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#include <stdlib.h>
#define assert(expression) ((expression) ? (void)0 : abort())
#endif
#endif
