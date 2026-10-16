#include "../system/control/control_common.h"
#define PM_HISTORY 36
#define PM_MAX_TASKS 128
#define PM_MAX_CPUS 16
static const cpl_tab_spec_t tabs[] = {
    {"Procesos", 110}, {"Rendimiento", 130}, {"Servicios", 110}, {"Inicio", 100}};
static const char *states[] = {"Detenido", "Iniciando", "Ejecutandose", "Deteniendose", "Fallido"};
static const char *phases[] = {"Sistema", "Escritorio", "Diferido"};
typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window, *editor;
    uint8_t cpu[PM_HISTORY], cpu_core[PM_MAX_CPUS][PM_HISTORY], ram[PM_HISTORY];
    uint32_t cpu_count, last_sample, last_refresh, selected_pid, selected_id, scroll_offset, tab,
        expanded;
    bk_proc_info_t rows[PM_MAX_TASKS], previous[PM_MAX_TASKS];
    uint32_t count, previous_count;
    uint32_t cpu_percent[PM_MAX_TASKS], threads[PM_MAX_TASKS];
    bool thread_row[PM_MAX_TASKS];
    bk_startup_info_t startup[BK_ADMIN_MAX], editing;
    uint32_t startup_count;
    uint32_t pending_action, pending_id, pending_pid, pending_edit_id, pending_toggle_id,
        pending_remove_id;
    uint32_t menu_pid, menu_id, menu_tab, events_pid, events_id, events_tab;
    bool pending_save, pending_toggle, pending_remove, pending_edit, pending_events;
    gui_widget_t *fields[7], *phase_button, *kind_button;
    uint32_t phase, service;
    char status[128];
    gui_scrollbar_drag_t drag;
    gui_rect_t client_rect;
    volatile uint32_t closing;
} processmanager_state_t;
static processmanager_state_t *g_processmanager;
static uint8_t pm_percent(uint32_t used, uint32_t total) {
    uint32_t step;
    uint32_t percent;

    if (!total)
        return 0;
    if (used >= total)
        return 100;
    if (total < 100U)
        return (uint8_t)((used * 100U) / total);

    step = (total + 99U) / 100U;
    percent = used / step;
    if (percent > 100U)
        percent = 100U;
    return (uint8_t)percent;
}

static uint32_t pm_cpu_count(void) {
    uint32_t count = bk_proc_cpu_count();
    if (!count)
        count = 1U;
    if (count > PM_MAX_CPUS)
        count = PM_MAX_CPUS;
    return count;
}

static void pm_history_shift(uint8_t values[PM_HISTORY]) {
    for (int i = 0; i < PM_HISTORY - 1; i++)
        values[i] = values[i + 1];
}

static void pm_sample(processmanager_state_t *st) {
    uint32_t now = bk_sys_ticks();
    uint32_t interval = bk_sys_tick_frequency() / 4U;
    system_memory_info_t info;

    if (!interval)
        interval = 1;
    if (st->last_sample && now - st->last_sample < interval)
        return;

    st->cpu_count = pm_cpu_count();
    mm_get_system_info(&info);
    pm_history_shift(st->cpu);
    pm_history_shift(st->ram);
    for (uint32_t cpu = 0U; cpu < PM_MAX_CPUS; cpu++) {
        pm_history_shift(st->cpu_core[cpu]);
        st->cpu_core[cpu][PM_HISTORY - 1] =
            cpu < st->cpu_count ? (uint8_t)bk_proc_cpu_usage_core(cpu) : 0U;
    }
    st->cpu[PM_HISTORY - 1] = (uint8_t)bk_proc_cpu_usage();
    /* Plot pressure against allocatable RAM, not all detected E820 RAM.
     * Regions above the identity mapping limit are unavailable, not used. */
    uint32_t managed = info.total_bytes >= info.reserved_bytes
        ? (uint32_t)(info.total_bytes - info.reserved_bytes) : 0U;
    st->ram[PM_HISTORY - 1] = pm_percent((uint32_t)info.used_bytes, managed);
    st->last_sample = now;
}

static void pm_graph(gui_surface_t *surface, gui_rect_t r, const uint8_t *values, uint32_t color) {
    if (r.w < 4 || r.h < 4)
        return;
    cpl_draw_bevel(surface, r, 0x00000000, true);
    r=cpl_inset(r,2);
    bk_gui_gfx_draw_rect(surface, r, 0x00008000);
    for (int line = 1; line < 4; line++) {
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){r.x + 1, r.y + line * r.h / 4, r.w - 2, 1},
                             0x00003800);
    }
    {
        gui_point_t points[PM_HISTORY];
        for (int i = 0; i < PM_HISTORY; i++) {
            points[i].x = r.x + 2 + i * (r.w - 4) / (PM_HISTORY - 1);
            points[i].y = r.y + r.h - 2 - values[i] * (r.h - 4) / 100;
        }
        bk_gui_surface_draw_polyline(surface, points, PM_HISTORY, color);
    }
}

static void pm_draw_button(gui_surface_t *surface, gui_rect_t rect, const char *label, bool enabled,
                           bool pressed, const gui_image_t *icon) {
    uint32_t border = enabled ? 0x00404040 : 0x00707070;
    uint32_t face = enabled ? (pressed ? 0x00B8B8B0 : 0x00D0D0C8) : 0x00C8C8C0;
    gui_rect_t inner = {rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2};

    bk_gui_gfx_fill_rect(surface, rect, border);
    bk_gui_gfx_fill_rect(surface, inner, face);
    if (pressed) {
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){inner.x, inner.y, inner.w, 1}, 0x00606060);
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){inner.x, inner.y, 1, inner.h}, 0x00606060);
    } else {
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){inner.x, inner.y, inner.w, 1}, 0x00FFFFFF);
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){inner.x, inner.y, 1, inner.h}, 0x00FFFFFF);
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){inner.x, inner.y + inner.h - 1, inner.w, 1},
                             0x00707070);
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){inner.x + inner.w - 1, inner.y, 1, inner.h},
                             0x00707070);
    }

    int text_x;
    int group_width = (int)bk_gui_font_text_width(label);
    if (icon && icon->pixels)
        group_width += 20;
    text_x = rect.x + (rect.w - group_width) / 2;
    if (text_x < rect.x + 3)
        text_x = rect.x + 3;
    if (icon && icon->pixels) {
        bk_gui_surface_draw_image(
            surface,
            (gui_rect_t){text_x + (pressed ? 1 : 0), rect.y + 2 + (pressed ? 1 : 0), 16, 16}, rect,
            icon);
        text_x += 20;
    }
    bk_gui_font_draw_string_clipped(surface, text_x, rect.y + 5, label,
                                    enabled ? 0x00243B4E : 0x00607070,
                                    (gui_rect_t){rect.x + 3, rect.y + 2, rect.w - 6, rect.h - 4});
}

static void label(gui_surface_t *s, gui_rect_t r, const char *text, uint32_t color) {
    bk_gui_font_draw_string_clipped(s, r.x, r.y, text, color, r);
}
static gui_rect_t client(processmanager_state_t *st) { return st->client_rect; }
static uint32_t visible_rows(processmanager_state_t *st) {
    int h = client(st).h - 166;
    return h > 18 ? (uint32_t)(h / 18) : 1;
}
static void list_bar(processmanager_state_t *st, gui_scrollbar_t *bar) {
    gui_rect_t c = client(st);
    uint32_t n = st->tab == 0 ? st->count : 0;
    if (st->tab >= 2)
        for (uint32_t i = 0; i < st->startup_count; i++)
            if (st->startup[i].name[0] && (st->tab == 3 || st->startup[i].service))
                n++;
    bk_gui_scrollbar_init_vertical(
        bar, (gui_rect_t){c.x + c.w - 32, c.y + 68, 16, (int)visible_rows(st) * 18},
        st->scroll_offset, visible_rows(st), n);
}
static uint32_t proc_id(const bk_proc_info_t *t) { return t->process_id ? t->process_id : t->pid; }
static void collect(processmanager_state_t *st) {
    bk_proc_info_t raw[PM_MAX_TASKS];
    uint32_t n = bk_proc_snapshot(raw, PM_MAX_TASKS);
    st->count = 0;
    uint32_t interval = bk_sys_ticks() - st->last_refresh;
    if (!interval)
        interval = 1;
    for (uint32_t i = 0; i < n && st->count < PM_MAX_TASKS; i++) {
        uint32_t pid = proc_id(&raw[i]);
        bool duplicate = false;
        for (uint32_t j = 0; j < i; j++)
            if (proc_id(&raw[j]) == pid)
                duplicate = true;
        if (duplicate)
            continue;
        uint32_t at = st->count++;
        st->rows[at] = raw[i];
        st->rows[at].pid = pid;
        st->threads[at] = 0;
        st->thread_row[at] = false;
        st->cpu_percent[at] = 0;
        uint32_t delta = 0;
        for (uint32_t t = 0; t < n; t++)
            if (proc_id(&raw[t]) == pid) {
                st->threads[at]++;
                if (raw[t].memory_bytes > st->rows[at].memory_bytes)
                    st->rows[at].memory_bytes = raw[t].memory_bytes;
                for (uint32_t p = 0; p < st->previous_count; p++)
                    if (st->previous[p].pid == raw[t].pid)
                        delta += raw[t].cpu_ticks - st->previous[p].cpu_ticks;
            }
        st->cpu_percent[at] = (uint32_t)((uint64_t)delta * 100U / interval);
        if (st->expanded == pid)
            for (uint32_t t = 0; t < n && st->count < PM_MAX_TASKS; t++)
                if (proc_id(&raw[t]) == pid) {
                    uint32_t r = st->count++;
                    st->rows[r] = raw[t];
                    st->thread_row[r] = true;
                    st->threads[r] = 0;
                    uint32_t delta = 0;
                    for (uint32_t p = 0; p < st->previous_count; p++)
                        if (st->previous[p].pid == raw[t].pid)
                            delta = raw[t].cpu_ticks - st->previous[p].cpu_ticks;
                    st->cpu_percent[r] = (uint32_t)((uint64_t)delta * 100U / interval);
                }
    }
    bk_runtime_memcpy(st->previous, raw, n * sizeof(*raw));
    st->previous_count = n;
    st->startup_count = bk_startup_count();
    if (st->startup_count > BK_ADMIN_MAX)
        st->startup_count = BK_ADMIN_MAX;
    for (uint32_t i = 0; i < st->startup_count; i++)
        bk_startup_get(i, &st->startup[i]);
}
static bk_startup_info_t *entry_by_id(processmanager_state_t *st, uint32_t id) {
    for (uint32_t i = 0; i < st->startup_count; i++)
        if (st->startup[i].id == id)
            return &st->startup[i];
    return NULL;
}
static bk_startup_info_t *selected_entry(processmanager_state_t *st) {
    return entry_by_id(st, st->selected_id);
}
static uint32_t service_count(processmanager_state_t *st) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < st->startup_count; i++)
        if (st->startup[i].name[0] && (st->tab == 3 || st->startup[i].service))
            n++;
    return n;
}
static bk_startup_info_t *service_at(processmanager_state_t *st, uint32_t at) {
    for (uint32_t i = 0; i < st->startup_count; i++)
        if (st->startup[i].name[0] && (st->tab == 3 || st->startup[i].service)) {
            if (!at--)
                return &st->startup[i];
        }
    return NULL;
}
/* Every processor stays in the upper half, including the 16-core layout. */
static gui_rect_t cpu_cell(gui_rect_t upper, uint32_t count, uint32_t index) {
    uint32_t cols = 1;
    while (cols * cols < count)
        cols++;
    uint32_t rows = (count + cols - 1) / cols;
    int w = (upper.w - 16) / (int)cols, h = (upper.h - 16) / (int)rows;
    return (gui_rect_t){upper.x + 8 + (int)(index % cols) * w,
                        upper.y + 10 + (int)(index / cols) * h, w - 8, h - 6};
}
/* Classic monitor frames: shadow edge followed by a white highlight. */
static void monitor_group(gui_surface_t *s, gui_rect_t r, const char *title) {
    bk_gui_gfx_draw_rect(s, (gui_rect_t){r.x+1,r.y+1,r.w-1,r.h-1}, CPL_LIGHT);
    bk_gui_gfx_draw_rect(s, (gui_rect_t){r.x,r.y,r.w-1,r.h-1}, CPL_SHADOW);
    int width = bk_gui_font_text_width(title) + 8;
    bk_gui_gfx_fill_rect(s, (gui_rect_t){r.x+8,r.y-4,width,12}, CPL_FACE);
    label(s, (gui_rect_t){r.x+12,r.y-3,width,14}, title, CPL_TEXT);
}
static void monitor_headers(gui_surface_t *s, gui_rect_t c, uint32_t tab) {
    const char *process[] = {"PID", "Proceso / hilo", "CPU", "RAM", "Hilos"};
    const char *service[] = {"Servicio", "Estado", "PID", "Dependencias"};
    const char *startup[] = {"Nombre", "Fase", "Ord.", "Habilitado", "Ultimo arranque"};
    const int process_width[] = {42,203,35,98};
    const int service_width[] = {147,126,49};
    const int startup_width[] = {154,77,35,84};
    const char **names = tab==0 ? process : tab==2 ? service : startup;
    const int *widths = tab==0 ? process_width : tab==2 ? service_width : startup_width;
    int count = tab==2 ? 4 : 5, x=c.x+20, right=c.x+c.w-32;
    for (int i=0;i<count;i++) {
        int width = i==count-1 ? right-x : widths[i];
        cpl_draw_bevel(s, (gui_rect_t){x,c.y+44,width,22}, CPL_FACE, false);
        label(s, (gui_rect_t){x+4,c.y+50,width-8,14}, names[i], CPL_TEXT);
        x+=width;
    }
}
static void paint(gui_window_t *window UNUSED, gui_surface_t *s, void *context) {
    processmanager_state_t *st = context;
    if (!st || st->closing)
        return;
    gui_rect_t c = client(st);
    cpl_draw_bevel(s, c, CPL_FACE, false);
    gui_rect_t page = {c.x + 8, c.y + 30, c.w - 16, c.h - (st->tab == 1 ? 66 : 92)};
    cpl_draw_tabs(s, c, tabs, 4, st->tab, page, 16, 6);
    if (st->tab == 1) {
        char text[192];
        bk_memory_detail_t m;
        bk_memory_detail(&m);
        int half = c.y + c.h / 2;
        gui_rect_t upper = {c.x + 18, c.y + 48, c.w - 36, half - c.y - 56};
        monitor_group(s, upper, "Uso de CPU");
        for (uint32_t cpu = 0; cpu < st->cpu_count; cpu++) {
            gui_rect_t cell = cpu_cell(upper, st->cpu_count, cpu);
            snprintf(text, sizeof(text), "CPU %u: %u%%", cpu + 1,
                     st->cpu_core[cpu][PM_HISTORY - 1]);
            label(s, (gui_rect_t){cell.x, cell.y, cell.w, 14}, text, CPL_TEXT);
            pm_graph(s, (gui_rect_t){cell.x, cell.y + 16, cell.w, cell.h - 16}, st->cpu_core[cpu],
                     0x0000FF00);
        }
        gui_rect_t lower = {c.x + 18, half + 8, c.w - 36, c.y + c.h - 42 - half - 8};
        monitor_group(s, lower, "Uso de memoria");
        snprintf(text, sizeof(text), "RAM %u%% | Usada %u KiB | Libre %u KiB | Total %u KiB",
                 pm_percent(m.used, m.total), m.used / 1024, m.free / 1024, m.total / 1024);
        label(s, (gui_rect_t){lower.x + 8, lower.y + 12, lower.w - 16, 16}, text, CPL_TEXT);
        pm_graph(s, (gui_rect_t){lower.x + 8, lower.y + 32, lower.w - 16, lower.h - 80}, st->ram,
                 0x0000FF00);
        snprintf(text, sizeof(text), "Heap: %u / %u KiB | Libre %u KiB", m.heap_used / 1024,
                 m.heap_total / 1024, m.heap_free / 1024);
        label(s, (gui_rect_t){lower.x + 8, lower.y + lower.h - 40, lower.w - 16, 16}, text,
              CPL_TEXT);
        snprintf(text, sizeof(text), "Frames VM: %u / %u KiB | Reservas: %u KiB",
                 (m.vm_total - m.vm_free) / 1024, m.vm_total / 1024, m.reserved / 1024);
        label(s, (gui_rect_t){lower.x + 8, lower.y + lower.h - 22, lower.w - 16, 16}, text,
              CPL_TEXT);
    } else {
        monitor_headers(s, c, st->tab);
        uint32_t n = st->tab == 0 ? st->count : service_count(st);
        if (st->scroll_offset >= n)
            st->scroll_offset = 0;
        cpl_draw_bevel(s,
                       (gui_rect_t){c.x + 18, c.y + 66, c.w - 36, (int)visible_rows(st) * 18 + 4},
                       CPL_WHITE, true);
        for (uint32_t r = 0; r < visible_rows(st) && st->scroll_offset + r < n; r++) {
            uint32_t at = st->scroll_offset + r;
            char line[256];
            bool selected = false;
            if (st->tab == 0) {
                bk_proc_info_t *t = &st->rows[at];
                selected = proc_id(t) == st->selected_pid;
                char name[36];
                snprintf(name, sizeof(name), "%s%.28s", st->thread_row[at] ? " -> " : "", t->name);
                snprintf(line, sizeof(line), "%-5u %-28.28s %3u%% %8u KiB  %3u", t->pid, name,
                         st->cpu_percent[at], t->memory_bytes / 1024, st->threads[at]);
            } else {
                bk_startup_info_t *e = service_at(st, at);
                selected = e->id == st->selected_id;
                if (st->tab == 2)
                    snprintf(line, sizeof(line), "%-20.20s %-17.17s %5u  0x%x", e->name,
                             states[e->state < 5 ? e->state : 4], e->pid, e->dependencies);
                else
                    snprintf(
                        line, sizeof(line), "%-21.21s %-10.10s %4u %-11s %.28s", e->name,
                        phases[e->phase < 3 ? e->phase : 0], e->order, e->enabled ? "Si" : "No",
                        e->last_error[0] ? e->last_error : states[e->state < 5 ? e->state : 4]);
            }
            gui_rect_t rect = {c.x + 20, c.y + 68 + (int)r * 18, c.w - 52, 18};
            bk_gui_gfx_fill_rect(s, rect, selected ? CPL_BLUE : CPL_WHITE);
            label(s, (gui_rect_t){rect.x + 4, rect.y + 3, rect.w - 8, 14}, line,
                  selected ? CPL_WHITE : CPL_TEXT);
        }
        gui_scrollbar_t bar;
        list_bar(st, &bar);
        bk_gui_scrollbar_paint_vertical(s, &bar);
        if (st->tab == 0)
            pm_draw_button(s, (gui_rect_t){c.x + c.w - 132, c.y + c.h - 56, 114, 23}, "Finalizar",
                           st->selected_pid != 0, false, NULL);
        bk_startup_info_t *e = selected_entry(st);
        if (st->tab >= 2 && e) {
            char details[256];
            snprintf(details, sizeof(details), "%s | %s | %s", e->description,
                     e->critical ? "Control manual protegido" : e->path, e->last_error);
            label(s, (gui_rect_t){c.x + 24, c.y + 74 + (int)visible_rows(st) * 18, c.w - 48, 30},
                  details, 0x00404040);
        }
    }
    bk_memory_detail_t memory;
    bk_memory_detail(&memory);
    uint32_t processes=0, threads=0;
    for (uint32_t i=0;i<st->count;i++)
        if (!st->thread_row[i]) { processes++; threads+=st->threads[i]; }
    char info[3][64];
    snprintf(info[0],sizeof(info[0]),"Procesos: %u | Hilos: %u",processes,threads);
    snprintf(info[1],sizeof(info[1]),"CPU: %u%%",st->cpu[PM_HISTORY-1]);
    snprintf(info[2],sizeof(info[2]),"RAM: %u%%",pm_percent(memory.used,memory.total));
    int widths[3] = { (c.w-16)/2, (c.w-16)/4, 0 }, x=c.x+6;
    for (int i=0;i<3;i++) {
        int width=i==2 ? c.x+c.w-6-x : widths[i];
        cpl_draw_bevel(s,(gui_rect_t){x,c.y+c.h-25,width-2,21},CPL_FACE,true);
        label(s,(gui_rect_t){x+5,c.y+c.h-19,width-12,14},info[i],CPL_TEXT);
        x+=width;
    }
    if (st->tab!=1) {
        gui_rect_t status={c.x+18,c.y+c.h-57,c.w-(st->tab==0?160:36),24};
        cpl_draw_bevel(s,status,CPL_FACE,true);
        label(s,(gui_rect_t){status.x+5,status.y+6,status.w-10,14},st->status,CPL_TEXT);
    }

}
static void editor_action(gui_window_t *w, uint32_t id) {
    processmanager_state_t *st = g_processmanager;
    if (!st)
        return;
    if (id == bk_gui_widget_id(st->phase_button)) {
        st->phase = (st->phase + 1) % 3;
        bk_gui_widget_set_text(st->phase_button, phases[st->phase]);
    } else if (id == bk_gui_widget_id(st->kind_button)) {
        st->service = !st->service;
        bk_gui_widget_set_text(st->kind_button,
                               st->service ? "Servicio (READY requerido)" : "Programa de inicio");
    } else {
        st->pending_save = true;
        (void)w;
    }
}
static void editor_paint(gui_window_t *w, gui_surface_t *s, void *context) {
    processmanager_state_t *st = context;
    gui_rect_t c = bk_gui_window_content_rect_raw(w);
    const char *names[] = {"Nombre",       "Ejecutable",     "Argumentos", "Orden (0..999)",
                           "Timeout (ms)", "Deps (mascara)", "Descripcion"};
    for (int i = 0; i < 7; i++)
        label(s, (gui_rect_t){c.x + 12, c.y + 12 + i * 34, 130, 18}, names[i], 0x00202020);
    label(s, (gui_rect_t){c.x + 12, c.y + 292, c.w - 24, 36},
          "Se aplica en el proximo arranque. Los servicios deben publicar READY.", 0x00404040);
    label(s, (gui_rect_t){c.x + 12, c.y + 330, c.w - 24, 24}, st->status, 0x00404040);
}
static void open_editor(processmanager_state_t *st, uint32_t id) {
    if (st->editor && st->editor->listed)
        return;
    bk_runtime_memset(&st->editing, 0, sizeof(st->editing));
    if (id) {
        bk_startup_info_t *e = entry_by_id(st, id);
        if (!e || e->id <= 3)
            return;
        st->editing = *e;
    } else {
        st->editing.enabled = 1;
        st->editing.phase = 1;
        st->editing.timeout_ms = 30000;
        st->editing.order = 10;
    }
    st->phase = st->editing.phase;
    st->service = st->editing.service;
    st->editor = bk_gui_create_window(st->desktop, 100, 80, 565, 400, "Editar inicio / servicio");
    if (!st->editor)
        return;
    bk_gui_window_set_compact_content(st->editor, true);
    bk_gui_set_window_content(st->editor, editor_paint, st);
    char nums[3][20];
    snprintf(nums[0], 20, "%u", st->editing.order);
    snprintf(nums[1], 20, "%u", st->editing.timeout_ms);
    snprintf(nums[2], 20, "%u", st->editing.dependencies);
    const char *values[] = {st->editing.name, st->editing.path, st->editing.arguments,  nums[0],
                            nums[1],          nums[2],          st->editing.description};
    for (int i = 0; i < 7; i++)
        st->fields[i] = bk_gui_create_textbox(st->desktop, st->editor,
                                              (gui_rect_t){148, 8 + i * 34, 390, 24}, values[i],
                                              i == 0   ? 31
                                              : i < 3  ? 159
                                              : i == 6 ? 79
                                                       : 10,
                                              NULL);
    st->phase_button = bk_gui_create_button(st->desktop, st->editor, (gui_rect_t){12, 252, 130, 24},
                                            phases[st->phase], editor_action);
    st->kind_button = bk_gui_create_button(
        st->desktop, st->editor, (gui_rect_t){150, 252, 250, 24},
        st->service ? "Servicio (READY requerido)" : "Programa de inicio", editor_action);
    bk_gui_create_button(st->desktop, st->editor, (gui_rect_t){418, 252, 120, 24}, "Guardar",
                         editor_action);
}
static void menu_action(gui_window_t *w UNUSED, uint32_t id, void *context) {
    processmanager_state_t *st = context;
    if (id == 20) {
        st->expanded = st->expanded == st->menu_pid ? 0 : st->menu_pid;
        collect(st);
    } else if (id == 21) {
        st->pending_events = true;
        st->events_pid = st->menu_pid;
        st->events_id = st->menu_id;
        st->events_tab = st->menu_tab;
    } else if (id == 30) {
        st->pending_toggle = true;
        st->pending_toggle_id = st->menu_id;
    } else if (id == 31 || id == 32) {
        st->pending_edit = true;
        st->pending_edit_id = id == 31 ? 0 : st->menu_id;
    } else if (id == 33) {
        st->pending_remove = true;
        st->pending_remove_id = st->menu_id;
    } else {
        st->pending_action = id;
        st->pending_pid = st->menu_pid;
        st->pending_id = st->menu_id;
    }
    bk_gui_window_invalidate(st->window);
}
static void show_actions(processmanager_state_t *st, int x, int y) {
    gui_window_t *w = st->window;
    st->menu_pid = st->selected_pid;
    st->menu_id = st->selected_id;
    st->menu_tab = st->tab;
    bk_gui_window_context_clear(w);
    if (st->tab == 0) {
        bk_gui_window_context_add_item(w, 20, "Mostrar / ocultar hilos", true, menu_action, st);
        bk_gui_window_context_add_item(w, 41, "Prioridad: baja", true, menu_action, st);
        bk_gui_window_context_add_item(w, 42, "Prioridad: normal", true, menu_action, st);
        bk_gui_window_context_add_item(w, 43, "Prioridad: alta", true, menu_action, st);
        bk_gui_window_context_add_item(w, 21, "Eventos del proceso", st->selected_pid != 0,
                                       menu_action, st);
        bk_gui_window_context_add_item(w, 10, "Finalizar proceso", st->selected_pid != 0,
                                       menu_action, st);
    } else if (st->tab == 2) {
        bk_startup_info_t *e = selected_entry(st);
        bool control = e && !e->critical;
        bk_gui_window_context_add_item(w, 1, "Iniciar", control && !e->pid, menu_action, st);
        bk_gui_window_context_add_item(w, 2, "Detener", control && e->state == 2, menu_action, st);
        bk_gui_window_context_add_item(w, 3, "Reiniciar", control && e->state == 2, menu_action,
                                       st);
        bk_gui_window_context_add_item(w, 21, "Eventos del servicio", e != NULL, menu_action, st);
    } else {
        bk_startup_info_t *e = selected_entry(st);
        bk_gui_window_context_add_item(w, 30, e && e->enabled ? "Deshabilitar" : "Habilitar",
                                       e && e->id != 1, menu_action, st);
        bk_gui_window_context_add_item(w, 31, "Nueva entrada...", true, menu_action, st);
        bk_gui_window_context_add_item(w, 32, "Editar...", e && e->id > 3, menu_action, st);
        bk_gui_window_context_add_item(w, 33, "Eliminar", e && e->id > 3, menu_action, st);
    }
    bk_gui_window_context_open(w, x, y);
}
static bool pm_event(gui_window_t *w, const gui_event_t *e, void *context) {
    processmanager_state_t *st = context;
    if (!st)
        return false;
    gui_rect_t c = client(st);
    if (st->tab != 1) {
        gui_scrollbar_t bar;
        list_bar(st, &bar);
        uint32_t value;
        if (bk_gui_scrollbar_handle_event_vertical(&bar, &st->drag, e, 3, &value)) {
            st->scroll_offset = value;
            bk_gui_window_invalidate(w);
            return true;
        }
    }
    if (e->type != GUI_EVENT_MOUSE_DOWN || (e->button != 1 && e->button != 2))
        return false;
    int tab = cpl_hit_tab(c, tabs, 4, 16, 6, e->x, e->y);
    if (tab >= 0 && e->button == 1) {
        st->tab = tab;
        st->scroll_offset = 0;
        st->selected_id = 0;
        st->drag.active = false;
        bk_gui_window_invalidate(w);
        return true;
    }
    if (e->button == 1 && st->tab == 0 &&
        bk_gui_rect_contains((gui_rect_t){c.x + c.w - 132, c.y + c.h - 56, 114, 23}, e->x, e->y)) {
        if (st->selected_pid) {
            st->pending_action = 10;
            st->pending_pid = st->selected_pid;
        }
        return true;
    }
    if (st->tab != 1 &&
        bk_gui_rect_contains((gui_rect_t){c.x + 20, c.y + 68, c.w - 52, (int)visible_rows(st) * 18},
                             e->x, e->y)) {
        uint32_t at = st->scroll_offset + (e->y - c.y - 68) / 18;
        if (st->tab == 0) {
            if (at >= st->count)
                return false;
            st->selected_pid = proc_id(&st->rows[at]);
        } else {
            bk_startup_info_t *entry = service_at(st, at);
            st->selected_id = entry ? entry->id : 0;
            if (!entry && st->tab != 3)
                return false;
        }
        if (e->button == 2) show_actions(st, e->x, e->y);
        bk_gui_window_invalidate(w);
        return true;
    }
    return false;
}
static bool number_field(gui_widget_t *field, uint32_t *out) {
    char str[16];
    bk_gui_widget_get_text(field, str, sizeof(str));
    uint32_t n = 0;
    if (!str[0])
        return false;
    for (char *p = str; *p; p++) {
        if (*p < '0' || *p > '9' || n > 180000)
            return false;
        n = n * 10 + *p - '0';
    }
    *out = n;
    return true;
}
static void work(processmanager_state_t *st) {
    if (st->editor && !st->editor->listed) {
        bk_gui_window_destroy_raw(st->editor);
        st->editor = NULL;
        st->pending_save = false;
    }

    if (st->pending_events) {
        st->pending_events = false;
        char arg[96];
        uint32_t pid = st->events_pid;
        bk_startup_info_t *e = entry_by_id(st, st->events_id);
        if (st->events_tab >= 2)
            pid = e ? e->pid : 0;
        snprintf(arg, sizeof(arg), "--pid %u", pid);
        if (st->events_tab >= 2 && e)
            snprintf(arg, sizeof(arg), "--source 5 --query %s", e->name);
        if ((st->events_tab == 0 && pid) || (st->events_tab >= 2 && e))
            bk_app_launch("/SYSTEM/PROGRAMS/EVENT_VIEWER.BEX", arg);
    }
    if (st->pending_action) {
        uint32_t action = st->pending_action;
        st->pending_action = 0;
        bool ok = false;
        if (action == 10)
            ok = bk_proc_end_process(st->pending_pid);
        else if (action >= 41 && action <= 43)
            ok = bk_proc_priority(st->pending_pid, action - 40) > 0;
        else
            ok = bk_service_control(st->pending_id, action);
        bk_runtime_strcpy(st->status,
                          ok ? "Operacion solicitada"
                             : "Operacion rechazada; revise el estado o proteccion del componente");
    }
    if (st->pending_toggle) {
        st->pending_toggle = false;
        bk_startup_info_t *e = entry_by_id(st, st->pending_toggle_id);
        if (e) {
            bk_startup_info_t changed = *e;
            changed.enabled = !changed.enabled;
            bk_runtime_strcpy(st->status, bk_startup_save(&changed)
                                              ? "Habilitacion guardada: proximo arranque"
                                              : "No se pudo guardar");
        }
    }
    if (st->pending_remove) {
        st->pending_remove = false;
        bk_runtime_strcpy(st->status, bk_startup_remove(st->pending_remove_id)
                                          ? "Entrada eliminada"
                                          : "No se puede eliminar una entrada activa o critica");
    }
    if (st->pending_edit) {
        st->pending_edit = false;
        open_editor(st, st->pending_edit_id);
    }
    if (st->pending_save) {
        st->pending_save = false;
        bk_gui_widget_get_text(st->fields[0], st->editing.name, 32);
        bk_gui_widget_get_text(st->fields[1], st->editing.path, 160);
        bk_gui_widget_get_text(st->fields[2], st->editing.arguments, 160);
        bk_gui_widget_get_text(st->fields[6], st->editing.description, 80);
        st->editing.phase = st->phase;
        st->editing.service = st->service;
        bool ok = number_field(st->fields[3], &st->editing.order) &&
                  number_field(st->fields[4], &st->editing.timeout_ms) &&
                  number_field(st->fields[5], &st->editing.dependencies) &&
                  bk_startup_save(&st->editing);
        bk_runtime_strcpy(st->status, ok ? "Entrada guardada: proximo arranque"
                                         : "Valores invalidos o fallo al guardar");
        if (!ok && st->editor)
            bk_gui_window_invalidate(st->editor);
        if (ok) {
            bk_gui_desktop_remove_window(st->desktop, st->editor);
            bk_gui_window_destroy_raw(st->editor);
            st->editor = NULL;
        }
    }
}
static void pm_main(void *argument) {
    processmanager_state_t *st = argument;
    const char *arg = bk_proc_launch_arg();
    if (arg && !bk_runtime_strcmp(arg, "--tab inicio"))
        st->tab = 3;
    st->cpu_count = pm_cpu_count();
    bk_memory_detail_t memory;
    bool small = bk_memory_detail(&memory) && memory.total < 96U * 1024U * 1024U;
    st->window = bk_gui_create_window(st->desktop, 60, 30, small ? 650 : 660, small ? 430 : 445,
                                      "Administrador de procesos");
    if (!st->window) {
        bk_sys_free(st);
        bk_proc_exit();
    }
    bk_proc_bind_window(st->window);
    bk_gui_set_window_min_size(st->window, 650, 430);
    bk_gui_window_set_compact_content(st->window, true);
    st->client_rect = bk_gui_window_content_rect_raw(st->window);
    bk_gui_set_window_content(st->window, paint, st);
    bk_gui_set_window_event_handler(st->window, pm_event, st);
    bk_runtime_strcpy(st->status, "Clic izquierdo en una fila para ver sus acciones.");
    collect(st);
    while (!bk_proc_exit_requested() && st->window->listed) {
        st->client_rect = bk_gui_window_content_rect_raw(st->window);
        work(st);
        pm_sample(st);
        /* Sampling stays at 250 ms, but a complete snapshot and window paint
         * need only run once a second. Input events still repaint immediately. */
        if (bk_sys_ticks() - st->last_refresh >= bk_sys_tick_frequency()) {
            collect(st);
            st->last_refresh = bk_sys_ticks();
            bk_gui_window_invalidate(st->window);
        }
        bk_sys_sleep_ms(20);
    }
    st->closing = 1;
    if (st->editor) {
        bk_gui_desktop_remove_window(st->desktop, st->editor);
        bk_gui_window_destroy_raw(st->editor);
    }
    bk_gui_desktop_remove_window(st->desktop, st->window);
    bk_gui_window_destroy_raw(st->window);
    g_processmanager = NULL;
    bk_sys_free(st);
    bk_proc_exit();
}
bool processmanager_get_runtime_info(program_runtime_info_t *info) {
    if (!info || !g_processmanager)
        return false;
    info->window = g_processmanager->window;
    info->memory_bytes = sizeof(*g_processmanager);
    return true;
}
void processmanager_open_from_desktop(gui_desktop_t *desktop) {
    if (!desktop)
        return;
    processmanager_state_t *st = bk_sys_alloc_zero(sizeof(*st));
    if (!st)
        return;
    st->desktop = desktop;
    g_processmanager = st;
    if (bk_proc_spawn_thread("processmgr", pm_main, st) < 0) {
        g_processmanager = NULL;
        bk_sys_free(st);
    }
}
void processmanager_install(gui_desktop_t *desktop UNUSED) {}
void bleskernos_program_main(gui_desktop_t *desktop) { processmanager_open_from_desktop(desktop); }
