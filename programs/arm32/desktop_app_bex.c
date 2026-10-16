#include "../../sdk/include/bleskernos_api.h"

#ifndef ARM_APP_MODE
#define ARM_APP_MODE 1
#endif
#ifndef ARM_APP_TITLE
#define ARM_APP_TITLE "BlesKernOS"
#endif

#define APP_ABOUT    1
#define APP_CALENDAR 2
#define APP_RUN      3
#define APP_EDITOR   4
#define APP_FILES    5
#define APP_IMAGE    6
#define APP_PAINT    7
#define APP_PROCESS  8
#define APP_CONTROL  10
#define APP_MOUSE    11
#define APP_KEYBOARD 12
#define APP_LANGUAGE 13
#define APP_SOUND    14
#define APP_NETWORK  15
#define APP_DISK     16
#define APP_CLIPBOARD 17

static bk_gui_window_t *g_window;
static bk_gui_desktop_t *g_desktop;
static char g_path[BK_PATH_MAX] = "/";
static char g_text[4096];
static uint32_t g_text_length;
static bk_file_entry_t g_entries[64];
static uint32_t g_entry_count;
static uint32_t g_selected;
static bk_gui_widget_t *g_input;
static uint32_t g_action_id;
static bk_gui_point_t g_points[512];
static uint32_t g_point_count;
static bk_gui_image_t g_image;

static void copy(char *to, const char *from, uint32_t capacity) {
    uint32_t i = 0U;
    if (!to || !capacity) return;
    if (!from) from = "";
    while (i + 1U < capacity && from[i]) { to[i] = from[i]; ++i; }
    to[i] = '\0';
}

static void append(char *to, const char *from, uint32_t capacity) {
    uint32_t n = (uint32_t)bk_string_length(to), i = 0U;
    while (from && from[i] && n + 1U < capacity) to[n++] = from[i++];
    to[n] = '\0';
}

static bool ends_with(const char *value, const char *suffix) {
    uint32_t a = (uint32_t)bk_string_length(value);
    uint32_t b = (uint32_t)bk_string_length(suffix), i;
    if (b > a) return false;
    for (i = 0U; i < b; ++i) {
        char x = value[a - b + i], y = suffix[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return false;
    }
    return true;
}

static void number(char *out, uint32_t value) {
    char reverse[12]; uint32_t n = 0U, i = 0U;
    if (!value) reverse[n++] = '0';
    while (value && n < sizeof(reverse)) {
        reverse[n++] = (char)('0' + value % 10U); value /= 10U;
    }
    while (n) out[i++] = reverse[--n];
    out[i] = '\0';
}

static void path_join(char *out, const char *base, const char *name) {
    copy(out, base, BK_PATH_MAX);
    if (out[0] && out[bk_string_length(out) - 1U] != '/') append(out, "/", BK_PATH_MAX);
    append(out, name, BK_PATH_MAX);
}

static void load_directory(const char *path) {
    uint32_t count = 0U;
    if (bk_file_list_dir(path, g_entries, 64U, &count)) {
        copy(g_path, path, sizeof(g_path)); g_entry_count = count; g_selected = 0U;
    }
    if (g_window) bk_gui_window_invalidate(g_window);
}

static void open_file_entry(uint32_t index) {
    char path[BK_PATH_MAX];
    if (index >= g_entry_count) return;
    path_join(path, g_path, g_entries[index].name);
    if (g_entries[index].type == BK_FILE_NODE_DIRECTORY) load_directory(path);
    else if (ends_with(path, ".BEX") || ends_with(path, ".CPL"))
        (void)bk_app_launch(path, NULL);
    else if (ends_with(path, ".BMP"))
        (void)bk_app_launch("/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX", path);
    else (void)bk_app_launch("/SYSTEM/PROGRAMS/TEXTEDITOR.BEX", path);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool load_bmp(const char *path) {
    void *raw = NULL; uint32_t size = 0U, offset, width, height, stride, x, y;
    uint16_t bits; uint8_t *data; uint32_t *pixels;
    if (!bk_file_read_all(path, &raw, &size) || size < 54U) return false;
    data = (uint8_t *)raw; offset = rd32(data + 10); width = rd32(data + 18);
    height = rd32(data + 22); bits = rd16(data + 28);
    if (data[0] != 'B' || data[1] != 'M' || rd32(data + 30) != 0U ||
        !width || !height || width > 1024U || height > 1024U ||
        (bits != 24U && bits != 32U)) { bk_sys_free(raw); return false; }
    stride = ((width * bits + 31U) / 32U) * 4U;
    if (offset > size || height > (size - offset) / stride) {
        bk_sys_free(raw); return false;
    }
    pixels = (uint32_t *)bk_sys_alloc(width * height * 4U);
    if (!pixels) { bk_sys_free(raw); return false; }
    for (y = 0U; y < height; ++y) for (x = 0U; x < width; ++x) {
        uint8_t *p = data + offset + (height - 1U - y) * stride + x * (bits / 8U);
        pixels[y * width + x] = 0xFF000000U | ((uint32_t)p[2] << 16) |
                                ((uint32_t)p[1] << 8) | p[0];
    }
    bk_sys_free(raw); g_image.pixels = pixels;
    g_image.width = (uint16_t)width; g_image.height = (uint16_t)height;
    return true;
}

static void draw_line_text(bk_gui_surface_t *surface, int x, int y,
                           const char *text, uint32_t color) {
    bk_gui_surface_draw_text(surface, x, y, text, color, 0x00C0C0C0U, false);
}

static void paint(bk_gui_window_t *window, bk_gui_surface_t *surface, void *context) {
    bk_gui_rect_t c; char line[320], value[16]; uint32_t i;
    (void)context;
    if (!bk_gui_window_content_rect(window, &c)) return;
    bk_gui_surface_fill_rect(surface, c, 0x00C0C0C0U);
#if ARM_APP_MODE == APP_ABOUT
    draw_line_text(surface, c.x + 22, c.y + 25, "BlesKernOS 0.9 - ARM32", 0x00000080U);
    draw_line_text(surface, c.x + 22, c.y + 48, "Raspberry Pi 3B / AArch32 Ring 3", 0x00000000U);
    draw_line_text(surface, c.x + 22, c.y + 69, "Aplicacion ELF .BEX aislada del kernel", 0x00000000U);
#elif ARM_APP_MODE == APP_CALENDAR
    {
        bk_datetime_t dt;
        if (bk_time_datetime(&dt)) {
            copy(line, "Fecha: ", sizeof(line)); number(value, dt.date.day); append(line, value, sizeof(line));
            append(line, "/", sizeof(line)); number(value, dt.date.month); append(line, value, sizeof(line));
            append(line, "/", sizeof(line)); number(value, dt.date.year); append(line, value, sizeof(line));
            draw_line_text(surface, c.x + 28, c.y + 34, line, 0x00000000U);
            copy(line, "Hora: ", sizeof(line)); number(value, dt.time.hour); append(line, value, sizeof(line));
            append(line, ":", sizeof(line)); number(value, dt.time.minute); append(line, value, sizeof(line));
            append(line, ":", sizeof(line)); number(value, dt.time.second); append(line, value, sizeof(line));
            draw_line_text(surface, c.x + 28, c.y + 58, line, 0x00000080U);
        }
    }
#elif ARM_APP_MODE == APP_EDITOR
    draw_line_text(surface, c.x + 5, c.y + 4, g_path[0] ? g_path : "Sin titulo", 0x00000080U);
    {
        uint32_t start = 0U, row = 0U;
        for (i = 0U; i <= g_text_length && row < 25U; ++i) {
            if (i == g_text_length || g_text[i] == '\n') {
                uint32_t n = i - start; if (n > sizeof(line) - 1U) n = sizeof(line) - 1U;
                for (uint32_t j = 0U; j < n; ++j) line[j] = g_text[start + j];
                line[n] = '\0'; draw_line_text(surface, c.x + 5, c.y + 24 + (int)row * 14, line, 0x00000000U);
                start = i + 1U; ++row;
            }
        }
    }
#elif ARM_APP_MODE == APP_FILES
    draw_line_text(surface, c.x + 5, c.y + 4, g_path, 0x00000080U);
    for (i = 0U; i < g_entry_count && i < 24U; ++i) {
        copy(line, g_entries[i].type == BK_FILE_NODE_DIRECTORY ? "[DIR] " : "      ", sizeof(line));
        append(line, g_entries[i].name, sizeof(line));
        if (i == g_selected) bk_gui_surface_fill_rect(surface,
            (bk_gui_rect_t){c.x + 2, c.y + 21 + (int)i * 14, c.w - 4, 14}, 0x008080FFU);
        draw_line_text(surface, c.x + 6, c.y + 23 + (int)i * 14, line, 0x00000000U);
    }
#elif ARM_APP_MODE == APP_IMAGE
    if (g_image.pixels) {
        int w = g_image.width, h = g_image.height;
        if (w > c.w - 12) { h = h * (c.w - 12) / w; w = c.w - 12; }
        if (h > c.h - 12) { w = w * (c.h - 12) / h; h = c.h - 12; }
        bk_gui_surface_draw_image(surface,
            (bk_gui_rect_t){c.x + (c.w - w) / 2, c.y + (c.h - h) / 2, w, h}, c, &g_image);
    } else draw_line_text(surface, c.x + 12, c.y + 20, "BMP no valido o no seleccionado", 0x00000000U);
#elif ARM_APP_MODE == APP_PAINT
    bk_gui_surface_fill_rect(surface, (bk_gui_rect_t){c.x + 4, c.y + 4, c.w - 8, c.h - 8}, 0x00FFFFFFU);
    for (i = 1U; i < g_point_count; ++i)
        bk_gui_surface_draw_line(surface, g_points[i - 1U].x, g_points[i - 1U].y,
                                g_points[i].x, g_points[i].y, 0x00000000U);
#elif ARM_APP_MODE == APP_PROCESS
    draw_line_text(surface, c.x + 5, c.y + 4, "PID   PROCESO                 ESTADO     RAM", 0x00000080U);
    for (i = 0U; i < bk_proc_count() && i < 22U; ++i) {
        bk_process_info_t p; if (!bk_proc_info(i, &p)) continue;
        number(line, p.pid); append(line, "     ", sizeof(line)); append(line, p.name, sizeof(line));
        append(line, "                         ", sizeof(line)); line[29] = '\0';
        append(line, p.state == BK_PROCESS_RUNNING ? "running" : p.state == BK_PROCESS_SLEEPING ? "sleep" : "ready", sizeof(line));
        append(line, "     ", sizeof(line)); number(value, p.memory_bytes / 1024U); append(line, value, sizeof(line)); append(line, " KB", sizeof(line));
        if (i == g_selected) bk_gui_surface_fill_rect(surface,
            (bk_gui_rect_t){c.x + 2, c.y + 21 + (int)i * 14, c.w - 4, 14}, 0x008080FFU);
        draw_line_text(surface, c.x + 5, c.y + 23 + (int)i * 14, line, 0x00000000U);
    }
#elif ARM_APP_MODE == APP_CONTROL
    draw_line_text(surface, c.x + 18, c.y + 24, "Panel de control ARM", 0x00000080U);
    draw_line_text(surface, c.x + 18, c.y + 49, "Mouse, teclado e idioma ahora se ejecutan en Ring 3.", 0x00000000U);
    draw_line_text(surface, c.x + 18, c.y + 72, "Abra los archivos .CPL desde /SYSTEM/CONTROL.", 0x00000000U);
#elif ARM_APP_MODE == APP_MOUSE
    copy(line, "Sensibilidad: ", sizeof(line)); number(value, bk_input_mouse_get_sensitivity()); append(line, value, sizeof(line));
    draw_line_text(surface, c.x + 20, c.y + 30, line, 0x00000000U);
    draw_line_text(surface, c.x + 20, c.y + 58, "Use - / + para ajustar", 0x00000080U);
#elif ARM_APP_MODE == APP_KEYBOARD || ARM_APP_MODE == APP_LANGUAGE
    draw_line_text(surface, c.x + 20, c.y + 25, "Idioma y distribucion", 0x00000080U);
    draw_line_text(surface, c.x + 20, c.y + 52, "[E] Espanol     [U] English", 0x00000000U);
#elif ARM_APP_MODE == APP_SOUND
    draw_line_text(surface, c.x + 20, c.y + 28, "Audio BCM2837 PWM/DMA PCM", 0x00000080U);
    draw_line_text(surface, c.x + 20, c.y + 54, "Pulse T para reproducir un tono de prueba.", 0x00000000U);
#elif ARM_APP_MODE == APP_NETWORK
    {
        bk_net_info_t net;
        if (bk_net_get_info(&net)) {
            copy(line, "Dispositivo: ", sizeof(line)); append(line, net.device, sizeof(line));
            append(line, net.link_up ? "  enlace activo" : "  sin enlace", sizeof(line));
            draw_line_text(surface, c.x + 20, c.y + 22, line, 0x00000080U);
            copy(line, "IPv4: ", sizeof(line));
            for (i = 0U; i < 4U; ++i) { number(value, net.address[i]); append(line, value, sizeof(line)); if (i != 3U) append(line, ".", sizeof(line)); }
            draw_line_text(surface, c.x + 20, c.y + 47, line, 0x00000000U);
            draw_line_text(surface, c.x + 20, c.y + 72, "Pulse D para renovar DHCP.", 0x00000000U);
        }
    }
#elif ARM_APP_MODE == APP_DISK
    {
        bk_volume_info_t volume;
        if (bk_device_volume_info(&volume)) {
            copy(line, "Volumen: ", sizeof(line)); append(line, volume.volume_label, sizeof(line));
            append(line, "  ", sizeof(line)); append(line, volume.filesystem, sizeof(line));
            draw_line_text(surface, c.x + 20, c.y + 22, line, 0x00000080U);
            copy(line, "Total: ", sizeof(line)); number(value, (uint32_t)(volume.total_bytes / 1048576U)); append(line, value, sizeof(line)); append(line, " MiB", sizeof(line));
            draw_line_text(surface, c.x + 20, c.y + 47, line, 0x00000000U);
            copy(line, "Libre: ", sizeof(line)); number(value, (uint32_t)(volume.free_bytes / 1048576U)); append(line, value, sizeof(line)); append(line, " MiB", sizeof(line));
            draw_line_text(surface, c.x + 20, c.y + 70, line, 0x00000000U);
        }
    }
#elif ARM_APP_MODE == APP_CLIPBOARD
    (void)bk_clipboard_get_text(line, sizeof(line));
    draw_line_text(surface, c.x + 15, c.y + 18, "Contenido del portapapeles:", 0x00000080U);
    draw_line_text(surface, c.x + 15, c.y + 44, line[0] ? line : "(vacio)", 0x00000000U);
#else
    draw_line_text(surface, c.x + 8, c.y + 8, "Escriba un comando y pulse Ejecutar", 0x00000000U);
#endif
}

static void run_action(bk_gui_window_t *window, uint32_t id) {
    char command[128], path[BK_PATH_MAX]; (void)window;
    if (id != g_action_id || !g_input || !bk_gui_widget_get_text(g_input, command, sizeof(command))) return;
    if (!command[0]) return;
    if (command[0] == '/') (void)bk_app_launch(command, NULL);
    else {
        copy(path, "/SYSTEM/COMMANDS/", sizeof(path)); append(path, command, sizeof(path));
        if (!ends_with(path, ".BEX")) append(path, ".BEX", sizeof(path));
        (void)bk_app_launch(path, NULL);
    }
}

static bool event(bk_gui_window_t *window, const bk_gui_event_t *e, void *context) {
    bk_gui_rect_t c; int x, y; (void)context;
    if (!e || !bk_gui_window_content_rect(window, &c)) return false;
    x = e->x - c.x; y = e->y - c.y;
#if ARM_APP_MODE == APP_EDITOR
    if (e->type != BK_GUI_EVENT_KEY) return false;
    if (e->ctrl && (e->key == 's' || e->key == 'S')) {
        if (g_path[0]) (void)bk_file_write_all(g_path, g_text, g_text_length); return true;
    }
    if (e->key == BK_KEY_BACKSPACE && g_text_length) --g_text_length;
    else if ((e->key == BK_KEY_ENTER || (e->key >= 32 && e->key < 127)) && g_text_length + 1U < sizeof(g_text))
        g_text[g_text_length++] = e->key == BK_KEY_ENTER ? '\n' : e->key;
    else return false;
    g_text[g_text_length] = '\0'; bk_gui_window_invalidate(window); return true;
#elif ARM_APP_MODE == APP_FILES
    if (e->type == BK_GUI_EVENT_MOUSE_UP && e->button == 1U && y >= 21) {
        uint32_t row = (uint32_t)(y - 21) / 14U;
        if (row < g_entry_count) { if (row == g_selected) open_file_entry(row); else g_selected = row; }
        bk_gui_window_invalidate(window); return true;
    }
#elif ARM_APP_MODE == APP_PAINT
    if ((e->type == BK_GUI_EVENT_MOUSE_DOWN || e->type == BK_GUI_EVENT_MOUSE_MOVE) &&
        (e->buttons & 1U || e->type == BK_GUI_EVENT_MOUSE_DOWN) && g_point_count < 512U) {
        g_points[g_point_count++] = (bk_gui_point_t){e->x, e->y};
        bk_gui_window_invalidate(window); return true;
    }
#elif ARM_APP_MODE == APP_PROCESS
    if (e->type == BK_GUI_EVENT_MOUSE_UP && e->button == 1U && y >= 21) {
        uint32_t row = (uint32_t)(y - 21) / 14U;
        if (row < bk_proc_count()) {
            if (row == g_selected) { bk_process_info_t p; if (bk_proc_info(row, &p) && p.pid != bk_sys_getpid()) (void)bk_proc_request_exit(p.pid); }
            g_selected = row; bk_gui_window_invalidate(window); return true;
        }
    }
#elif ARM_APP_MODE == APP_MOUSE
    if (e->type == BK_GUI_EVENT_KEY && (e->key == '+' || e->key == '-')) {
        uint8_t s = bk_input_mouse_get_sensitivity();
        if (e->key == '+' && s < 10U) ++s; if (e->key == '-' && s > 1U) --s;
        bk_input_mouse_set_sensitivity(s); bk_gui_window_invalidate(window); return true;
    }
#elif ARM_APP_MODE == APP_KEYBOARD || ARM_APP_MODE == APP_LANGUAGE
    if (e->type == BK_GUI_EVENT_KEY && (e->key == 'e' || e->key == 'E' || e->key == 'u' || e->key == 'U')) {
        (void)bk_lang_set((e->key == 'e' || e->key == 'E') ? "es" : "en");
        bk_gui_window_invalidate(window); return true;
    }
#elif ARM_APP_MODE == APP_SOUND
    if (e->type == BK_GUI_EVENT_KEY && (e->key == 't' || e->key == 'T')) {
        (void)bk_sound_tone(440U, 300U); return true;
    }
#elif ARM_APP_MODE == APP_NETWORK
    if (e->type == BK_GUI_EVENT_KEY && (e->key == 'd' || e->key == 'D')) {
        (void)bk_net_dhcp(5000U); bk_gui_window_invalidate(window); return true;
    }
#endif
    return false;
}

void bleskernos_program_main(void *unused) {
    char argument[BK_PATH_MAX]; void *raw = NULL; uint32_t bytes = 0U;
    (void)unused; argument[0] = '\0'; (void)bk_proc_launch_arg_copy(argument, sizeof(argument));
    g_desktop = bk_gui_desktop(); if (!g_desktop) return;
#if ARM_APP_MODE == APP_FILES
    load_directory(argument[0] ? argument : "/");
#elif ARM_APP_MODE == APP_EDITOR
    if (argument[0]) {
        copy(g_path, argument, sizeof(g_path));
        if (bk_file_read_all(g_path, &raw, &bytes)) {
            if (bytes >= sizeof(g_text)) bytes = sizeof(g_text) - 1U;
            for (g_text_length = 0U; g_text_length < bytes; ++g_text_length) g_text[g_text_length] = ((char *)raw)[g_text_length];
            g_text[g_text_length] = '\0'; bk_sys_free(raw);
        }
    } else g_path[0] = '\0';
#elif ARM_APP_MODE == APP_IMAGE
    if (argument[0]) { copy(g_path, argument, sizeof(g_path)); (void)load_bmp(argument); }
#endif
    g_window = bk_gui_create_window(g_desktop, 115, 45,
#if ARM_APP_MODE == APP_ABOUT || ARM_APP_MODE == APP_CALENDAR || ARM_APP_MODE >= APP_CONTROL
        430, 180,
#elif ARM_APP_MODE == APP_RUN
        470, 150,
#else
        600, 400,
#endif
        ARM_APP_TITLE);
    if (!g_window) return;
    bk_gui_set_window_content(g_window, paint, NULL);
    bk_gui_set_window_event_handler(g_window, event, NULL);
    bk_gui_set_window_min_size(g_window, 320, 150);
#if ARM_APP_MODE == APP_RUN
    g_input = bk_gui_create_textbox(g_desktop, g_window, (bk_gui_rect_t){10, 38, 330, 24}, "", 120U, NULL);
    {
        bk_gui_widget_t *button = bk_gui_create_button(g_desktop, g_window,
            (bk_gui_rect_t){350, 38, 90, 24}, "Ejecutar", run_action);
        g_action_id = bk_gui_widget_id(button);
    }
    bk_gui_widget_set_focus(g_window, g_input, true);
#endif
    bk_gui_window_invalidate(g_window);
    while (bk_gui_window_is_open(g_window)) {
#if ARM_APP_MODE == APP_CALENDAR || ARM_APP_MODE == APP_PROCESS
        bk_gui_window_invalidate(g_window);
#endif
        bk_sys_sleep_ms(ARM_APP_MODE == APP_PROCESS ? 500U : 50U);
    }
    if (g_image.pixels) bk_sys_free(g_image.pixels);
    bk_gui_destroy_window(g_desktop, g_window); g_window = NULL;
}
