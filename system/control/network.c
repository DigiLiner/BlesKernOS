#include "control_common.h"
typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_widget_t *fields[4], *mode, *profile_button;
    uint32_t profile;
    bool load;
    bool dhcp, apply, diagnose;
    char status[160];
    net_info_t info;
} profile_t;
static profile_t *active;
static void mode(gui_window_t *w UNUSED, uint32_t id UNUSED) {
    if (!active)
        return;
    active->dhcp = !active->dhcp;
    bk_gui_widget_set_text(active->mode, active->dhcp ? "DHCP automatico" : "IP fija");
}
static void profile(gui_window_t *w UNUSED, uint32_t id UNUSED) {
    if (active) {
        active->profile = (active->profile + 1) % 3;
        active->load = true;
        char label[24];
        snprintf(label, sizeof(label), "Perfil %u", active->profile + 1);
        bk_gui_widget_set_text(active->profile_button, label);
    }
}
static void apply(gui_window_t *w UNUSED, uint32_t id UNUSED) {
    if (active)
        active->apply = true;
}
static void diagnose(gui_window_t *w UNUSED, uint32_t id UNUSED) {
    if (active)
        active->diagnose = true;
}
static void paint(gui_window_t *w, gui_surface_t *s, void *context) {
    profile_t *p = context;
    gui_rect_t c = bk_gui_window_content_rect_raw(w);
    const char *names[] = {"Direccion IP", "Mascara", "Gateway", "DNS"};
    for (int i = 0; i < 4; i++)
        cpl_draw_text_clipped(s, (gui_rect_t){c.x + 16, c.y + 62 + i * 38, 140, 18}, names[i],
                              CPL_TEXT);
    char line[160];
    snprintf(line, sizeof(line), "Adaptador: %s | enlace %s | RX %u / TX %u", p->info.device,
             p->info.link_up ? "activo" : "caido", p->info.rx_packets, p->info.tx_packets);
    cpl_draw_text_clipped(s, (gui_rect_t){c.x + 16, c.y + 222, c.w - 32, 24}, line, CPL_TEXT);
    cpl_draw_text_wrapped(
        s, (gui_rect_t){c.x + 16, c.y + 280, c.w - 32, 58},
        "Aplicar cambia la red inmediatamente y guarda el perfil para futuros arranques.",
        CPL_SHADOW, 16, 3);
    cpl_draw_text_wrapped(s, (gui_rect_t){c.x + 16, c.y + 342, c.w - 32, 60}, p->status, CPL_TEXT,
                          16, 3);
}
static void iptext(char *s, const uint8_t *ip) {
    snprintf(s, 32, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    profile_t *p = bk_sys_alloc_zero(sizeof(*p));
    if (!p)
        return;
    active = p;
    p->desktop = desktop;
    p->dhcp = true;
    p->load = true;
    bk_net_get_info(&p->info);
    p->window = bk_gui_create_window(desktop, 90, 40, 525, 435, "Configuracion de red");
    if (!p->window) {
        active = NULL;
        bk_sys_free(p);
        return;
    }
    bk_proc_bind_window(p->window);
    bk_gui_set_window_min_size(p->window, 525, 435);
    bk_gui_window_set_compact_content(p->window,true);
    bk_gui_set_window_content(p->window, paint, p);
    p->profile_button = bk_gui_create_button(desktop, p->window, (gui_rect_t){226, 14, 160, 25},
                                             "Perfil 1", profile);
    p->mode = bk_gui_create_button(desktop, p->window, (gui_rect_t){16, 14, 190, 25},
                                   "DHCP automatico", mode);
    const uint8_t *ips[] = {p->info.address, p->info.netmask, p->info.gateway, p->info.dns};
    for (int i = 0; i < 4; i++) {
        char ip[32];
        iptext(ip, ips[i]);
        p->fields[i] = bk_gui_create_textbox(desktop, p->window,
                                             (gui_rect_t){164, 55 + i * 38, 325, 26}, ip, 31, NULL);
    }
    bk_gui_create_button(desktop, p->window, (gui_rect_t){16, 246, 140, 25}, "Aplicar y guardar",
                         apply);
    bk_gui_create_button(desktop, p->window, (gui_rect_t){170, 246, 150, 25}, "Diagnosticar DNS",
                         diagnose);
    while (!bk_proc_exit_requested() && p->window->listed) {
        if (p->load) {
            p->load = false;
            bk_network_profile_t saved;
            if (bk_network_profile_get(p->profile, &saved)) {
                p->dhcp = saved.dhcp;
                bk_gui_widget_set_text(p->mode, p->dhcp ? "DHCP automatico" : "IP fija");
                const char *values[] = {saved.ip, saved.mask, saved.gateway, saved.dns};
                for (int i = 0; i < 4; i++)
                    bk_gui_widget_set_text(p->fields[i], values[i]);
                bk_runtime_strcpy(p->status, "Perfil cargado. Aplicar lo activa.");
            } else
                bk_runtime_strcpy(p->status, "Perfil sin guardar; introduzca sus valores.");
            bk_gui_window_invalidate(p->window);
        }
        if (p->apply) {
            p->apply = false;
            char ip[4][32];
            for (int i = 0; i < 4; i++)
                bk_gui_widget_get_text(p->fields[i], ip[i], 32);
            bool ok = bk_network_profile_apply(p->profile, p->dhcp, ip[0], ip[1], ip[2], ip[3]);
            bk_runtime_strcpy(
                p->status,
                ok ? "Perfil aplicado y guardado."
                   : "No se pudo aplicar/guardar. Revise IP, mascara y disponibilidad de red.");
            bk_net_get_info(&p->info);
            bk_gui_window_invalidate(p->window);
        }
        if (p->diagnose) {
            p->diagnose = false;
            uint8_t ip[4];
            bool ok = bk_net_resolve("example.com", ip, 4000);
            bk_runtime_strcpy(p->status,
                              ok ? "DNS responde; example.com resuelto."
                                 : "DNS no respondio. Revise enlace, gateway y servidor DNS.");
            bk_gui_window_invalidate(p->window);
        }
        bk_sys_sleep_ms(20);
    }
    active = NULL;
    bk_gui_desktop_remove_window(desktop, p->window);
    bk_gui_window_destroy_raw(p->window);
    bk_sys_free(p);
}
