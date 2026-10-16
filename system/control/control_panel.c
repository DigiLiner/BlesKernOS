#include "control_common.h"
#include "kernel/include/api.h"

#define CONTROL_ITEM_COUNT 11
#define CONTROL_PAGE_ITEMS 12
#define CONTROL_CATEGORY_COUNT 4
#define CONTROL_COLS       4
#define CONTROL_CELL_W     96
#define CONTROL_CELL_H     76
#define CONTROL_ICON_SIZE  32
#define CONTROL_HEADER_H   36
#define CONTROL_STATUS_H   18
#define CONTROL_SIDEBAR_W  150

typedef struct {
    const char *name;
    const char *description;
    const char *path;
    const char *icon_path;
    uint8_t category;
} control_item_t;

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    int selected;
    uint32_t page;
    int hover;
    int pressed;
    int last_clicked;
    uint32_t last_click_tick;
    char status[96];
    uint32_t *icons[CONTROL_ITEM_COUNT];
} control_state_t;

static const control_item_t g_control_items[CONTROL_ITEM_COUNT] = {
    {"@H4C25F8DE", "@HBD6886FF", "/SYSTEM/CONTROL/DISPLAY.CPL",
     "/ICONS/DISPLAY.BMP", 1},
    {"@H1A175EBB", "@HCE0D56F8", "/SYSTEM/CONTROL/SOUND.CPL",
     "/ICONS/SOUND.BMP", 1},
    {"@H207DE777", "@H49AA501C", "/SYSTEM/CONTROL/DATETIME.CPL",
     "/ICONS/DATETIME.BMP", 1},
    {"@H2B1D9B64", "@H4B1BED21", "/SYSTEM/CONTROL/MOUSE.CPL",
     "/ICONS/MOUSE.BMP", 1},
    {"@HF56C750D", "@H464BC894", "/SYSTEM/CONTROL/KEYBOARD.CPL",
     "/ICONS/KEYBOARD.BMP", 1},
    {"@HAF21B997", "@H95EF33C0", "/SYSTEM/CONTROL/SYSTEM.CPL",
     "/ICONS/SYSTEM.BMP", 0},
    {"@H619D37EF", "@H0333C1AB", "/SYSTEM/CONTROL/DEVMGR.CPL",
     "/ICONS/DEVICES.BMP", 0},
    {"@HAF34335C", "@HC0C280C1", "/SYSTEM/CONTROL/INTERNET.CPL",
     "/ICONS/INTERNET.BMP", 2},
    {"@H8385EA8D", "@HA4F0F315", "/SYSTEM/CONTROL/MODEM.CPL",
     "/ICONS/MODEM.BMP", 2},
    {"@H012FAD36", "@H9EC85AA6", "/SYSTEM/CONTROL/LANGUAGE.CPL",
     "/ICONS/LANGUAGE.BMP", 3},
    {"Wine", "Configuracion y acerca de Wine 11", "/SYSTEM/CONTROL/WINE.CPL",
     "/ICONS/OBJECT.BMP", 0},
};

static gui_rect_t control_client_rect(const control_state_t *st) {
    return st && st->window
        ? bk_gui_window_content_rect_raw(st->window)
        : (gui_rect_t){0, 0, 0, 0};
}

static gui_rect_t control_header_rect(const control_state_t *st) {
    gui_rect_t client = control_client_rect(st);
    return (gui_rect_t){client.x + 8, client.y + 8, client.w - 16, CONTROL_HEADER_H};
}

static gui_rect_t control_status_rect(const control_state_t *st) {
    gui_rect_t client = control_client_rect(st);
    return (gui_rect_t){client.x + 8, client.y + client.h - CONTROL_STATUS_H - 6,
                        client.w - 16, CONTROL_STATUS_H};
}

static gui_rect_t control_sidebar_rect(const control_state_t *st) {
    gui_rect_t c=control_client_rect(st),status=control_status_rect(st);
    return (gui_rect_t){c.x+8,c.y+12,CONTROL_SIDEBAR_W,status.y-c.y-20};
}
static gui_rect_t control_main_rect(const control_state_t *st) {
    gui_rect_t sidebar=control_sidebar_rect(st),c=control_client_rect(st);
    return (gui_rect_t){sidebar.x+sidebar.w+8,sidebar.y,c.x+c.w-8-(sidebar.x+sidebar.w+8),sidebar.h};
}
static gui_rect_t control_info_rect(const control_state_t *st) {
    gui_rect_t main=control_main_rect(st);return (gui_rect_t){main.x,main.y,main.w,82};
}
static gui_rect_t control_grid_outer_rect(const control_state_t *st) {
    gui_rect_t main=control_main_rect(st),info=control_info_rect(st);
    return (gui_rect_t){main.x,info.y+info.h+8,main.w,main.h-info.h-8};
}
static void control_draw_sidebar(gui_surface_t *surface,const control_state_t *st,gui_rect_t r) {
    static const char *names[]={"Sistema","Hardware","Red","Regional"};
    int index=st->selected>=0 ? st->selected : st->hover;
    int active=index>=0 ? g_control_items[index].category : 0;
    cpl_draw_group(surface,r,"Categorias");
    gui_rect_t inner={r.x+8,r.y+12,r.w-16,r.h-20};cpl_draw_bevel(surface,inner,CPL_FACE,true);
    for(int i=0;i<4;i++) {
        gui_rect_t row={inner.x+4,inner.y+6+i*24,inner.w-8,20};
        if(i==active) cpl_draw_bevel(surface,row,0x00D9E8FB,true);
        bk_gui_gfx_fill_rect(surface,(gui_rect_t){row.x+5,row.y+7,5,5},i==active ? CPL_BLUE : CPL_SHADOW);
        cpl_draw_text_clipped(surface,(gui_rect_t){row.x+16,row.y+6,row.w-20,10},names[i],CPL_TEXT);
    }
}

static gui_rect_t control_grid_inner_rect(const control_state_t *st) {
    gui_rect_t outer=control_grid_outer_rect(st);
    return (gui_rect_t){outer.x+10,outer.y+16,outer.w-20,outer.h-26};
}

static gui_rect_t control_item_rect(const control_state_t *st, int index) {
    gui_rect_t grid = control_grid_inner_rect(st);
    int local=index-(int)st->page*CONTROL_PAGE_ITEMS;
    int row = local / CONTROL_COLS;
    int col = local % CONTROL_COLS;
    int first = (int)st->page*CONTROL_PAGE_ITEMS+row * CONTROL_COLS;
    int remaining = CONTROL_ITEM_COUNT - first;
    int items_in_row = remaining > CONTROL_COLS ? CONTROL_COLS : remaining;
    int row_w = items_in_row * CONTROL_CELL_W;
    int left = grid.x + (grid.w - row_w) / 2;
    int top = grid.y + 8;

    if (left < grid.x + 2) left = grid.x + 2;
    return (gui_rect_t){left + col * CONTROL_CELL_W,
                        top + row * CONTROL_CELL_H,
                        CONTROL_CELL_W - 6, CONTROL_CELL_H - 6};
}

static void control_draw_fallback_icon(gui_surface_t *surface, int x, int y) {
    int inner_x = x + 8;
    int inner_y = y + 7;
    int inner_w = CONTROL_ICON_SIZE - 16;
    int inner_h = CONTROL_ICON_SIZE - 18;

    cpl_draw_bevel(surface, (gui_rect_t){x, y, CONTROL_ICON_SIZE, CONTROL_ICON_SIZE},
                   CPL_FACE, false);
    bk_gui_gfx_fill_rect(surface,
        (gui_rect_t){inner_x, inner_y, inner_w, inner_h},
        0x00E8E8E8);
    bk_gui_gfx_draw_rect(surface,
        (gui_rect_t){inner_x, inner_y, inner_w, inner_h},
        CPL_DARK);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 12, y + 12, 14, 4}, CPL_BLUE);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 12, y + 20, 14, 4}, CPL_SHADOW);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 12, y + 28, 14, 4}, CPL_SHADOW);
}

static void control_draw_icon(gui_surface_t *surface, int x, int y,
                              const uint32_t *pixels) {
    if (pixels) {
        bk_app_draw_icon(surface, x, y, pixels,
                         CONTROL_ICON_SIZE, CONTROL_ICON_SIZE);
        return;
    }
    control_draw_fallback_icon(surface, x, y);
}

static void control_load_icons(control_state_t *st) {
    if (!st) return;
    for (int i = 0; i < CONTROL_ITEM_COUNT; i++) {
        if (st->icons[i]) continue;
        st->icons[i] = bk_app_load_icon(g_control_items[i].icon_path,
                                        CONTROL_ICON_SIZE,
                                        CONTROL_ICON_SIZE);
    }
}

static void control_free_icons(control_state_t *st) {
    if (!st) return;
    for (int i = 0; i < CONTROL_ITEM_COUNT; i++) {
        if (!st->icons[i]) continue;
        bk_sys_free(st->icons[i]);
        st->icons[i] = NULL;
    }
}

static int control_hit(const control_state_t *st, int x, int y) {
    for (int i = st->page*CONTROL_PAGE_ITEMS; i < CONTROL_ITEM_COUNT && i < (int)(st->page+1)*CONTROL_PAGE_ITEMS; i++)
        if (bk_gui_rect_contains(control_item_rect(st, i), x, y)) return i;
    return -1;
}

static void control_open(control_state_t *st, int index) {
    const char *name;
    if (!st || index < 0 || index >= CONTROL_ITEM_COUNT) return;
    name = bk_lang_translate(g_control_items[index].name);
#if defined(BLES_ARCH_ARM32_RPI3)
    if (program_execute_path_async(st->desktop, g_control_items[index].path)) {
#else
    if (bk_app_execute_path(st->desktop, g_control_items[index].path)) {
#endif
        snprintf(st->status, sizeof(st->status), "@HD842EE6B", name);
    } else {
        snprintf(st->status, sizeof(st->status), "@H2F930F1A", name);
    }
    st->window->dirty = true;
}

enum {
    CONTROL_CONTEXT_OPEN = 1,
    CONTROL_CONTEXT_INFO,
};

static void control_context_callback(gui_window_t *window, uint32_t item_id,
                                     void *context) {
    control_state_t *st = (control_state_t *)context;
    if (!st || st->selected < 0 || st->selected >= CONTROL_ITEM_COUNT) return;
    if (item_id == CONTROL_CONTEXT_OPEN) {
        control_open(st, st->selected);
    } else if (item_id == CONTROL_CONTEXT_INFO) {
        snprintf(st->status, sizeof(st->status), "@H0FF02D3B",
                 bk_lang_translate(g_control_items[st->selected].name),
                 bk_lang_translate(g_control_items[st->selected].description));
        if (window) window->dirty = true;
    }
}

static void control_open_context(control_state_t *st, int hit, int x, int y) {
    if (!st || !st->window || hit < 0 || hit >= CONTROL_ITEM_COUNT) return;
    st->selected = hit;
    bk_gui_window_context_clear(st->window);
    (void)bk_gui_window_context_add_item(st->window, CONTROL_CONTEXT_OPEN,
        "@H75B3827E", true, control_context_callback, st);
    (void)bk_gui_window_context_add_item(st->window, CONTROL_CONTEXT_INFO,
        "@H7049965E", true, control_context_callback, st);
    bk_gui_window_context_open(st->window, x, y);
}

static gui_rect_t control_page_button(const control_state_t *st,int which);
static void control_paint(gui_window_t *window, gui_surface_t *surface,
                          void *context) {
    control_state_t *st=context;
    gui_rect_t client=control_client_rect(st),status=control_status_rect(st);
    (void)window;
    bk_gui_gfx_fill_rect(surface,client,CPL_FACE);
    control_draw_sidebar(surface,st,control_sidebar_rect(st));
    gui_rect_t info=control_info_rect(st),grid=control_grid_outer_rect(st);
    int selected=st->selected>=0 ? st->selected : st->hover;
    const control_item_t *item=selected>=0 ? &g_control_items[selected] : NULL;
    cpl_draw_group(surface,info,"Seleccion");
    cpl_draw_bevel(surface,(gui_rect_t){info.x+8,info.y+12,info.w-16,info.h-20},CPL_WHITE,true);
    cpl_draw_text_clipped(surface,(gui_rect_t){info.x+16,info.y+22,info.w-32,12},item ? item->name : "Panel de control",CPL_TEXT);
    cpl_draw_text_wrapped(surface,(gui_rect_t){info.x+16,info.y+40,info.w-32,32},
        item ? item->description : "Seleccione un icono para configurar BlesKernOS.",CPL_TEXT,12,2);
    cpl_draw_group(surface,grid,"Elementos");
    cpl_draw_bevel(surface,(gui_rect_t){grid.x+8,grid.y+12,grid.w-16,grid.h-20},CPL_FACE,true);
    for (int i=st->page*CONTROL_PAGE_ITEMS;i<CONTROL_ITEM_COUNT && i<(int)(st->page+1)*CONTROL_PAGE_ITEMS;i++) {
        gui_rect_t cell=control_item_rect(st,i);
        bool selected=i==st->selected;
        control_draw_icon(surface,cell.x+(cell.w-CONTROL_ICON_SIZE)/2,cell.y+4,st->icons[i]);
        gui_rect_t label={cell.x+2,cell.y+44,cell.w-4,24};
        if (selected) bk_gui_gfx_fill_rect(surface,label,CPL_BLUE);
        int x=cell.x+(cell.w-(int)bk_gui_font_text_width(g_control_items[i].name))/2;
        if (x<label.x+2) x=label.x+2;
        bk_gui_font_draw_string_clipped(surface,x,label.y+5,g_control_items[i].name,
            selected ? BK_THEME_SELECTION_TEXT : CPL_TEXT,cpl_inset(label,2));
    }
    cpl_draw_bevel(surface,status,CPL_FACE,true);
    int index=st->hover>=0 ? st->hover : st->selected;
    const char *text=st->status[0] ? st->status :
        (index>=0 ? g_control_items[index].description : "Listo");
    cpl_draw_text_clipped(surface,cpl_inset(status,4),text,CPL_TEXT);
}

static gui_rect_t control_page_button(const control_state_t *st,int which) {
    gui_rect_t h=control_header_rect(st);
    return (gui_rect_t){h.x+h.w-180+which*88,h.y+6,84,24};
}
static bool control_event(gui_window_t *window UNUSED, const gui_event_t *event,
                          void *context) {
    control_state_t *st = (control_state_t *)context;
    if(event->type==GUI_EVENT_MOUSE_DOWN)for(int i=0;CONTROL_ITEM_COUNT>CONTROL_PAGE_ITEMS && i<2;i++) {
        if(bk_gui_rect_contains(control_page_button(st,i),event->x,event->y)) {
            if(i && (st->page+1)*CONTROL_PAGE_ITEMS<CONTROL_ITEM_COUNT)st->page++;
            if(!i && st->page)st->page--;
            st->selected=st->hover=st->pressed=-1;st->window->dirty=true;return true;
        }
    }
    int hit = control_hit(st, event->x, event->y);

    if (event->type == GUI_EVENT_MOUSE_MOVE) {
        if (st->hover != hit) {
            st->hover = hit;
            st->window->dirty = true;
        }
        return hit >= 0;
    }

    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        if (event->button == MOUSE_RIGHT_BUTTON && hit >= 0) {
            control_open_context(st, hit, event->x, event->y);
            st->window->dirty = true;
            return true;
        }
        st->pressed = hit;
        st->selected = hit;
        st->window->dirty = true;
        return hit >= 0;
    }

    if (event->type == GUI_EVENT_MOUSE_UP && hit >= 0 && hit == st->pressed) {
        uint32_t now = bk_sys_uptime_ms();
        if (st->last_clicked == hit && now - st->last_click_tick <= bk_input_preference_get(BK_INPUT_DOUBLE_CLICK))
            control_open(st, hit);
        st->last_clicked = hit;
        st->last_click_tick = now;
        st->pressed = -1;
        st->window->dirty = true;
        return true;
    }

    if (event->type == GUI_EVENT_MOUSE_UP) {
        if (st->pressed >= 0) st->window->dirty = true;
        st->pressed = -1;
    }

    if (event->type == GUI_EVENT_KEY && event->key == KEY_ENTER &&
        st->selected >= 0) {
        control_open(st, st->selected);
        return true;
    }

    return false;
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    control_state_t *st;

    if (!desktop) return;
    st = (control_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) return;

    st->desktop = desktop;
    st->selected = st->hover = st->pressed = st->last_clicked = -1;
    st->window = bk_gui_create_window(desktop, 48, 20, 620, 545,
                                      "@H59EF0B79");
    if (!st->window) {
        bk_sys_free(st);
        return;
    }

    (void)bk_about_attach(st->window, desktop, &(bk_about_info_t){
        "@H59EF0B79", "@H1C1E4EC2", "@H375DAB2B",
        "@H7A28E1E5", "/ICONS/CONTROL.BMP"});

    bk_gui_set_window_min_size(st->window, 620, 545);
    bk_gui_set_window_content(st->window, control_paint, st);
    bk_gui_set_window_event_handler(st->window, control_event, st);
    st->window->bg_color = 0x00C0C0C0;
    st->window->owner_pid = bk_sys_getpid();
    bk_proc_bind_window(st->window);

    control_load_icons(st);

    while (!bk_proc_exit_requested() && st->window->listed) bk_sys_sleep_ticks(2);

    control_free_icons(st);
    cpl_destroy_window(st->desktop, st->window);
    bk_sys_free(st);
    bk_proc_exit();
}
