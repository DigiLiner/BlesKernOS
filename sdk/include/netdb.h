#ifndef BLESKERNOS_POSIX_NETDB_H
#define BLESKERNOS_POSIX_NETDB_H

#include <sys/socket.h>

struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE 0x1
#define EAI_NONAME -2
#define EAI_MEMORY -10
#define EAI_FAMILY -6
#define EAI_SERVICE -8

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result);
void freeaddrinfo(struct addrinfo *result);
const char *gai_strerror(int error);

#endif
