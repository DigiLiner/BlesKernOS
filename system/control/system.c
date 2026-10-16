#include "control_common.h"
#include "kernel/include/api.h"

#define SYSTEM_WINDOW_W         628
#define SYSTEM_WINDOW_H         462
#define SYSTEM_TAB_LEFT          16
#define SYSTEM_TAB_TOP            6
#define SYSTEM_TAB_COUNT          6
#define SYSTEM_BUILD_VERSION    "0.9"
#define SYSTEM_LABEL_W          104

#if defined(BLES_ARCH_ARM32_RPI3)
#define SYSTEM_ARCH_NAME        "ARM32 (Raspberry Pi)"
#else
#define SYSTEM_ARCH_NAME        "IA-32 / x86"
#endif

#define SYSTEM_BUILD_STAMP      __DATE__ " " __TIME__

typedef enum {
    SYSTEM_TAB_SUMMARY = 0,
    SYSTEM_TAB_KERNEL = 1,
    SYSTEM_TAB_HARDWARE = 2,
    SYSTEM_TAB_DEVICES = 3,
    SYSTEM_TAB_DRIVERS = 4,
} system_tab_t;

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    uint8_t current_tab;
    bool request_recovery,request_admin,request_events;

    system_memory_info_t memory;
    gfx_info_t gfx;
    mouse_state_t mouse;
    rtc_datetime_t now;
    bool memory_ok;
    bool gfx_ok;
    bool rtc_ok;
    bool sb16_present;
    bool pcm_available;
    const char *pcm_name;
    uint32_t uptime_ms;
    uint32_t processes;
    uint32_t api_version;
    uint32_t cpu_count;
    uint32_t block_count;
    uint32_t pci_count;
    uint32_t last_refresh_ms;

    uint32_t *icon;
    char status[96];
} system_cpl_state_t;

static const cpl_tab_spec_t g_system_tabs[SYSTEM_TAB_COUNT] = {
    {"Resumen", 92},
    {"Kernel", 82},
    {"Hardware", 92},
    {"Dispositivos", 106},
    {"Drivers", 82},
    {"Administrar", 110},
};

static void system_set_status(system_cpl_state_t *st, const char *text) {
    if (!st) return;
    snprintf(st->status, sizeof(st->status), "%s", text ? text : "");
}

static gui_rect_t system_client_rect(const system_cpl_state_t *st) {
    return st && st->window
        ? bk_gui_window_content_rect_raw(st->window)
        : (gui_rect_t){0, 0, 0, 0};
}

static gui_rect_t system_page_rect(const system_cpl_state_t *st) {
    gui_rect_t client = system_client_rect(st);
    return (gui_rect_t){client.x + 8, client.y + 30,
                        client.w - 16, client.h - 54};
}

static gui_rect_t system_status_rect(const system_cpl_state_t *st) {
    gui_rect_t client = system_client_rect(st);
    return (gui_rect_t){client.x + 8, client.y + client.h - 18,
                        client.w - 16, 15};
}

static const char *system_tab_status(uint8_t tab) {
    switch (tab) {
        case SYSTEM_TAB_SUMMARY: return "Resumen general del sistema.";
        case SYSTEM_TAB_KERNEL: return "Informacion del kernel y del entorno de ejecucion.";
        case SYSTEM_TAB_HARDWARE: return "Hardware principal, video, audio y reloj.";
        case SYSTEM_TAB_DEVICES: return "Inventario de dispositivos fisicos detectados.";
        case SYSTEM_TAB_DRIVERS: return "Controladores instalados, version y estado.";
        default: return "Listo.";
    }
}

static const char *system_gfx_mode_name(uint32_t mode) {
    switch (mode) {
        case GFX_MODE_TEXT: return "Texto";
        case GFX_MODE_VGA_13H: return "VGA 320x200x8";
        case GFX_MODE_VGA_12H: return "VGA 640x480x4";
        case GFX_MODE_VESA_LFB: return "VESA LFB";
        case GFX_MODE_VMWARE_SVGA: return "VMware SVGA-II";
        case GFX_MODE_VIRTIO_GPU: return "VirtIO GPU";
        case GFX_MODE_ATI_RAGE128: return "ATI Rage 128";
        case GFX_MODE_INTEL_GMA9XX: return "Intel GMA9xx";
        case GFX_MODE_RPI_FRAMEBUFFER: return "Framebuffer Raspberry Pi";
        default: return "Desconocido";
    }
}

static void system_format_uptime(char *buffer, size_t capacity,
                                 uint32_t uptime_ms) {
    uint32_t total_seconds = uptime_ms / 1000U;
    uint32_t days = total_seconds / 86400U;
    uint32_t hours = (total_seconds / 3600U) % 24U;
    uint32_t minutes = (total_seconds / 60U) % 60U;
    uint32_t seconds = total_seconds % 60U;

    if (!buffer || !capacity) return;
    if (days)
        snprintf(buffer, capacity, "%u d %02u:%02u:%02u",
                 days, hours, minutes, seconds);
    else snprintf(buffer, capacity, "%02u:%02u:%02u",
                  hours, minutes, seconds);
}

static void system_refresh(system_cpl_state_t *st) {
    if (!st) return;
    st->memory_ok = bk_sys_memory_info(&st->memory);
    st->gfx_ok = bk_gfx_info(&st->gfx);
    (void)bk_input_mouse(&st->mouse);
    st->rtc_ok = bk_time_datetime(&st->now);
    st->sb16_present = bk_sound_has_sb16();
    st->pcm_available = bk_sound_pcm_available();
    st->pcm_name = bk_sound_pcm_name();
    st->uptime_ms = bk_sys_uptime_ms();
    st->processes = bk_proc_count();
    st->api_version = bk_sys_api_version();
    st->cpu_count = bk_proc_cpu_count();
    st->block_count = bk_device_block_count();
    st->pci_count = bk_device_pci_count();
    st->last_refresh_ms = st->uptime_ms;
    if (st->window) st->window->dirty = true;
}

static void system_switch_tab(system_cpl_state_t *st, uint8_t tab) {
    if (!st || tab >= SYSTEM_TAB_COUNT || st->current_tab == tab) return;
    st->current_tab = tab;
    system_set_status(st, system_tab_status(tab));
    if (st->window) st->window->dirty = true;
}

static void system_draw_logo(gui_surface_t *s, system_cpl_state_t *st,
                             int x, int y, int w) {
    int icon_x = x + (w - 32) / 2;
    if (icon_x < x) icon_x = x;
    if (st && st->icon) {
        bk_app_draw_icon(s, icon_x, y, st->icon, 32, 32);
    } else {
        cpl_draw_bevel(s, (gui_rect_t){icon_x, y, 32, 32}, CPL_FACE, false);
        bk_gui_font_draw_string_scaled(s, icon_x + 9, y + 8, "B", CPL_TEXT, 2);
    }
}

static void system_draw_main_tabs(gui_surface_t *surface,
                                  gui_rect_t client,
                                  gui_rect_t page,
                                  int active) {
    cpl_draw_tabs(surface, client, g_system_tabs, SYSTEM_TAB_COUNT,
                  active, page, SYSTEM_TAB_LEFT, SYSTEM_TAB_TOP);
}

static gui_rect_t system_draw_header_box(gui_surface_t *s,
                                         system_cpl_state_t *st,
                                         gui_rect_t page,
                                         const char *title,
                                         const char *subtitle) {
    gui_rect_t header = {page.x + 12, page.y + 12, page.w - 24, 48};
    gui_rect_t icon_box = {header.x + 8, header.y + 8, 32, 32};
    gui_rect_t text_clip = {header.x + 52, header.y + 6,
                            header.w - 60, 26};

    cpl_draw_bevel(s, header, CPL_FACE, true);
    cpl_draw_bevel(s, icon_box, CPL_WHITE, true);
    system_draw_logo(s, st, icon_box.x, icon_box.y, icon_box.w);
    bk_gui_font_draw_string_scaled(s, header.x + 52, header.y + 7,
                                   title, CPL_TEXT, 2);
    bk_gui_font_draw_string_clipped(s, header.x + 53, header.y + 23,
                                    subtitle, CPL_TEXT, text_clip);
    return header;
}

static gui_rect_t system_draw_body_panel(gui_surface_t *s,
                                          gui_rect_t page,
                                          gui_rect_t header) {
    gui_rect_t body = {page.x + 14, header.y + header.h + 10,
                       page.w - 28,
                       page.y + page.h - 8 - (header.y + header.h + 10)};

    cpl_draw_bevel(s, body, CPL_WHITE, true);
    return (gui_rect_t){body.x + 10, body.y + 9,
                        body.w - 20, body.h - 18};
}

/*
 * Encabezado de seccion tipo System Profiler: texto y una linea grabada.
 * Las pestañas son la unica navegacion; dentro de cada una se muestra todo.
 */
static int system_draw_section_title(gui_surface_t *s, gui_rect_t inner,
                                     int y, const char *title) {
    int line_x;
    int line_y;
    int text_w;

    if (!title || y + 12 > inner.y + inner.h) return y;
    bk_gui_font_draw_string(s, inner.x + 2, y, title, CPL_TEXT, 0, false);
    text_w = (int)bk_gui_font_text_width(title);
    line_x = inner.x + 10 + text_w;
    line_y = y + 5;
    if (line_x < inner.x + inner.w - 2) {
        bk_gui_gfx_draw_line(s, line_x, line_y,
                             inner.x + inner.w - 2, line_y, CPL_SHADOW);
        bk_gui_gfx_draw_line(s, line_x, line_y + 1,
                             inner.x + inner.w - 2, line_y + 1, CPL_LIGHT);
    }
    return y + 16;
}

static int system_draw_value_row(gui_surface_t *s, gui_rect_t inner,
                                 int y, const char *label,
                                 const char *value) {
    gui_rect_t value_clip;
    int value_x = inner.x + SYSTEM_LABEL_W;

    if (y + 12 > inner.y + inner.h) return y;
    value_clip = (gui_rect_t){value_x, y, inner.x + inner.w - value_x, 13};
    bk_gui_font_draw_string(s, inner.x + 8, y, label, CPL_TEXT, 0, false);
    bk_gui_font_draw_string_clipped(s, value_x, y,
                                    value ? value : "",
                                    CPL_TEXT, value_clip);
    return y + 14;
}

static int system_draw_section_gap(gui_rect_t inner, int y) {
    int limit = inner.y + inner.h;
    y += 3;
    return y < limit ? y : limit;
}

static void system_paint_summary(system_cpl_state_t *st, gui_surface_t *s) {
    gui_rect_t page = system_page_rect(st);
    gui_rect_t header;
    gui_rect_t inner;
    int y;
    char uptime[96], version[32], arch[96];
    char cpus[32], usage[32];
    char total[32], used[32], free_mem[32];

    header = system_draw_header_box(s, st, page, "Perfil del sistema",
                                    "Informacion general del equipo y del sistema.");
    inner = system_draw_body_panel(s, page, header);
    y = inner.y;

    system_format_uptime(uptime, sizeof(uptime), st->uptime_ms);
    snprintf(version, sizeof(version), "%s", SYSTEM_BUILD_VERSION);
    snprintf(arch, sizeof(arch), "%s", SYSTEM_ARCH_NAME);
    snprintf(cpus, sizeof(cpus), "%u", st->cpu_count);
    snprintf(usage, sizeof(usage), "%u%%", bk_proc_cpu_usage());
    snprintf(total, sizeof(total), "%u KB",
             (uint32_t)(st->memory.total_bytes / 1024U));
    snprintf(used, sizeof(used), "%u KB",
             (uint32_t)(st->memory.used_bytes / 1024U));
    snprintf(free_mem, sizeof(free_mem), "%u KB",
             (uint32_t)(st->memory.free_bytes / 1024U));

    y = system_draw_section_title(s, inner, y, "Sistema");
    y = system_draw_value_row(s, inner, y, "Sistema:", "BlesKernOS");
    y = system_draw_value_row(s, inner, y, "Version:", version);
    y = system_draw_value_row(s, inner, y, "Arquitectura:", arch);
    y = system_draw_value_row(s, inner, y, "Uptime:", uptime);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Procesador");
    y = system_draw_value_row(s, inner, y, "CPU online:", cpus);
    y = system_draw_value_row(s, inner, y, "Uso total:", usage);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Memoria");
    y = system_draw_value_row(s, inner, y, "Total:",
                              st->memory_ok ? total : "No disponible");
    y = system_draw_value_row(s, inner, y, "Usada:",
                              st->memory_ok ? used : "No disponible");
    (void)system_draw_value_row(s, inner, y, "Libre:",
                                st->memory_ok ? free_mem : "No disponible");
}

static void system_paint_kernel(system_cpl_state_t *st, gui_surface_t *s) {
    gui_rect_t page = system_page_rect(st);
    gui_rect_t header;
    gui_rect_t inner;
    int y;
    char build[96], ticks[32], pit[32], processes[32], cpu[32];
    char api[32], caps[32];

    header = system_draw_header_box(s, st, page, "Kernel",
                                    "Nucleo, temporizacion, procesos y API publica.");
    inner = system_draw_body_panel(s, page, header);
    y = inner.y;

    snprintf(build, sizeof(build), "%s", SYSTEM_BUILD_STAMP);
    snprintf(ticks, sizeof(ticks), "%u", bk_sys_ticks());
    snprintf(pit, sizeof(pit), "%u Hz", bk_sys_tick_frequency());
    snprintf(processes, sizeof(processes), "%u", st->processes);
    snprintf(cpu, sizeof(cpu), "%u", bk_proc_current_cpu());
    snprintf(api, sizeof(api), "v%u", st->api_version);
    snprintf(caps, sizeof(caps), "0x%08X", bk_sys_capabilities());

    y = system_draw_section_title(s, inner, y, "Identidad del kernel");
    y = system_draw_value_row(s, inner, y, "Nucleo:", "BlesKernOS kernel");
    y = system_draw_value_row(s, inner, y, "Modo:", "32-bit protected mode");
    y = system_draw_value_row(s, inner, y, "Compilado:", build);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Temporizacion");
    y = system_draw_value_row(s, inner, y, "Ticks PIT:", ticks);
    y = system_draw_value_row(s, inner, y, "Frecuencia PIT:", pit);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Procesos");
    y = system_draw_value_row(s, inner, y, "Procesos activos:", processes);
    y = system_draw_value_row(s, inner, y, "CPU del panel:", cpu);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "API publica");
    y = system_draw_value_row(s, inner, y, "Version API:", api);
    (void)system_draw_value_row(s, inner, y, "Capacidades:", caps);
}

static void system_paint_hardware(system_cpl_state_t *st, gui_surface_t *s) {
    gui_rect_t page = system_page_rect(st);
    gui_rect_t header;
    gui_rect_t inner;
    int y;
    char backend[96], resolution[96], pitch[96], framebuffer[96];
    char mouse_state[32], packet[32], position[48], storage[32], rtc[96];

    header = system_draw_header_box(s, st, page, "Hardware",
                                    "Video, audio, entrada, almacenamiento y reloj.");
    inner = system_draw_body_panel(s, page, header);
    y = inner.y;

    snprintf(backend, sizeof(backend), "%s", system_gfx_mode_name(st->gfx.mode));
    snprintf(resolution, sizeof(resolution), "%ux%ux%u",
             st->gfx.width, st->gfx.height, st->gfx.bpp);
    snprintf(pitch, sizeof(pitch), "%u bytes", st->gfx.pitch);
    snprintf(framebuffer, sizeof(framebuffer), "0x%08X", st->gfx.framebuffer);
    snprintf(mouse_state, sizeof(mouse_state), "%s",
             st->mouse.present ? "Presente" : "No detectado");
    snprintf(packet, sizeof(packet), "%u bytes", st->mouse.packet_size);
    snprintf(position, sizeof(position), "(%d, %d)",
             (int)st->mouse.x, (int)st->mouse.y);
    snprintf(storage, sizeof(storage), "%u", st->block_count);
    if (st->rtc_ok) {
        snprintf(rtc, sizeof(rtc), "%04u-%02u-%02u %02u:%02u:%02u",
                 st->now.date.year, st->now.date.month, st->now.date.day,
                 st->now.time.hour, st->now.time.minute, st->now.time.second);
    } else {
        snprintf(rtc, sizeof(rtc), "No disponible");
    }

    y = system_draw_section_title(s, inner, y, "Pantalla");
    y = system_draw_value_row(s, inner, y, "Backend:", backend);
    y = system_draw_value_row(s, inner, y, "Resolucion:", resolution);
    y = system_draw_value_row(s, inner, y, "Pitch:", pitch);
    y = system_draw_value_row(s, inner, y, "Framebuffer:", framebuffer);
    y = system_draw_value_row(s, inner, y, "Estado:",
                              st->gfx_ok ? "Activo" : "No disponible");

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Audio");
    y = system_draw_value_row(s, inner, y, "PCM:",
                              st->pcm_available ? "Disponible" : "No disponible");
    y = system_draw_value_row(s, inner, y, "Dispositivo:",
                              st->pcm_name ? st->pcm_name : "No disponible");
    y = system_draw_value_row(s, inner, y, "SB16:",
                              st->sb16_present ? "Detectado" : "No detectado");

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Entrada");
    y = system_draw_value_row(s, inner, y, "Mouse:", mouse_state);
    y = system_draw_value_row(s, inner, y, "Paquete:", packet);
    y = system_draw_value_row(s, inner, y, "Posicion:", position);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Almacenamiento");
    y = system_draw_value_row(s, inner, y, "Unidades detectadas:", storage);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Reloj");
    (void)system_draw_value_row(s, inner, y, "RTC:", rtc);
}

static uint32_t system_status_color(int status) {
    if (status == 1) return 0x0000A040;
    if (status == 2) return 0x00C08000;
    if (status == 3) return 0x00C02020;
    return 0x00909090;
}

static int system_draw_device_line(gui_surface_t *s, gui_rect_t inner,
                                   int y, const char *name,
                                   const char *detail, int status) {
    gui_rect_t line = {inner.x + 6, y, inner.w - 12, 14};

    if (line.y + line.h > inner.y + inner.h) return y;
    bk_gui_gfx_fill_rect(s, (gui_rect_t){line.x + 2, line.y + 4, 5, 5},
                         system_status_color(status));
    bk_gui_font_draw_string_clipped(s, line.x + 12, line.y + 2,
                                    name, CPL_TEXT,
                                    (gui_rect_t){line.x + 12, line.y,
                                                 118, line.h});
    bk_gui_font_draw_string_clipped(s, line.x + 128, line.y + 2,
                                    detail ? detail : "", CPL_TEXT,
                                    (gui_rect_t){line.x + 128, line.y,
                                                 line.w - 128, line.h});
    return y + 14;
}

static void system_paint_devices(system_cpl_state_t *st, gui_surface_t *s) {
    gui_rect_t page = system_page_rect(st);
    gui_rect_t header;
    gui_rect_t inner;
    int y;
    char a[128], b[128];
    uint32_t shown;

    header = system_draw_header_box(s, st, page, "Dispositivos",
                                    "Inventario fisico detectado por el sistema.");
    inner = system_draw_body_panel(s, page, header);
    y = inner.y;

    y = system_draw_section_title(s, inner, y, "Almacenamiento");
    shown = 0;
    for (uint32_t i = 0; i < st->block_count && shown < 4U; i++) {
        bk_block_info_t dev;
        if (!bk_device_block_info(i, &dev)) continue;
        snprintf(a, sizeof(a), "%s, %u sectores%s",
                 dev.type_name, dev.sector_count,
                 dev.read_only ? ", solo lectura" : "");
        y = system_draw_device_line(s, inner, y, dev.name, a, 1);
        shown++;
    }
    if (!shown)
        y = system_draw_device_line(s, inner, y,
                                    "Almacenamiento", "No detectado", 2);
    else if (st->block_count > shown) {
        snprintf(a, sizeof(a), "%u unidad(es) mas", st->block_count - shown);
        y = system_draw_device_line(s, inner, y, "...", a, 0);
    }

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "Entrada");
    snprintf(a, sizeof(a), "id=%u packet=%u irq=%u",
             (uint32_t)st->mouse.device_id,
             (uint32_t)st->mouse.packet_size,
             st->mouse.irq_count);
    y = system_draw_device_line(s, inner, y, "Mouse", a,
                                st->mouse.present ? 1 : 3);
    y = system_draw_device_line(s, inner, y, "Teclado",
                                "Controlador PS/2 / GUI", 1);

    y = system_draw_section_gap(inner, y);
    y = system_draw_section_title(s, inner, y, "PCI");
    shown = 0;
    for (uint32_t i = 0; i < st->pci_count && shown < 7U; i++) {
        bk_pci_info_t dev;
        if (!bk_device_pci_info(i, &dev)) continue;
        snprintf(a, sizeof(a), "%04X:%04X bus=%u slot=%u irq=%u",
                 dev.vendor_id, dev.device_id, dev.bus, dev.slot,
                 dev.interrupt_line);
        snprintf(b, sizeof(b), "%s", dev.class_name);
        y = system_draw_device_line(s, inner, y, b, a, 1);
        shown++;
    }
    if (!shown)
        y = system_draw_device_line(s, inner, y, "PCI", "Sin dispositivos", 2);
    else if (st->pci_count > shown) {
        snprintf(a, sizeof(a), "%u dispositivo(s) mas", st->pci_count - shown);
        y = system_draw_device_line(s, inner, y, "...", a, 0);
    }
    (void)y;
}

static const char *system_driver_filename(const char *path) {
    const char *name = path ? path : "";
    if (!path) return name;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name;
}

static void system_paint_drivers(system_cpl_state_t *st, gui_surface_t *s) {
    gui_rect_t page = system_page_rect(st);
    gui_rect_t header;
    gui_rect_t inner;
    uint32_t total = bk_device_driver_count();
    uint32_t active = 0;
    char subtitle[96];
    char version[16];
    int row_y;
    const int row_h = 16;
    const int name_x = 8;
    const int version_x = 166;
    const int state_x = 232;
    const int file_x = 302;

    for (uint32_t i = 0; i < total; i++) {
        bk_driver_info_t info;
        if (bk_device_driver_info(i, &info) && info.active) active++;
    }
    snprintf(subtitle, sizeof(subtitle),
             "%u controladores instalados; %u activos.", total, active);
    header = system_draw_header_box(s, st, page, "Drivers", subtitle);
    inner = system_draw_body_panel(s, page, header);
    row_y = system_draw_section_title(s, inner, inner.y,
                                      "Controladores del sistema");

    if (row_y + 18 <= inner.y + inner.h) {
        bk_gui_gfx_fill_rect(s,
            (gui_rect_t){inner.x, row_y, inner.w, 18}, CPL_FACE);
        cpl_draw_bevel(s,
            (gui_rect_t){inner.x, row_y, inner.w, 18}, CPL_FACE, false);
        bk_gui_font_draw_string(s, inner.x + name_x, row_y + 5,
                                "Nombre", CPL_TEXT, 0, false);
        bk_gui_font_draw_string(s, inner.x + version_x, row_y + 5,
                                "Version", CPL_TEXT, 0, false);
        bk_gui_font_draw_string(s, inner.x + state_x, row_y + 5,
                                "Estado", CPL_TEXT, 0, false);
        bk_gui_font_draw_string(s, inner.x + file_x, row_y + 5,
                                "Archivo", CPL_TEXT, 0, false);
    }

    row_y += 22;
    for (uint32_t i = 0; i < total; i++) {
        bk_driver_info_t info;
        gui_rect_t row;
        uint32_t marker;
        const char *state;
        const char *filename;

        if (!bk_device_driver_info(i, &info)) continue;
        if (row_y + row_h > inner.y + inner.h) break;
        row = (gui_rect_t){inner.x, row_y, inner.w, row_h};
        if (i & 1U)
            bk_gui_gfx_fill_rect(s, row, 0x00F4F4F4);
        marker = info.active ? 0x0000A040 : CPL_SHADOW;
        state = info.active ? "Activo" : "Inactivo";
        filename = system_driver_filename(info.path);
        snprintf(version, sizeof(version), "%u.%u",
                 (uint32_t)info.version_major,
                 (uint32_t)info.version_minor);

        bk_gui_gfx_fill_rect(s,
            (gui_rect_t){row.x + 2, row.y + 5, 6, 6}, marker);
        bk_gui_gfx_draw_rect(s,
            (gui_rect_t){row.x + 2, row.y + 5, 6, 6}, CPL_DARK);
        bk_gui_font_draw_string_clipped(s, row.x + name_x + 6, row.y + 3,
                                        info.name, CPL_TEXT,
                                        (gui_rect_t){row.x + name_x + 6, row.y,
                                                     version_x - name_x - 10,
                                                     row.h});
        bk_gui_font_draw_string_clipped(s, row.x + version_x, row.y + 3,
                                        version, CPL_TEXT,
                                        (gui_rect_t){row.x + version_x, row.y,
                                                     60, row.h});
        bk_gui_font_draw_string_clipped(s, row.x + state_x, row.y + 3,
                                        state, CPL_TEXT,
                                        (gui_rect_t){row.x + state_x, row.y,
                                                     64, row.h});
        bk_gui_font_draw_string_clipped(s, row.x + file_x, row.y + 3,
                                        filename, CPL_TEXT,
                                        (gui_rect_t){row.x + file_x, row.y,
                                                     row.w - file_x, row.h});
        row_y += row_h;
    }
}

static void system_paint(gui_window_t *window UNUSED, gui_surface_t *s,
                         void *context) {
    system_cpl_state_t *st = (system_cpl_state_t *)context;
    gui_rect_t client;
    gui_rect_t page;
    gui_rect_t status;

    if (!st || !st->window || !s) return;
    client = system_client_rect(st);
    page = system_page_rect(st);
    status = system_status_rect(st);

    cpl_draw_bevel(s, client, CPL_FACE, false);
    system_draw_main_tabs(s, client, page, st->current_tab);
    switch (st->current_tab) {
        case SYSTEM_TAB_SUMMARY:
            system_paint_summary(st, s);
            break;
        case SYSTEM_TAB_KERNEL:
            system_paint_kernel(st, s);
            break;
        case SYSTEM_TAB_HARDWARE:
            system_paint_hardware(st, s);
            break;
        case SYSTEM_TAB_DEVICES:
            system_paint_devices(st, s);
            break;
        case SYSTEM_TAB_DRIVERS:
            system_paint_drivers(st, s);
            break;
        case 5: {
            gui_rect_t p=system_page_rect(st);bk_memory_detail_t m;bk_memory_detail(&m);char line[160];
            snprintf(line,sizeof(line),"Kernel %s | API %u | heap %u / %u KiB",SYSTEM_BUILD_VERSION,st->api_version,m.heap_used/1024,m.heap_total/1024);
            cpl_draw_text_clipped(s,(gui_rect_t){p.x+16,p.y+24,p.w-32,24},line,CPL_TEXT);
            snprintf(line,sizeof(line),"Frames VM libres %u KiB | reservada %u KiB",m.vm_free/1024,m.reserved/1024);
            cpl_draw_text_clipped(s,(gui_rect_t){p.x+16,p.y+54,p.w-32,24},line,CPL_TEXT);
            const char *labels[]={"Servicios y programas de inicio","Visor de eventos","Recuperacion en el proximo arranque"};
            for(int i=0;i<3;i++){gui_rect_t r={p.x+16,p.y+104+i*46,p.w-32,32};cpl_draw_bevel(s,r,CPL_FACE,false);cpl_draw_text_clipped(s,cpl_inset(r,8),labels[i],CPL_TEXT);}
            cpl_draw_text_wrapped(s,(gui_rect_t){p.x+16,p.y+262,p.w-32,65},"Los ajustes de inicio se aplican al reiniciar. Recuperacion abre una consola antes de drivers opcionales y del escritorio.",CPL_SHADOW,16,4);
            break;
        }
        default: break;
    }

    cpl_draw_bevel(s, status, CPL_FACE, true);
    bk_gui_font_draw_string_clipped(s, status.x + 4, status.y + 3,
                                    st->status[0] ? st->status : "Listo.",
                                    CPL_SHADOW, status);
}

static bool system_event(gui_window_t *window UNUSED,
                         const gui_event_t *event,
                         void *context) {
    system_cpl_state_t *st = (system_cpl_state_t *)context;
    int tab_hit;

    if (!st || !event) return false;
    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        if(st->current_tab==5){gui_rect_t p=system_page_rect(st);for(int i=0;i<3;i++){gui_rect_t r={p.x+16,p.y+104+i*46,p.w-32,32};if(bk_gui_rect_contains(r,event->x,event->y)){if(i==0)st->request_admin=true;else if(i==1)st->request_events=true;else st->request_recovery=true;return true;}}}
        tab_hit = cpl_hit_tab(system_client_rect(st), g_system_tabs,
                              SYSTEM_TAB_COUNT, SYSTEM_TAB_LEFT,
                              SYSTEM_TAB_TOP, event->x, event->y);
        if (tab_hit >= 0) {
            system_switch_tab(st, (uint8_t)tab_hit);
            return true;
        }
    }
    if (event->type == GUI_EVENT_KEY) {
        if (event->key == '	') {
            system_switch_tab(st,
                (uint8_t)((st->current_tab + 1U) % SYSTEM_TAB_COUNT));
            return true;
        }
    }
    return false;
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    system_cpl_state_t *st;
    uint32_t next_refresh;

    if (!desktop) return;
    st = (system_cpl_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) return;
    st->desktop = desktop;
    st->current_tab = SYSTEM_TAB_SUMMARY;
    system_set_status(st, system_tab_status(st->current_tab));

    st->window = bk_gui_create_window(desktop, 72, 42,
                                      SYSTEM_WINDOW_W, SYSTEM_WINDOW_H,
                                      "Sistema");
    if (!st->window) {
        bk_sys_free(st);
        return;
    }


    bk_gui_set_window_min_size(st->window, SYSTEM_WINDOW_W, SYSTEM_WINDOW_H);
    st->window->bg_color = CPL_FACE;
    bk_gui_set_window_content(st->window, system_paint, st);
    bk_gui_set_window_event_handler(st->window, system_event, st);
    st->window->owner_pid = bk_sys_getpid();
    bk_proc_bind_window(st->window);

    st->icon = bk_app_load_icon("/ICONS/SYSTEM.BMP", 32, 32);

    system_refresh(st);
    next_refresh = bk_sys_uptime_ms() + 1000U;
    bk_gui_request_paint();

    while (!bk_proc_exit_requested() && st->window->listed) {
        if(st->request_admin){st->request_admin=false;bk_app_launch("/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX", "--tab inicio");}
        if(st->request_events){st->request_events=false;bk_app_launch("/SYSTEM/PROGRAMS/EVENT_VIEWER.BEX",NULL);}
        if(st->request_recovery){st->request_recovery=false;bool ok=bk_file_write_all("/SYSTEM/USER/CONFIG/RECOVERY.NEXT","1\n",2);system_set_status(st,ok?"Recuperacion preparada. Reinicie para entrar a la consola.":"No se pudo preparar recuperacion.");st->window->dirty=true;}
        uint32_t now = bk_sys_uptime_ms();
        if ((int32_t)(now - next_refresh) >= 0) {
            system_refresh(st);
            next_refresh = now + 1000U;
            if (st->window) {
                st->window->dirty = true;
                bk_gui_request_paint();
            }
        }
        bk_sys_sleep_ticks(4);
    }

    cpl_destroy_window(st->desktop, st->window);
    if (st->icon) bk_sys_free(st->icon);
    bk_sys_free(st);
    bk_proc_exit();
}
