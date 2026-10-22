#ifndef BLESKERNOS_DISPLAY_PROPS_SHARED_H
#define BLESKERNOS_DISPLAY_PROPS_SHARED_H

#include "control_common.h"
#include "system/screensavers/jelly_settings.h"
#include "kernel/include/api.h"
#include <math.h>
#include "system/screensavers/ss_mesa.h"
#define SS_BALLS_EMBEDDED_PREVIEW 1
#include "system/screensavers/ss_balls.c"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DISPLAY_PROPS_MODE_MAX      128
#define DISPLAY_PROPS_COLOR_COUNT    12
#define DISPLAY_PROPS_TIMEOUT_COUNT   3
#define DISPLAY_PROPS_TAB_COUNT       4
#define DISPLAY_PROPS_MAX_WIDGETS    40
#define DISPLAY_PROPS_MAX_SAVERS     16
#define DISPLAY_PREVIEW_MONITOR_W   192
#define DISPLAY_PREVIEW_MONITOR_H   192
#define DISPLAY_PREVIEW_ASSET_W     256
#define DISPLAY_PREVIEW_ASSET_H     256
/* Rectangulo magenta exacto de MONITOR.BMP.  No se debe agrandar: el
 * contenido de la vista previa (en especial la barra de tareas) tiene que
 * quedar enteramente dentro del vidrio, sin pintar sobre el marco. */
#define DISPLAY_PREVIEW_SCREEN_X     54
#define DISPLAY_PREVIEW_SCREEN_Y     52
#define DISPLAY_PREVIEW_SCREEN_W    146
#define DISPLAY_PREVIEW_SCREEN_H    113
#define DISPLAY_PREVIEW_PIPE_MAX     48
#define DISPLAY_PREVIEW_PIPE_CELL  1.15f
#define DISPLAY_PREVIEW_PIPE_RADIUS 0.18f
#define DISPLAY_PREVIEW_PIPE_SIDES    12
#define DISPLAY_PREVIEW_FRAME_MS      SS_FRAME_MS
#define DISPLAY_PREVIEW_MONITOR_PATH   "/ICONS/MONITOR.BMP"
#define DISPLAY_SAVER_DIR             "/SYSTEM/SCREENS"
#define DISPLAY_WALLPAPER_DIR         "/SYSTEM/WALLPAPR"

#ifndef DISPLAY_PROPS_WINDOW_TITLE
#define DISPLAY_PROPS_WINDOW_TITLE "Propiedades de pantalla"
#endif

#ifndef DISPLAY_PROPS_START_TAB
#define DISPLAY_PROPS_START_TAB 0
#endif

typedef enum {
    DISPLAY_TAB_BACKGROUND = 0,
    DISPLAY_TAB_SAVER      = 1,
    DISPLAY_TAB_SCREEN     = 2,
    DISPLAY_TAB_PERFORMANCE = 3,
} display_props_tab_t;

typedef struct {
    int x;
    int y;
    int z;
    uint8_t dir;
    uint8_t color;
} display_preview_pipe_segment_t;

typedef struct {
    gui_widget_t *widget;
    uint8_t tab;
} display_props_widget_ref_t;

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gfx_display_mode_t modes[DISPLAY_PROPS_MODE_MAX];
    uint32_t count;
    bool live;
    uint8_t current_tab;
    int selected_mode;
    uint32_t preview_color;
    uint8_t wallpaper_mode;
    char status[112];
    char wallpaper_path[VFS_MAX_PATH];

    gui_widget_t *color_buttons[DISPLAY_PROPS_COLOR_COUNT];
    gui_widget_t *wallpaper_dropdown;
    gui_widget_t *wallpaper_mode_dropdown;

    gui_widget_t *saver_dropdown;
    gui_widget_t *saver_config_button;

    gui_widget_t *mode_dropdown;
    gui_widget_t *depth_dropdown;
    gui_widget_t *drag_outline_buttons[2];
    bool drag_outline;

    display_props_widget_ref_t widgets[DISPLAY_PROPS_MAX_WIDGETS];
    uint8_t widget_count;

    uint32_t *monitor_pixels;
    uint32_t *wallpaper_pixels;
    uint16_t wallpaper_preview_w, wallpaper_preview_h;

    uint32_t logo_seed;
    int logo_x;
    int logo_y;
    int logo_dx;
    int logo_dy;
    uint32_t last_logo_ms;

    uint32_t pipe_seed;
    int pipe_cursor_x;
    int pipe_cursor_y;
    int pipe_cursor_z;
    uint8_t pipe_last_dir;
    uint8_t pipe_color_index;
    uint8_t pipe_segment_count;
    uint32_t last_pipe_grow_ms;
    uint32_t last_pipe_frame_ms;
    uint32_t last_pipe_motion_ms;
    display_preview_pipe_segment_t pipe_segments[DISPLAY_PREVIEW_PIPE_MAX];

    ss_mesa_surface_t pipe_mesa;
    uint32_t *pipe_pixels;
    uint16_t pipe_render_w;
    uint16_t pipe_render_h;
    bool pipe_gl_ready;
    GLfloat pipe_rot_x;
    GLfloat pipe_rot_y;
    GLfloat pipe_rot_z;
} display_props_state_t;

static const uint32_t g_display_props_colors[DISPLAY_PROPS_COLOR_COUNT] = {
    0x00008080, 0x00000080, 0x00404040, 0x00800080,
    0x00808000, 0x00407FBF, 0x00005E9A, 0x00A27A42,
    0x00A04F7A, 0x00376A2C, 0x00607088, 0x00C8B98C
};

static const char *g_display_props_color_names[DISPLAY_PROPS_COLOR_COUNT] = {
    "Teal", "Azul", "Gris", "Violeta",
    "Oliva", "Cielo", "Mar", "Arena",
    "Rosa", "Bosque", "Acero", "Crema"
};

static const uint32_t g_display_preview_pipe_colors[] = {
    0x0019A6FF, 0x0028E060, 0x00FF5038,
    0x00FFCE4A, 0x00D86CFF, 0x00F6F6F6
};

static const int8_t g_display_preview_pipe_dirs[6][3] = {
    { 1,  0,  0},
    {-1,  0,  0},
    { 0,  1,  0},
    { 0, -1,  0},
    { 0,  0,  1},
    { 0,  0, -1},
};

static display_props_state_t *display_props_state(gui_window_t *window) {
    return window ? (display_props_state_t *)window->content_context : NULL;
}

static const char *display_props_tab_name(uint8_t tab) {
    switch (tab) {
        case DISPLAY_TAB_BACKGROUND: return "Fondo";
        case DISPLAY_TAB_SAVER: return "Protector";
        case DISPLAY_TAB_SCREEN: return "Pantalla";
        default: return "Rendimiento";
    }
}

static char display_props_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool display_props_text_contains_ci(const char *text,
                                           const char *needle) {
    if (!text || !needle || !needle[0]) return false;
    while (*text) {
        const char *a = text;
        const char *b = needle;
        while (*a && *b && display_props_upper(*a) == display_props_upper(*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
        text++;
    }
    return false;
}

static bool display_props_text_ends_with_ci(const char *text,
                                            const char *suffix) {
    size_t text_len;
    size_t suffix_len;

    if (!text || !suffix) return false;
    text_len = bk_runtime_strlen(text);
    suffix_len = bk_runtime_strlen(suffix);
    if (text_len < suffix_len) return false;
    text += text_len - suffix_len;
    while (*text && *suffix) {
        if (display_props_upper(*text++) != display_props_upper(*suffix++))
            return false;
    }
    return true;
}

static const char *display_props_saver_path_local(void) {
    /* screensaver_get_path() returns kernel-owned storage.  That pointer is
       intentionally supervisor-only now, so Ring 3 must ask the kernel to
       copy it into memory owned by this CPL process. */
    static char path[VFS_MAX_PATH];
    if (!bk_screensaver_copy_path(path, sizeof(path))) path[0] = '\0';
    path[sizeof(path) - 1U] = '\0';
    return path;
}

static const char *display_props_saver_name_from_path(const char *path) {
    if (display_props_text_contains_ci(path, "BALLS")) return "Balls 3D";
    if (display_props_text_contains_ci(path, "PIPES")) return "Pipes 3D";
    if (display_props_text_contains_ci(path, "LOGO")) return "Logo";
    return "Protector";
}

static bool display_props_saver_is_balls(void) {
    return display_props_text_contains_ci(display_props_saver_path_local(), "BALLS");
}

static bool display_props_saver_is_pipes(void) {
    return display_props_text_contains_ci(display_props_saver_path_local(), "PIPES");
}

static bool display_props_saver_is_jelly(void) {
    return display_props_text_contains_ci(display_props_saver_path_local(), "SSJELLY");
}

static void display_props_saver_label_from_name(const char *name,
                                                char *label,
                                                size_t label_size) {
    const char *base = name;
    size_t i = 0;

    if (!label || !label_size) return;
    label[0] = '\0';
    if (!name) return;
    while (*base) {
        if (*base == '/' || *base == '\\') name = base + 1;
        base++;
    }
    if (display_props_text_contains_ci(name, "SSJELLY")) {
        bk_runtime_strncpy(label, "Medusa (Gallium)", label_size - 1);
        label[label_size - 1] = '\0';
        return;
    }
    if (display_props_text_contains_ci(name, "SSPIPES")) {
        bk_runtime_strncpy(label, "Pipes 3D", label_size - 1);
        label[label_size - 1] = '\0';
        return;
    }
    if (display_props_text_contains_ci(name, "SSLOGO")) {
        bk_runtime_strncpy(label, "Logo", label_size - 1);
        label[label_size - 1] = '\0';
        return;
    }
    while (name[i] && name[i] != '.' && i + 1 < label_size) {
        char c = name[i];
        label[i] = c == '_' ? ' ' : c;
        i++;
    }
    label[i] = '\0';
}

static bool display_props_wallpaper_is_supported(const char *name) {
    return display_props_text_ends_with_ci(name, ".BMP") ||
           display_props_text_ends_with_ci(name, ".PNG") ||
           display_props_text_ends_with_ci(name, ".JPG") ||
           display_props_text_ends_with_ci(name, ".JPE") ||
           display_props_text_ends_with_ci(name, ".JPN") ||
           display_props_text_ends_with_ci(name, ".JPNG");
}

static void display_props_wallpaper_label_from_name(const char *name,
                                                    char *label,
                                                    size_t label_size) {
    const char *base = name;
    size_t i = 0;

    if (!label || !label_size) return;
    label[0] = '\0';
    if (!name || !name[0]) {
        bk_runtime_strncpy(label, "Sin wallpaper", label_size - 1);
        label[label_size - 1] = '\0';
        return;
    }

    while (*base) {
        if (*base == '/' || *base == '\\') name = base + 1;
        base++;
    }
    while (name[i] && name[i] != '.' && i + 1 < label_size) {
        label[i] = name[i] == '_' ? ' ' : name[i];
        i++;
    }
    label[i] = '\0';
}

static void display_props_force_redraw(display_props_state_t *st) {
    gui_desktop_t *desktop;

    if (!st || !st->window) return;
    st->window->dirty = true;
    desktop = bk_gui_get_desktop();
    if (desktop)
        bk_gui_desktop_invalidate_rect(desktop, st->window->bounds);
    bk_gui_request_paint();
}

static void display_props_force_full_refresh(gui_desktop_t *desktop) {
    if (!desktop) return;
    bk_gui_desktop_invalidate_all(desktop);
    bk_gui_request_paint();
}

static gui_rect_t display_props_page_rect(const display_props_state_t *st) {
    gui_rect_t client = bk_gui_window_content_rect_raw(st->window);
    return (gui_rect_t){client.x + 8, client.y + 28, client.w - 16, client.h - 46};
}

static gui_rect_t display_props_monitor_rect(const display_props_state_t *st) {
    gui_rect_t page = display_props_page_rect(st);
    return (gui_rect_t){
        page.x + (page.w - DISPLAY_PREVIEW_MONITOR_W) / 2,
        page.y + 12,
        DISPLAY_PREVIEW_MONITOR_W,
        DISPLAY_PREVIEW_MONITOR_H
    };
}

static gui_rect_t display_props_monitor_screen_rect(const display_props_state_t *st) {
    gui_rect_t monitor = display_props_monitor_rect(st);
    int left = (DISPLAY_PREVIEW_SCREEN_X * monitor.w) / DISPLAY_PREVIEW_ASSET_W;
    int top = (DISPLAY_PREVIEW_SCREEN_Y * monitor.h) / DISPLAY_PREVIEW_ASSET_H;
    int right = ((DISPLAY_PREVIEW_SCREEN_X + DISPLAY_PREVIEW_SCREEN_W) * monitor.w +
                 DISPLAY_PREVIEW_ASSET_W - 1) / DISPLAY_PREVIEW_ASSET_W;
    int bottom = ((DISPLAY_PREVIEW_SCREEN_Y + DISPLAY_PREVIEW_SCREEN_H) * monitor.h +
                  DISPLAY_PREVIEW_ASSET_H - 1) / DISPLAY_PREVIEW_ASSET_H;

    /* El hueco magenta de MONITOR.BMP es 146x113 dentro de un asset 256x256.
       Escalando a 192x192 los extremos caen entre pixeles; truncar ancho/alto
       dejaba un borde magenta visible. Escalar ambos extremos y redondear el
       extremo lejano hacia arriba cubre el vidrio completo sin comer bisel. */
    return (gui_rect_t){monitor.x + left, monitor.y + top,
                        right - left, bottom - top};
}

static gui_rect_t display_props_tab_rect(const display_props_state_t *st, int index) {
    static const int widths[DISPLAY_PROPS_TAB_COUNT] = {68, 82, 74, 104};
    gui_rect_t client = bk_gui_window_content_rect_raw(st->window);
    int x = client.x + 16;
    int y = client.y + 6;

    for (int i = 0; i < index; i++) x += widths[i] - 1;
    return (gui_rect_t){x, y, widths[index], 22};
}

static const char *display_props_default_status(const display_props_state_t *st) {
    if (!st) return "";
    switch (st->current_tab) {
        case DISPLAY_TAB_BACKGROUND:
            return "Color y fondo se aplican al instante.";
        case DISPLAY_TAB_SAVER:
            return "Abra Configurar para ajustar el protector.";
        case DISPLAY_TAB_SCREEN:
            return st->live
                ? "Ajuste resolucion y colores."
                : "Solo se expone el modo actual.";
        default:
            return "Los efectos opcionales se aplican al instante.";
    }
}

static void display_props_set_status(display_props_state_t *st, const char *text) {
    if (!st) return;
    bk_runtime_strncpy(st->status, text ? text : "", sizeof(st->status) - 1);
    st->status[sizeof(st->status) - 1] = '\0';
    display_props_force_redraw(st);
}

static void display_props_attach_widget(display_props_state_t *st,
                                        gui_widget_t *widget,
                                        uint8_t tab) {
    if (!st || !widget || st->widget_count >= DISPLAY_PROPS_MAX_WIDGETS) return;
    st->widgets[st->widget_count].widget = widget;
    st->widgets[st->widget_count].tab = tab;
    st->widget_count++;
}

static gui_widget_t *display_props_add_widget(display_props_state_t *st,
                                              gui_widget_t *widget,
                                              uint8_t tab) {
    display_props_attach_widget(st, widget, tab);
    return widget;
}

static gui_widget_t *display_props_button(display_props_state_t *st,
                                          uint8_t tab, gui_rect_t rect,
                                          const char *label,
                                          gui_widget_callback_t cb) {
    return display_props_add_widget(st,
        bk_gui_widget_create_button(st->desktop, st->window, rect, label, cb), tab);
}

static gui_widget_t *display_props_select_button(display_props_state_t *st,
                                                 uint8_t tab, gui_rect_t rect,
                                                 const char *label,
                                                 gui_widget_callback_t cb) {
    return display_props_add_widget(st,
        bk_gui_widget_create_selectable_button(st->desktop, st->window,
                                            rect, label, cb),
        tab);
}

static gui_rect_t display_props_color_button_bounds(int index) {
    int col = index / 6;
    int row = index % 6;

    return (gui_rect_t){56 + col * 102, 255 + row * 22, 76, 18};
}

static uint32_t display_props_rand_next(uint32_t *seed) {
    *seed = (*seed * 1103515245U) + 12345U;
    return *seed;
}

static void display_props_reset_logo_preview(display_props_state_t *st) {
    if (!st) return;
    st->logo_seed = bk_sys_ticks() ^ 0x516A0E0U;
    st->logo_x = 8;
    st->logo_y = 10;
    st->logo_dx = 2;
    st->logo_dy = 2;
    st->last_logo_ms = bk_sys_uptime_ms();
}

static bool display_props_pipe_inside(int x, int y, int z) {
    return x >= -5 && x <= 5 &&
           y >= -5 && y <= 5 &&
           z >= -5 && z <= 5;
}

static uint8_t display_props_pipe_opposite(uint8_t dir) {
    return (uint8_t)(dir ^ 1U);
}

static void display_props_pipe_push(display_props_state_t *st, uint8_t dir) {
    display_preview_pipe_segment_t *seg;

    if (!st) return;
    if (st->pipe_segment_count >= DISPLAY_PREVIEW_PIPE_MAX) {
        for (uint8_t i = 1; i < DISPLAY_PREVIEW_PIPE_MAX; i++)
            st->pipe_segments[i - 1] = st->pipe_segments[i];
        st->pipe_segment_count = DISPLAY_PREVIEW_PIPE_MAX - 1;
    }

    seg = &st->pipe_segments[st->pipe_segment_count++];
    seg->x = st->pipe_cursor_x;
    seg->y = st->pipe_cursor_y;
    seg->z = st->pipe_cursor_z;
    seg->dir = dir;
    seg->color = st->pipe_color_index;

    st->pipe_cursor_x += g_display_preview_pipe_dirs[dir][0];
    st->pipe_cursor_y += g_display_preview_pipe_dirs[dir][1];
    st->pipe_cursor_z += g_display_preview_pipe_dirs[dir][2];
    st->pipe_last_dir = dir;
}

static void display_props_pipe_grow(display_props_state_t *st) {
    uint8_t chosen = display_props_pipe_opposite(st->pipe_last_dir);
    bool found = false;

    if (!st) return;

    for (uint32_t tries = 0; tries < 16; tries++) {
        uint8_t dir = (uint8_t)(display_props_rand_next(&st->pipe_seed) % 6U);
        if (st->pipe_segment_count &&
            dir == display_props_pipe_opposite(st->pipe_last_dir))
            continue;
        if (display_props_pipe_inside(
                st->pipe_cursor_x + g_display_preview_pipe_dirs[dir][0],
                st->pipe_cursor_y + g_display_preview_pipe_dirs[dir][1],
                st->pipe_cursor_z + g_display_preview_pipe_dirs[dir][2])) {
            chosen = dir;
            found = true;
            break;
        }
    }

    if (!found) {
        for (uint8_t dir = 0; dir < 6; dir++) {
            if (display_props_pipe_inside(
                    st->pipe_cursor_x + g_display_preview_pipe_dirs[dir][0],
                    st->pipe_cursor_y + g_display_preview_pipe_dirs[dir][1],
                    st->pipe_cursor_z + g_display_preview_pipe_dirs[dir][2])) {
                chosen = dir;
                break;
            }
        }
    }

    if ((display_props_rand_next(&st->pipe_seed) & 7U) == 0)
        st->pipe_color_index = (uint8_t)((st->pipe_color_index + 1U) %
            (sizeof(g_display_preview_pipe_colors) /
             sizeof(g_display_preview_pipe_colors[0])));

    display_props_pipe_push(st, chosen);
}

static void display_props_reset_pipe_preview(display_props_state_t *st) {
    if (!st) return;
    st->pipe_seed = bk_sys_ticks() ^ 0x51F1E50U;
    st->pipe_cursor_x = 0;
    st->pipe_cursor_y = 0;
    st->pipe_cursor_z = 0;
    st->pipe_last_dir = 0;
    st->pipe_color_index = 0;
    st->pipe_segment_count = 0;
    st->last_pipe_grow_ms = bk_sys_uptime_ms();
    st->last_pipe_motion_ms = bk_sys_uptime_ms();
    st->last_pipe_frame_ms = st->last_pipe_motion_ms - SS_FRAME_MS;
    st->pipe_rot_x = 25.0f;
    st->pipe_rot_y = 0.0f;
    st->pipe_rot_z = 0.0f;
    for (uint32_t i = 0; i < 18; i++)
        display_props_pipe_grow(st);
}

static void display_props_reset_preview(display_props_state_t *st) {
    display_props_reset_logo_preview(st);
    display_props_reset_pipe_preview(st);
}

static void display_props_load_wallpaper_preview(display_props_state_t *st) {
    gui_rect_t screen;
    uint16_t width, height;

    if (!st || !st->window) return;
    if (st->wallpaper_pixels) {
        bk_sys_free(st->wallpaper_pixels);
        st->wallpaper_pixels = NULL;
    }

    if (!st->wallpaper_path[0]) return;
    screen = display_props_monitor_screen_rect(st);
    uint8_t header[26];
    uint32_t source_w = 256, source_h = 256;
    int fd = bk_file_open(st->wallpaper_path, VFS_O_RDONLY);
    if (fd >= 0) {
        if (bk_file_read(fd, header, sizeof(header)) == sizeof(header) &&
            header[0] == 'B' && header[1] == 'M') {
            source_w = header[18] | header[19]<<8 | header[20]<<16 | (uint32_t)header[21]<<24;
            int32_t h = header[22] | header[23]<<8 | header[24]<<16 | (uint32_t)header[25]<<24;
            source_h = h < 0 ? (uint32_t)(-(int64_t)h) : (uint32_t)h;
        }
        bk_file_close(fd);
    }
    if (!source_w || !source_h || source_w > 4096 || source_h > 4096) source_w = source_h = 256;
    width = (uint16_t)screen.w; height = (uint16_t)screen.h;
    if (st->wallpaper_mode == DESK_WALLPAPER_TILE ||
        st->wallpaper_mode == DESK_WALLPAPER_CENTER) {
        width = (uint16_t)((source_w * (uint32_t)screen.w) / st->desktop->surface.width);
        height = (uint16_t)((source_h * (uint32_t)screen.h) / st->desktop->surface.height);
    } else if (st->wallpaper_mode == DESK_WALLPAPER_FIT ||
               st->wallpaper_mode == DESK_WALLPAPER_FILL) {
        bool wider = source_w * (uint32_t)screen.h > source_h * (uint32_t)screen.w;
        if (wider == (st->wallpaper_mode == DESK_WALLPAPER_FIT))
            height = (uint16_t)(source_h * width / source_w);
        else width = (uint16_t)(source_w * height / source_h);
    }
    if (!width) width = 1;
    if (!height) height = 1;
    st->wallpaper_preview_w = width; st->wallpaper_preview_h = height;
    st->wallpaper_pixels = bk_app_load_wallpaper(st->wallpaper_path, width, height);
}

static void display_props_load_monitor_asset(display_props_state_t *st) {
    if (!st) return;
    if (st->monitor_pixels) {
        bk_sys_free(st->monitor_pixels);
        st->monitor_pixels = NULL;
    }
    /* MONITOR.BMP es una plantilla raster especial: su rectangulo magenta
       delimita la mini pantalla. No debe redirigirse al icono BVI. */
    st->monitor_pixels = bk_app_load_wallpaper(
        DISPLAY_PREVIEW_MONITOR_PATH,
        DISPLAY_PREVIEW_MONITOR_W, DISPLAY_PREVIEW_MONITOR_H);
    /* Quitar solo el fondo conectado al borde, conservando los grises del
       monitor y el hueco magenta que se rellena en la vista previa. */
    if (st->monitor_pixels) {
        const uint32_t width = DISPLAY_PREVIEW_MONITOR_W;
        const uint32_t count = width * DISPLAY_PREVIEW_MONITOR_H;
        uint32_t *queue = bk_sys_alloc(count * sizeof(uint32_t));
        if (queue) {
            uint32_t key = st->monitor_pixels[0];
            uint32_t head = 0, tail = 0;
            queue[tail++] = 0;
            st->monitor_pixels[0] = 0;
            while (head < tail) {
                uint32_t pos = queue[head++];
                uint32_t neighbors[4] = {
                    pos % width ? pos - 1 : count,
                    pos % width + 1 < width ? pos + 1 : count,
                    pos >= width ? pos - width : count,
                    pos + width < count ? pos + width : count
                };
                for (uint32_t i = 0; i < 4; i++) {
                    uint32_t next = neighbors[i];
                    if (next < count && st->monitor_pixels[next] == key) {
                        st->monitor_pixels[next] = 0;
                        queue[tail++] = next;
                    }
                }
            }
            bk_sys_free(queue);
        }
    }
}

static void display_props_refresh_modes(display_props_state_t *st) {
    gfx_info_t info;
    uint32_t hardware_count;
    uint8_t current_bpp;
    bool found = false;

    if (!st) return;
    st->count = 0;
    st->live = gfx_list_all_display_modes(st->modes, DISPLAY_PROPS_MODE_MAX,
                                          &st->count);
    if (!bk_gfx_info(&info)) return;

    /*
     * VBE de 4 bpp no es uniforme entre adaptadores. Ofrecemos la misma
     * experiencia mediante un modo indexado de 8 bpp cuya salida queda
     * limitada a la paleta EGA de 16 entradas.
     */
    /* Modern SVGA drivers expose 32-bit scanout. The GUI can quantize that
     * output in place, so expose usable color choices at these resolutions. */
    hardware_count = st->count;
    for (uint32_t i = 0; i < hardware_count; ++i) {
        if (st->modes[i].bpp != 32U) continue;
        const uint8_t depths[] = {8, 16};
        for (uint32_t d = 0; d < 2U && st->count < DISPLAY_PROPS_MODE_MAX; ++d) {
            bool exists = false;
            for (uint32_t j = 0; j < st->count; ++j)
                if (st->modes[j].width == st->modes[i].width &&
                    st->modes[j].height == st->modes[i].height &&
                    st->modes[j].bpp == depths[d]) exists = true;
            if (!exists) st->modes[st->count++] = (gfx_display_mode_t){
                st->modes[i].width, st->modes[i].height, depths[d]};
        }
    }
    hardware_count = st->count;
    for (uint32_t i = 0; i < hardware_count &&
                         st->count < DISPLAY_PROPS_MODE_MAX; i++) {
        bool resolution_seen = false;
        bool indexed_8_available = false;
        for (uint32_t j = 0; j < hardware_count; j++) {
            if (st->modes[j].width == st->modes[i].width &&
                st->modes[j].height == st->modes[i].height &&
                st->modes[j].bpp == 8U) {
                indexed_8_available = true;
                break;
            }
        }
        for (uint32_t j = 0; j < i; j++) {
            if (st->modes[j].width == st->modes[i].width &&
                st->modes[j].height == st->modes[i].height) {
                resolution_seen = true;
                break;
            }
        }
        /* EGA uses indexed output, or software quantization on 32-bit GPUs. */
        if (!resolution_seen && indexed_8_available) {
            st->modes[st->count++] = (gfx_display_mode_t){
                st->modes[i].width, st->modes[i].height, 4
            };
        }
    }

    current_bpp = bk_gui_display_color_depth();
    st->selected_mode = -1;
    for (uint32_t i = 0; i < st->count; i++) {
        if (st->modes[i].width == info.width &&
            st->modes[i].height == info.height &&
            st->modes[i].bpp == current_bpp) {
            found = true;
            st->selected_mode = (int)i;
            break;
        }
    }

    if (!found && st->count < DISPLAY_PROPS_MODE_MAX) {
        st->selected_mode = (int)st->count;
        st->modes[st->count++] = (gfx_display_mode_t){
            info.width, info.height, current_bpp
        };
    }
}

static void display_props_mode_resolution_value(const gfx_display_mode_t *mode,
                                                char *value,
                                                size_t value_size) {
    if (!value || !value_size) return;
    value[0] = '\0';
    if (!mode) return;
    snprintf(value, value_size, "%ux%u", mode->width, mode->height);
}

static void display_props_mode_depth_value(uint8_t bpp,
                                           char *value,
                                           size_t value_size) {
    if (!value || !value_size) return;
    snprintf(value, value_size, "%u", bpp);
}

static void display_props_mode_depth_label(uint8_t bpp,
                                           char *label,
                                           size_t label_size) {
    if (!label || !label_size) return;
    switch (bpp) {
        case 4:
            bk_runtime_strncpy(label, "16 colores", label_size - 1);
            break;
        case 8:
            bk_runtime_strncpy(label, "256 colores", label_size - 1);
            break;
        case 15:
        case 16:
            bk_runtime_strncpy(label, "65 mil colores", label_size - 1);
            break;
        case 24:
        case 32:
            bk_runtime_strncpy(label, "16 millones", label_size - 1);
            break;
        default:
            snprintf(label, label_size, "%u-bit", bpp);
            break;
    }
    label[label_size - 1] = '\0';
}

static bool display_props_parse_resolution_value(const char *value,
                                                 uint16_t *width,
                                                 uint16_t *height) {
    uint32_t w = 0;
    uint32_t h = 0;

    if (!value || !value[0]) return false;
    while (*value >= '0' && *value <= '9') {
        w = w * 10U + (uint32_t)(*value - '0');
        value++;
    }
    if (*value != 'x' && *value != 'X') return false;
    value++;
    while (*value >= '0' && *value <= '9') {
        h = h * 10U + (uint32_t)(*value - '0');
        value++;
    }
    if (!w || !h) return false;
    if (width) *width = (uint16_t)w;
    if (height) *height = (uint16_t)h;
    return true;
}

static int display_props_find_mode(const display_props_state_t *st,
                                   uint16_t width,
                                   uint16_t height,
                                   uint8_t bpp) {
    if (!st) return -1;
    for (uint32_t i = 0; i < st->count; i++) {
        if (st->modes[i].width == width &&
            st->modes[i].height == height &&
            st->modes[i].bpp == bpp)
            return (int)i;
    }
    return -1;
}

static void display_props_refresh_mode_dropdown(display_props_state_t *st) {
    if (!st || !st->mode_dropdown) return;
    bk_gui_widget_dropdown_clear(st->mode_dropdown);
    for (uint32_t i = 0; i < st->count; i++) {
        char label[24];
        bool duplicate = false;

        display_props_mode_resolution_value(&st->modes[i], label, sizeof(label));
        for (uint32_t j = 0; j < i; j++) {
            if (st->modes[j].width == st->modes[i].width &&
                st->modes[j].height == st->modes[i].height) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        (void)bk_gui_widget_dropdown_add_item(st->mode_dropdown, label, label);
    }
}

static void display_props_refresh_depth_dropdown(display_props_state_t *st) {
    const char *selected;
    uint16_t width;
    uint16_t height;

    if (!st || !st->depth_dropdown) return;
    bk_gui_widget_dropdown_clear(st->depth_dropdown);

    selected = bk_gui_widget_dropdown_get_selected_value(st->mode_dropdown);
    if ((!selected || !selected[0]) && st->selected_mode >= 0) {
        static char current_resolution[24];
        display_props_mode_resolution_value(&st->modes[st->selected_mode],
                                            current_resolution,
                                            sizeof(current_resolution));
        selected = current_resolution;
    }
    if (!display_props_parse_resolution_value(selected, &width, &height)) return;

    for (uint32_t i = 0; i < st->count; i++) {
        char label[32];
        char value[8];
        bool duplicate = false;

        if (st->modes[i].width != width || st->modes[i].height != height)
            continue;
        for (uint32_t j = 0; j < i; j++) {
            if (st->modes[j].width == width &&
                st->modes[j].height == height &&
                st->modes[j].bpp == st->modes[i].bpp) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        display_props_mode_depth_label(st->modes[i].bpp, label, sizeof(label));
        display_props_mode_depth_value(st->modes[i].bpp, value, sizeof(value));
        (void)bk_gui_widget_dropdown_add_item(st->depth_dropdown, label, value);
    }
}

static void display_props_refresh_saver_dropdown(display_props_state_t *st) {
    vfs_dir_entry_t entries[VFS_MAX_DIR_ENTRIES];
    uint32_t found = 0;
    bool matched = false;

    if (!st || !st->saver_dropdown) return;
    bk_gui_widget_dropdown_clear(st->saver_dropdown);

    if (bk_file_list_dir(DISPLAY_SAVER_DIR, entries, VFS_MAX_DIR_ENTRIES, &found)) {
        for (uint32_t i = 0; i < found; i++) {
            char label[40];
            char path[VFS_MAX_PATH];

            if (entries[i].type != VFS_NODE_FILE ||
                !display_props_text_ends_with_ci(entries[i].name, ".SCV"))
                continue;

            display_props_saver_label_from_name(entries[i].name, label,
                                                sizeof(label));
            snprintf(path, sizeof(path), "%s/%s", DISPLAY_SAVER_DIR,
                     entries[i].name);
            if (bk_gui_widget_dropdown_add_item(st->saver_dropdown, label, path) &&
                bk_runtime_strcmp(path, display_props_saver_path_local()) == 0)
                matched = true;
        }
    }

    if (!matched && display_props_saver_path_local()[0]) {
        char label[40];
        display_props_saver_label_from_name(display_props_saver_path_local(), label,
                                            sizeof(label));
        (void)bk_gui_widget_dropdown_add_item(st->saver_dropdown, label,
                                           display_props_saver_path_local());
    }
}

static void display_props_refresh_wallpaper_dropdown(display_props_state_t *st) {
    vfs_dir_entry_t entries[VFS_MAX_DIR_ENTRIES];
    uint32_t found = 0;
    bool matched = false;

    if (!st || !st->wallpaper_dropdown) return;
    bk_gui_widget_dropdown_clear(st->wallpaper_dropdown);
    (void)bk_gui_widget_dropdown_add_item(st->wallpaper_dropdown,
                                       "Sin wallpaper", "");

    if (bk_file_list_dir(DISPLAY_WALLPAPER_DIR, entries, VFS_MAX_DIR_ENTRIES, &found)) {
        for (uint32_t i = 0; i < found; i++) {
            char label[40];
            char path[VFS_MAX_PATH];

            if (entries[i].type != VFS_NODE_FILE ||
                !display_props_wallpaper_is_supported(entries[i].name))
                continue;

            display_props_wallpaper_label_from_name(entries[i].name, label,
                                                    sizeof(label));
            snprintf(path, sizeof(path), "%s/%s", DISPLAY_WALLPAPER_DIR,
                     entries[i].name);
            if (bk_gui_widget_dropdown_add_item(st->wallpaper_dropdown, label, path) &&
                st->wallpaper_path[0] &&
                bk_runtime_strcmp(path, st->wallpaper_path) == 0)
                matched = true;
        }
    }

    if (st->wallpaper_path[0] && !matched) {
        char label[40];
        display_props_wallpaper_label_from_name(st->wallpaper_path, label,
                                                sizeof(label));
        (void)bk_gui_widget_dropdown_add_item(st->wallpaper_dropdown, label,
                                           st->wallpaper_path);
    }
}

static void display_props_sync_selection(display_props_state_t *st);

static void display_props_load_performance(display_props_state_t *st) {
    void *raw = NULL;
    uint32_t size = 0;

    if (!st) return;
    st->drag_outline = bk_gui_desktop_drag_outline_enabled(st->desktop);
    if (!bk_user_config_read_all(BK_DISPLAY_CONFIG_PATH,
                                 BK_DISPLAY_CONFIG_LEGACY_PATH,
                                 &raw, &size) || !raw)
        return;
    (void)size;

    {
        char *line = (char *)raw;
        while (*line) {
            char *end = line;
            char *eq = NULL;
            char saved;
            while (*end && *end != '\r' && *end != '\n') {
                if (*end == '=' && !eq) eq = end;
                end++;
            }
            saved = *end;
            *end = '\0';
            if (eq) {
                *eq++ = '\0';
                if (bk_runtime_strcmp(line, "drag_outline") == 0)
                    st->drag_outline = eq[0] != '0';
            }
            *end = saved;
            line = end;
            while (*line == '\r' || *line == '\n') line++;
        }
    }
    bk_sys_free(raw);
}

static void display_props_save_performance(display_props_state_t *st) {
    char config[40];

    if (!st) return;
    snprintf(config, sizeof(config), "drag_outline=%u\r\n",
             st->drag_outline ? 1U : 0U);
    (void)bk_user_config_write_text(BK_DISPLAY_CONFIG_PATH, config);
}

static void display_props_apply_drag_mode(gui_window_t *window, uint32_t id) {
    display_props_state_t *st = display_props_state(window);

    if (!st) return;
    if (st->drag_outline_buttons[0] &&
        st->drag_outline_buttons[0]->id == id)
        st->drag_outline = true;
    else if (st->drag_outline_buttons[1] &&
             st->drag_outline_buttons[1]->id == id)
        st->drag_outline = false;
    else
        return;

    bk_gui_desktop_set_drag_outline(st->desktop, st->drag_outline);
    display_props_save_performance(st);
    display_props_sync_selection(st);
    display_props_set_status(st, st->drag_outline
        ? "Movimiento por silueta activado."
        : "Movimiento con contenido activado.");
}

static void display_props_sync_widget_visibility(display_props_state_t *st) {
    if (!st) return;
    for (uint8_t i = 0; i < st->widget_count; i++) {
        gui_widget_t *widget = st->widgets[i].widget;
        if (!widget) continue;
        widget->visible = st->widgets[i].tab == st->current_tab;
        widget->pressed = false;
        widget->hovered = false;
    }
    if (st->window) st->window->dirty = true;
}

static void display_props_sync_selection(display_props_state_t *st) {
    char resolution[24];
    char depth[8];

    if (!st) return;

    for (uint32_t i = 0; i < DISPLAY_PROPS_COLOR_COUNT; i++)
        bk_gui_widget_set_selected(st->color_buttons[i],
            st->preview_color == g_display_props_colors[i]);

    if (st->wallpaper_path[0])
        (void)bk_gui_widget_dropdown_set_selected_by_value(st->wallpaper_dropdown,
                                                        st->wallpaper_path);
    else
        bk_gui_widget_dropdown_set_selected(st->wallpaper_dropdown, 0);
    (void)bk_gui_widget_dropdown_set_selected_by_value(st->saver_dropdown,
        display_props_saver_path_local());
    if (st->selected_mode >= 0 && st->selected_mode < (int)st->count) {
        display_props_mode_resolution_value(&st->modes[st->selected_mode],
                                            resolution, sizeof(resolution));
        (void)bk_gui_widget_dropdown_set_selected_by_value(st->mode_dropdown,
                                                        resolution);
    }
    display_props_refresh_depth_dropdown(st);
    if (st->selected_mode >= 0 && st->selected_mode < (int)st->count) {
        display_props_mode_depth_value(st->modes[st->selected_mode].bpp,
                                       depth, sizeof(depth));
        (void)bk_gui_widget_dropdown_set_selected_by_value(st->depth_dropdown,
                                                        depth);
    }
    bk_gui_widget_set_enabled(st->wallpaper_dropdown, true);
    if (st->wallpaper_mode_dropdown) {
        char mode_value[2] = {(char)('0' + st->wallpaper_mode), '\0'};
        (void)bk_gui_widget_dropdown_set_selected_by_value(
            st->wallpaper_mode_dropdown, mode_value);
        bk_gui_widget_set_enabled(st->wallpaper_mode_dropdown,
                                  st->wallpaper_path[0] != '\0');
    }
    bk_gui_widget_set_enabled(st->saver_dropdown,
        bk_gui_widget_dropdown_get_selected(st->saver_dropdown) >= 0);
    bk_gui_widget_set_enabled(st->mode_dropdown, st->count > 0);
    bk_gui_widget_set_enabled(st->depth_dropdown, st->count > 0);
    bk_gui_widget_set_selected(st->drag_outline_buttons[0],
                               st->drag_outline);
    bk_gui_widget_set_selected(st->drag_outline_buttons[1],
                               !st->drag_outline);
}

static void display_props_switch_tab(display_props_state_t *st, uint8_t tab) {
    if (!st || tab >= DISPLAY_PROPS_TAB_COUNT || st->current_tab == tab) return;
    st->current_tab = tab;
    if (tab == DISPLAY_TAB_SAVER) display_props_reset_preview(st);
    display_props_sync_widget_visibility(st);
    display_props_sync_selection(st);
    display_props_force_redraw(st);
}

static int display_props_hit_tab(const display_props_state_t *st, int x, int y) {
    if (!st) return -1;
    for (int i = 0; i < DISPLAY_PROPS_TAB_COUNT; i++)
        if (bk_gui_rect_contains(display_props_tab_rect(st, i), x, y)) return i;
    return -1;
}

static void display_props_apply_color(gui_window_t *window, uint32_t id) {
    display_props_state_t *st = display_props_state(window);

    for (uint32_t i = 0; st && i < DISPLAY_PROPS_COLOR_COUNT; i++) {
        if (!st->color_buttons[i] || st->color_buttons[i]->id != id) continue;
        st->preview_color = g_display_props_colors[i];
        bk_desktop_set_background(st->preview_color);
        st->wallpaper_path[0] = '\0';
        display_props_load_wallpaper_preview(st);
        display_props_sync_selection(st);
        display_props_set_status(st, "Color de escritorio aplicado.");
        return;
    }
}

static void display_props_apply_wallpaper_selection(gui_window_t *window,
                                                    uint32_t id) {
    display_props_state_t *st = display_props_state(window);
    const char *path;
    const char *label;
    (void)id;

    if (!st) return;
    path = bk_gui_widget_dropdown_get_selected_value(st->wallpaper_dropdown);
    label = bk_gui_widget_dropdown_get_selected_label(st->wallpaper_dropdown);

    if (!path || !path[0]) {
        st->wallpaper_path[0] = '\0';
        bk_desktop_set_background(st->preview_color);
        display_props_load_wallpaper_preview(st);
        display_props_sync_selection(st);
        display_props_set_status(st, "Fondo quitado.");
        return;
    }

    bk_runtime_strncpy(st->wallpaper_path, path, sizeof(st->wallpaper_path) - 1);
    st->wallpaper_path[sizeof(st->wallpaper_path) - 1] = '\0';
    display_props_load_wallpaper_preview(st);

    bk_desktop_set_wallpaper_mode(st->wallpaper_mode);
    if (bk_desktop_set_wallpaper(path)) {
        snprintf(st->status, sizeof(st->status), "Fondo aplicado: %s.",
                 label && label[0] ? label : "Wallpaper");
    } else {
        snprintf(st->status, sizeof(st->status),
                 "No se pudo cargar: %s.",
                 label && label[0] ? label : path);
    }
    /* La disponibilidad de Posicion depende de wallpaper_path. */
    display_props_sync_selection(st);
    display_props_force_redraw(st);
    display_props_force_full_refresh(st->desktop);
}

static void display_props_apply_wallpaper_mode(gui_window_t *window,
                                               uint32_t id) {
    display_props_state_t *st = display_props_state(window);
    const char *value;
    (void)id;
    if (!st || !st->wallpaper_mode_dropdown) return;
    value = bk_gui_widget_dropdown_get_selected_value(
        st->wallpaper_mode_dropdown);
    if (!value || value[0] < '0' || value[0] > '4') return;
    st->wallpaper_mode = (uint8_t)(value[0] - '0');
    bk_desktop_set_wallpaper_mode(st->wallpaper_mode);
    display_props_load_wallpaper_preview(st);
    display_props_set_status(st, "Posicion del fondo aplicada.");
    display_props_sync_selection(st);
    display_props_force_full_refresh(st->desktop);
}

static void display_props_saver_kind(gui_window_t *window, uint32_t id) {
    display_props_state_t *st = display_props_state(window);
    const char *path;
    const char *label;
    (void)id;

    if (!st) return;
    path = bk_gui_widget_dropdown_get_selected_value(st->saver_dropdown);
    label = bk_gui_widget_dropdown_get_selected_label(st->saver_dropdown);
    if (!path || !path[0]) return;
    bk_screensaver_set_path(path);
    display_props_reset_preview(st);
    display_props_sync_selection(st);
    snprintf(st->status, sizeof(st->status), "Protector seleccionado: %s.",
             label && label[0] ? label : display_props_saver_name_from_path(path));
    display_props_force_redraw(st);
}

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    int spawn_x;
    int spawn_y;
    gui_widget_t *enabled_buttons[2];
    gui_widget_t *timeout_dropdown;
    gui_widget_t *jelly_count_dropdown;
    gui_widget_t *jelly_variety_dropdown;
    bk_jelly_settings_t jelly;
    bool jelly_selected;
    gui_widget_t *preview_button;
    gui_widget_t *close_button;
    int layout_w;
    int layout_h;
    char status[80];
} display_saver_config_state_t;

static uint32_t display_props_timeout_from_value(const char *value) {
    uint32_t seconds = 0;

    if (!value) return 300U;
    while (*value >= '0' && *value <= '9') {
        seconds = seconds * 10U + (uint32_t)(*value - '0');
        value++;
    }
    return seconds ? seconds : 300U;
}

static void display_saver_config_sync(display_saver_config_state_t *cfg) {
    uint32_t timeout;
    char value[16];

    if (!cfg) return;
    timeout = bk_screensaver_get_timeout();
    bk_gui_widget_set_selected(cfg->enabled_buttons[0], bk_screensaver_is_enabled());
    bk_gui_widget_set_selected(cfg->enabled_buttons[1], !bk_screensaver_is_enabled());
    snprintf(value, sizeof(value), "%u", timeout);
    (void)bk_gui_widget_dropdown_set_selected_by_value(cfg->timeout_dropdown, value);
    if (cfg->jelly_selected) {
        char number[12];
        snprintf(number,sizeof(number),"%u",cfg->jelly.count);
        (void)bk_gui_widget_dropdown_set_selected_by_value(cfg->jelly_count_dropdown,number);
        snprintf(number,sizeof(number),"%u",cfg->jelly.variation);
        (void)bk_gui_widget_dropdown_set_selected_by_value(cfg->jelly_variety_dropdown,number);
    }
    if (cfg->window) cfg->window->dirty = true;
}

static void display_saver_config_jelly_changed(gui_window_t *window,uint32_t id) {
    display_saver_config_state_t *cfg=(display_saver_config_state_t *)window->content_context;
    (void)id;
    if(!cfg || !cfg->jelly_selected)return;
    const char *count_value=bk_gui_widget_dropdown_get_selected_value(cfg->jelly_count_dropdown);
    const char *variety_value=bk_gui_widget_dropdown_get_selected_value(cfg->jelly_variety_dropdown);
    if(!count_value || !variety_value)return;
    cfg->jelly.count=bk_jelly_clamp(display_props_timeout_from_value(count_value),1,10);
    cfg->jelly.variation=variety_value[0]>='0' && variety_value[0]<='3' &&
        variety_value[1]=='\0' ? (uint32_t)(variety_value[0]-'0') : 2U;
    bool saved=bk_jelly_save(&cfg->jelly);
    snprintf(cfg->status,sizeof(cfg->status),saved ?
        "Jelly: %u medusas, variedad %u. Guardado." :
        "Jelly: %u medusas, variedad %u. Error al guardar.",
        cfg->jelly.count,cfg->jelly.variation);
    display_saver_config_sync(cfg);
    display_props_force_full_refresh(cfg->desktop);
}

static void display_saver_config_toggle(gui_window_t *window, uint32_t id) {
    display_saver_config_state_t *cfg =
        (display_saver_config_state_t *)window->content_context;

    if (!cfg) return;
    if (cfg->enabled_buttons[0] && cfg->enabled_buttons[0]->id == id) {
        bk_screensaver_set_enabled(true);
        bk_runtime_strncpy(cfg->status, "Protector activado.", sizeof(cfg->status) - 1);
    } else {
        bk_screensaver_set_enabled(false);
        bk_runtime_strncpy(cfg->status, "Protector desactivado.", sizeof(cfg->status) - 1);
    }
    cfg->status[sizeof(cfg->status) - 1] = '\0';
    display_saver_config_sync(cfg);
    display_props_force_full_refresh(cfg->desktop);
}

static void display_saver_config_timeout(gui_window_t *window, uint32_t id) {
    display_saver_config_state_t *cfg =
        (display_saver_config_state_t *)window->content_context;
    const char *value;
    uint32_t seconds;
    (void)id;

    if (!cfg) return;
    value = bk_gui_widget_dropdown_get_selected_value(cfg->timeout_dropdown);
    seconds = display_props_timeout_from_value(value);
    bk_screensaver_set_timeout(seconds);
    snprintf(cfg->status, sizeof(cfg->status),
             "Tiempo guardado: %u min.", seconds / 60U);
    display_saver_config_sync(cfg);
    display_props_force_full_refresh(cfg->desktop);
}

static void display_saver_config_preview(gui_window_t *window, uint32_t id) {
    display_saver_config_state_t *cfg =
        (display_saver_config_state_t *)window->content_context;
    (void)id;

    if (!cfg) return;
    if (bk_screensaver_preview(cfg->desktop))
        bk_runtime_strncpy(cfg->status, "Vista previa lanzada.", sizeof(cfg->status) - 1);
    else
        bk_runtime_strncpy(cfg->status, "No se pudo abrir el protector.", sizeof(cfg->status) - 1);
    cfg->status[sizeof(cfg->status) - 1] = '\0';
    if (cfg->window) cfg->window->dirty = true;
    display_props_force_full_refresh(cfg->desktop);
}

static void display_saver_config_close(gui_window_t *window, uint32_t id) {
    (void)id;
    if (!window) return;
    bk_gui_close_window(window);
    bk_gui_request_paint();
}

static void display_saver_config_rects(gui_window_t *window,
                                       gui_rect_t *state_group,
                                       gui_rect_t *time_group,
                                       gui_rect_t *extra_group,
                                       gui_rect_t *status) {
    gui_rect_t client = bk_gui_window_content_rect_raw(window);
    int left = client.x + 12;
    int width = client.w - 24;
    int status_h = 16;
    int status_y = client.y + client.h - 18;
    int y = client.y + 14;
    int gap = 10;
    int h1 = 70;
    int h2 = 76;
    int extra_y;
    int extra_h;

    if (width < 120) width = 120;
    extra_y = y + h1 + gap + h2 + gap;
    extra_h = status_y - 8 - extra_y;
    if (extra_h < 68) extra_h = 68;

    if (state_group) *state_group = (gui_rect_t){left, y, width, h1};
    y += h1 + gap;
    if (time_group) *time_group = (gui_rect_t){left, y, width, h2};
    if (extra_group) *extra_group = (gui_rect_t){left, extra_y, width, extra_h};
    if (status) *status = (gui_rect_t){client.x + 6, status_y,
                                       client.w - 12, status_h};
}

static void display_saver_config_layout(display_saver_config_state_t *cfg) {
    gui_rect_t client;
    gui_rect_t state_group, time_group, extra_group, status;
    gui_rect_t row;
    gui_rect_t cell;

    if (!cfg || !cfg->window) return;
    client = bk_gui_window_content_rect_raw(cfg->window);
    if (cfg->layout_w == client.w && cfg->layout_h == client.h) return;
    cfg->layout_w = client.w;
    cfg->layout_h = client.h;
    display_saver_config_rects(cfg->window, &state_group, &time_group,
                               &extra_group, &status);

    row = (gui_rect_t){state_group.x + 14, state_group.y + 42,
                       state_group.w - 28, 22};
    cell = cpl_row_cell(row, 0, 2, 10);
    bk_gui_widget_set_bounds(cfg->window, cfg->enabled_buttons[0],
                             cpl_local_rect(cell, client));
    cell = cpl_row_cell(row, 1, 2, 10);
    bk_gui_widget_set_bounds(cfg->window, cfg->enabled_buttons[1],
                             cpl_local_rect(cell, client));

    row = (gui_rect_t){time_group.x + 14, time_group.y + 44,
                       time_group.w - 28, 22};
    bk_gui_widget_set_bounds(cfg->window, cfg->timeout_dropdown,
                             cpl_local_rect(row, client));

    if(cfg->jelly_selected) {
        row=(gui_rect_t){extra_group.x+14,extra_group.y+50,extra_group.w-28,22};
        bk_gui_widget_set_bounds(cfg->window,cfg->jelly_count_dropdown,cpl_local_rect(row,client));
        row.y=extra_group.y+114;
        bk_gui_widget_set_bounds(cfg->window,cfg->jelly_variety_dropdown,cpl_local_rect(row,client));
    }
    row = (gui_rect_t){extra_group.x + 14,
                       extra_group.y + extra_group.h - 30,
                       extra_group.w - 28, 22};
    cell = cpl_row_cell(row, 0, 2, 10);
    bk_gui_widget_set_bounds(cfg->window, cfg->preview_button,
                             cpl_local_rect(cell, client));
    cell = cpl_row_cell(row, 1, 2, 10);
    bk_gui_widget_set_bounds(cfg->window, cfg->close_button,
                             cpl_local_rect(cell, client));
}

static void display_saver_config_paint(gui_window_t *window,
                                       gui_surface_t *surface,
                                       void *context) {
    display_saver_config_state_t *cfg =
        (display_saver_config_state_t *)context;
    gui_rect_t state_group, time_group, extra_group, status;
    gui_rect_t inner;

    display_saver_config_rects(window, &state_group, &time_group,
                               &extra_group, &status);

    cpl_draw_group(surface, state_group, "Estado");
    inner = cpl_group_inner(state_group);
    cpl_draw_text_wrapped(surface,
        (gui_rect_t){inner.x, inner.y + 4, inner.w, 22},
        "Active o desactive el protector.", CPL_TEXT, 10, 2);

    cpl_draw_group(surface, time_group, "Tiempo");
    inner = cpl_group_inner(time_group);
    cpl_draw_text_wrapped(surface,
        (gui_rect_t){inner.x, inner.y + 4, inner.w, 24},
        "Elegir demora antes de activarse.", CPL_TEXT, 10, 2);

    cpl_draw_group(surface, extra_group,cfg && cfg->jelly_selected ? "Medusa (Gallium)" : "Extra");
    inner = cpl_group_inner(extra_group);
    if(cfg && cfg->jelly_selected) {
        cpl_draw_text_wrapped(surface,(gui_rect_t){inner.x,inner.y+5,inner.w,18},
                              "Cantidad de medusas (1 a 10)",CPL_TEXT,11,1);
        cpl_draw_text_wrapped(surface,(gui_rect_t){inner.x,inner.y+70,inner.w,18},
                              "Variedad de tamanos",CPL_TEXT,11,1);
        cpl_draw_text_wrapped(surface,(gui_rect_t){inner.x,extra_group.y+extra_group.h-61,inner.w,14},
                              "Ascienden y reaparecen por abajo.",CPL_TEXT,10,1);
    } else {
        cpl_draw_text_wrapped(surface,
            (gui_rect_t){inner.x, inner.y + 4, inner.w,extra_group.h - 48},
            "Puede probar el protector completo.", CPL_TEXT, 10, 2);
    }

    cpl_draw_bevel(surface, status, CPL_FACE, true);
    bk_gui_font_draw_string_clipped(surface, status.x + 4, status.y + 4,
        cfg && cfg->status[0] ? cfg->status : "Configuracion lista.",
        CPL_TEXT, (gui_rect_t){status.x + 4, status.y + 2,
        status.w - 8, status.h - 4});
}

static bool display_saver_config_event(gui_window_t *window,
                                       const gui_event_t *event,
                                       void *context) {
    (void)context;

    if (!window || !event) return false;
    if (event->type == GUI_EVENT_RESIZE) {
        display_saver_config_state_t *cfg =
            (display_saver_config_state_t *)context;
        if (cfg) {
            cfg->layout_w = 0;
            cfg->layout_h = 0;
            display_saver_config_layout(cfg);
        }
        return true;
    }
    if (event->type == GUI_EVENT_KEY && event->key == 27) {
        bk_gui_close_window(window);
        bk_gui_request_paint();
        return true;
    }
    return false;
}

static void display_saver_config_main(void *arg) {
    display_saver_config_state_t *cfg;

    cfg = (display_saver_config_state_t *)arg;
    if (!cfg || !cfg->desktop) {
        if (cfg) bk_sys_free(cfg);
        bk_proc_exit();
    }

    cfg->jelly_selected=display_props_saver_is_jelly();
    if(cfg->jelly_selected)bk_jelly_load(&cfg->jelly);
    cfg->window = bk_gui_create_window(cfg->desktop,
        cfg->spawn_x + 34, cfg->spawn_y + 48,
        cfg->jelly_selected?340:300,cfg->jelly_selected?440:310,
        "Configurar protector");
    if (!cfg->window) {
        bk_sys_free(cfg);
        bk_proc_exit();
    }

    bk_gui_set_window_min_size(cfg->window,
        cfg->jelly_selected?340:300,cfg->jelly_selected?440:310);
    bk_gui_set_window_content(cfg->window, display_saver_config_paint, cfg);
    bk_gui_set_window_event_handler(cfg->window, display_saver_config_event, cfg);
    cfg->window->bg_color = 0x00C0C0C0;
    cfg->window->owner_pid = bk_sys_getpid();
    bk_proc_bind_window(cfg->window);

    cfg->enabled_buttons[0] = bk_gui_widget_create_selectable_button(
        cfg->desktop, cfg->window, (gui_rect_t){26, 52, 110, 22},
        "Activado", display_saver_config_toggle);
    cfg->enabled_buttons[1] = bk_gui_widget_create_selectable_button(
        cfg->desktop, cfg->window, (gui_rect_t){146, 52, 110, 22},
        "Desactivado", display_saver_config_toggle);
    cfg->timeout_dropdown = bk_gui_widget_create_dropdown(
        cfg->desktop, cfg->window, (gui_rect_t){26, 138, 230, 22},
        display_saver_config_timeout);
    (void)bk_gui_widget_dropdown_add_item(cfg->timeout_dropdown, "1 minuto", "60");
    (void)bk_gui_widget_dropdown_add_item(cfg->timeout_dropdown, "5 minutos", "300");
    (void)bk_gui_widget_dropdown_add_item(cfg->timeout_dropdown, "10 minutos", "600");
    (void)bk_gui_widget_dropdown_add_item(cfg->timeout_dropdown, "15 minutos", "900");
    if(cfg->jelly_selected) {
        cfg->jelly_count_dropdown=bk_gui_widget_create_dropdown(
            cfg->desktop,cfg->window,(gui_rect_t){26,225,260,22},display_saver_config_jelly_changed);
        for(unsigned n=1;n<=10;n++) {
            char value[12],label[32];
            snprintf(value,sizeof(value),"%u",n);
            snprintf(label,sizeof(label),"%u %s",n,n==1?"medusa":"medusas");
            (void)bk_gui_widget_dropdown_add_item(cfg->jelly_count_dropdown,label,value);
        }
        cfg->jelly_variety_dropdown=bk_gui_widget_create_dropdown(
            cfg->desktop,cfg->window,(gui_rect_t){26,290,260,22},display_saver_config_jelly_changed);
        (void)bk_gui_widget_dropdown_add_item(cfg->jelly_variety_dropdown,"Uniformes","0");
        (void)bk_gui_widget_dropdown_add_item(cfg->jelly_variety_dropdown,"Poca variedad","1");
        (void)bk_gui_widget_dropdown_add_item(cfg->jelly_variety_dropdown,"Variedad media","2");
        (void)bk_gui_widget_dropdown_add_item(cfg->jelly_variety_dropdown,"Mucha variedad","3");
    }
    cfg->preview_button = bk_gui_widget_create_button(
        cfg->desktop, cfg->window, (gui_rect_t){26, 242, 110, 22},
        "Probar", display_saver_config_preview);
    cfg->close_button = bk_gui_widget_create_button(
        cfg->desktop, cfg->window, (gui_rect_t){146, 242, 110, 22},
        "Cerrar", display_saver_config_close);
    (void)bk_gui_widget_set_icon(cfg->preview_button, "Runonce106");
    (void)bk_gui_widget_set_icon(cfg->close_button, "Close");

    cfg->layout_w = 0;
    cfg->layout_h = 0;
    display_saver_config_layout(cfg);
    display_saver_config_sync(cfg);
    display_props_force_full_refresh(cfg->desktop);

    while (!bk_proc_exit_requested() && cfg->window->listed) bk_sys_sleep_ticks(2);

    cpl_destroy_window(cfg->desktop, cfg->window);
    display_props_force_full_refresh(cfg->desktop);
    bk_sys_free(cfg);
    bk_proc_exit();
}

static void display_props_open_saver_config(gui_window_t *window, uint32_t id) {
    display_props_state_t *st = display_props_state(window);
    display_saver_config_state_t *cfg;

    (void)id;
    if (!st || !st->desktop || !st->window) return;

    cfg = (display_saver_config_state_t *)bk_sys_alloc_zero(sizeof(*cfg));
    if (!cfg) return;

    cfg->desktop = st->desktop;
    cfg->spawn_x = st->window->bounds.x;
    cfg->spawn_y = st->window->bounds.y;
    if (bk_proc_spawn_thread("ssconfig", display_saver_config_main, cfg) < 0) {
        bk_sys_free(cfg);
        display_props_set_status(st, "No se pudo abrir Configurar.");
        return;
    }

    display_props_set_status(st, "Configuracion del protector abierta.");
    display_props_force_full_refresh(st->desktop);
}

static bool display_props_apply_mode_index(display_props_state_t *st,
                                           int mode_index) {
    if (!st || mode_index < 0 || mode_index >= (int)st->count) return false;

    if (bk_gui_change_resolution(st->desktop, st->modes[mode_index].width,
                              st->modes[mode_index].height,
                              st->modes[mode_index].bpp)) {
        st->selected_mode = mode_index;
        display_props_refresh_modes(st);
        display_props_refresh_mode_dropdown(st);
        display_props_sync_selection(st);
        snprintf(st->status, sizeof(st->status), "Aplicado: %ux%ux%u",
                 st->modes[st->selected_mode].width,
                 st->modes[st->selected_mode].height,
                 st->modes[st->selected_mode].bpp);
        display_props_force_redraw(st);
        return true;
    }

    snprintf(st->status, sizeof(st->status), "No se pudo aplicar ese modo.");
    display_props_force_redraw(st);
    return false;
}

static void display_props_apply_mode(gui_window_t *window, uint32_t id) {
    display_props_state_t *st = display_props_state(window);
    const char *value;
    uint16_t width;
    uint16_t height;
    uint8_t current_bpp;
    int mode_index;
    (void)id;

    if (!st) return;
    value = bk_gui_widget_dropdown_get_selected_value(st->mode_dropdown);
    if (!display_props_parse_resolution_value(value, &width, &height)) return;
    if (!st->count) return;

    current_bpp = (st->selected_mode >= 0 && st->selected_mode < (int)st->count)
        ? st->modes[st->selected_mode].bpp
        : st->modes[0].bpp;
    mode_index = display_props_find_mode(st, width, height, current_bpp);
    if (mode_index < 0) {
        for (uint32_t i = 0; i < st->count; i++) {
            if (st->modes[i].width == width && st->modes[i].height == height) {
                mode_index = (int)i;
                break;
            }
        }
    }
    if (mode_index < 0) return;

    st->selected_mode = mode_index;
    display_props_sync_selection(st);
    (void)display_props_apply_mode_index(st, mode_index);
}

static void display_props_apply_depth(gui_window_t *window, uint32_t id) {
    display_props_state_t *st = display_props_state(window);
    const char *res_value;
    const char *depth_value;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
    int mode_index;
    (void)id;

    if (!st) return;
    res_value = bk_gui_widget_dropdown_get_selected_value(st->mode_dropdown);
    depth_value = bk_gui_widget_dropdown_get_selected_value(st->depth_dropdown);
    if (!display_props_parse_resolution_value(res_value, &width, &height))
        return;
    if (!depth_value || !depth_value[0]) return;
    uint32_t depth = 0;
    for (const char *p = depth_value; *p; ++p) {
        if (*p < '0' || *p > '9' || depth > 32U) return;
        depth = depth * 10U + (uint32_t)(*p - '0');
    }
    if (depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32) return;
    bpp = (uint8_t)depth;
    mode_index = display_props_find_mode(st, width, height, bpp);
    if (mode_index < 0) return;
    (void)display_props_apply_mode_index(st, mode_index);
}

static void display_props_draw_monitor_fallback(display_props_state_t *st,
                                                gui_surface_t *surface) {
    gui_rect_t monitor = display_props_monitor_rect(st);
    gui_rect_t screen = display_props_monitor_screen_rect(st);

    cpl_draw_bevel(surface, monitor, 0x00D8D8D8, false);
    cpl_draw_bevel(surface,
        (gui_rect_t){screen.x - 8, screen.y - 8, screen.w + 16, screen.h + 16},
        0x00B8B8B8, true);
    bk_gui_gfx_fill_rect(surface,
        (gui_rect_t){monitor.x + 78, monitor.y + 162, 36, 10}, 0x00B0B0B0);
    bk_gui_gfx_fill_rect(surface,
        (gui_rect_t){monitor.x + 58, monitor.y + 173, 76, 5}, 0x00909090);
    bk_gui_gfx_fill_rect(surface,
        (gui_rect_t){monitor.x + 44, monitor.y + 178, 104, 4}, 0x00D6D6D6);
    bk_gui_gfx_draw_rect(surface,
        (gui_rect_t){monitor.x + 44, monitor.y + 178, 104, 4}, CPL_DARK);
    bk_gui_gfx_fill_rect(surface,
        (gui_rect_t){monitor.x + 120, monitor.y + 163, 6, 4}, 0x0000C020);
}

static void display_props_draw_preview_background(display_props_state_t *st,
                                                  gui_surface_t *surface,
                                                  gui_rect_t screen) {
    bk_gui_gfx_fill_rect(surface, screen, st->preview_color);
    if (st->wallpaper_pixels) {
        int w = st->wallpaper_preview_w, h = st->wallpaper_preview_h;
        int ox = (screen.w - w) / 2, oy = (screen.h - h) / 2;
        for (int y = 0; y < screen.h; ++y) {
            for (int x = 0; x < screen.w; ++x) {
                int sx = x - ox, sy = y - oy;
                if (st->wallpaper_mode == DESK_WALLPAPER_TILE) { sx = x % w; sy = y % h; }
                if (sx >= 0 && sy >= 0 && sx < w && sy < h)
                    bk_gui_gfx_putpixel(surface, screen.x+x, screen.y+y,
                        st->wallpaper_pixels[sy*w+sx]);
            }
        }
    }

    bk_gui_gfx_fill_rect(surface,
        (gui_rect_t){screen.x, screen.y + screen.h - 12, screen.w, 12}, CPL_FACE);
    bk_gui_gfx_draw_line(surface, screen.x, screen.y + screen.h - 12,
                      screen.x + screen.w - 1, screen.y + screen.h - 12, CPL_LIGHT);
    bk_gui_gfx_draw_line(surface, screen.x, screen.y + screen.h - 1,
                      screen.x + screen.w - 1, screen.y + screen.h - 1, CPL_DARK);
    bk_gui_gfx_fill_rect(surface,
        (gui_rect_t){screen.x + 3, screen.y + screen.h - 10, 24, 8}, 0x00D4D4D4);
    bk_gui_gfx_draw_rect(surface,
        (gui_rect_t){screen.x + 3, screen.y + screen.h - 10, 24, 8}, CPL_DARK);
    bk_gui_font_draw_string_clipped(surface, screen.x + 7, screen.y + screen.h - 8,
                                 "Ini", CPL_TEXT,
                                 (gui_rect_t){screen.x + 4, screen.y + screen.h - 10,
                                 22, 8});
}

static void display_props_draw_preview_screen_info(display_props_state_t *st,
                                                   gui_surface_t *surface,
                                                   gui_rect_t screen) {
    (void)st;
    display_props_draw_preview_background(st, surface, screen);
}

static void display_props_logo_draw_stars(gui_surface_t *surface, gui_rect_t rect,
                                          uint32_t seed) {
    for (uint32_t i = 0; i < 42; i++) {
        int x = rect.x + (int)(display_props_rand_next(&seed) % (uint32_t)rect.w);
        int y = rect.y + (int)(display_props_rand_next(&seed) % (uint32_t)rect.h);
        uint32_t shade = 0x00303030 + ((i & 3U) * 0x00181818);
        ss_putpixel_rgb(surface, x, y, shade);
    }
}

static void display_props_draw_logo_box(gui_surface_t *surface,
                                        gui_rect_t rect, int x, int y) {
    int w = rect.w > 78 ? 78 : rect.w - 8;
    int h = rect.h > 30 ? 30 : rect.h - 8;
    gui_rect_t box = {x, y, w, h};

    bk_gui_gfx_fill_rect(surface, box, 0x00000000);
    bk_gui_gfx_draw_rect(surface, box, 0x00FFFFFF);
    bk_gui_gfx_draw_rect(surface, (gui_rect_t){x + 2, y + 2, w - 4, h - 4}, 0x000080FF);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 4, y + 4, w - 8, h - 8}, 0x00002040);
    bk_gui_font_draw_string_clipped(surface, x + 9, y + 9, "BlesKernOS",
                                 0x00FFFFFF, box);
    bk_gui_font_draw_string_clipped(surface, x + 18, y + 18, "Saver",
                                 0x0000C0FF, box);
}

static void display_props_draw_saver_logo(display_props_state_t *st,
                                          gui_surface_t *surface,
                                          gui_rect_t screen) {
    uint32_t now = bk_sys_uptime_ms();
    int logo_w = screen.w > 78 ? 78 : screen.w - 8;
    int logo_h = screen.h > 30 ? 30 : screen.h - 8;
    uint32_t steps = ss_steps_due(now, &st->last_logo_ms, 30U, 8U);
    while (steps--) {
        st->logo_x += st->logo_dx;
        st->logo_y += st->logo_dy;
        if (st->logo_x <= 2) { st->logo_x = 2; st->logo_dx = 2; }
        if (st->logo_y <= 2) { st->logo_y = 2; st->logo_dy = 2; }
        if (st->logo_x + logo_w >= screen.w - 2) {
            st->logo_x = screen.w - logo_w - 3; st->logo_dx = -2;
        }
        if (st->logo_y + logo_h >= screen.h - 2) {
            st->logo_y = screen.h - logo_h - 3; st->logo_dy = -2;
        }
    }

    bk_gui_gfx_fill_rect(surface, screen, 0x00000000);
    display_props_logo_draw_stars(surface, screen, 0x00B10506U);
    display_props_draw_logo_box(surface, screen,
        screen.x + st->logo_x, screen.y + st->logo_y);
}

static void display_props_draw_saver_balls(display_props_state_t *st,
                                           gui_surface_t *surface,
                                           gui_rect_t screen) {
    (void)st;
    ss_balls_preview_draw(surface, screen);
}

static void display_props_pipe_preview_release(display_props_state_t *st) {
    if (!st) return;
    if (st->pipe_gl_ready) ss_mesa_surface_destroy(&st->pipe_mesa);
    st->pipe_pixels = NULL;
    st->pipe_render_w = 0;
    st->pipe_render_h = 0;
    st->pipe_gl_ready = false;
}

static bool display_props_pipe_preview_prepare(display_props_state_t *st,
                                               gui_rect_t screen) {
    GLfloat h;
    GLfloat light_pos[4] = {3.0f, 4.0f, 8.0f, 0.0f};
    GLfloat white[4] = {0.85f, 0.85f, 0.85f, 1.0f};
    GLfloat ambient[4] = {0.16f, 0.16f, 0.20f, 1.0f};

    if (!st || screen.w < 16 || screen.h < 16) return false;
    if (st->pipe_gl_ready &&
        st->pipe_render_w == (uint16_t)screen.w &&
        st->pipe_render_h == (uint16_t)screen.h)
        return ss_mesa_surface_make_current(&st->pipe_mesa);

    display_props_pipe_preview_release(st);

    st->pipe_render_w = (uint16_t)screen.w;
    st->pipe_render_h = (uint16_t)screen.h;
    /* Preview intentionally uses Mesa SWRAST.  It runs inside a control-panel
       paint callback, so it must not contend with the desktop's GFX3D path. */
    if (!ss_mesa_surface_create(&st->pipe_mesa,
                                st->pipe_render_w,
                                st->pipe_render_h,
                                false)) {
        display_props_pipe_preview_release(st);
        return false;
    }
    st->pipe_pixels = st->pipe_mesa.pixels;
    st->pipe_gl_ready = true;

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glViewport(0, 0, st->pipe_render_w, st->pipe_render_h);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightfv(GL_LIGHT0, GL_SPECULAR, white);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);

    h = (GLfloat)st->pipe_render_h / (GLfloat)st->pipe_render_w;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -h, h, 4.0, 80.0);
    return true;
}

static void display_props_pipe_material(uint8_t color) {
    GLfloat diffuse[4];
    GLfloat specular[4] = {0.90f, 0.90f, 0.90f, 1.0f};
    GLfloat shininess = 18.0f;
    uint32_t rgb = g_display_preview_pipe_colors[
        color % (sizeof(g_display_preview_pipe_colors) /
                 sizeof(g_display_preview_pipe_colors[0]))];

    diffuse[0] = (GLfloat)((rgb >> 16) & 0xFF) / 255.0f;
    diffuse[1] = (GLfloat)((rgb >> 8) & 0xFF) / 255.0f;
    diffuse[2] = (GLfloat)(rgb & 0xFF) / 255.0f;
    diffuse[3] = 1.0f;

    glMaterialfv(GL_FRONT, GL_DIFFUSE, diffuse);
    glMaterialfv(GL_FRONT, GL_SPECULAR, specular);
    glMaterialfv(GL_FRONT, GL_SHININESS, &shininess);
    glColor3fv(diffuse);
}

static void display_props_pipe_draw_box(GLfloat cx, GLfloat cy,
                                        GLfloat cz, GLfloat r) {
    GLfloat x0 = cx - r, x1 = cx + r;
    GLfloat y0 = cy - r, y1 = cy + r;
    GLfloat z0 = cz - r, z1 = cz + r;

    glBegin(GL_QUADS);

    glNormal3f(0.0f, 0.0f, 1.0f);
    glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1);
    glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);

    glNormal3f(0.0f, 0.0f, -1.0f);
    glVertex3f(x1, y0, z0); glVertex3f(x0, y0, z0);
    glVertex3f(x0, y1, z0); glVertex3f(x1, y1, z0);

    glNormal3f(1.0f, 0.0f, 0.0f);
    glVertex3f(x1, y0, z1); glVertex3f(x1, y0, z0);
    glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1);

    glNormal3f(-1.0f, 0.0f, 0.0f);
    glVertex3f(x0, y0, z0); glVertex3f(x0, y0, z1);
    glVertex3f(x0, y1, z1); glVertex3f(x0, y1, z0);

    glNormal3f(0.0f, 1.0f, 0.0f);
    glVertex3f(x0, y1, z1); glVertex3f(x1, y1, z1);
    glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);

    glNormal3f(0.0f, -1.0f, 0.0f);
    glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0);
    glVertex3f(x1, y0, z1); glVertex3f(x0, y0, z1);

    glEnd();
}

static void display_props_pipe_draw_cylinder(int axis,
                                             GLfloat x, GLfloat y, GLfloat z,
                                             GLfloat length, GLfloat radius) {
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= DISPLAY_PREVIEW_PIPE_SIDES; i++) {
        GLfloat a = (GLfloat)i * (GLfloat)(2.0 * M_PI) /
                    (GLfloat)DISPLAY_PREVIEW_PIPE_SIDES;
        GLfloat c = cos(a);
        GLfloat s = sin(a);

        if (axis == 0) {
            glNormal3f(0.0f, c, s);
            glVertex3f(x,          y + c * radius, z + s * radius);
            glVertex3f(x + length, y + c * radius, z + s * radius);
        } else if (axis == 1) {
            glNormal3f(c, 0.0f, s);
            glVertex3f(x + c * radius, y,          z + s * radius);
            glVertex3f(x + c * radius, y + length, z + s * radius);
        } else {
            glNormal3f(c, s, 0.0f);
            glVertex3f(x + c * radius, y + s * radius, z);
            glVertex3f(x + c * radius, y + s * radius, z + length);
        }
    }
    glEnd();
}

static void display_props_pipe_draw_segment(
    const display_preview_pipe_segment_t *seg) {
    GLfloat x0 = (GLfloat)seg->x * DISPLAY_PREVIEW_PIPE_CELL;
    GLfloat y0 = (GLfloat)seg->y * DISPLAY_PREVIEW_PIPE_CELL;
    GLfloat z0 = (GLfloat)seg->z * DISPLAY_PREVIEW_PIPE_CELL;
    int dx = g_display_preview_pipe_dirs[seg->dir][0];
    int dy = g_display_preview_pipe_dirs[seg->dir][1];
    int dz = g_display_preview_pipe_dirs[seg->dir][2];

    display_props_pipe_material(seg->color);

    if (dx) {
        if (dx < 0) x0 -= DISPLAY_PREVIEW_PIPE_CELL;
        display_props_pipe_draw_cylinder(0, x0, y0, z0,
            DISPLAY_PREVIEW_PIPE_CELL, DISPLAY_PREVIEW_PIPE_RADIUS);
    } else if (dy) {
        if (dy < 0) y0 -= DISPLAY_PREVIEW_PIPE_CELL;
        display_props_pipe_draw_cylinder(1, x0, y0, z0,
            DISPLAY_PREVIEW_PIPE_CELL, DISPLAY_PREVIEW_PIPE_RADIUS);
    } else {
        if (dz < 0) z0 -= DISPLAY_PREVIEW_PIPE_CELL;
        display_props_pipe_draw_cylinder(2, x0, y0, z0,
            DISPLAY_PREVIEW_PIPE_CELL, DISPLAY_PREVIEW_PIPE_RADIUS);
    }

    display_props_pipe_draw_box((GLfloat)seg->x * DISPLAY_PREVIEW_PIPE_CELL,
        (GLfloat)seg->y * DISPLAY_PREVIEW_PIPE_CELL,
        (GLfloat)seg->z * DISPLAY_PREVIEW_PIPE_CELL,
        DISPLAY_PREVIEW_PIPE_RADIUS * 1.35f);
    display_props_pipe_draw_box(
        (GLfloat)(seg->x + dx) * DISPLAY_PREVIEW_PIPE_CELL,
        (GLfloat)(seg->y + dy) * DISPLAY_PREVIEW_PIPE_CELL,
        (GLfloat)(seg->z + dz) * DISPLAY_PREVIEW_PIPE_CELL,
        DISPLAY_PREVIEW_PIPE_RADIUS * 1.35f);
}

static void display_props_draw_saver_pipes(display_props_state_t *st,
                                           gui_surface_t *surface,
                                           gui_rect_t screen) {
    uint32_t now;
    if (!st || !surface) return;
    if (!display_props_pipe_preview_prepare(st, screen)) {
        display_props_draw_saver_logo(st, surface, screen);
        return;
    }

    now = bk_sys_uptime_ms();
    uint32_t elapsed = now - st->last_pipe_motion_ms;
    if (ss_frame_due(now, &st->last_pipe_frame_ms)) {
        st->last_pipe_motion_ms = now;
        if (!ss_mesa_surface_make_current(&st->pipe_mesa)) return;
        uint32_t steps=ss_steps_due(now,&st->last_pipe_grow_ms,100U,8U);
        while(steps--) display_props_pipe_grow(st);
        if (elapsed>250U) elapsed=250U;
        st->pipe_rot_y += 1.2f * (GLfloat)elapsed / (GLfloat)SS_FRAME_MS;
        st->pipe_rot_z += 0.25f * (GLfloat)elapsed / (GLfloat)SS_FRAME_MS;
        while (st->pipe_rot_y >= 360.0f) st->pipe_rot_y -= 360.0f;
        while (st->pipe_rot_z >= 360.0f) st->pipe_rot_z -= 360.0f;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -18.0f);
        glRotatef(st->pipe_rot_x, 1.0f, 0.0f, 0.0f);
        glRotatef(st->pipe_rot_y, 0.0f, 1.0f, 0.0f);
        glRotatef(st->pipe_rot_z, 0.0f, 0.0f, 1.0f);
        for (uint32_t i = 0; i < st->pipe_segment_count; i++)
            display_props_pipe_draw_segment(&st->pipe_segments[i]);
        if (!ss_mesa_surface_present(&st->pipe_mesa)) return;
        st->pipe_pixels = st->pipe_mesa.pixels;
    }

    if (!st->pipe_pixels) return;
    ss_blit_rgb(surface,st->pipe_pixels,st->pipe_render_w,st->pipe_render_h,screen);
}

static void display_props_draw_monitor_preview(display_props_state_t *st,
                                               gui_surface_t *surface) {
    gui_rect_t screen = display_props_monitor_screen_rect(st);

    /* Primero el marco opaco y despues el contenido de la pantalla. Si el BMP
       se dibuja al final, su rectangulo magenta tapa completamente la vista
       previa que acabamos de componer. */
    if (st->monitor_pixels)
        bk_app_draw_icon(surface,
            display_props_monitor_rect(st).x,
            display_props_monitor_rect(st).y,
            st->monitor_pixels,
            DISPLAY_PREVIEW_MONITOR_W, DISPLAY_PREVIEW_MONITOR_H);
    else
        display_props_draw_monitor_fallback(st, surface);

    switch (st->current_tab) {
        case DISPLAY_TAB_BACKGROUND:
            display_props_draw_preview_background(st, surface, screen);
            break;
        case DISPLAY_TAB_SAVER:
            if (display_props_saver_is_jelly()) {
                bk_gui_gfx_fill_rect(surface, screen, 0x00000000);
                bk_gui_font_draw_string(surface, screen.x+8, screen.y+screen.h/2,
                    "Medusa (Gallium)", 0x00FFFFFF, 0, false);
            } else if (display_props_saver_is_balls())
                display_props_draw_saver_balls(st, surface, screen);
            else if (display_props_saver_is_pipes())
                display_props_draw_saver_pipes(st, surface, screen);
            else
                display_props_draw_saver_logo(st, surface, screen);
            bk_gui_request_paint();
            break;
        default:
            display_props_draw_preview_screen_info(st, surface, screen);
            break;
    }

}

static void display_props_draw_tabs(display_props_state_t *st,
                                    gui_surface_t *surface,
                                    gui_rect_t page) {
    for (int i = 0; i < DISPLAY_PROPS_TAB_COUNT; ++i)
        if (i != st->current_tab)
            cpl_draw_tab_face(surface, display_props_tab_rect(st, i),
                display_props_tab_name((uint8_t)i), false, page);
    cpl_draw_bevel(surface, page, CPL_FACE, false);
    cpl_draw_tab_face(surface, display_props_tab_rect(st, st->current_tab),
        display_props_tab_name(st->current_tab), true, page);
}

static void display_props_paint_background_tab(display_props_state_t *st,
                                               gui_surface_t *surface,
                                               gui_rect_t page) {
    cpl_draw_group(surface, (gui_rect_t){page.x + 16, page.y + 216, 236, 156},
                   "Color");
    for (int i = 0; i < DISPLAY_PROPS_COLOR_COUNT; i++) {
        gui_rect_t button = bk_gui_widget_screen_bounds(st->window,
                                                     st->color_buttons[i]);

        bk_gui_gfx_fill_rect(surface,
                          (gui_rect_t){button.x - 14, button.y + 4, 10, 10},
                          g_display_props_colors[i]);
        bk_gui_gfx_draw_rect(surface,
                          (gui_rect_t){button.x - 14, button.y + 4, 10, 10},
                          CPL_DARK);
    }

    cpl_draw_group(surface, (gui_rect_t){page.x + 272, page.y + 216, 236, 156},
                   "Imagen");
    cpl_draw_text_clipped(surface,
        (gui_rect_t){page.x + 284, page.y + 318, 210, 10},
        "Posicion:", CPL_TEXT);
}

static void display_props_paint_saver_tab(display_props_state_t *st,
                                          gui_surface_t *surface,
                                          gui_rect_t page) {
    const char *selected = bk_gui_widget_dropdown_get_selected_label(st->saver_dropdown);

    if (!selected || !selected[0])
        selected = display_props_saver_name_from_path(display_props_saver_path_local());

    cpl_draw_group(surface, (gui_rect_t){page.x + 16, page.y + 216, 236, 128},
                   "Protector");
    cpl_draw_text_clipped(surface,
        (gui_rect_t){page.x + 28, page.y + 318, 208, 10},
        selected, CPL_SHADOW);

    cpl_draw_group(surface, (gui_rect_t){page.x + 272, page.y + 216, 236, 128},
                   "Opciones");
}

static void display_props_draw_depth_preview(gui_surface_t *surface,
                                             gui_rect_t rect,
                                             uint8_t bpp) {
    static const uint32_t classic[16] = {
        0x00000000, 0x00800000, 0x00008000, 0x00808000,
        0x00000080, 0x00800080, 0x00008080, 0x00C0C0C0,
        0x00808080, 0x00FF0000, 0x0000FF00, 0x00FFFF00,
        0x000000FF, 0x00FF00FF, 0x0000FFFF, 0x00FFFFFF
    };
    gui_rect_t inner = {rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6};

    cpl_draw_bevel(surface, rect, CPL_FACE, true);
    if (inner.w <= 0 || inner.h <= 0) return;

    if (bpp <= 8) {
        int cols = 8;
        int rows = 2;
        int sw = inner.w / cols;
        int sh = inner.h / rows;
        for (int i = 0; i < 16; i++) {
            gui_rect_t cell = {
                inner.x + (i % cols) * sw,
                inner.y + (i / cols) * sh,
                sw,
                sh
            };
            bk_gui_gfx_fill_rect(surface, cell, classic[i]);
        }
        return;
    }

    for (int y = 0; y < inner.h; y++) {
        for (int x = 0; x < inner.w; x++) {
            uint8_t r = (uint8_t)((x * 255) / (inner.w > 1 ? inner.w - 1 : 1));
            uint8_t g = (uint8_t)((y * 255) / (inner.h > 1 ? inner.h - 1 : 1));
            uint8_t b = (uint8_t)(255 - ((x + y) * 255) /
                                  ((inner.w + inner.h) > 2
                                      ? (inner.w + inner.h - 2) : 1));
            bk_gui_gfx_putpixel(surface, inner.x + x, inner.y + y,
                             ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        }
    }
}

static void display_props_paint_screen_tab(display_props_state_t *st,
                                           gui_surface_t *surface,
                                           gui_rect_t page) {
    gfx_info_t info;
    bool info_ok = bk_gfx_info(&info);
    char line[64];
    char depth_label[32];
    uint8_t bpp = info_ok ? bk_gui_display_color_depth() : 0;
    (void)st;

    cpl_draw_group(surface, (gui_rect_t){page.x + 16, page.y + 216, 236, 128},
                   "Area de escritorio");
    cpl_draw_group(surface, (gui_rect_t){page.x + 272, page.y + 216, 236, 128},
                   "Cantidad de colores");
    display_props_draw_depth_preview(surface,
        (gui_rect_t){page.x + 308, page.y + 302, 188, 16}, bpp);
    display_props_mode_depth_label(bpp, depth_label, sizeof(depth_label));
    snprintf(line, sizeof(line), "%ux%u",
             info_ok ? info.width : 0, info_ok ? info.height : 0);
    cpl_draw_text_clipped(surface,
        (gui_rect_t){page.x + 28, page.y + 318, 208, 10},
        line, CPL_SHADOW);
    cpl_draw_text_clipped(surface,
        (gui_rect_t){page.x + 308, page.y + 324, 188, 10},
        depth_label, CPL_SHADOW);
}

static void display_props_paint_performance_tab(display_props_state_t *st,
                                                gui_surface_t *surface,
                                                gui_rect_t page) {
    (void)st;
    cpl_draw_group(surface,
        (gui_rect_t){page.x + 54, page.y + 218, page.w - 108, 132},
        "Movimiento de ventanas");
    cpl_draw_text_clipped(surface,
        (gui_rect_t){page.x + 78, page.y + 246, page.w - 156, 10},
        "Durante el arrastre:", CPL_TEXT);
    cpl_draw_text_wrapped(surface,
        (gui_rect_t){page.x + 78, page.y + 326, page.w - 156, 22},
        "La silueta reduce repintados y uso del framebuffer.",
        CPL_SHADOW, 10, 2);
}

static void display_props_paint(gui_window_t *window, gui_surface_t *surface,
                                void *context) {
    display_props_state_t *st = (display_props_state_t *)context;
    gui_rect_t client = bk_gui_window_content_rect_raw(window);
    gui_rect_t page = display_props_page_rect(st);
    gui_rect_t status_bar = {
        client.x + 6,
        client.y + client.h - 18,
        client.w - 12,
        16
    };

    display_props_draw_tabs(st, surface, page);
    display_props_draw_monitor_preview(st, surface);

    switch (st->current_tab) {
        case DISPLAY_TAB_BACKGROUND:
            display_props_paint_background_tab(st, surface, page);
            break;
        case DISPLAY_TAB_SAVER:
            display_props_paint_saver_tab(st, surface, page);
            break;
        case DISPLAY_TAB_SCREEN:
            display_props_paint_screen_tab(st, surface, page);
            break;
        default:
            display_props_paint_performance_tab(st, surface, page);
            break;
    }

    cpl_draw_bevel(surface, status_bar, CPL_FACE, true);
    bk_gui_font_draw_string_clipped(surface, status_bar.x + 6, status_bar.y + 4,
        st->status[0] ? st->status : display_props_default_status(st),
        CPL_TEXT, (gui_rect_t){status_bar.x + 6, status_bar.y + 2,
        status_bar.w - 12, status_bar.h - 4});
}

static bool display_props_event(gui_window_t *window,
                                const gui_event_t *event,
                                void *context) {
    display_props_state_t *st = (display_props_state_t *)context;
    int tab_hit;

    if (!st || !event) return false;
    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        tab_hit = display_props_hit_tab(st, event->x, event->y);
        if (tab_hit >= 0) {
            display_props_switch_tab(st, (uint8_t)tab_hit);
            return true;
        }
    }
    if (event->type == GUI_EVENT_KEY && event->key == '\t') {
        display_props_switch_tab(st,
            (uint8_t)((st->current_tab + 1) % DISPLAY_PROPS_TAB_COUNT));
        return true;
    }
    return false;
}

static void display_props_build_ui(display_props_state_t *st) {
    for (int i = 0; i < DISPLAY_PROPS_COLOR_COUNT; i++) {
        st->color_buttons[i] = display_props_select_button(st, DISPLAY_TAB_BACKGROUND,
            display_props_color_button_bounds(i),
            g_display_props_color_names[i],
            display_props_apply_color);
    }

    st->wallpaper_dropdown = display_props_add_widget(st,
        bk_gui_widget_create_dropdown(st->desktop, st->window,
            (gui_rect_t){308, 311, 188, 22},
            display_props_apply_wallpaper_selection),
        DISPLAY_TAB_BACKGROUND);

    st->wallpaper_mode_dropdown = display_props_add_widget(st,
        bk_gui_widget_create_dropdown(st->desktop, st->window,
            (gui_rect_t){380, 345, 116, 22},
            display_props_apply_wallpaper_mode),
        DISPLAY_TAB_BACKGROUND);
    (void)bk_gui_widget_dropdown_add_item(st->wallpaper_mode_dropdown,
                                          "Estirar", "0");
    (void)bk_gui_widget_dropdown_add_item(st->wallpaper_mode_dropdown,
                                          "Centrar", "1");
    (void)bk_gui_widget_dropdown_add_item(st->wallpaper_mode_dropdown,
                                          "Mosaico", "2");
    (void)bk_gui_widget_dropdown_add_item(st->wallpaper_mode_dropdown,
                                          "Ajustar", "3");
    (void)bk_gui_widget_dropdown_add_item(st->wallpaper_mode_dropdown,
                                          "Rellenar", "4");

    st->saver_dropdown = display_props_add_widget(st,
        bk_gui_widget_create_dropdown(st->desktop, st->window,
            (gui_rect_t){52, 282, 188, 22}, display_props_saver_kind),
        DISPLAY_TAB_SAVER);
    st->saver_config_button = display_props_button(st, DISPLAY_TAB_SAVER,
        (gui_rect_t){343, 292, 118, 24}, "Configurar...",
        display_props_open_saver_config);
    (void)bk_gui_widget_set_icon(st->saver_config_button, "Settings");

    st->mode_dropdown = display_props_add_widget(st,
        bk_gui_widget_create_dropdown(st->desktop, st->window,
            (gui_rect_t){52, 276, 188, 22}, display_props_apply_mode),
        DISPLAY_TAB_SCREEN);
    st->depth_dropdown = display_props_add_widget(st,
        bk_gui_widget_create_dropdown(st->desktop, st->window,
            (gui_rect_t){308, 276, 188, 22}, display_props_apply_depth),
        DISPLAY_TAB_SCREEN);

    st->drag_outline_buttons[0] = display_props_select_button(st,
        DISPLAY_TAB_PERFORMANCE, (gui_rect_t){92, 300, 162, 26},
        "Mostrar silueta", display_props_apply_drag_mode);
    st->drag_outline_buttons[1] = display_props_select_button(st,
        DISPLAY_TAB_PERFORMANCE, (gui_rect_t){276, 300, 162, 26},
        "Mostrar contenido", display_props_apply_drag_mode);
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    display_props_state_t *st;

    if (!desktop) return;
    st = (display_props_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) return;

    st->desktop = desktop;
    st->preview_color = bk_desktop_get_background();
    st->wallpaper_mode = bk_desktop_get_wallpaper_mode();
    bk_runtime_strncpy(st->wallpaper_path, bk_desktop_get_wallpaper(),
             sizeof(st->wallpaper_path) - 1);
    st->wallpaper_path[sizeof(st->wallpaper_path) - 1] = '\0';
    st->current_tab = (uint8_t)DISPLAY_PROPS_START_TAB;
    if (st->current_tab >= DISPLAY_PROPS_TAB_COUNT)
        st->current_tab = DISPLAY_TAB_BACKGROUND;

    display_props_refresh_modes(st);
    display_props_load_performance(st);
    display_props_reset_preview(st);

    st->window = bk_gui_create_window(desktop, 84, 28, 536, 448,
                                           DISPLAY_PROPS_WINDOW_TITLE);
    if (!st->window) {
        bk_sys_free(st);
        return;
    }

    bk_gui_set_window_min_size(st->window, 536, 448);
    st->window->bg_color = 0x00C0C0C0;
    st->window->owner_pid = bk_sys_getpid();
    bk_proc_bind_window(st->window);

    display_props_load_monitor_asset(st);
    display_props_load_wallpaper_preview(st);
    display_props_build_ui(st);
    display_props_refresh_mode_dropdown(st);
    display_props_refresh_wallpaper_dropdown(st);
    display_props_refresh_saver_dropdown(st);
    display_props_sync_widget_visibility(st);
    display_props_sync_selection(st);
    /* Publish callbacks only after tab visibility and both mode selectors are
     * initialized. Early paint upcalls could interrupt UI construction. */
    bk_gui_set_window_content(st->window, display_props_paint, st);
    bk_gui_set_window_event_handler(st->window, display_props_event, st);
    display_props_force_redraw(st);
    kprintf("[DISPLAY] ready modes=%u depth=%u uptime_ms=%u\n",
            st->count,bk_gui_display_color_depth(),bk_sys_uptime_ms());

    {
        uint32_t last_preview_ms = bk_sys_uptime_ms();

        while (!bk_proc_exit_requested() && st->window->listed) {
            uint32_t now = bk_sys_uptime_ms();

            /*
             * Los callbacks de contenido Ring 3 solo vuelven a ejecutarse si la
             * ventana queda sucia. bk_gui_request_paint() por si solo despierta
             * al compositor, pero no solicita otro callback para esta ventana.
             * Mantener el pulso fuera del callback evita que la animacion dependa
             * de movimientos del mouse o de clicks.
             */
            if (st->current_tab == DISPLAY_TAB_SAVER) {
                if (ss_frame_due(now, &last_preview_ms)) {
                    bk_gui_window_invalidate(st->window);
                }
            } else {
                last_preview_ms = now;
            }

            bk_sys_sleep_ticks(1);
        }
    }

    display_props_pipe_preview_release(st);
    if (st->monitor_pixels) bk_sys_free(st->monitor_pixels);
    if (st->wallpaper_pixels) bk_sys_free(st->wallpaper_pixels);
    cpl_destroy_window(st->desktop, st->window);
    bk_sys_free(st);
    bk_proc_exit();
}

#endif
