#include "sdk/include/bleskernos_theme.h"
#ifndef BLESKERNOS_CONTROL_COMMON_H
#define BLESKERNOS_CONTROL_COMMON_H

#include "kernel/include/api.h"
#include <stdio.h>

#define CPL_FACE BK_THEME_FACE
#define CPL_LIGHT BK_THEME_LIGHT
#define CPL_SHADOW BK_THEME_SHADOW
#define CPL_DARK BK_THEME_DARK
#define CPL_TEXT BK_THEME_TEXT
#define CPL_BLUE BK_THEME_SELECTION
#define CPL_WHITE BK_THEME_PANEL

typedef struct {
    const char *label;
    int width;
} cpl_tab_spec_t;

static inline void cpl_draw_bevel(gui_surface_t *surface, gui_rect_t rect,
                           uint32_t fill, bool sunken) {
    if (!surface || rect.w <= 0 || rect.h <= 0) return;
    uint32_t top = BK_THEME_BEVEL_OUTER_TL(sunken);
    uint32_t bottom = BK_THEME_BEVEL_OUTER_BR(sunken);
    bk_gui_gfx_fill_rect(surface, rect, fill);
    bk_gui_gfx_draw_line(surface, rect.x, rect.y, rect.x + rect.w - 1, rect.y, top);
    bk_gui_gfx_draw_line(surface, rect.x, rect.y, rect.x, rect.y + rect.h - 1, top);
    bk_gui_gfx_draw_line(surface, rect.x, rect.y + rect.h - 1,
                      rect.x + rect.w - 1, rect.y + rect.h - 1, bottom);
    bk_gui_gfx_draw_line(surface, rect.x + rect.w - 1, rect.y,
                      rect.x + rect.w - 1, rect.y + rect.h - 1, bottom);
    if (rect.w > 4 && rect.h > 4) {
        uint32_t inner_top = BK_THEME_BEVEL_INNER_TL(sunken);
        uint32_t inner_bottom = BK_THEME_BEVEL_INNER_BR(sunken);
        bk_gui_gfx_draw_line(surface, rect.x+1,rect.y+1,rect.x+rect.w-2,rect.y+1,inner_top);
        bk_gui_gfx_draw_line(surface, rect.x+1,rect.y+1,rect.x+1,rect.y+rect.h-2,inner_top);
        bk_gui_gfx_draw_line(surface, rect.x+1,rect.y+rect.h-2,rect.x+rect.w-2,rect.y+rect.h-2,inner_bottom);
        bk_gui_gfx_draw_line(surface, rect.x+rect.w-2,rect.y+1,rect.x+rect.w-2,rect.y+rect.h-2,inner_bottom);
    }
}

static inline void cpl_draw_group(gui_surface_t *surface, gui_rect_t rect,
                           const char *label) {
    bk_gui_gfx_draw_rect(surface, (gui_rect_t){rect.x+1,rect.y+1,rect.w,rect.h}, CPL_LIGHT);
    bk_gui_gfx_draw_rect(surface, rect, CPL_SHADOW);
    if (label) {
        bk_gui_gfx_fill_rect(surface,
            (gui_rect_t){rect.x + 8, rect.y - 4,
                         bk_gui_font_text_width(label) + 8, 10}, CPL_FACE);
        bk_gui_font_draw_string(surface, rect.x + 12, rect.y - 3, label,
                             CPL_TEXT, 0, false);
    }
}

/* Layout seguro para paneles de control.  Todo el texto estatico debe
 * dibujarse dentro de un rectangulo conocido: asi una traduccion larga nunca
 * invade botones, otros grupos o la barra de estado. */
static inline gui_rect_t cpl_inset(gui_rect_t rect, int inset) {
    if (inset < 0) inset = 0;
    rect.x += inset;
    rect.y += inset;
    rect.w -= inset * 2;
    rect.h -= inset * 2;
    if (rect.w < 0) rect.w = 0;
    if (rect.h < 0) rect.h = 0;
    return rect;
}

static inline gui_rect_t cpl_group_inner(gui_rect_t group) {
    group.x += 12;
    group.y += 14;
    group.w -= 24;
    group.h -= 22;
    if (group.w < 0) group.w = 0;
    if (group.h < 0) group.h = 0;
    return group;
}

static inline void cpl_draw_text_clipped(gui_surface_t *surface,
                                         gui_rect_t rect,
                                         const char *text,
                                         uint32_t color) {
    if (!surface || !text || rect.w <= 0 || rect.h <= 0) return;
    bk_gui_font_draw_string_clipped(surface, rect.x, rect.y, text, color, rect);
}

/* Word-wrap pequeno y determinista para la fuente de 8 px del escritorio.
 * Devuelve la coordenada Y inmediatamente posterior a la ultima linea. */
static inline int cpl_draw_text_wrapped(gui_surface_t *surface,
                                        gui_rect_t rect,
                                        const char *text,
                                        uint32_t color,
                                        int line_height,
                                        int max_lines) {
    char translated[384];
    char line[192];
    const char *src;
    int y;
    int lines = 0;

    if (!surface || !text || rect.w <= 0 || rect.h <= 0) return rect.y;
    if (line_height < 9) line_height = 12;
    src = bk_lang_translate(text);
    if (!src) src = text;
    bk_runtime_strncpy(translated, src, sizeof(translated) - 1);
    translated[sizeof(translated) - 1] = '\0';
    src = translated;
    y = rect.y;

    while (*src && y + 8 <= rect.y + rect.h &&
           (max_lines <= 0 || lines < max_lines)) {
        int used = 0;
        int last_space = -1;
        const char *start = src;

        while (*src && *src != '\n' && used + 1 < (int)sizeof(line)) {
            line[used] = *src;
            line[used + 1] = '\0';
            if (*src == ' ') last_space = used;
            if ((int)bk_gui_font_text_width(line) > rect.w) {
                if (last_space >= 0) {
                    used = last_space;
                    src = start + last_space + 1;
                }
                break;
            }
            used++;
            src++;
        }
        if (used == 0 && *src) {
            line[0] = *src++;
            used = 1;
        }
        line[used] = '\0';
        while (used > 0 && line[used - 1] == ' ') line[--used] = '\0';
        cpl_draw_text_clipped(surface,
            (gui_rect_t){rect.x, y, rect.w, 8}, line, color);
        lines++;
        y += line_height;
        while (*src == ' ') src++;
        if (*src == '\n') src++;
    }
    return y;
}

static inline gui_rect_t cpl_local_rect(gui_rect_t screen_rect,
                                        gui_rect_t client) {
    screen_rect.x -= client.x;
    screen_rect.y -= client.y;
    return screen_rect;
}

static inline gui_rect_t cpl_row_cell(gui_rect_t row, int index, int count,
                                      int gap) {
    int total_gap;
    int cell_w;
    if (count < 1) count = 1;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    if (gap < 0) gap = 0;
    total_gap = gap * (count - 1);
    cell_w = (row.w - total_gap) / count;
    if (cell_w < 1) cell_w = 1;
    return (gui_rect_t){row.x + index * (cell_w + gap), row.y, cell_w, row.h};
}

static inline gui_rect_t cpl_tab_rect(gui_rect_t client,
                                      const cpl_tab_spec_t *tabs,
                                      int index,
                                      int left,
                                      int top) {
    int x = client.x + left;

    if (!tabs || index < 0) return (gui_rect_t){0, 0, 0, 0};
    for (int i = 0; i < index; i++) x += tabs[i].width;
    return (gui_rect_t){x, client.y + top, tabs[index].width, BK_THEME_TAB_HEIGHT};
}

static inline int cpl_hit_tab(gui_rect_t client,
                              const cpl_tab_spec_t *tabs,
                              int count,
                              int left,
                              int top,
                              int x,
                              int y) {
    if (!tabs || count <= 0) return -1;
    for (int i = 0; i < count; i++) {
        if (bk_gui_rect_contains(cpl_tab_rect(client, tabs, i, left, top), x, y))
            return i;
    }
    return -1;
}

static inline void cpl_draw_tab_face(gui_surface_t *surface, gui_rect_t tab,
                                      const char *label, bool selected,
                                      gui_rect_t page) {
    bk_gui_surface_draw_tab(surface,tab,label,selected,page);
}

static inline void cpl_draw_tabs(gui_surface_t *surface,
                                 gui_rect_t client,
                                 const cpl_tab_spec_t *tabs,
                                 int count, int active, gui_rect_t page,
                                 int left, int top) {
    if (!surface || !tabs || count <= 0) return;
    for (int i = 0; i < count; ++i)
        if (i != active)
            cpl_draw_tab_face(surface, cpl_tab_rect(client,tabs,i,left,top),
                              tabs[i].label, false, page);
    cpl_draw_bevel(surface, page, CPL_FACE, false);
    if (active >= 0 && active < count)
        cpl_draw_tab_face(surface, cpl_tab_rect(client,tabs,active,left,top),
                          tabs[active].label, true, page);
}

static inline bool cpl_write_text(const char *path, const char *text) {
    return path && text && bk_file_write_all(path, text, (uint32_t)bk_runtime_strlen(text));
}

static inline void cpl_destroy_window(gui_desktop_t *desktop, gui_window_t *window) {
    if (!window) return;
    bk_gui_desktop_remove_window(desktop, window);
    bk_gui_window_destroy_raw(window);
    bk_proc_bind_window(NULL);
}

#endif
