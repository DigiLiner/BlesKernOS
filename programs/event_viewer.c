#include "system_tools_common.h"
#define EV_BLACK 0x00000000U
#define EV_TEXT 0x00FFFFFFU
static const char *sources[] = {"Kernel", "Memoria",   "Drivers",      "Discos",
                                "Red",    "Servicios", "Aplicaciones", "Todos"};
static const char *levels[] = {"Info", "Aviso", "Error", "Todos"};
typedef struct {
    bk_gui_desktop_t *desktop;
    bk_gui_window_t *window;
    bk_gui_widget_t *search;
    bk_gui_window_t *search_window;
    char query[96];
    uint32_t visual_count, layout_columns;
    uint16_t visual_event[BK_EVENT_CAPACITY * 8], visual_part[BK_EVENT_CAPACITY * 8];
    uint32_t source, level, offset, count, selected, filter_pid, last_refresh, notice_time;
    bool pending_export, pending_copy;
    bk_gui_scrollbar_drag_t drag;
    bk_gui_rect_t client;
    bk_event_info_t rows[BK_EVENT_CAPACITY];
    char status[128];
} event_state_t;
static event_state_t *state;
static void draw_line(bk_gui_surface_t *surface, bk_gui_rect_t rect, const char *text,
                      uint32_t color) {
    char line[320];
    /* The classic GUI font advances seven pixels per character. */
    uint32_t advance = 7;
    uint32_t limit = advance ? (uint32_t)rect.w / advance : 0;
    if (limit >= sizeof(line))
        limit = sizeof(line) - 1;
    st_copy(line, limit + 1, text);
    bk_gui_surface_draw_text(surface, rect.x, rect.y, line, color, 0, false);
}
static bk_gui_rect_t log_rect(bk_gui_rect_t c) {
    return (bk_gui_rect_t){c.x+2,c.y+2,c.w-4,c.h-4};
}
static uint32_t visible_rows(bk_gui_rect_t r) {
    int h=r.h-8;
    return h>18 ? (uint32_t)(h/18) : 1;
}
static uint32_t line_columns(event_state_t *s) {
    int width=log_rect(s->client).w-42;
    return width>7 ? (uint32_t)(width/7) : 1;
}
static void event_line(bk_event_info_t *e, char *line, uint32_t capacity);
static void reflow(event_state_t *s) {
    s->visual_count=0;
    uint32_t columns=line_columns(s);
    s->layout_columns=columns;
    for (uint32_t i=0;i<s->count;i++) {
        char line[320];
        event_line(&s->rows[i],line,sizeof(line));
        uint32_t parts=(st_length(line)+columns-1)/columns;
        if (!parts) parts=1;
        for (uint32_t part=0;part<parts && s->visual_count<BK_EVENT_CAPACITY*8;part++) {
            s->visual_event[s->visual_count]=i;
            s->visual_part[s->visual_count++]=part;
        }
    }
}
static void refresh(event_state_t *s) {
    const char *query=s->query;
    uint32_t anchor = s->offset && s->offset < s->visual_count
        ? s->rows[s->visual_event[s->offset]].sequence : 0;
    uint32_t part=s->offset<s->visual_count ? s->visual_part[s->offset] : 0;
    s->count = 0;
    uint32_t count = bk_event_count();
    for (uint32_t i = 0; i < count; i++) {
        bk_event_info_t e;
        if (!bk_event_get(i, &e))
            continue;
        if (s->source < 7 && e.source != s->source)
            continue;
        if (s->level < 3 && e.severity != s->level)
            continue;
        if (s->filter_pid && e.pid != s->filter_pid)
            continue;
        if (!st_contains_ci(e.message, query) && !st_contains_ci(e.code, query))
            continue;
        s->rows[s->count++] = e;
    }
    reflow(s);
    if (anchor)
        for (uint32_t i=0;i<s->visual_count;i++)
            if (s->rows[s->visual_event[i]].sequence==anchor && s->visual_part[i]==part) {
                s->offset=i;
                break;
            }
    uint32_t visible=visible_rows(log_rect(s->client));
    uint32_t max_offset=s->visual_count>visible ? s->visual_count-visible : 0;
    if (s->offset>max_offset) s->offset=max_offset;
}

static void action(bk_gui_window_t *w, uint32_t id) {
    (void)id;
    event_state_t *s=state;
    if (!s) return;
    bk_gui_widget_get_text(s->search,s->query,sizeof(s->query));
    s->offset=0;
    refresh(s);
    bk_gui_close_window(w);
    bk_gui_window_invalidate(s->window);
}
static void open_search(event_state_t *s) {
    if (s->search_window && bk_gui_window_is_open(s->search_window)) return;
    if (s->search_window) {
        bk_gui_destroy_window(s->desktop,s->search_window);
        s->search_window=NULL;
    }
    s->search_window=bk_gui_create_window(s->desktop,100,110,390,100,"Buscar en eventos");
    if (!s->search_window) return;
    s->search=bk_gui_create_textbox(s->desktop,s->search_window,
        (bk_gui_rect_t){10,10,270,24},s->query,95,NULL);
    bk_gui_create_button(s->desktop,s->search_window,(bk_gui_rect_t){288,10,80,24},"Buscar",action);
}
static void menu_action(bk_gui_window_t *w, uint32_t id, void *context) {
    event_state_t *s = context;
    if (id == 1)
        s->pending_export = true;
    else if (id == 2)
        bk_gui_close_window(w);
    else if (id == 3)
        s->pending_copy = true;
    else if (id == 4) {
        s->offset = 0;
        refresh(s);
    } else if (id == 5) {
        s->query[0]=0;
        s->offset = 0;
        refresh(s);
    } else if (id == 7)
        open_search(s);
    else if (id >= 100 && id < 108) {
        s->source=id-100; s->offset=0; refresh(s);
    } else if (id >= 200 && id < 204) {
        s->level=id-200; s->offset=0; refresh(s);
    } else if (id == 6)
        st_copy(s->status, sizeof(s->status),
                "Rojo: error | Amarillo: advertencia | Blanco: informacion. Rueda o scrollbar para "
                "recorrer.");
    s->notice_time=bk_sys_uptime_ms();
    bk_gui_window_invalidate(w);
}
static void scroll_bar(event_state_t *s, bk_gui_scrollbar_t *bar, bk_gui_rect_t r) {
    bk_gui_scrollbar_init_vertical(bar, (bk_gui_rect_t){r.x + r.w - 18, r.y + 2, 16, r.h - 4},
                                   s->offset, visible_rows(r), s->visual_count);
}
static void event_line(bk_event_info_t *e, char *line, uint32_t capacity) {
    char num[16];
    st_u32(line, capacity, e->uptime_ms / 1000);
    st_append(line, capacity, "s [");
    st_append(line, capacity, sources[e->source < 7 ? e->source : 0]);
    st_append(line, capacity, "] PID ");
    st_u32(num, sizeof(num), e->pid);
    st_append(line, capacity, num);
    st_append(line, capacity, " ");
    st_append(line, capacity, e->code);
    st_append(line, capacity, ": ");
    st_append(line, capacity, e->message);
}
static void dot(bk_gui_surface_t *surface, int x, int y, uint32_t color) {
    for (int dy = -3; dy <= 3; dy++) {
        int radius = (dy == 3 || dy == -3) ? 1 : (dy == 2 || dy == -2) ? 2 : 3;
        bk_gui_surface_fill_rect(surface, (bk_gui_rect_t){x - radius, y + dy, radius * 2 + 1, 1},
                                 color);
    }
}
static void paint(bk_gui_window_t *w, bk_gui_surface_t *surface, void *context) {
    event_state_t *s = context;
    (void)w;
    bk_gui_rect_t c = s->client;
    bk_gui_surface_fill_rect(surface, c, EV_BLACK);
    bk_gui_rect_t r = log_rect(c);
    st_draw_panel(surface, r, EV_BLACK);
    for (uint32_t row = 0; row < visible_rows(r) && s->offset + row < s->visual_count; row++) {
        uint32_t at=s->offset+row;
        bk_event_info_t *e = &s->rows[s->visual_event[at]];
        int y = r.y + 5 + row * 18;
        char line[320];
        event_line(e, line, sizeof(line));
        if (e->sequence == s->selected)
            bk_gui_surface_fill_rect(surface, (bk_gui_rect_t){r.x + 2, y - 2, r.w - 22, 18},
                                     0x00282828);
        if (!s->visual_part[at]) dot(surface, r.x + 10, y + 5,
            e->severity == 2   ? 0x00FF3030
            : e->severity == 1 ? 0x00FFFF00
                               : EV_TEXT);
        uint32_t start=s->visual_part[at]*s->layout_columns;
        draw_line(surface, (bk_gui_rect_t){r.x + 20, y, r.w - 42, 16},
                  start<st_length(line) ? line+start : "", EV_TEXT);
    }
    if (!s->count)
        draw_line(surface, (bk_gui_rect_t){r.x + 12, r.y + 12, r.w - 34, 16},
                  s->filter_pid ? "No hay eventos de este proceso."
                                : "No hay eventos que coincidan con los filtros.",
                  EV_TEXT);
    bk_gui_scrollbar_t bar;
    scroll_bar(s, &bar, r);
    bk_gui_scrollbar_paint_vertical(surface, &bar);
    if (s->status[0])
        draw_line(surface,(bk_gui_rect_t){r.x+8,r.y+r.h-18,r.w-32,16},s->status,0x00AAAAAA);

}
static bool event(bk_gui_window_t *w, const bk_gui_event_t *e, void *context) {
    event_state_t *s = context;
    bk_gui_rect_t c;
    bk_gui_window_content_rect(w, &c);
    bk_gui_rect_t r = log_rect(c);
    bk_gui_scrollbar_t bar;
    scroll_bar(s, &bar, r);
    uint32_t value;
    if (bk_gui_scrollbar_handle_event_vertical(&bar, &s->drag, e, 3, &value)) {
        s->offset = value;
        bk_gui_window_invalidate(w);
        return true;
    }
    if (e->type != BK_GUI_EVENT_MOUSE_DOWN || e->button != 1 ||
        !st_rect_contains((bk_gui_rect_t){r.x + 2, r.y + 3, r.w - 22, (int)visible_rows(r) * 18},
                          e->x, e->y))
        return false;
    uint32_t row = (e->y - r.y - 3) / 18;
    if (s->offset + row < s->visual_count) {
        s->selected = s->rows[s->visual_event[s->offset + row]].sequence;
        bk_gui_window_invalidate(w);
        return true;
    }
    return false;
}
void bleskernos_program_main(bk_gui_desktop_t *desktop) {
    if (bk_sys_api_version() < 35)
        return;
    event_state_t *s = bk_sys_alloc(sizeof(*s));
    if (!s)
        return;
    st_zero(s, sizeof(*s));
    state = s;
    s->desktop = desktop;
    s->source = 7;
    s->level = 3;
    char arg[128] = "", initial_query[96] = "";
    if (bk_proc_launch_arg_copy(arg, sizeof(arg))) {
        const char *p = arg;
        if (!st_length(arg))
            p = "";
        if (st_length(arg) > 6 && arg[0] == '-' && arg[1] == '-' && arg[2] == 'p' &&
            arg[3] == 'i' && arg[4] == 'd' && arg[5] == ' ') {
            p = arg + 6;
            while (*p >= '0' && *p <= '9') {
                s->filter_pid = s->filter_pid * 10 + *p++ - '0';
            }
        } else if (st_length(arg) >= 10 && arg[0] == '-' && arg[1] == '-' && arg[2] == 's' &&
                   arg[3] == 'o' && arg[4] == 'u' && arg[5] == 'r' && arg[6] == 'c' &&
                   arg[7] == 'e' && arg[8] == ' ') {
            p = arg + 9;
            if (*p >= '0' && *p <= '6')
                s->source = *p - '0';
        }
    }
    for (uint32_t i = 0; i + 8 <= st_length(arg); i++) {
        const char *p = arg + i;
        if (p[0] == '-' && p[1] == '-' && p[2] == 'q' && p[3] == 'u' && p[4] == 'e' &&
            p[5] == 'r' && p[6] == 'y' && p[7] == ' ') {
            st_copy(initial_query, sizeof(initial_query), p + 8);
            break;
        }
    }
    bk_memory_detail_t memory;
    bool small = bk_memory_detail(&memory) && memory.total < 96U * 1024U * 1024U;
    st_copy(s->query,sizeof(s->query),initial_query);
    char title[64]="Visor de eventos";
    if (s->filter_pid) {
        char pid[16]; st_u32(pid,sizeof(pid),s->filter_pid);
        st_append(title,sizeof(title)," - PID "); st_append(title,sizeof(title),pid);
    }
    s->window = bk_gui_create_window(desktop, 45, 40, small ? 660 : 700, small ? 450 : 485,
                                     title);
    if (!s->window) {
        bk_sys_free(s);
        state = NULL;
        return;
    }
    bk_proc_bind_window(s->window);
    bk_gui_set_window_min_size(s->window, 650, 430);
    int file = bk_gui_add_menu(s->window, "File"), edit = bk_gui_add_menu(s->window, "Edit"),
        help = bk_gui_add_menu(s->window, "Help");
    bk_gui_add_menu_item(s->window, file, 1, "Exportar todos", menu_action, s);
    bk_gui_add_menu_item(s->window, file, 4, "Eventos recientes", menu_action, s);
    bk_gui_add_menu_item(s->window, file, 2, "Cerrar", menu_action, s);
    bk_gui_add_menu_item(s->window, edit, 3, "Copiar evento", menu_action, s);
    bk_gui_add_menu_item(s->window, edit, 7, "Buscar...", menu_action, s);
    bk_gui_add_menu_item(s->window, edit, 5, "Limpiar busqueda", menu_action, s);
    bk_gui_add_menu_item(s->window, help, 6, "Como leer los eventos", menu_action, s);
    int origin=bk_gui_add_menu(s->window,"Origen"), level=bk_gui_add_menu(s->window,"Nivel");
    for (uint32_t i=0;i<8;i++) bk_gui_add_menu_item(s->window,origin,100+i,sources[i],menu_action,s);
    for (uint32_t i=0;i<4;i++) bk_gui_add_menu_item(s->window,level,200+i,levels[i],menu_action,s);
    bk_gui_window_content_rect(s->window, &s->client);
    bk_gui_window_set_compact_content(s->window, true);
    bk_gui_set_window_content(s->window, paint, s);
    bk_gui_set_window_event_handler(s->window, event, s);
    refresh(s);
    while (!bk_proc_exit_requested() && bk_gui_window_is_open(s->window)) {
        bk_gui_window_content_rect(s->window, &s->client);
        if (s->pending_export) {
            s->pending_export = false;
            s->notice_time=bk_sys_uptime_ms();
            st_copy(s->status, sizeof(s->status),
                    bk_event_export() ? "Exportacion solicitada: /SYSTEM/LOGS/EVENTS-EXPORT.TXT"
                                      : "No se pudo solicitar la exportacion");
        }
        if (s->pending_copy) {
            s->pending_copy = false;
            s->notice_time=bk_sys_uptime_ms();
            for (uint32_t i = 0; i < s->count; i++)
                if (s->rows[i].sequence == s->selected) {
                    char details[320];
                    st_copy(details, sizeof(details), s->rows[i].code);
                    st_append(details, sizeof(details), "\n");
                    st_append(details, sizeof(details), s->rows[i].message);
                    bk_clipboard_set_text(details);
                    st_copy(s->status, sizeof(s->status), "Evento copiado al portapapeles");
                    break;
                }
        }
        if (bk_sys_uptime_ms() - s->last_refresh > 500) {
            s->last_refresh = bk_sys_uptime_ms();
            if (s->status[0] && bk_sys_uptime_ms()-s->notice_time>5000) s->status[0]=0;
            refresh(s);
            bk_gui_window_invalidate(s->window);
        }
        bk_sys_sleep_ms(20);
    }
    state = NULL;
    if (s->search_window) bk_gui_destroy_window(desktop,s->search_window);
    bk_gui_destroy_window(desktop, s->window);
    bk_sys_free(s);
}
