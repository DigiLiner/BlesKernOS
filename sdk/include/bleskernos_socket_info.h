#ifndef BLESKERNOS_SOCKET_INFO_H
#define BLESKERNOS_SOCKET_INFO_H
/* API 40. TCP states follow CLOSED, ALLOCATED, SYN_SENT, ESTABLISHED,
   CLOSE_WAIT, FIN_WAIT, LISTEN, SYN_RECEIVED. UDP state is 1. */
typedef struct {
    uint32_t handle,type,state,pending;
    uint8_t remote_ip[4];
    uint16_t local_port,remote_port;
} bk_socket_info_t;
#endif
