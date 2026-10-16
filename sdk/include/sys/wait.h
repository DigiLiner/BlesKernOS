#ifndef BLESKERNOS_POSIX_SYS_WAIT_H
#define BLESKERNOS_POSIX_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1
#define WEXITSTATUS(status) ((status) & 0xFF)
#define WIFEXITED(status) 1

pid_t waitpid(pid_t pid, int *status, int options);

#endif
