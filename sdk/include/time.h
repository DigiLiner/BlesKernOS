#ifndef BLESKERNOS_POSIX_TIME_H
#define BLESKERNOS_POSIX_TIME_H

#include <sys/types.h>
#include <stddef.h>

typedef signed int clockid_t;
typedef signed int clock_t;

struct timespec {
    time_t tv_sec;
    signed long tv_nsec;
};
struct tm { int tm_sec,tm_min,tm_hour,tm_mday,tm_mon,tm_year,tm_wday,tm_yday,tm_isdst; };

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC 1000

int clock_gettime(clockid_t clock_id, struct timespec *value);
int nanosleep(const struct timespec *request, struct timespec *remaining);
time_t time(time_t *value);
clock_t clock(void);
double difftime(time_t end, time_t beginning);
char *asctime(const struct tm *value);
struct tm *gmtime(const time_t *value);
struct tm *localtime(const time_t *value);
time_t mktime(struct tm *value);
size_t strftime(char *destination, size_t capacity, const char *format,
                const struct tm *value);

#endif
