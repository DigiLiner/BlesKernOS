#include "../include/admin.h"
#include "../include/network.h"
#include "../include/memory.h"
#include "../include/task.h"
#include "../include/vga.h"

static const net_device_ops_t *g_device;
static const net_stack_ops_t *g_stack;
static const net_tls_ops_t *g_tls;
static net_rx_handler_t g_rx_handler;
static uint32_t g_rx_packets;
static uint32_t g_tx_packets;
static uint32_t g_rx_dropped;
static volatile uint32_t g_poll_lock;

static void network_spin_unlock(volatile uint32_t *lock) {
    __sync_synchronize();
    __sync_lock_release(lock);
}

bool netdev_register(const net_device_ops_t *ops) {
    if (!ops || !ops->name || !ops->mac || !ops->send ||
        ops->mtu < 576U || ops->mtu > 1500U || g_device) return false;
    g_device = ops;
    g_rx_packets = g_tx_packets = g_rx_dropped = 0;
    return true;
}

void netdev_unregister(const net_device_ops_t *ops) {
    if (g_device == ops) {
        g_device = NULL;
        g_rx_handler = NULL;
    }
}

bool netdev_present(void) { return g_device != NULL; }

bool netdev_send(const void *frame, uint16_t length) {
    if (!g_device || !frame || length < 14U || length > NET_FRAME_MAX) return false;
    if (!g_device->send(frame, length)) return false;
    __sync_fetch_and_add(&g_tx_packets, 1U);
    return true;
}

void netdev_poll(void) {
    const net_device_ops_t *device;
    if (!g_device || !g_device->poll) return;

    /* IMPORTANTE SMP: un bool normal + preempt_disable() sólo protegía al CPU
       local. Después de encender los AP, rtl8139-rx y una espera TCP/DNS en
       otro core podían entrar simultáneamente al ring RX y consumir el mismo
       descriptor. Eso explica que DHCP funcionase antes de SMP y que NetSurf
       perdiese DNS/TCP después. El lock es global entre CPUs y es try-lock: si
       otro core ya está drenando la NIC no hay trabajo adicional que hacer. */
    if (!__sync_bool_compare_and_swap(&g_poll_lock, 0U, 1U)) return;
    __sync_synchronize();
    device = g_device;
    if (device && device->poll) device->poll();
    network_spin_unlock(&g_poll_lock);
}

void netdev_receive(const void *frame, uint16_t length) {
    if (!g_device || !frame || length < 14U || length > NET_FRAME_MAX) {
        g_rx_dropped++;
        return;
    }
    g_rx_packets++;
    if (g_rx_handler) g_rx_handler((const uint8_t *)frame, length);
    else g_rx_dropped++;
}

void netdev_set_rx_handler(net_rx_handler_t handler) { g_rx_handler = handler; }

void netdev_get_info(net_info_t *info) {
    if (!info) return;
    kmemset(info, 0, sizeof(*info));
    if (g_device) {
        kstrncpy(info->device, g_device->name, sizeof(info->device) - 1U);
        kmemcpy(info->mac, g_device->mac, NET_MAC_LENGTH);
        info->mtu = g_device->mtu;
        info->link_up = g_device->link_up ? g_device->link_up() : true;
    }
    info->rx_packets = g_rx_packets;
    info->tx_packets = g_tx_packets;
    info->rx_dropped = g_rx_dropped;
}

bool network_register_stack(const net_stack_ops_t *ops) {
    if (!ops || !ops->get_info || g_stack) return false;
    g_stack = ops;
    return true;
}

void network_unregister_stack(const net_stack_ops_t *ops) {
    if (g_stack == ops) {
        g_stack = NULL;
        g_rx_handler = NULL;
    }
}

bool network_dhcp(uint32_t timeout_ms) {
    return g_stack && g_stack->dhcp && g_stack->dhcp(timeout_ms);
}

bool network_configure(const uint8_t address[4], const uint8_t netmask[4],
                       const uint8_t gateway[4], const uint8_t dns[4]) {
    return g_stack && g_stack->configure &&
           g_stack->configure(address, netmask, gateway, dns);
}

bool network_ping(const uint8_t address[4], uint32_t timeout_ms,
                  uint32_t *round_trip_ms) {
    return g_stack && g_stack->ping &&
           g_stack->ping(address, timeout_ms, round_trip_ms);
}

bool network_resolve(const char *hostname, uint8_t address[4],
                     uint32_t timeout_ms) {
    return g_stack && g_stack->resolve &&
           g_stack->resolve(hostname, address, timeout_ms);
}

int network_socket_open(uint8_t type) {
    return g_stack && g_stack->socket_open ? g_stack->socket_open(type) : -1;
}

bool network_socket_connect(int socket, const uint8_t address[4],
                            uint16_t port, uint32_t timeout_ms) {
    return g_stack && g_stack->socket_connect &&
           g_stack->socket_connect(socket, address, port, timeout_ms);
}

bool network_socket_bind(int socket, const uint8_t address[4], uint16_t port) {
    return g_stack && g_stack->socket_bind &&
           g_stack->socket_bind(socket, address, port);
}

bool network_socket_listen(int socket, uint8_t backlog) {
    return g_stack && g_stack->socket_listen &&
           g_stack->socket_listen(socket, backlog);
}

int network_socket_accept(int socket, uint8_t address[4], uint16_t *port,
                          uint32_t timeout_ms) {
    return g_stack && g_stack->socket_accept
        ? g_stack->socket_accept(socket, address, port, timeout_ms) : -1;
}

int32_t network_socket_send(int socket, const void *data, uint32_t length,
                            uint32_t timeout_ms) {
    return g_stack && g_stack->socket_send
        ? g_stack->socket_send(socket, data, length, timeout_ms) : -1;
}

int32_t network_socket_receive(int socket, void *data, uint32_t capacity,
                               uint32_t timeout_ms) {
    return g_stack && g_stack->socket_receive
        ? g_stack->socket_receive(socket, data, capacity, timeout_ms) : -1;
}

int32_t network_socket_sendto(int socket, const uint8_t address[4], uint16_t port,
                              const void *data, uint32_t length, uint32_t timeout_ms) {
    return g_stack && g_stack->socket_sendto
        ? g_stack->socket_sendto(socket, address, port, data, length, timeout_ms) : -1;
}

int32_t network_socket_receivefrom(int socket, uint8_t address[4], uint16_t *port,
                                   void *data, uint32_t capacity, uint32_t timeout_ms) {
    return g_stack && g_stack->socket_receivefrom
        ? g_stack->socket_receivefrom(socket, address, port, data, capacity, timeout_ms) : -1;
}

bool network_socket_readable(int socket) {
    return g_stack && g_stack->socket_readable && g_stack->socket_readable(socket);
}

void network_socket_close(int socket) {
    if (g_stack && g_stack->socket_close) g_stack->socket_close(socket);
}

int32_t network_http_get(const char *url, void *data, uint32_t capacity,
                         uint32_t timeout_ms) {
    return g_stack && g_stack->http_get
        ? g_stack->http_get(url, data, capacity, timeout_ms) : -1;
}

int32_t network_http_exchange(const char *url, const void *request,
                              uint32_t request_length, void *data,
                              uint32_t capacity, uint32_t timeout_ms) {
    return g_stack && g_stack->http_exchange
        ? g_stack->http_exchange(url, request, request_length, data,
                                 capacity, timeout_ms) : -1;
}

bool network_register_tls(const net_tls_ops_t *ops) {
    if (!ops || !ops->https_get || !ops->https_exchange || g_tls) return false;
    g_tls = ops;
    return true;
}

void network_unregister_tls(const net_tls_ops_t *ops) {
    if (g_tls == ops) g_tls = NULL;
}

int32_t network_https_get(const char *url, void *data, uint32_t capacity,
                          uint32_t timeout_ms) {
    return g_tls && g_tls->https_get
        ? g_tls->https_get(url, data, capacity, timeout_ms) : -1;
}

int32_t network_https_exchange(const char *url, const void *request,
                               uint32_t request_length, void *data,
                               uint32_t capacity, uint32_t timeout_ms) {
    return g_tls && g_tls->https_exchange
        ? g_tls->https_exchange(url, request, request_length, data,
                                capacity, timeout_ms) : -1;
}

int network_tls_last_error(void) {
    return g_tls && g_tls->last_error ? g_tls->last_error() : -1;
}

void network_get_info(net_info_t *info) {
    if (!info) return;
    netdev_get_info(info);
    if (g_stack) g_stack->get_info(info);
    info->stack_ready = g_stack != NULL;
    info->tls_ready = g_tls != NULL;
}

static void network_autoconfigure_task(void *argument UNUSED) {
    net_info_t info;
    if (admin_network_restore() || network_dhcp(10000U)) {
        network_get_info(&info);
        kprintf("[NET] Configuracion IPv4=%u.%u.%u.%u gateway=%u.%u.%u.%u DNS=%u.%u.%u.%u\n",
                info.address[0], info.address[1], info.address[2], info.address[3],
                info.gateway[0], info.gateway[1], info.gateway[2], info.gateway[3],
                info.dns[0], info.dns[1], info.dns[2], info.dns[3]);
#ifdef NETWORK_SELFTEST
        {
            uint8_t resolved[4];
            uint8_t *response = (uint8_t *)kmalloc(4096U);
            /* Dejar que el escritorio y sus tareas arranquen antes de probar
               para reproducir el uso interactivo de nettest. */
            task_sleep(3000U);
            if (network_resolve("example.com", resolved, 5000U))
                kprintf("[NET-TEST] DNS example.com=%u.%u.%u.%u\n",
                        resolved[0], resolved[1], resolved[2], resolved[3]);
            else kprintf("[NET-TEST] DNS fallo\n");
            if (response) {
                int32_t bytes = network_https_get("https://example.com/",
                                                   response, 4096U, 15000U);
                kprintf("[NET-TEST] HTTPS bytes=%d TLS=%d\n", bytes,
                        network_tls_last_error());
                kfree(response);
            }
        }
#endif
    } else kprintf("[NET] DHCP automatico sin respuesta\n");
    task_exit();
}

void network_start_autoconfigure(void) {
    if (g_device && g_stack)
        (void)task_create("network-dhcp", network_autoconfigure_task, NULL);
}

static net_socket_inspector_t socket_inspector;
void network_register_socket_inspector(net_socket_inspector_t inspect){socket_inspector=inspect;}
uint32_t network_socket_snapshot(bk_socket_info_t *out,uint32_t capacity) {
    return socket_inspector && out && capacity?socket_inspector(out,capacity):0;
}
