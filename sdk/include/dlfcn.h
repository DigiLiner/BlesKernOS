#ifndef BLESKERNOS_POSIX_DLFCN_H
#define BLESKERNOS_POSIX_DLFCN_H
#include "bleskernos_lbs_stats.h"
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT ((void *)-1)
void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);
#endif
