#ifndef BLESKERNOS_POSIX_SYS_SELECT_H
#define BLESKERNOS_POSIX_SYS_SELECT_H

#include <sys/time.h>
#include <sys/types.h>

#define FD_SETSIZE 1024
typedef struct { unsigned int bits[FD_SETSIZE / 32]; } fd_set;
#define FD_ZERO(set) do { for (unsigned int _i=0; _i<FD_SETSIZE/32; _i++) (set)->bits[_i]=0U; } while (0)
#define FD_SET(fd,set) ((set)->bits[(unsigned)(fd)/32U] |= 1U<<((unsigned)(fd)%32U))
#define FD_CLR(fd,set) ((set)->bits[(unsigned)(fd)/32U] &= ~(1U<<((unsigned)(fd)%32U)))
#define FD_ISSET(fd,set) (((set)->bits[(unsigned)(fd)/32U] & (1U<<((unsigned)(fd)%32U))) != 0U)

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#endif
