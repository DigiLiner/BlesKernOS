#include "control_common.h"
typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_widget_t *ext, *open, *edit;
    bool save, load;
    char status[128];
} defaults_t;
static defaults_t *active;
static void save(gui_window_t *w UNUSED, uint32_t id UNUSED) {
    if (active)
        active->save = true;
}
static void load(gui_window_t *w UNUSED, uint32_t id UNUSED) {
    if (active)
        active->load = true;
}
static void paint(gui_window_t *w, gui_surface_t *s, void *context) {
    defaults_t *p = context;
    gui_rect_t c = bk_gui_window_content_rect_raw(w);
    const char *labels[] = {"Extension", "Abrir con", "Editar con"};
    for (int i = 0; i < 3; i++)
        cpl_draw_text_clipped(s, (gui_rect_t){c.x + 16, c.y + 20 + i * 46, 110, 18}, labels[i],
                              CPL_TEXT);
    cpl_draw_text_wrapped(s, (gui_rect_t){c.x + 16, c.y + 216, c.w - 32, 54},
                          "Use extensiones sin punto (TXT). Ejecutables con ruta absoluta. Se "
                          "aplica al volver a abrir el explorador.",
                          CPL_SHADOW, 16, 3);
    cpl_draw_text_wrapped(s, (gui_rect_t){c.x + 16, c.y + 286, c.w - 32, 55}, p->status, CPL_TEXT,
                          16, 3);
}
static bool extension(char *s) {
    if (!s[0])
        return false;
    for (char *p = s; *p; p++) {
        if (*p >= 'a' && *p <= 'z')
            *p -= 32;
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')))
            return false;
    }
    return true;
}
static void modify(defaults_t *p) {
    char ext[16], app[160], edit[160];
    bk_gui_widget_get_text(p->ext, ext, 16);
    bk_gui_widget_get_text(p->open, app, 160);
    bk_gui_widget_get_text(p->edit, edit, 160);
    if (!extension(ext)) {
        bk_runtime_strcpy(p->status, "Extension invalida.");
        return;
    }
    void *raw = NULL;
    uint32_t size = 0;
    (void)bk_file_read_all("/ASSOC.INI", &raw, &size);
    if (size > 32768) {
        bk_sys_free(raw);
        bk_runtime_strcpy(p->status, "Archivo de asociaciones demasiado grande.");
        return;
    }
    char *output = bk_sys_alloc(size + 512);
    if (!output) {
        bk_sys_free(raw);
        return;
    }
    uint32_t used = 0, pos = 0;
    char icon[160] = "/ICONS/OBJECT.BMP", current_open[160] = "", current_edit[160] = "";
    while (pos < size) {
        uint32_t begin = pos;
        while (pos < size && ((char *)raw)[pos] != '\n')
            pos++;
        uint32_t len = pos - begin;
        if (pos < size)
            pos++;
        char *line = (char *)raw + begin;
        bool match = len > bk_runtime_strlen(ext) &&
                     !bk_runtime_strncmp(line, ext, bk_runtime_strlen(ext)) &&
                     line[bk_runtime_strlen(ext)] == '=';
        if (match) {
            char value[512];
            uint32_t n = len - bk_runtime_strlen(ext) - 1;
            if (n >= sizeof(value))
                n = sizeof(value) - 1;
            bk_runtime_memcpy(value, line + bk_runtime_strlen(ext) + 1, n);
            value[n] = 0;
            while (n && (value[n - 1] == '\r' || value[n - 1] == '\n'))
                value[--n] = 0;
            char *parts[3] = {value, NULL, NULL};
            unsigned count = 1;
            for (char *q = value; *q; q++)
                if (*q == '|' && count < 3) {
                    *q = 0;
                    parts[count++] = q + 1;
                }
            bk_runtime_strncpy(icon, parts[0], 159);
            icon[159] = 0;
            if (parts[1]) {
                bk_runtime_strncpy(current_open, parts[1], 159);
                current_open[159] = 0;
            }
            if (parts[2]) {
                bk_runtime_strncpy(current_edit, parts[2], 159);
                current_edit[159] = 0;
            }
        } else {
            bk_runtime_memcpy(output + used, line, len);
            used += len;
            output[used++] = '\n';
        }
    }
    if (p->load) {
        bk_gui_widget_set_text(p->open, current_open);
        bk_gui_widget_set_text(p->edit, current_edit);
        bk_runtime_strcpy(p->status, "Asociacion cargada.");
    } else {
        bool valid = app[0] == '/' && (!edit[0] || edit[0] == '/');
        for (char *q = app; *q; q++)
            if (*q == '|' || *q == '\n' || *q == '\r')
                valid = false;
        for (char *q = edit; *q; q++)
            if (*q == '|' || *q == '\n' || *q == '\r')
                valid = false;
        vfs_dir_entry_t stat;
        if (!bk_file_stat(app, &stat) || stat.type != VFS_NODE_FILE)
            valid = false;
        if (edit[0] && (!bk_file_stat(edit, &stat) || stat.type != VFS_NODE_FILE))
            valid = false;
        if (valid) {
            int n =
                snprintf(output + used, size + 512 - used, "%s=%s|%s|%s\n", ext, icon, app, edit);
            used += n;
            valid = bk_file_write_all("/ASSOC.INI.NEW", output, used) &&
                    bk_file_rename("/ASSOC.INI.NEW", "/ASSOC.INI");
        }
        bk_runtime_strcpy(p->status, valid
                                         ? "Guardado. Vuelva a abrir el explorador."
                                         : "No se pudo guardar: revise las rutas de aplicaciones.");
    }
    bk_sys_free(output);
    bk_sys_free(raw);
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    defaults_t *p = bk_sys_alloc_zero(sizeof(*p));
    if (!p)
        return;
    active = p;
    p->desktop = desktop;
    p->window = bk_gui_create_window(desktop, 90, 70, 590, 380, "Aplicaciones predeterminadas");
    if (!p->window) {
        active = NULL;
        bk_sys_free(p);
        return;
    }
    bk_proc_bind_window(p->window);
    bk_gui_set_window_min_size(p->window, 590, 380);
    bk_gui_window_set_compact_content(p->window,true);
    bk_gui_set_window_content(p->window, paint, p);
    p->ext =
        bk_gui_create_textbox(desktop, p->window, (gui_rect_t){130, 14, 120, 26}, "TXT", 15, NULL);
    p->open =
        bk_gui_create_textbox(desktop, p->window, (gui_rect_t){130, 60, 430, 26}, "", 159, NULL);
    p->edit =
        bk_gui_create_textbox(desktop, p->window, (gui_rect_t){130, 106, 430, 26}, "", 159, NULL);
    bk_gui_create_button(desktop, p->window, (gui_rect_t){16, 164, 120, 25}, "Cargar", load);
    bk_gui_create_button(desktop, p->window, (gui_rect_t){146, 164, 130, 25}, "Guardar", save);
    p->load = true;
    while (!bk_proc_exit_requested() && p->window->listed) {
        if (p->save || p->load) {
            modify(p);
            p->save = p->load = false;
            bk_gui_window_invalidate(p->window);
        }
        bk_sys_sleep_ms(20);
    }
    active = NULL;
    bk_gui_desktop_remove_window(desktop, p->window);
    bk_gui_window_destroy_raw(p->window);
    bk_sys_free(p);
}
