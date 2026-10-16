#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    bk_net_info_t info;
    bool ok = bk_net_get_info(&info);
    kprintf("[DRIVERTEST] device=%s link=%u ready=%u\n", ok ? info.device : "none",
            ok ? info.link_up : 0, ok ? info.stack_ready : 0);
    bool configured = ok && bk_net_dhcp(12000);
    uint8_t gateway[4] = {10, 0, 2, 2};
    uint32_t rtt = 0;
    bool ping = configured && bk_net_ping(gateway, 5000, &rtt);
    bool result =
        bk_net_get_info(&info) && configured && ping && info.rx_packets > 0 && info.tx_packets > 0;
    kprintf("[DRIVERTEST] DHCP=%u ping=%u rtt=%u rx=%u tx=%u\n", configured, ping, rtt,
            info.rx_packets, info.tx_packets);
    kprintf("[DRIVERTEST] DONE result=%s\n", result ? "PASS" : "FAIL");
}
