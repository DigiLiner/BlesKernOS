#ifndef BLESKERNOS_POSIX_SYS_UTSNAME_H
#define BLESKERNOS_POSIX_SYS_UTSNAME_H
struct utsname { char sysname[32],nodename[32],release[32],version[64],machine[32]; };
int uname(struct utsname *name);
#endif
