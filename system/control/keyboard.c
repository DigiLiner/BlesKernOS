#include "control_common.h"

static const cpl_tab_spec_t tabs[]={{"Velocidad",88},{"Idioma",72},{"General",76}};
typedef struct {
    gui_desktop_t *desktop; gui_window_t *window;
    gui_widget_t *delay,*rate,*blink,*layout[2],*test,*clear;
    uint8_t tab;
    uint32_t last_values[4];
    char status[112];
} keyboard_cpl_state_t;
static gui_rect_t keyboard_page(const keyboard_cpl_state_t *st) {
    gui_rect_t c=bk_gui_window_content_rect_raw(st->window);
    return (gui_rect_t){c.x+8,c.y+30,c.w-16,c.h-60};
}
static void keyboard_sync(keyboard_cpl_state_t *st) {
    gui_widget_t *sliders[]={st->delay,st->rate,st->blink};
    uint32_t values[]={1000-bk_input_preference_get(BK_INPUT_REPEAT_DELAY),
        250-bk_input_preference_get(BK_INPUT_REPEAT_INTERVAL),
        bk_input_preference_get(BK_INPUT_CARET_BLINK) ? 1250-bk_input_preference_get(BK_INPUT_CARET_BLINK) : 0};
    for(int i=0;i<3;i++) if(sliders[i]) {
        sliders[i]->visible=st->tab==0;
        if(!sliders[i]->pressed) bk_gui_slider_set_value(sliders[i],values[i]);
    }
    for(int i=0;i<2;i++) if(st->layout[i]) {
        st->layout[i]->visible=st->tab==1;
        bk_gui_widget_set_selected(st->layout[i],bk_input_preference_get(BK_INPUT_LAYOUT)==(uint32_t)i);
    }
    if(st->test) st->test->visible=st->tab==0;
    if(st->clear) st->clear->visible=st->tab==0;
}
static void keyboard_save(keyboard_cpl_state_t *st) {
    char data[128];
    snprintf(data,sizeof(data),"layout=%s\r\ndelay=%u\r\ninterval=%u\r\nblink=%u\r\n",
        bk_input_preference_get(BK_INPUT_LAYOUT) ? "ES" : "US",
        bk_input_preference_get(BK_INPUT_REPEAT_DELAY),bk_input_preference_get(BK_INPUT_REPEAT_INTERVAL),
        bk_input_preference_get(BK_INPUT_CARET_BLINK));
    bk_runtime_strcpy(st->status,bk_user_config_write_text("/SYSTEM/USER/CONFIG/KEYBD.INI",data)
        ? "Cambio global aplicado y guardado." : "Cambio aplicado; no se pudo guardar en disco.");
}
static void keyboard_change(gui_window_t *window,uint32_t id) {
    keyboard_cpl_state_t *st=window->content_context;
    if(st->delay && st->delay->id==id) (void)bk_input_preference_set(BK_INPUT_REPEAT_DELAY,1000-bk_gui_slider_get_value(st->delay));
    if(st->rate && st->rate->id==id) (void)bk_input_preference_set(BK_INPUT_REPEAT_INTERVAL,250-bk_gui_slider_get_value(st->rate));
    if(st->blink && st->blink->id==id) {
        uint32_t v=bk_gui_slider_get_value(st->blink);
        (void)bk_input_preference_set(BK_INPUT_CARET_BLINK,v ? 1250-v : 0);
    }
    for(int i=0;i<2;i++) if(st->layout[i] && st->layout[i]->id==id) (void)bk_input_preference_set(BK_INPUT_LAYOUT,(uint32_t)i);
    keyboard_sync(st);keyboard_save(st);window->dirty=true;
}
static void keyboard_clear(gui_window_t *window,uint32_t id UNUSED) {
    keyboard_cpl_state_t *st=window->content_context;
    bk_gui_widget_set_text(st->test,"");window->dirty=true;
}
static bool keyboard_event(gui_window_t *window,const gui_event_t *event,void *context) {
    keyboard_cpl_state_t *st=context;
    if(event->type==GUI_EVENT_MOUSE_DOWN && event->button==MOUSE_LEFT_BUTTON) {
        int tab=cpl_hit_tab(bk_gui_window_content_rect_raw(window),tabs,3,12,8,event->x,event->y);
        if(tab>=0) {st->tab=(uint8_t)tab;keyboard_sync(st);window->dirty=true;return true;}
    }
    return false;
}
static void keyboard_paint(gui_window_t *window,gui_surface_t *surface,void *context) {
    keyboard_cpl_state_t *st=context;gui_rect_t c=bk_gui_window_content_rect_raw(window),p=keyboard_page(st);char line[112];
    cpl_draw_tabs(surface,c,tabs,3,st->tab,p,12,8);
    if(st->tab==0) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+18,p.w-32,136},"Repeticion de caracteres");
        snprintf(line,sizeof(line),"Espera antes de repetir: %u ms",bk_input_preference_get(BK_INPUT_REPEAT_DELAY));
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+36,p.w-56,16},line,CPL_TEXT);
        snprintf(line,sizeof(line),"Velocidad: %u caracteres por segundo",1000U/bk_input_preference_get(BK_INPUT_REPEAT_INTERVAL));
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+94,p.w-56,16},line,CPL_TEXT);
        const char *ends[][2]={{"Larga","Corta"},{"Lenta","Rapida"},{"Fijo","Rapido"}};
        const int ys[]={82,140};
        for(int i=0;i<2;i++) {
            cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+ys[i],80,12},ends[i][0],BK_THEME_MUTED);
            cpl_draw_text_clipped(surface,(gui_rect_t){p.x+p.w-90,p.y+ys[i],64,12},ends[i][1],BK_THEME_MUTED);
        }
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+172,p.w-32,92},"Parpadeo del cursor de texto");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+190,p.w-56,16},"De fijo a rapido; afecta los cuadros de texto.",CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+244,80,12},"Fijo",BK_THEME_MUTED);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+p.w-90,p.y+244,64,12},"Rapido",BK_THEME_MUTED);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+282,p.w-32,86},"Prueba del teclado");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+300,p.w-56,16},"Escribi o mantene una tecla presionada:",CPL_TEXT);
    } else if(st->tab==1) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,130},"Distribucion activa");
        cpl_draw_text_wrapped(surface,(gui_rect_t){p.x+28,p.y+40,p.w-56,42},
            "La distribucion se aplica al teclado PS/2 y USB y se conserva al reiniciar.",CPL_TEXT,16,2);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+174,p.w-32,110},"Teclas de referencia");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+198,p.w-56,18},"US: ; : [ ] / ?",CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+226,p.w-56,18},"ES: tecla de ene, + * - _",CPL_TEXT);
        cpl_draw_text_wrapped(surface,(gui_rect_t){p.x+28,p.y+304,p.w-56,48},
            "La composicion de acentos y AltGr aun no esta disponible.",BK_THEME_MUTED,16,3);
    } else {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,160},"Dispositivo de entrada");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+42,p.w-56,18},"Teclado compatible PS/2 y USB HID",CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+74,p.w-56,18},"Estado: controlador de entrada activo",CPL_TEXT);
        snprintf(line,sizeof(line),"Distribucion: %s",bk_input_preference_get(BK_INPUT_LAYOUT) ? "Espanol" : "Estados Unidos");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+106,p.w-56,18},line,CPL_TEXT);
        cpl_draw_text_wrapped(surface,(gui_rect_t){p.x+28,p.y+216,p.w-56,80},
            "La velocidad de repeticion regula el escritorio. Las aplicaciones que leen el dispositivo directamente conservan su propio manejo de teclas.",BK_THEME_MUTED,16,5);
    }
    gui_rect_t status={c.x+8,c.y+c.h-24,c.w-16,20};cpl_draw_bevel(surface,status,CPL_FACE,true);
    cpl_draw_text_clipped(surface,cpl_inset(status,4),st->status,CPL_TEXT);
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    if(!desktop) return;
    keyboard_cpl_state_t *st=bk_sys_alloc_zero(sizeof(*st));if(!st) return;st->desktop=desktop;
    st->window=bk_gui_create_window(desktop,102,40,478,476,"Propiedades del teclado");
    if(!st->window){bk_sys_free(st);return;}st->window->resizable=false;
    bk_gui_set_window_content(st->window,keyboard_paint,st);bk_gui_set_window_event_handler(st->window,keyboard_event,st);
    st->window->owner_pid=bk_sys_getpid();bk_proc_bind_window(st->window);
    gui_rect_t c=bk_gui_window_content_rect_raw(st->window),p=cpl_local_rect(keyboard_page(st),c);
    st->delay=bk_gui_create_slider(desktop,st->window,(gui_rect_t){p.x+28,p.y+58,p.w-56,24},0,750,250,500,keyboard_change);
    st->rate=bk_gui_create_slider(desktop,st->window,(gui_rect_t){p.x+28,p.y+116,p.w-56,24},0,220,10,190,keyboard_change);
    st->blink=bk_gui_create_slider(desktop,st->window,(gui_rect_t){p.x+28,p.y+218,p.w-56,24},0,1000,250,750,keyboard_change);
    st->layout[0]=bk_gui_create_choice(desktop,st->window,(gui_rect_t){p.x+28,p.y+96,172,26},"Estados Unidos",true,keyboard_change);
    st->layout[1]=bk_gui_create_choice(desktop,st->window,(gui_rect_t){p.x+214,p.y+96,172,26},"Espanol",true,keyboard_change);
    st->test=bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+28,p.y+324,p.w-154,26},"",127,NULL);
    st->clear=bk_gui_create_button(desktop,st->window,(gui_rect_t){p.x+p.w-116,p.y+324,88,26},"Limpiar",keyboard_clear);
    bk_runtime_strcpy(st->status,"Los ajustes se aplican al escritorio.");keyboard_sync(st);
    while(!bk_proc_exit_requested()&&st->window->listed) {
        bool changed=false;for(uint32_t i=0;i<4;i++){uint32_t v=bk_input_preference_get(i);if(st->last_values[i]!=v){st->last_values[i]=v;changed=true;}}
        if(changed){keyboard_sync(st);st->window->dirty=true;}bk_sys_sleep_ticks(5);
    }
    cpl_destroy_window(desktop,st->window);bk_sys_free(st);bk_proc_exit();
}
