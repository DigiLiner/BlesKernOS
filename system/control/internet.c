#include "kernel/include/api.h"
#include "control_common.h"
#include "kernel/include/browser_preferences.h"

#define INTERNET_CONFIG_PATH "/SYSTEM/USER/CONFIG/INTERNET.INI"
#define INTERNET_DEFAULT_HOME "https://www.google.com/"

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_widget_t *home_box,*search_box,*browser_box,*editor_box;
    gui_widget_t *widgets[24];uint8_t widget_tabs[24],widget_count,tab;
    char search[256],browser[256],editor[256];
    bk_net_info_t network;
    gui_widget_t *images_button;
    gui_widget_t *css_button;
    gui_widget_t *cookies_button;
    gui_widget_t *timeout_slider;
    gui_widget_t *action_buttons[4];
    int layout_w;
    int layout_h;
    uint32_t images_id;
    uint32_t css_id;
    uint32_t cookies_id;
    bool images;
    bool css;
    bool cookies;
    uint32_t timeout_ms;
    uint32_t ethernet_count;
    bool dns_tested;
    bool dns_ok;
    uint8_t dns_address[4];
    char status[112];
} internet_cpl_state_t;

static bool internet_equal(const char *left, const char *right) {
    uint32_t i = 0U;
    if (!left || !right) return false;
    while (left[i] && right[i] && left[i] == right[i]) i++;
    return left[i] == '\0' && right[i] == '\0';
}

static uint32_t internet_parse_uint(const char *text, uint32_t fallback) {
    uint32_t value = 0U;
    bool digit = false;
    while (text && *text >= '0' && *text <= '9') {
        digit = true;
        value = value * 10U + (uint32_t)(*text++ - '0');
        if (value > 30000U) return fallback;
    }
    return digit ? value : fallback;
}

static void internet_defaults(internet_cpl_state_t *st) {
    if (!st) return;
    st->images = true;
    st->css = true;
    st->cookies = true;
    st->timeout_ms = 10000U;
    bk_runtime_strcpy(st->search,"https://www.google.com/search?q=");
    bk_runtime_strcpy(st->browser,BK_DEFAULT_BROWSER_PATH);
    bk_runtime_strcpy(st->editor,"/SYSTEM/PROGRAMS/TEXTEDITOR.BEX");
}

static void internet_apply_line(internet_cpl_state_t *st, const char *line,
                                char *home, uint32_t home_capacity) {
    char key[24];
    char value[256];
    uint32_t split = 0U, i = 0U, j = 0U;
    if (!st || !line) return;
    while (line[split] && line[split] != '=') split++;
    if (line[split] != '=') return;
    while (i < split && i + 1U < sizeof(key)) {
        key[i] = line[i];
        i++;
    }
    key[i] = '\0';
    i = split + 1U;
    while (line[i] && j + 1U < sizeof(value)) value[j++] = line[i++];
    value[j] = '\0';
    if (internet_equal(key, "home") && value[0]) {
        bk_runtime_strncpy(home, value, home_capacity - 1U);
        home[home_capacity - 1U] = '\0';
    } else if (internet_equal(key,"search") && value[0]) {
        bk_runtime_strncpy(st->search,value,sizeof(st->search)-1);
    } else if (internet_equal(key,"browser") && value[0]=='/') {
        bk_runtime_strncpy(st->browser,value,sizeof(st->browser)-1);
    } else if (internet_equal(key,"editor") && value[0]=='/') {
        bk_runtime_strncpy(st->editor,value,sizeof(st->editor)-1);
    } else if (internet_equal(key, "images")) {
        st->images = value[0] != '0';
    } else if (internet_equal(key, "css")) {
        st->css = value[0] != '0';
    } else if (internet_equal(key, "cookies")) {
        st->cookies = value[0] != '0';
    } else if (internet_equal(key, "timeout")) {
        uint32_t timeout = internet_parse_uint(value, st->timeout_ms);
        if (timeout >= 2000U && timeout <= 30000U) st->timeout_ms = timeout;
    }
}

static void internet_load(internet_cpl_state_t *st) {
    void *raw = NULL;
    uint32_t size = 0U, position = 0U;
    char home[256];
    char line[320];
    internet_defaults(st);
    bk_runtime_strcpy(home, INTERNET_DEFAULT_HOME);
    if (bk_file_read_all(INTERNET_CONFIG_PATH, &raw, &size) && raw) {
        while (position < size) {
            uint32_t used = 0U;
            while (position < size && ((char *)raw)[position] != '\n' &&
                   used + 1U < sizeof(line)) {
                char c = ((char *)raw)[position++];
                if (c != '\r') line[used++] = c;
            }
            while (position < size && ((char *)raw)[position] != '\n') position++;
            if (position < size) position++;
            line[used] = '\0';
            internet_apply_line(st, line, home, sizeof(home));
        }
        bk_sys_free(raw);
    }
    if (st->home_box) bk_gui_widget_set_text(st->home_box, home);
    bk_gui_widget_set_text(st->search_box,st->search);
    bk_gui_widget_set_text(st->browser_box,st->browser);
    bk_gui_widget_set_text(st->editor_box,st->editor);
}

static void internet_scan_adapters(internet_cpl_state_t *st) {
    uint32_t count, i;
    if (!st) return;
    st->ethernet_count = 0U;
    count = bk_device_pci_count();
    for (i = 0U; i < count; i++) {
        bk_pci_info_t info;
        if (bk_device_pci_info(i, &info) && info.class_code == 0x02U)
            st->ethernet_count++;
    }
}

static void internet_sync_buttons(internet_cpl_state_t *st) {
    if (!st) return;
    if (st->images_button)
        bk_gui_choice_set_checked(st->images_button, st->images);
    if (st->css_button)
        bk_gui_choice_set_checked(st->css_button, st->css);
    if (st->cookies_button)
        bk_gui_choice_set_checked(st->cookies_button, st->cookies);
    if(st->timeout_slider && !st->timeout_slider->pressed) bk_gui_slider_set_value(st->timeout_slider,st->timeout_ms/1000U);

}

static bool internet_save_config(internet_cpl_state_t *st) {
    char home[256];
    char data[1400];
    if (!st || !st->home_box) return false;
    home[0] = '\0';
    if (!bk_gui_widget_get_text(st->home_box, home, sizeof(home)) || !home[0])
        bk_runtime_strcpy(home, INTERNET_DEFAULT_HOME);
    bk_gui_widget_get_text(st->search_box,st->search,sizeof(st->search));
    bk_gui_widget_get_text(st->browser_box,st->browser,sizeof(st->browser));
    bk_gui_widget_get_text(st->editor_box,st->editor,sizeof(st->editor));
    const char *fields[]={home,st->search,st->browser,st->editor};
    for(int i=0;i<4;i++) for(const char *q=fields[i];*q;q++) if(*q=='\r'||*q=='\n'||*q=='|') return false;
    vfs_dir_entry_t entry;
    if(st->browser[0]!='/' || !bk_file_stat(st->browser,&entry) || entry.type!=VFS_NODE_FILE) return false;
    if(st->editor[0]!='/' || !bk_file_stat(st->editor,&entry) || entry.type!=VFS_NODE_FILE) return false;
    if(bk_runtime_strncmp(st->search,"https://",8) && bk_runtime_strncmp(st->search,"http://",7)) return false;
    snprintf(data, sizeof(data),
        "home=%s\r\nimages=%u\r\ncss=%u\r\ncookies=%u\r\ntimeout=%u\r\nsearch=%s\r\nbrowser=%s\r\neditor=%s\r\n",
        home, st->images ? 1U : 0U, st->css ? 1U : 0U,
        st->cookies ? 1U : 0U, st->timeout_ms,st->search,st->browser,st->editor);
    return bk_file_write_all(INTERNET_CONFIG_PATH, data,
                             (uint32_t)bk_runtime_strlen(data));
}

static void internet_toggle(gui_window_t *window, uint32_t id) {
    internet_cpl_state_t *st = window
        ? (internet_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    if (id == st->images_id) st->images = !st->images;
    else if (id == st->css_id) st->css = !st->css;
    else if (id == st->cookies_id) st->cookies = !st->cookies;
    internet_sync_buttons(st);
    bk_runtime_strcpy(st->status, "@H00CDDE8C");
    window->dirty = true;
}

static void internet_timeout(gui_window_t *window, uint32_t id) {
    internet_cpl_state_t *st = window
        ? (internet_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    if(!st->timeout_slider || st->timeout_slider->id!=id) return;
    st->timeout_ms=bk_gui_slider_get_value(st->timeout_slider)*1000U;
    snprintf(st->status, sizeof(st->status), "@H4116837A",
             st->timeout_ms / 1000U);
    internet_sync_buttons(st);
    window->dirty = true;
}

static void internet_save(gui_window_t *window, uint32_t id UNUSED) {
    internet_cpl_state_t *st = window
        ? (internet_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    bk_runtime_strcpy(st->status, internet_save_config(st)
        ? "Configuracion guardada para las proximas ventanas."
        : "No se pudo guardar. Revisa las rutas y el buscador.");
    window->dirty = true;
}

static void internet_default(gui_window_t *window, uint32_t id UNUSED) {
    internet_cpl_state_t *st = window
        ? (internet_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    internet_defaults(st);
    bk_gui_widget_set_text(st->home_box, INTERNET_DEFAULT_HOME);
    bk_gui_widget_set_text(st->search_box,st->search);
    bk_gui_widget_set_text(st->browser_box,st->browser);
    bk_gui_widget_set_text(st->editor_box,st->editor);
    internet_sync_buttons(st);
    bk_runtime_strcpy(st->status, "@H82EBD102");
    window->dirty = true;
}

static void internet_test(gui_window_t *window, uint32_t id UNUSED) {
    internet_cpl_state_t *st = window
        ? (internet_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    bk_runtime_strcpy(st->status, "@HD11129A2");
    window->dirty = true;
    bk_gui_request_paint();
    bk_sys_sleep_ms(1U);
    st->dns_tested = true;
    st->dns_ok = bk_net_resolve("www.google.com", st->dns_address,
                                st->timeout_ms);
    if (st->dns_ok) {
        snprintf(st->status, sizeof(st->status),
            "@H1C703501", st->dns_address[0],
            st->dns_address[1], st->dns_address[2], st->dns_address[3]);
    } else {
        bk_runtime_strcpy(st->status,
            "@H6700B719");
    }
    window->dirty = true;
}

static void internet_open_browser(gui_window_t *window, uint32_t id UNUSED) {
    internet_cpl_state_t *st = window
        ? (internet_cpl_state_t *)window->content_context : NULL;
    char home[256];
    if (!st) return;
    home[0] = '\0';
    (void)bk_gui_widget_get_text(st->home_box, home, sizeof(home));
    if (!home[0]) bk_runtime_strcpy(home, INTERNET_DEFAULT_HOME);
    bk_gui_widget_get_text(st->browser_box,st->browser,sizeof(st->browser));
    bk_runtime_strcpy(st->status,
        bk_app_launch(st->browser, home)
            ? "@HADE153A8" : "@H2E4349A3");
    window->dirty = true;
}

static const cpl_tab_spec_t tabs[]={{"General",80},{"Contenido",94},{"Conexion",90},{"Programas",100}};
static gui_rect_t internet_page(internet_cpl_state_t *st) {
    gui_rect_t c=bk_gui_window_content_rect_raw(st->window);
    return (gui_rect_t){c.x+8,c.y+30,c.w-16,c.h-100};
}
static gui_widget_t *internet_register(internet_cpl_state_t *st,gui_widget_t *widget,uint8_t tab) {
    if(widget && st->widget_count<24) {st->widgets[st->widget_count]=widget;st->widget_tabs[st->widget_count++]=tab;}
    return widget;
}
static void internet_visibility(internet_cpl_state_t *st) {
    for(int i=0;i<st->widget_count;i++) st->widgets[i]->visible=st->widget_tabs[i]==st->tab;
}
static void internet_associate(gui_window_t *window,uint32_t id UNUSED) {
    internet_cpl_state_t *st=window->content_context;
    if(!internet_save_config(st)) {bk_runtime_strcpy(st->status,"Revisa las rutas y la direccion de busqueda.");window->dirty=true;return;}
    void *raw=NULL;uint32_t size=0;char temporary[64];
    bool read=bk_file_read_all("/ASSOC.INI",&raw,&size);
    if(!read || !raw || size>32768U) {if(raw) bk_sys_free(raw);bk_runtime_strcpy(st->status,"No se pudo leer el archivo de asociaciones.");window->dirty=true;return;}
    char *output=bk_sys_alloc(size+1200U);if(!output){bk_sys_free(raw);return;}
    uint32_t used=0,pos=0;
    while(pos<size) {
        uint32_t begin=pos;while(pos<size && ((char *)raw)[pos]!='\n') pos++;if(pos<size) pos++;
        char *line=(char *)raw+begin;
        bool html=pos-begin>=5 && (!bk_runtime_strncmp(line,"HTML=",5) || !bk_runtime_strncmp(line,"html=",5));
        bool htm=pos-begin>=4 && (!bk_runtime_strncmp(line,"HTM=",4) || !bk_runtime_strncmp(line,"htm=",4));
        if(!html && !htm) {bk_runtime_memcpy(output+used,line,pos-begin);used+=pos-begin;}
    }
    if(used && output[used-1]!='\n') output[used++]='\n';
    used+=(uint32_t)snprintf(output+used,size+1200U-used,"HTML=/ICONS/INTERNET.BMP|%s|%s\nHTM=/ICONS/INTERNET.BMP|%s|%s\n",st->browser,st->editor,st->browser,st->editor);
    snprintf(temporary,sizeof(temporary),"/ASSOC.INTERNET.%u.NEW",bk_sys_getpid());
    bool ok=bk_file_write_all(temporary,output,used) && bk_file_rename(temporary,"/ASSOC.INI");
    bk_runtime_strcpy(st->status,ok ? "HTML y HTM usaran los programas elegidos." : "No se pudieron actualizar las asociaciones.");
    bk_sys_free(output);bk_sys_free(raw);window->dirty=true;
}
static void internet_refresh(gui_window_t *window,uint32_t id UNUSED) {
    internet_cpl_state_t *st=window->content_context;internet_scan_adapters(st);bk_net_get_info(&st->network);
    bk_runtime_strcpy(st->status,"Estado de la red actualizado.");window->dirty=true;
}
static bool internet_event(gui_window_t *window,const gui_event_t *event,void *context) {
    internet_cpl_state_t *st=context;
    if(event->type==GUI_EVENT_MOUSE_DOWN && event->button==MOUSE_LEFT_BUTTON) {
        int tab=cpl_hit_tab(bk_gui_window_content_rect_raw(window),tabs,4,12,8,event->x,event->y);
        if(tab>=0){st->tab=(uint8_t)tab;internet_visibility(st);window->dirty=true;return true;}
    }
    return false;
}
static void internet_paint(gui_window_t *window,gui_surface_t *surface,void *context) {
    internet_cpl_state_t *st=context;gui_rect_t c=bk_gui_window_content_rect_raw(window),p=internet_page(st);char line[128];
    cpl_draw_tabs(surface,c,tabs,4,st->tab,p,12,8);
    if(st->tab==0) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,110},"Pagina principal");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+44,p.w-56,18},"Direccion usada por el boton Inicio:",CPL_TEXT);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+154,p.w-32,130},"Busqueda en NetSurf");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+178,p.w-56,18},"Direccion del buscador, terminada en ?q=:",CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+254,p.w-56,18},"En NetSurf, escribi ? texto en la direccion.",BK_THEME_MUTED);
    } else if(st->tab==1) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,200},"Contenido de paginas");
        cpl_draw_text_wrapped(surface,(gui_rect_t){p.x+28,p.y+248,p.w-56,60},
            "Las cookies y el historial se mantienen en memoria y se descartan al cerrar NetSurf. Estas preferencias se leen al abrir otra ventana.",BK_THEME_MUTED,16,4);
    } else if(st->tab==2) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,174},"Red actual");
        snprintf(line,sizeof(line),"Adaptadores PCI: %u  Enlace: %s",st->ethernet_count,st->network.link_up ? "activo" : "sin conexion");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+42,p.w-56,16},line,CPL_TEXT);
        snprintf(line,sizeof(line),"IPv4: %u.%u.%u.%u",st->network.address[0],st->network.address[1],st->network.address[2],st->network.address[3]);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+76,p.w-56,16},line,CPL_TEXT);
        snprintf(line,sizeof(line),"DNS: %u.%u.%u.%u",st->network.dns[0],st->network.dns[1],st->network.dns[2],st->network.dns[3]);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+108,p.w-56,16},line,CPL_TEXT);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+218,p.w-32,94},"Tiempo de espera HTTP");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+238,p.w-56,16},"Limite para cada solicitud de NetSurf:",CPL_TEXT);
        snprintf(line,sizeof(line),"%u segundos",st->timeout_ms/1000U);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+p.w-142,p.y+238,114,16},line,BK_THEME_MUTED);
    } else {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,118},"Navegador predeterminado");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+42,p.w-56,16},"Ejecutable instalado, con ruta absoluta:",CPL_TEXT);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+160,p.w-32,116},"Editor de paginas HTML");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+184,p.w-56,16},"Programa usado para Editar HTML y HTM:",CPL_TEXT);
    }
    gui_rect_t status={c.x+8,c.y+c.h-24,c.w-16,20};cpl_draw_bevel(surface,status,CPL_FACE,true);
    cpl_draw_text_clipped(surface,cpl_inset(status,4),st->status,CPL_TEXT);
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    if(!desktop) return;
    internet_cpl_state_t *st=bk_sys_alloc_zero(sizeof(*st));if(!st) return;
    st->desktop=desktop;st->window=bk_gui_create_window(desktop,96,40,548,462,"Opciones de Internet");
    if(!st->window){bk_sys_free(st);return;}st->window->resizable=false;
    bk_gui_set_window_content(st->window,internet_paint,st);bk_gui_set_window_event_handler(st->window,internet_event,st);
    st->window->owner_pid=bk_sys_getpid();bk_proc_bind_window(st->window);
    gui_rect_t c=bk_gui_window_content_rect_raw(st->window),p=cpl_local_rect(internet_page(st),c);
    st->home_box=internet_register(st,bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+28,p.y+72,p.w-56,26},INTERNET_DEFAULT_HOME,255,NULL),0);
    st->search_box=internet_register(st,bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+28,p.y+208,p.w-56,26},"",255,NULL),0);
    st->browser_box=internet_register(st,bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+28,p.y+70,p.w-56,26},"",255,NULL),3);
    st->editor_box=internet_register(st,bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+28,p.y+210,p.w-56,26},"",255,NULL),3);
    st->images_button=internet_register(st,bk_gui_create_choice(desktop,st->window,(gui_rect_t){p.x+28,p.y+42,p.w-56,26},"Mostrar imagenes",false,internet_toggle),1);
    st->css_button=internet_register(st,bk_gui_create_choice(desktop,st->window,(gui_rect_t){p.x+28,p.y+94,p.w-56,26},"Cargar hojas de estilo",false,internet_toggle),1);
    st->cookies_button=internet_register(st,bk_gui_create_choice(desktop,st->window,(gui_rect_t){p.x+28,p.y+146,p.w-56,26},"Aceptar cookies de sesion",false,internet_toggle),1);
    st->images_id=bk_gui_widget_id(st->images_button);st->css_id=bk_gui_widget_id(st->css_button);st->cookies_id=bk_gui_widget_id(st->cookies_button);
    st->timeout_slider=internet_register(st,bk_gui_create_slider(desktop,st->window,
        (gui_rect_t){p.x+28,p.y+268,p.w-56,26},2,30,1,10,internet_timeout),2);
    internet_register(st,bk_gui_create_button(desktop,st->window,(gui_rect_t){p.x+28,p.y+146,174,26},"Probar DNS",internet_test),2);
    internet_register(st,bk_gui_create_button(desktop,st->window,(gui_rect_t){p.x+216,p.y+146,174,26},"Actualizar estado",internet_refresh),2);
    internet_register(st,bk_gui_create_button(desktop,st->window,(gui_rect_t){p.x+28,p.y+300,p.w-56,26},"Usar para archivos HTML y HTM",internet_associate),3);
    bk_gui_create_button(desktop,st->window,(gui_rect_t){14,c.h-58,156,26},"Abrir navegador",internet_open_browser);
    bk_gui_create_button(desktop,st->window,(gui_rect_t){180,c.h-58,156,26},"Restaurar ajustes",internet_default);
    bk_gui_create_button(desktop,st->window,(gui_rect_t){346,c.h-58,174,26},"Guardar",internet_save);
    internet_load(st);internet_scan_adapters(st);bk_net_get_info(&st->network);internet_sync_buttons(st);internet_visibility(st);
    bk_runtime_strcpy(st->status,"Los ajustes de contenido se leen al abrir NetSurf.");
    while(!bk_proc_exit_requested()&&st->window->listed) bk_sys_sleep_ticks(2);
    cpl_destroy_window(desktop,st->window);bk_sys_free(st);bk_proc_exit();
}
