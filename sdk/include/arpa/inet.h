#ifndef BLESKERNOS_POSIX_ARPA_INET_H
#define BLESKERNOS_POSIX_ARPA_INET_H

#include <netinet/in.h>

int inet_aton(const char *text, struct in_addr *address);
in_addr_t inet_addr(const char *text);
const char *inet_ntop(int family, const void *address, char *text,
                      socklen_t capacity);

#endif
