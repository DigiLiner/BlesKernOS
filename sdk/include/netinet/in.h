#ifndef BLESKERNOS_POSIX_NETINET_IN_H
#define BLESKERNOS_POSIX_NETINET_IN_H

#include <sys/socket.h>
#include <stdint.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

#define INADDR_ANY      ((in_addr_t)0x00000000U)
#define INADDR_LOOPBACK ((in_addr_t)0x0100007FU)

uint16_t htons(uint16_t value);
uint16_t ntohs(uint16_t value);
uint32_t htonl(uint32_t value);
uint32_t ntohl(uint32_t value);

#endif
