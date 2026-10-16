#include "kernel/include/api.h"
#include "control_common.h"

#define MOUSE_CPL_WINDOW_W    478
#define MOUSE_CPL_WINDOW_H    476
#define MOUSE_CPL_ICON_SIZE    32
#define MOUSE_CPL_TAB_LEFT     12
#define MOUSE_CPL_TAB_TOP       8
#define MOUSE_CPL_TAB_COUNT     4

typedef enum {
    MOUSE_TAB_POINTER = 0,
    MOUSE_TAB_OPTIONS = 1,
    MOUSE_TAB_BUTTONS = 2,
    MOUSE_TAB_GENERAL = 3,
} mouse_cpl_tab_t;

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    mouse_state_t mouse;
    uint8_t current_tab;
    int sensitivity;
    bool draw_trail;
    gui_widget_t *sensitivity_slider;
    gui_widget_t *trail_checkbox;
    uint32_t *mouse_icon;
    gui_widget_t *reset_button;
    gui_widget_t *scheme_buttons[3],*double_slider,*swap_buttons[2];
    uint32_t preference_values[3];
    uint32_t clicks[3];
    uint32_t double_clicks;
    uint32_t last_click_ms;
    int last_click_x;
    int last_click_y;
    bool pending_double;
    int click_x;
    int click_y;
    bool click_visible;
    uint32_t next_refresh;
    char status[80];
} mouse_cpl_state_t;

static const cpl_tab_spec_t g_mouse_tabs[MOUSE_CPL_TAB_COUNT] = {
    {"Punteros", 82},
    {"Movimiento", 96},
    {"Botones", 76},
    {"General", 76},
};

static gui_rect_t mouse_cpl_client_rect(const mouse_cpl_state_t *st) {
    return st && st->window
        ? bk_gui_window_content_rect_raw(st->window)
        : (gui_rect_t){0, 0, 0, 0};
}

static gui_rect_t mouse_cpl_page_rect(const mouse_cpl_state_t *st) {
    gui_rect_t client = mouse_cpl_client_rect(st);
    return (gui_rect_t){client.x + 8, client.y + 30, client.w - 16, client.h - 50};
}

static gui_rect_t mouse_cpl_status_rect(const mouse_cpl_state_t *st) {
    gui_rect_t client = mouse_cpl_client_rect(st);
    return (gui_rect_t){client.x + 6, client.y + client.h - 18,
                        client.w - 12, 16};
}

/* Drawing and hit testing use the same screen rectangle. */
static gui_rect_t mouse_cpl_preview_group_rect(const mouse_cpl_state_t *st) {
    gui_rect_t page = mouse_cpl_page_rect(st);
    return (gui_rect_t){page.x + 16, page.y + 108, page.w - 32, page.h - 124};
}

static gui_rect_t mouse_cpl_preview_canvas_rect(const mouse_cpl_state_t *st) {
    gui_rect_t group = mouse_cpl_preview_group_rect(st);
    return (gui_rect_t){group.x + 12, group.y + 30, group.w - 24, group.h - 104};
}

static gui_rect_t mouse_cpl_speed_group_rect(const mouse_cpl_state_t *st) {
    gui_rect_t page = mouse_cpl_page_rect(st);
    return (gui_rect_t){page.x + 18, page.y + 20, (page.w - 54) / 2, 142};
}

static gui_rect_t mouse_cpl_trail_group_rect(const mouse_cpl_state_t *st) {
    gui_rect_t page = mouse_cpl_page_rect(st);
    return (gui_rect_t){page.x + 36 + (page.w - 54) / 2, page.y + 20, (page.w - 54) / 2, 142};
}

static const char *mouse_cpl_default_status(const mouse_cpl_state_t *st) {
    if (!st) return "";
    return st->current_tab == MOUSE_TAB_POINTER
        ? "Proba los botones dentro del recuadro."
        : "Los cambios se aplican a todo el escritorio.";
}

static void mouse_cpl_set_status(mouse_cpl_state_t *st, const char *text) {
    if (!st) return;
    bk_runtime_strncpy(st->status, text ? text : "", sizeof(st->status) - 1);
    st->status[sizeof(st->status) - 1] = '\0';
}

static void mouse_cpl_sync_buttons(mouse_cpl_state_t *st) {
    if (!st) return;
    if(st->sensitivity_slider && !st->sensitivity_slider->pressed) bk_gui_slider_set_value(st->sensitivity_slider,st->sensitivity);
    if(st->double_slider && !st->double_slider->pressed) bk_gui_slider_set_value(st->double_slider,1100-bk_input_preference_get(BK_INPUT_DOUBLE_CLICK));
    for(int i=0;i<3;i++) bk_gui_choice_set_checked(st->scheme_buttons[i],bk_input_preference_get(BK_INPUT_CURSOR_SCHEME)==(uint32_t)i);
    for(int i=0;i<2;i++) bk_gui_choice_set_checked(st->swap_buttons[i],bk_input_preference_get(BK_INPUT_SWAP_BUTTONS)==(uint32_t)i);
    bk_gui_choice_set_checked(st->trail_checkbox,st->draw_trail);

}

static void mouse_cpl_sync_widget_visibility(mouse_cpl_state_t *st) {
    bool options_visible;

    if (!st) return;
    if (st->reset_button) st->reset_button->visible = st->current_tab == MOUSE_TAB_POINTER;
    for(int i=0;i<3;i++) if(st->scheme_buttons[i]) st->scheme_buttons[i]->visible=st->current_tab==MOUSE_TAB_POINTER;
    if(st->double_slider) st->double_slider->visible=st->current_tab==MOUSE_TAB_BUTTONS;
    for(int i=0;i<2;i++) if(st->swap_buttons[i]) st->swap_buttons[i]->visible=st->current_tab==MOUSE_TAB_BUTTONS;
    options_visible = st->current_tab == MOUSE_TAB_OPTIONS;
    if(st->sensitivity_slider) st->sensitivity_slider->visible=options_visible;
    if(st->trail_checkbox) st->trail_checkbox->visible=options_visible;

}

static void mouse_cpl_save_config(mouse_cpl_state_t *st) {
    char config[128];

    if (!st) return;
    st->sensitivity = bk_input_mouse_get_sensitivity();
    st->draw_trail = bk_gui_desktop_cursor_trail_enabled(st->desktop);
    snprintf(config, sizeof(config), "sensitivity=%d\r\ntrail=%d\r\nswap=%u\r\ndouble_click=%u\r\nscheme=%u\r\n",
             st->sensitivity, st->draw_trail ? 1 : 0,
             bk_input_preference_get(BK_INPUT_SWAP_BUTTONS),bk_input_preference_get(BK_INPUT_DOUBLE_CLICK),
             bk_input_preference_get(BK_INPUT_CURSOR_SCHEME));
    mouse_cpl_sync_buttons(st);
    if (!bk_user_config_write_text(BK_MOUSE_CONFIG_PATH, config))
        mouse_cpl_set_status(st, "Cambio aplicado; no se pudo guardar en disco.");
}

/* Preferences are loaded once by the desktop at boot. Every panel reads the
 * live shared state; opening an old panel must never restore stale values. */
static bool mouse_cpl_sync_system_state(mouse_cpl_state_t *st) {
    int sensitivity = bk_input_mouse_get_sensitivity();
    bool trail = bk_gui_desktop_cursor_trail_enabled(st->desktop);
    bool changed=st->sensitivity!=sensitivity || st->draw_trail!=trail;
    for(uint32_t i=0;i<3;i++) {
        uint32_t value=bk_input_preference_get(BK_INPUT_SWAP_BUTTONS+i);
        if(st->preference_values[i]!=value) {st->preference_values[i]=value;changed=true;}
    }
    if(!changed) return false;
    st->sensitivity = sensitivity;
    st->draw_trail = trail;
    mouse_cpl_sync_buttons(st);
    return true;
}

static void mouse_cpl_preference(gui_window_t *window,uint32_t id) {
    mouse_cpl_state_t *st=window->content_context;
    for(int i=0;i<3;i++) if(st->scheme_buttons[i] && st->scheme_buttons[i]->id==id)
        (void)bk_input_preference_set(BK_INPUT_CURSOR_SCHEME,(uint32_t)i);
    if(st->double_slider && st->double_slider->id==id)
        (void)bk_input_preference_set(BK_INPUT_DOUBLE_CLICK,1100-bk_gui_slider_get_value(st->double_slider));
    for(int i=0;i<2;i++) if(st->swap_buttons[i] && st->swap_buttons[i]->id==id)
        (void)bk_input_preference_set(BK_INPUT_SWAP_BUTTONS,(uint32_t)i);
    st->pending_double=false;
    mouse_cpl_set_status(st,"Preferencia global aplicada y guardada.");
    mouse_cpl_save_config(st);window->dirty=true;
}

static void mouse_cpl_reset_test(gui_window_t *window, uint32_t id) {
    mouse_cpl_state_t *st = window->content_context;
    (void)id;
    for (int i = 0; i < 3; i++) st->clicks[i] = 0;
    st->double_clicks = 0;
    st->pending_double = false;
    st->click_visible = false;
    mouse_cpl_set_status(st, mouse_cpl_default_status(st));
    window->dirty = true;
}

static void mouse_cpl_switch_tab(mouse_cpl_state_t *st, uint8_t tab) {
    if (st) st->pending_double = false;
    if (!st || tab >= MOUSE_CPL_TAB_COUNT || st->current_tab == tab) return;
    st->current_tab = tab;
    mouse_cpl_sync_widget_visibility(st);
    mouse_cpl_set_status(st, mouse_cpl_default_status(st));
    st->window->dirty = true;
}

static void mouse_cpl_sensitivity(gui_window_t *window, uint32_t id) {
    mouse_cpl_state_t *st = window
        ? (mouse_cpl_state_t *)window->content_context : NULL;

    if (!st) return;
    if(!st->sensitivity_slider || st->sensitivity_slider->id!=id) return;
    st->sensitivity=bk_gui_slider_get_value(st->sensitivity_slider);
    bk_input_mouse_set_sensitivity((uint8_t)st->sensitivity);
    mouse_cpl_sync_buttons(st);
    snprintf(st->status,sizeof(st->status),"Sensibilidad global: %d de 5.",st->sensitivity);
    mouse_cpl_save_config(st); window->dirty=true;

}

static void mouse_cpl_toggle_trail(gui_window_t *window, uint32_t id) {
    mouse_cpl_state_t *st = window
        ? (mouse_cpl_state_t *)window->content_context : NULL;

    if (!st) return;
    if(!st->trail_checkbox || st->trail_checkbox->id!=id) return;
    /* The polling loop may synchronize the checkbox before this queued
       callback runs. Toggle the authoritative preference, not that copy. */
    st->draw_trail=!bk_gui_desktop_cursor_trail_enabled(st->desktop);

    bk_gui_desktop_set_cursor_trail(st->desktop, st->draw_trail);
    mouse_cpl_sync_buttons(st);
    snprintf(st->status, sizeof(st->status), "Estela global %s.",
             st->draw_trail ? "activada" : "desactivada");
    mouse_cpl_save_config(st);
    window->dirty = true;
}

static void mouse_cpl_draw_fallback_icon(gui_surface_t *surface, int x, int y) {
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 13, y + 6, 10, 24}, CPL_LIGHT);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 24, y + 6, 10, 24}, CPL_LIGHT);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 20, y + 18, 8, 16}, CPL_SHADOW);
    bk_gui_gfx_draw_line(surface, x + 12, y + 5, x + 23, y + 39, CPL_DARK);
    bk_gui_gfx_draw_line(surface, x + 35, y + 5, x + 24, y + 39, CPL_DARK);
    bk_gui_gfx_draw_line(surface, x + 18, y + 40, x + 30, y + 40, CPL_DARK);
}

static void mouse_cpl_draw_preview_icon(const mouse_cpl_state_t *st,
                                        gui_surface_t *surface,
                                        int x, int y) {
    if (st->mouse_icon) {
        bk_app_draw_icon(surface, x, y, st->mouse_icon,
                                 MOUSE_CPL_ICON_SIZE, MOUSE_CPL_ICON_SIZE);
    } else {
        mouse_cpl_draw_fallback_icon(surface, x, y);
    }
}

static void mouse_cpl_paint_pointer_tab(mouse_cpl_state_t *st,
                                        gui_surface_t *surface) {
    gui_rect_t preview = mouse_cpl_preview_group_rect(st);
    gui_rect_t canvas = mouse_cpl_preview_canvas_rect(st);
    gui_rect_t page = mouse_cpl_page_rect(st);
    char line[96];
    mouse_cpl_draw_preview_icon(st, surface, page.x + 16, page.y + 8);
    cpl_draw_text_clipped(surface, (gui_rect_t){page.x+76,page.y+12,page.w-92,16},
                          "Puntero del sistema", CPL_TEXT);
    cpl_draw_text_clipped(surface, (gui_rect_t){page.x+76,page.y+32,page.w-92,16},
                          "Una configuracion para todas las aplicaciones.", BK_THEME_MUTED);
    cpl_draw_text_clipped(surface,(gui_rect_t){page.x+76,page.y+52,page.w-92,16},"Esquema de los punteros del sistema:",CPL_TEXT);
    cpl_draw_group(surface, preview, "Zona de prueba");
    cpl_draw_text_clipped(surface, (gui_rect_t){preview.x+12,preview.y+14,preview.w-24,14},
                          "Clic izquierdo, derecho, central o doble clic.", CPL_TEXT);
    cpl_draw_bevel(surface, canvas, CPL_WHITE, true);
    if (st->click_visible) {
        int x = canvas.x + st->click_x;
        int y = canvas.y + st->click_y;
        if (x >= canvas.x+8 && x < canvas.x+canvas.w-8 &&
            y >= canvas.y+8 && y < canvas.y+canvas.h-8) {
            bk_gui_gfx_draw_line(surface,x-5,y,x+5,y,CPL_BLUE);
            bk_gui_gfx_draw_line(surface,x,y-5,x,y+5,CPL_BLUE);
        }
    }
    snprintf(line,sizeof(line),"Izq: %u   Der: %u   Central: %u",
             st->clicks[0],st->clicks[1],st->clicks[2]);
    cpl_draw_text_clipped(surface,(gui_rect_t){canvas.x,canvas.y+canvas.h+10,canvas.w,14},line,CPL_TEXT);
    snprintf(line,sizeof(line),"Dobles: %u",st->double_clicks);
    cpl_draw_text_clipped(surface,(gui_rect_t){canvas.x,canvas.y+canvas.h+30,canvas.w-130,14},line,CPL_TEXT);
    cpl_draw_text_clipped(surface,(gui_rect_t){canvas.x,preview.y+preview.h-22,canvas.w,14},
        st->mouse.present ? "Mouse conectado" : "Mouse no detectado",BK_THEME_MUTED);
}

static void mouse_cpl_paint_options_tab(mouse_cpl_state_t *st,
                                        gui_surface_t *surface) {
    gui_rect_t speed = mouse_cpl_speed_group_rect(st);
    gui_rect_t trail = mouse_cpl_trail_group_rect(st);
    gui_rect_t page = mouse_cpl_page_rect(st);
    char line[48];
    cpl_draw_group(surface, speed, "Sensibilidad");
    snprintf(line, sizeof(line), "Nivel actual: %d de 5", st->sensitivity);
    cpl_draw_text_clipped(surface,(gui_rect_t){speed.x+12,speed.y+20,speed.w-24,14},line,CPL_TEXT);
    cpl_draw_text_clipped(surface,(gui_rect_t){speed.x+12,speed.y+46,60,14},"Lenta",BK_THEME_MUTED);
    cpl_draw_text_clipped(surface,(gui_rect_t){speed.x+speed.w-60,speed.y+46,48,14},"Rapida",BK_THEME_MUTED);
    cpl_draw_text_wrapped(surface,(gui_rect_t){speed.x+12,speed.y+104,speed.w-24,28},
                          "Velocidad de movimiento del puntero.",BK_THEME_MUTED,14,2);
    cpl_draw_group(surface, trail, "Estela");
    cpl_draw_text_clipped(surface,(gui_rect_t){trail.x+12,trail.y+20,trail.w-24,14},"Rastro del puntero",CPL_TEXT);
    cpl_draw_text_wrapped(surface,(gui_rect_t){trail.x+12,trail.y+84,trail.w-24,42},
        st->draw_trail ? "Activada en todo el escritorio." : "Desactivada en todo el escritorio.",
        BK_THEME_MUTED,14,3);
    cpl_draw_group(surface,(gui_rect_t){page.x+18,page.y+180,page.w-36,64},"Configuracion global");
    cpl_draw_text_wrapped(surface,(gui_rect_t){page.x+30,page.y+198,page.w-60,34},
        "Los cambios se aplican al instante y se guardan para el proximo inicio.",CPL_TEXT,14,2);
}

static void mouse_cpl_paint(gui_window_t *window, gui_surface_t *surface,
                            void *context) {
    mouse_cpl_state_t *st = (mouse_cpl_state_t *)context;
    gui_rect_t client = bk_gui_window_content_rect_raw(window);
    gui_rect_t page = mouse_cpl_page_rect(st);
    gui_rect_t status = mouse_cpl_status_rect(st);

    cpl_draw_tabs(surface, client, g_mouse_tabs, MOUSE_CPL_TAB_COUNT,
                  st->current_tab, page, MOUSE_CPL_TAB_LEFT, MOUSE_CPL_TAB_TOP);

    if (st->current_tab == MOUSE_TAB_POINTER)
        mouse_cpl_paint_pointer_tab(st, surface);
    else if(st->current_tab==MOUSE_TAB_OPTIONS)
        mouse_cpl_paint_options_tab(st, surface);
    else if(st->current_tab==MOUSE_TAB_BUTTONS) {
        char line[96];
        cpl_draw_group(surface,(gui_rect_t){page.x+16,page.y+20,page.w-32,120},"Boton principal");
        cpl_draw_text_wrapped(surface,(gui_rect_t){page.x+28,page.y+42,page.w-56,40},
            "Elegir la mano cambia los botones izquierdo y derecho en todo el sistema.",CPL_TEXT,16,2);
        cpl_draw_group(surface,(gui_rect_t){page.x+16,page.y+164,page.w-32,136},"Velocidad del doble clic");
        snprintf(line,sizeof(line),"Tiempo maximo entre clics: %u ms",bk_input_preference_get(BK_INPUT_DOUBLE_CLICK));
        cpl_draw_text_clipped(surface,(gui_rect_t){page.x+28,page.y+188,page.w-56,16},line,CPL_TEXT);
        cpl_draw_text_wrapped(surface,(gui_rect_t){page.x+28,page.y+258,page.w-56,32},
            "Proba el doble clic en Punteros. Este ajuste tambien afecta los iconos del escritorio.",BK_THEME_MUTED,16,2);
    } else {
        char line[96];
        cpl_draw_group(surface,(gui_rect_t){page.x+16,page.y+20,page.w-32,190},"Dispositivo");
        snprintf(line,sizeof(line),"Estado: %s",st->mouse.present ? "conectado" : "no detectado");
        cpl_draw_text_clipped(surface,(gui_rect_t){page.x+28,page.y+44,page.w-56,16},line,CPL_TEXT);
        snprintf(line,sizeof(line),"Identificador: %u  Paquete: %u bytes",st->mouse.device_id,st->mouse.packet_size);
        cpl_draw_text_clipped(surface,(gui_rect_t){page.x+28,page.y+76,page.w-56,16},line,CPL_TEXT);
        snprintf(line,sizeof(line),"Paquetes recibidos: %u",st->mouse.packets);
        cpl_draw_text_clipped(surface,(gui_rect_t){page.x+28,page.y+108,page.w-56,16},line,CPL_TEXT);
        cpl_draw_text_wrapped(surface,(gui_rect_t){page.x+28,page.y+148,page.w-56,42},
            "Compatible con mouse PS/2, USB HID y tabletas absolutas.",BK_THEME_MUTED,16,3);
        cpl_draw_text_wrapped(surface,(gui_rect_t){page.x+28,page.y+248,page.w-56,64},
            "Los esquemas cambian los punteros del sistema. Las aplicaciones pueden usar su propio cursor mientras tienen el foco.",BK_THEME_MUTED,16,4);
    }

    cpl_draw_bevel(surface, status, CPL_FACE, true);
    bk_gui_font_draw_string_clipped(surface, status.x + 4, status.y + 4,
        st->status[0] ? st->status : mouse_cpl_default_status(st),
        CPL_TEXT, (gui_rect_t){status.x + 4, status.y + 2,
        status.w - 8, status.h - 4});
}

static bool mouse_cpl_event(gui_window_t *window,
                            const gui_event_t *event,
                            void *context) {
    mouse_cpl_state_t *st = (mouse_cpl_state_t *)context;
    int tab_hit;

    if (!st || !event) return false;
    if (event->type == GUI_EVENT_MOUSE_DOWN && event->button == 1) {
        tab_hit = cpl_hit_tab(mouse_cpl_client_rect(st), g_mouse_tabs,
                              MOUSE_CPL_TAB_COUNT, MOUSE_CPL_TAB_LEFT,
                              MOUSE_CPL_TAB_TOP, event->x, event->y);
        if (tab_hit >= 0) {
            mouse_cpl_switch_tab(st, (uint8_t)tab_hit);
            return true;
        }
    }
    if (st->current_tab == MOUSE_TAB_POINTER &&
        event->type == GUI_EVENT_MOUSE_DOWN && (event->button == MOUSE_LEFT_BUTTON || event->button == MOUSE_RIGHT_BUTTON ||
         event->button == MOUSE_MIDDLE_BUTTON)) {
        gui_rect_t canvas = mouse_cpl_preview_canvas_rect(st);
        if (bk_gui_rect_contains(cpl_inset(canvas, 2),event->x,event->y)) {
            uint32_t now = bk_sys_uptime_ms();
            st->clicks[event->button == MOUSE_LEFT_BUTTON ? 0 :
                       event->button == MOUSE_RIGHT_BUTTON ? 1 : 2]++;
            st->click_x = event->x-canvas.x;
            st->click_y = event->y-canvas.y;
            st->click_visible = true;
            if (event->button == 1) {
                int dx = event->x-st->last_click_x, dy = event->y-st->last_click_y;
                bool twice = st->pending_double && now-st->last_click_ms <= bk_input_preference_get(BK_INPUT_DOUBLE_CLICK) &&
                             dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4;
                if (twice) st->double_clicks++;
                st->pending_double = !twice;
                st->last_click_ms = now;
                st->last_click_x = event->x;
                st->last_click_y = event->y;
                mouse_cpl_set_status(st,twice ? "Doble clic detectado." : "Clic izquierdo detectado.");
            } else {
                st->pending_double = false;
                mouse_cpl_set_status(st,event->button == 2 ? "Clic derecho detectado." : "Clic central detectado.");
            }
            window->dirty = true;
            return true;
        }
        st->pending_double = false;
    }
    if (event->type == GUI_EVENT_KEY && event->key == '\t') {
        mouse_cpl_switch_tab(st,
            (uint8_t)((st->current_tab + 1) % MOUSE_CPL_TAB_COUNT));
        return true;
    }
    return false;
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    mouse_cpl_state_t *st;

    if (!desktop) return;
    st = (mouse_cpl_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) return;

    st->desktop = desktop;
    st->window = bk_gui_create_window(desktop, 100, 60,
                                           MOUSE_CPL_WINDOW_W,
                                           MOUSE_CPL_WINDOW_H,
                                           "@H71C0925D");
    if (!st->window) {
        bk_sys_free(st);
        return;
    }

    (void)bk_about_attach(st->window, desktop, &(bk_about_info_t){
        "@H2B1D9B64", "@H1C1E4EC2", "@H393D5903",
        "@H7A28E1E5", "/ICONS/MOUSE.BMP"});

    bk_gui_set_window_min_size(st->window, MOUSE_CPL_WINDOW_W, MOUSE_CPL_WINDOW_H);
    bk_gui_set_window_content(st->window, mouse_cpl_paint, st);
    bk_gui_set_window_event_handler(st->window, mouse_cpl_event, st);
    st->window->owner_pid = bk_sys_getpid();
    bk_proc_bind_window(st->window);

    st->window->resizable = false;
    st->current_tab = MOUSE_TAB_POINTER;
    mouse_cpl_sync_system_state(st);
    st->mouse_icon = bk_app_load_icon("/ICONS/MOUSE.BMP",
                                                  MOUSE_CPL_ICON_SIZE,
                                                  MOUSE_CPL_ICON_SIZE);


    gui_rect_t client = mouse_cpl_client_rect(st);
    gui_rect_t speed = cpl_local_rect(mouse_cpl_speed_group_rect(st), client);
    gui_rect_t trail = cpl_local_rect(mouse_cpl_trail_group_rect(st), client);
    st->sensitivity_slider=bk_gui_create_slider(desktop,st->window,
        (gui_rect_t){speed.x+12,speed.y+68,speed.w-24,24},1,5,1,st->sensitivity,mouse_cpl_sensitivity);
    st->trail_checkbox=bk_gui_create_choice(desktop,st->window,
        (gui_rect_t){trail.x+12,trail.y+44,trail.w-24,24},"Mostrar estela",false,mouse_cpl_toggle_trail);
    gui_rect_t page=cpl_local_rect(mouse_cpl_page_rect(st),client);
    const char *schemes[]={"Retrosmart","Oscuro","Turquesa"};
    for(int i=0;i<3;i++) st->scheme_buttons[i]=bk_gui_create_choice(desktop,st->window,
        cpl_row_cell((gui_rect_t){page.x+16,page.y+70,page.w-32,24},i,3,8),schemes[i],true,mouse_cpl_preference);
    st->double_slider=bk_gui_create_slider(desktop,st->window,
        (gui_rect_t){page.x+28,page.y+220,page.w-56,24},200,900,50,600,mouse_cpl_preference);
    st->swap_buttons[0]=bk_gui_create_choice(desktop,st->window,(gui_rect_t){page.x+28,page.y+94,174,26},"Diestro (izquierdo)",true,mouse_cpl_preference);
    st->swap_buttons[1]=bk_gui_create_choice(desktop,st->window,(gui_rect_t){page.x+216,page.y+94,174,26},"Zurdo (derecho)",true,mouse_cpl_preference);
    gui_rect_t canvas = cpl_local_rect(mouse_cpl_preview_canvas_rect(st),client);
    st->reset_button = bk_gui_widget_create_button(desktop,st->window,
        (gui_rect_t){canvas.x+canvas.w-120,canvas.y+canvas.h+28,120,24},
        "Reiniciar prueba",mouse_cpl_reset_test);
    mouse_cpl_sync_buttons(st);
    mouse_cpl_sync_widget_visibility(st);
    mouse_cpl_set_status(st, mouse_cpl_default_status(st));

    st->next_refresh = bk_sys_uptime_ms();
    while (!bk_proc_exit_requested() && st->window->listed) {
        uint32_t now = bk_sys_uptime_ms();

        if ((int32_t)(now - st->next_refresh) >= 0) {
            bool changed = mouse_cpl_sync_system_state(st);
            mouse_state_t before = st->mouse;
            (void)bk_input_mouse(&st->mouse);
            if (changed || before.present != st->mouse.present)
                st->window->dirty = true;
            st->next_refresh = now + 50;
        }
        bk_sys_sleep_ticks(2);
    }

    if (st->mouse_icon) bk_sys_free(st->mouse_icon);
    cpl_destroy_window(st->desktop, st->window);
    bk_sys_free(st);
    bk_proc_exit();
}
