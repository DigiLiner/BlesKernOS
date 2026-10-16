#ifndef BLESKERNOS_POSIX_SYS_TIME_H
#define BLESKERNOS_POSIX_SYS_TIME_H

#include <sys/types.h>

typedef signed int suseconds_t;
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
int gettimeofday(struct timeval *value, void *timezone_unused);

#endif
