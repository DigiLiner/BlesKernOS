#include "../kernel/include/api.h"
#include "../system/control/control_common.h"

#define RUNBOX_W 480
#define RUNBOX_H 238
#define RUNBOX_INPUT_MAX 128
#define RUNBOX_STATUS_MAX 96

#define RUNBOX_BTN_W 104
#define RUNBOX_BTN_H 26

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_image_t button_icons[2];
    bool button_icon_loaded[2];
    char input[RUNBOX_INPUT_MAX];
    char status[RUNBOX_STATUS_MAX];
    bool focused;
} runbox_state_t;

static runbox_state_t *g_runbox;

static void runbox_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || !dst_len) return;
    if (!src) src = "";
    bk_runtime_strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void runbox_append(char *dst, size_t dst_len, const char *src) {
    size_t len;
    if (!dst || !dst_len || !src) return;
    len = bk_runtime_strlen(dst);
    if (len >= dst_len) return;
    bk_runtime_strncpy(dst + len, src, dst_len - len - 1);
    dst[dst_len - 1] = '\0';
}

static bool runbox_has_char(const char *s, char c) {
    if (!s) return false;
    while (*s) {
        if (*s == c) return true;
        s++;
    }
    return false;
}

static char runbox_upper_char(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static bool runbox_name_is(const char *left, const char *right) {
    if (!left || !right) return false;
    while (*left && *right) {
        if (runbox_upper_char(*left++) != runbox_upper_char(*right++)) return false;
    }
    return *left == '\0' && *right == '\0';
}

static void runbox_append_upper(char *dst, size_t dst_len, const char *src) {
    size_t len;
    if (!dst || !dst_len || !src) return;
    len = bk_runtime_strlen(dst);
    while (*src && len + 1 < dst_len) {
        dst[len++] = runbox_upper_char(*src++);
    }
    dst[len] = '\0';
}

static const char *runbox_extension(const char *path) {
    const char *dot = NULL;
    if (!path) return "";
    while (*path) {
        if (*path == '.') dot = path;
        path++;
    }
    return dot ? dot : "";
}

static bool runbox_ext_is(const char *ext, const char *a, const char *b) {
    return bk_runtime_strcmp(ext, a) == 0 || bk_runtime_strcmp(ext, b) == 0;
}

static void runbox_make_path(const char *input, char *out, size_t out_len) {
    if (!out || !out_len) return;
    out[0] = '\0';
    if (!input) return;

    while (*input == ' ') input++;

    if (*input == '/') {
        runbox_copy(out, out_len, input);
        return;
    }

    if (runbox_name_is(input, "control") ||
        runbox_name_is(input, "control.o")) {
        runbox_copy(out, out_len, "/SYSTEM/CONTROL/CONTROL.BEX");
        return;
    }

    /* Alias de los nombres 8.3 usados antes de /SYSTEM/PROGRAMS. */
    if (runbox_name_is(input, "calc") || runbox_name_is(input, "calc.o")) {
        runbox_copy(out, out_len, "/SYSTEM/PROGRAMS/CALCULATOR.BEX");
        return;
    }
    if (runbox_name_is(input, "files") || runbox_name_is(input, "files.o")) {
        runbox_copy(out, out_len, "/SYSTEM/PROGRAMS/FILE.BEX");
        return;
    }
    if (runbox_name_is(input, "viewer") || runbox_name_is(input, "viewer.o")) {
        runbox_copy(out, out_len, "/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX");
        return;
    }
    if (runbox_name_is(input, "textedit") ||
        runbox_name_is(input, "textedit.o")) {
        runbox_copy(out, out_len, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX");
        return;
    }

    if (runbox_ext_is(runbox_extension(input), ".CPL", ".cpl")) {
        runbox_copy(out, out_len, "/SYSTEM/CONTROL/");
        runbox_append_upper(out, out_len, input);
        return;
    }

    runbox_copy(out, out_len, "/SYSTEM/PROGRAMS/");
    if (runbox_has_char(input, '.')) {
        runbox_append_upper(out, out_len, input);
    } else {
        runbox_append_upper(out, out_len, input);
        runbox_append(out, out_len, ".BEX");
    }
}

static void runbox_set_status_path(runbox_state_t *st,
                                   const char *prefix,
                                   const char *path) {
    if (!st) return;
    runbox_copy(st->status, sizeof(st->status), prefix ? prefix : "");
    runbox_append(st->status, sizeof(st->status), path ? path : "");
    if (st->window) st->window->dirty = true;
}

static bool runbox_path_is_dir(const char *path) {
    vfs_dir_entry_t dummy[1];
    uint32_t count = 0;
    if (!path || !*path) return false;
    return bk_file_list_dir(path, dummy, 1, &count);
}

static void runbox_close(runbox_state_t *st) {
    if (!st) return;
    if (st->window) {
        bk_gui_desktop_remove_window(st->desktop, st->window);
        bk_gui_window_destroy_raw(st->window);
        bk_proc_bind_window(NULL);
        st->window = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (st->button_icon_loaded[i])
            bk_gui_image_free(&st->button_icons[i]);
    }
    if (g_runbox == st) g_runbox = NULL;
    bk_sys_free(st);
    bk_gui_request_paint();
    bk_proc_exit();
}

static void runbox_execute(runbox_state_t *st) {
    char path[VFS_MAX_PATH];
    const char *ext;

    if (!st) return;
    if (!st->input[0]) {
        runbox_copy(st->status, sizeof(st->status), "@HD0578EDE");
        if (st->window) st->window->dirty = true;
        return;
    }

    runbox_make_path(st->input, path, sizeof(path));
    ext = runbox_extension(path);

    if (runbox_path_is_dir(path)) {
        (void)bk_app_execute_path_arg(st->desktop, "/SYSTEM/PROGRAMS/FILE.BEX",
                                       path);
        runbox_set_status_path(st, "Abriendo carpeta: ", path);
        return;
    }

    if (runbox_ext_is(ext, ".EXE", ".exe") ||
        runbox_ext_is(ext, ".BAT", ".bat")) {
        if (bk_app_execute_path_arg(st->desktop,
                "/SYSTEM/PROGRAMS/WINE.BEX", path))
            runbox_set_status_path(st, "Ejecutado con Wine: ", path);
        else runbox_set_status_path(st, "No se pudo ejecutar: ", path);
        return;
    }
    if (bk_app_is_object(path) || runbox_name_is(ext, ".BEX") ||
        bk_app_is_win32(path)) {
        if (bk_app_execute_path(st->desktop, path)) {
            runbox_set_status_path(st, "Ejecutado: ", path);
            return;
        }
        runbox_set_status_path(st, "No se pudo ejecutar: ", path);
        return;
    }

    if (runbox_ext_is(ext, ".BMP", ".bmp") ||
        runbox_ext_is(ext, ".GIF", ".gif")) {
        (void)bk_app_execute_path_arg(st->desktop, "/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX",
                                       path);
        runbox_set_status_path(st, "Abriendo imagen: ", path);
        return;
    }

    if (runbox_ext_is(ext, ".TXT", ".txt") ||
        runbox_ext_is(ext, ".INI", ".ini") ||
        runbox_ext_is(ext, ".MD", ".md") ||
        runbox_ext_is(ext, ".C", ".c") ||
        runbox_ext_is(ext, ".H", ".h") ||
        runbox_ext_is(ext, ".ASM", ".asm")) {
        (void)bk_app_execute_path_arg(st->desktop, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX",
                                       path);
        runbox_set_status_path(st, "Abriendo texto: ", path);
        return;
    }

    /* Ultimo recurso: intentar abrir como texto. Si no existe, el editor deberia avisar. */
    (void)bk_app_execute_path_arg(st->desktop, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX", path);
    runbox_set_status_path(st, "Intentando abrir: ", path);
}

static gui_rect_t runbox_input_rect(runbox_state_t *st) {
    gui_rect_t client = bk_gui_window_content_rect_raw(st->window);
    return (gui_rect_t){client.x + 16, client.y + 56, client.w - 32, 26};
}

static gui_rect_t runbox_ok_rect(runbox_state_t *st) {
    gui_rect_t client = bk_gui_window_content_rect_raw(st->window);
    return (gui_rect_t){client.x + client.w - 16 - 2 * RUNBOX_BTN_W - 8,
                        client.y + client.h - 12 - RUNBOX_BTN_H,
                        RUNBOX_BTN_W, RUNBOX_BTN_H};
}

static gui_rect_t runbox_cancel_rect(runbox_state_t *st) {
    gui_rect_t client = bk_gui_window_content_rect_raw(st->window);
    return (gui_rect_t){client.x + client.w - 16 - RUNBOX_BTN_W,
                        client.y + client.h - 12 - RUNBOX_BTN_H,
                        RUNBOX_BTN_W, RUNBOX_BTN_H};
}

static void runbox_draw_button(gui_surface_t *s, gui_rect_t r,
                               const char *text, const gui_image_t *icon) {
    int text_x = r.x + 12;
    cpl_draw_bevel(s, r, CPL_FACE, false);
    if (icon && icon->pixels) {
        bk_gui_surface_draw_image(s, (gui_rect_t){r.x + 7, r.y + (r.h - 16) / 2, 16, 16},
                                  r, icon);
        text_x = r.x + 29;
    }
    bk_gui_font_draw_string_clipped(s, text_x, r.y + 7, text,
                                    0x00202020,
                                    (gui_rect_t){text_x, r.y + 2,
                                                 r.x + r.w - text_x - 3,
                                                 r.h - 4});
}

static void runbox_content(gui_window_t *window, gui_surface_t *s,
                           void *ctx) {
    runbox_state_t *st = (runbox_state_t *)ctx;
    gui_rect_t client;
    gui_rect_t input;
    const char *visible_text;

    if (!st || !window || !s) return;
    client = bk_gui_window_content_rect_raw(window);
    bk_gui_gfx_fill_rect(s, client, CPL_FACE);
    cpl_draw_text_wrapped(s,
        (gui_rect_t){client.x + 16, client.y + 12, client.w - 32, 32},
        "@H1A6BF702", CPL_TEXT, 16, 2);

    input = runbox_input_rect(st);
    cpl_draw_bevel(s, input, CPL_WHITE, true);
    if (st->focused) bk_gui_gfx_draw_rect(s, input, CPL_BLUE);
    visible_text = st->input;
    while (*visible_text && bk_gui_font_text_width(visible_text) > input.w - 14)
        visible_text++;
    cpl_draw_text_clipped(s,
        (gui_rect_t){input.x + 6, input.y + 8, input.w - 12, 14},
        visible_text, CPL_TEXT);
    if (st->focused) {
        int cursor_x = input.x + 6 + (int)bk_gui_font_text_width(visible_text);
        bk_gui_gfx_fill_rect(s, (gui_rect_t){cursor_x, input.y + 6, 1, 13}, CPL_TEXT);
    }
    cpl_draw_text_wrapped(s,
        (gui_rect_t){client.x + 16, client.y + 96, client.w - 32,
                     runbox_ok_rect(st).y - client.y - 106},
        st->status, CPL_SHADOW, 16, 0);

    runbox_draw_button(s, runbox_ok_rect(st), "@H69D93100",
        st->button_icon_loaded[0] ? &st->button_icons[0] : NULL);
    runbox_draw_button(s, runbox_cancel_rect(st), "@HEA3D2B54",
        st->button_icon_loaded[1] ? &st->button_icons[1] : NULL);
}

static bool runbox_event(gui_window_t *window, const gui_event_t *event,
                         void *ctx) {
    runbox_state_t *st = (runbox_state_t *)ctx;
    size_t len;

    if (!st || !window || !event) return false;

    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        if (bk_gui_rect_contains(runbox_input_rect(st), event->x, event->y)) {
            st->focused = true;
            window->dirty = true;
            return true;
        }
        if (bk_gui_rect_contains(runbox_ok_rect(st), event->x, event->y)) {
            runbox_execute(st);
            return true;
        }
        if (bk_gui_rect_contains(runbox_cancel_rect(st), event->x, event->y)) {
            runbox_close(st);
            return true;
        }
        st->focused = false;
        window->dirty = true;
        return bk_gui_window_contains(window, event->x, event->y);
    }

    if (event->type != GUI_EVENT_KEY) return false;

    if (event->key == 27) {
        runbox_close(st);
        return true;
    }

    if (event->key == '\n' || event->key == '\r') {
        runbox_execute(st);
        return true;
    }

    len = bk_runtime_strlen(st->input);
    if (event->key == '\b') {
        if (len) st->input[len - 1] = '\0';
        window->dirty = true;
        return true;
    }

    if (event->key >= 32 && event->key < 127 &&
        len + 1 < sizeof(st->input)) {
        st->input[len] = (char)event->key;
        st->input[len + 1] = '\0';
        st->status[0] = '\0';
        window->dirty = true;
        return true;
    }

    return false;
}

static void runbox_cleanup(runbox_state_t *st) {
    if (!st) return;
    if (st->window) {
        bk_gui_desktop_remove_window(st->desktop, st->window);
        bk_gui_window_destroy_raw(st->window);
        bk_proc_bind_window(NULL);
        st->window = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (st->button_icon_loaded[i])
            bk_gui_image_free(&st->button_icons[i]);
    }
    if (g_runbox == st) g_runbox = NULL;
    bk_sys_free(st);
}

static void runbox_main(void *arg) {
    runbox_state_t *st = (runbox_state_t *)arg;
    int x;
    int y;

    if (!st || !st->desktop) {
        runbox_cleanup(st);
        bk_proc_exit();
    }

    x = ((int)st->desktop->surface.width - RUNBOX_W) / 2;
    y = ((int)st->desktop->surface.height - RUNBOX_H) / 2;
    if (x < 8) x = 8;
    if (y < 8) y = 8;

    st->window = bk_gui_create_window(st->desktop, x, y,
                                           RUNBOX_W, RUNBOX_H,
                                           "@H69D93100");
    if (!st->window) {
        runbox_cleanup(st);
        bk_proc_exit();
    }

    (void)bk_about_attach(st->window, st->desktop, &(bk_about_info_t){
        "@H69D93100", "@H1C1E4EC2", "@H45333E14",
        "@H7A28E1E5", "/ICONS/OBJECT.BMP"});

    bk_gui_set_window_min_size(st->window, RUNBOX_W, RUNBOX_H);
    st->focused = true;
    st->button_icon_loaded[0] =
        bk_graphics_icon_load("16/Runonce106", &st->button_icons[0]);
    st->button_icon_loaded[1] =
        bk_graphics_icon_load("16/Close", &st->button_icons[1]);
    runbox_copy(st->status, sizeof(st->status),
                "@H7F4FCECA");

    bk_gui_set_window_content(st->window, runbox_content, st);
    bk_gui_set_window_event_handler(st->window, runbox_event, st);
    st->window->owner_pid = bk_sys_getpid();
    bk_proc_bind_window(st->window);

    bk_gui_desktop_raise_window(st->desktop, st->window);
    bk_gui_focus_window(st->desktop, st->window);
    st->window->dirty = true;
    bk_gui_request_paint();

    while (!bk_proc_exit_requested() && st->window && st->window->listed) {
        bk_sys_sleep_ticks(20);
    }

    runbox_cleanup(st);
    bk_proc_exit();
}

void runbox_open_from_desktop(gui_desktop_t *desktop) {
    runbox_state_t *st;

    if (!desktop) return;

    if (g_runbox && g_runbox->window) {
        bk_gui_window_restore(g_runbox->window);
        bk_gui_desktop_raise_window(desktop, g_runbox->window);
        bk_gui_focus_window(desktop, g_runbox->window);
        g_runbox->window->dirty = true;
        return;
    }

    st = (runbox_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) return;
    st->desktop = desktop;
    g_runbox = st;

    if (bk_proc_spawn_thread("runbox", runbox_main, st) < 0) {
        g_runbox = NULL;
        bk_sys_free(st);
    }
}

bool runbox_get_runtime_info(program_runtime_info_t *info) {
    if (!info || !g_runbox) return false;
    info->window = g_runbox->window;
    info->memory_bytes = sizeof(*g_runbox);
    return true;
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    runbox_open_from_desktop(desktop);
}
