#include "kernel/include/api.h"
#include "control_common.h"

#define MODEM_CONFIG_PATH "/SYSTEM/USER/CONFIG/MODEM.INI"

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_widget_t *prefix_box,*phone_box,*init_box;
    gui_widget_t *widgets[48];uint8_t widget_tabs[48],widget_count,tab;
    bk_pci_info_t modems[16];uint8_t listed,page;int selected;
    uint8_t data_bits,stop_bits,parity,flow;
    char phone[48],init[64];
    gui_widget_t *tone_button;
    gui_widget_t *speaker_button;
    gui_widget_t *port_choice,*speed_choice,*format_choices[4];
    uint32_t tone_id;
    uint32_t speaker_id;
    uint8_t port;
    uint32_t baud;
    bool tone;
    bool speaker;
    uint32_t modem_count;
    uint32_t serial_count;
    char status[112];
} modem_cpl_state_t;

static void modem_defaults(modem_cpl_state_t *st) {
    if (!st) return;
    st->port = 1U;
    st->baud = 57600U;
    st->tone = true;
    st->speaker = true;
    st->data_bits=8;st->stop_bits=1;st->parity=0;st->flow=1;
    bk_runtime_strcpy(st->phone,"");bk_runtime_strcpy(st->init,"ATZ");
}

static bool modem_equal(const char *left, const char *right) {
    uint32_t i = 0U;
    if (!left || !right) return false;
    while (left[i] && right[i] && left[i] == right[i]) i++;
    return left[i] == '\0' && right[i] == '\0';
}

static uint32_t modem_parse_uint(const char *text, uint32_t fallback) {
    uint32_t value = 0U;
    bool digit = false;
    while (text && *text >= '0' && *text <= '9') {
        digit = true;
        value = value * 10U + (uint32_t)(*text++ - '0');
        if (value > 115200U) return fallback;
    }
    return digit ? value : fallback;
}

static void modem_apply_line(modem_cpl_state_t *st, const char *line,
                             char *prefix, uint32_t prefix_capacity) {
    char key[24], value[64];
    uint32_t split = 0U, i = 0U, j = 0U;
    if (!st || !line) return;
    while (line[split] && line[split] != '=') split++;
    if (line[split] != '=') return;
    while (i < split && i + 1U < sizeof(key)) { key[i] = line[i]; i++; }
    key[i] = '\0';
    i = split + 1U;
    while (line[i] && j + 1U < sizeof(value)) value[j++] = line[i++];
    value[j] = '\0';
    if (modem_equal(key, "port")) {
        if (value[0] == 'C' && value[1] == 'O' && value[2] == 'M' &&
            (value[3] == '1' || value[3] == '2'))
            st->port = (uint8_t)(value[3] - '0');
    } else if (modem_equal(key, "baud")) {
        uint32_t baud = modem_parse_uint(value, st->baud);
        if (baud == 9600U || baud == 33600U || baud == 57600U ||
            baud == 115200U) st->baud = baud;
    } else if (modem_equal(key, "prefix") && value[0]) {
        bk_runtime_strncpy(prefix, value, prefix_capacity - 1U);
        prefix[prefix_capacity - 1U] = '\0';
    } else if (modem_equal(key, "tone")) st->tone = value[0] != '0';
    else if (modem_equal(key, "speaker")) st->speaker = value[0] != '0';
    else if(modem_equal(key,"data_bits") && (value[0]=='7'||value[0]=='8')) st->data_bits=(uint8_t)(value[0]-'0');
    else if(modem_equal(key,"stop_bits") && (value[0]=='1'||value[0]=='2')) st->stop_bits=(uint8_t)(value[0]-'0');
    else if(modem_equal(key,"parity") && value[0]>='0' && value[0]<='2') st->parity=(uint8_t)(value[0]-'0');
    else if(modem_equal(key,"flow") && value[0]>='0' && value[0]<='2') st->flow=(uint8_t)(value[0]-'0');
    else if(modem_equal(key,"phone")) bk_runtime_strncpy(st->phone,value,sizeof(st->phone)-1);
    else if(modem_equal(key,"init")) bk_runtime_strncpy(st->init,value,sizeof(st->init)-1);
}

static void modem_config_path(modem_cpl_state_t *st,char *path,uint32_t capacity) {
    if(st->selected>=0 && st->selected<st->listed) {
        bk_pci_info_t *d=&st->modems[st->selected];
        snprintf(path,capacity,"/SYSTEM/USER/CONFIG/MODEM-%02X-%02X-%u.INI",d->bus,d->slot,d->function);
    } else bk_runtime_strncpy(path,MODEM_CONFIG_PATH,capacity-1);
}

static void modem_load(modem_cpl_state_t *st) {
    void *raw = NULL;
    uint32_t size = 0U, position = 0U;
    char prefix[32], line[96];
    if (!st) return;
    bk_runtime_strcpy(prefix, "ATDT");
    char path[96];modem_config_path(st,path,sizeof(path));
    if (bk_file_read_all(path, &raw, &size) && raw) {
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
            modem_apply_line(st, line, prefix, sizeof(prefix));
        }
        bk_sys_free(raw);
    }
    if (st->prefix_box) bk_gui_widget_set_text(st->prefix_box, prefix);
    bk_gui_widget_set_text(st->phone_box,st->phone);
    bk_gui_widget_set_text(st->init_box,st->init);
}

static void modem_detect(modem_cpl_state_t *st) {
    uint32_t count, i;
    if (!st) return;
    st->listed=0;st->selected=-1;st->page=0;
    st->modem_count = 0U;
    st->serial_count = 0U;
    count = bk_device_pci_count();
    for (i = 0U; i < count; i++) {
        bk_pci_info_t info;
        if (!bk_device_pci_info(i, &info) || info.class_code != 0x07U)
            continue;
        if (info.subclass == 0x03U) {st->modem_count++;if(st->listed<16) st->modems[st->listed++]=info;}
        if (info.subclass == 0x00U) st->serial_count++;
    }
}

static void modem_sync_buttons(modem_cpl_state_t *st) {
    if (!st) return;
    if(st->tone_button) bk_gui_choice_set_checked(st->tone_button,st->tone);
    if(st->speaker_button) bk_gui_choice_set_checked(st->speaker_button,st->speaker);
    if(st->port_choice) bk_gui_widget_dropdown_set_selected(st->port_choice,st->port-1);
    static const uint32_t speeds[]={9600,33600,57600,115200};
    for(int i=0;i<4;i++) if(st->speed_choice && st->baud==speeds[i]) bk_gui_widget_dropdown_set_selected(st->speed_choice,i);
    uint32_t indexes[]={st->data_bits-7,st->parity,st->stop_bits-1,st->flow};
    for(int i=0;i<4;i++) if(st->format_choices[i]) bk_gui_widget_dropdown_set_selected(st->format_choices[i],indexes[i]);

}

static bool modem_save_config(modem_cpl_state_t *st) {
    char prefix[32];
    char data[512];
    if (!st) return false;
    prefix[0] = '\0';
    if (st->prefix_box)
        (void)bk_gui_widget_get_text(st->prefix_box, prefix, sizeof(prefix));
    if (!prefix[0]) bk_runtime_strcpy(prefix, "ATDT");
    bk_gui_widget_get_text(st->phone_box,st->phone,sizeof(st->phone));
    bk_gui_widget_get_text(st->init_box,st->init,sizeof(st->init));
    snprintf(data, sizeof(data),
        "port=COM%u\r\nbaud=%u\r\nprefix=%s\r\ntone=%u\r\nspeaker=%u\r\ndata_bits=%u\r\nstop_bits=%u\r\nparity=%u\r\nflow=%u\r\nphone=%s\r\ninit=%s\r\n",
        st->port, st->baud, prefix, st->tone ? 1U : 0U,
        st->speaker ? 1U : 0U,st->data_bits,st->stop_bits,st->parity,st->flow,st->phone,st->init);
    char path[96];modem_config_path(st,path,sizeof(path));
    return bk_file_write_all(path, data,
                             (uint32_t)bk_runtime_strlen(data));
}

static void modem_select(gui_window_t *window, uint32_t id) {
    modem_cpl_state_t *st = window
        ? (modem_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    if(st->port_choice && id==st->port_choice->id) st->port=1+bk_gui_widget_dropdown_get_selected(st->port_choice);
    if(st->speed_choice && id==st->speed_choice->id) {
        static const uint32_t speeds[]={9600,33600,57600,115200};
        int i=bk_gui_widget_dropdown_get_selected(st->speed_choice);if(i>=0 && i<4) st->baud=speeds[i];
    }
    if(id==st->tone_id) st->tone=bk_gui_choice_is_checked(st->tone_button);
    if(id==st->speaker_id) st->speaker=bk_gui_choice_is_checked(st->speaker_button);
    if(id==st->tone_id) bk_gui_widget_set_text(st->prefix_box,st->tone ? "ATDT" : "ATDP");
    modem_sync_buttons(st);
    snprintf(st->status, sizeof(st->status), "@H7BB121F9",
             st->port, st->baud);
    window->dirty = true;
}

static void modem_scan(gui_window_t *window, uint32_t id UNUSED) {
    modem_cpl_state_t *st = window
        ? (modem_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    modem_detect(st);
    if (st->modem_count) {
        snprintf(st->status, sizeof(st->status),
            "@HB822C4DB", st->modem_count);
    } else if (st->serial_count) {
        snprintf(st->status, sizeof(st->status),
            "@H3E80AB24", st->serial_count);
    } else {
        bk_runtime_strcpy(st->status,
            "@H080C846C");
    }
    window->dirty = true;
}

static void modem_save(gui_window_t *window, uint32_t id UNUSED) {
    modem_cpl_state_t *st = window
        ? (modem_cpl_state_t *)window->content_context : NULL;
    if (!st) return;
    bk_runtime_strcpy(st->status, modem_save_config(st)
        ? "@H7E52A0A4"
        : "@H7E3D9A6E");
    window->dirty = true;
}

static const cpl_tab_spec_t tabs[]={{"Modems",78},{"Conexion",90},{"Marcacion",96},{"Diagnostico",104}};
static gui_rect_t modem_page(modem_cpl_state_t *st) {
    gui_rect_t c=bk_gui_window_content_rect_raw(st->window);return (gui_rect_t){c.x+8,c.y+30,c.w-16,c.h-100};
}
static gui_widget_t *modem_register(modem_cpl_state_t *st,gui_widget_t *w,uint8_t tab) {
    if(w && st->widget_count<48){st->widgets[st->widget_count]=w;st->widget_tabs[st->widget_count++]=tab;}return w;
}
static void modem_visibility(modem_cpl_state_t *st) {
    for(int i=0;i<st->widget_count;i++) st->widgets[i]->visible=st->widget_tabs[i]==st->tab;
}
static void modem_framing(gui_window_t *window,uint32_t id) {
    modem_cpl_state_t *st=window->content_context;
    for(int i=0;i<4;i++) if(st->format_choices[i] && st->format_choices[i]->id==id) {
        int v=bk_gui_widget_dropdown_get_selected(st->format_choices[i]);
        if(v<0) return;
        if(i==0) st->data_bits=7+v;
        if(i==1) st->parity=v;
        if(i==2) st->stop_bits=1+v;
        if(i==3) st->flow=v;
    }
    bk_runtime_strcpy(st->status,"Perfil modificado. Guarda para conservarlo.");window->dirty=true;
}
static void modem_next(gui_window_t *window,uint32_t id UNUSED) {
    modem_cpl_state_t *st=window->content_context;
    st->page=(uint8_t)((st->page+1)%((st->listed+5U)/6U ? (st->listed+5U)/6U : 1U));window->dirty=true;
}
static void modem_configure(gui_window_t *window,uint32_t id UNUSED) {
    modem_cpl_state_t *st=window->content_context;st->tab=1;modem_visibility(st);window->dirty=true;
}
static bool modem_event(gui_window_t *window,const gui_event_t *event,void *context) {
    modem_cpl_state_t *st=context;
    if(event->type!=GUI_EVENT_MOUSE_DOWN || event->button!=MOUSE_LEFT_BUTTON) return false;
    int tab=cpl_hit_tab(bk_gui_window_content_rect_raw(window),tabs,4,12,8,event->x,event->y);
    if(tab>=0){st->tab=(uint8_t)tab;modem_visibility(st);window->dirty=true;return true;}
    gui_rect_t p=modem_page(st);
    if(st->tab==0) for(int i=0;i<6;i++) {
        int index=st->page*6+i;
        if(index<st->listed && bk_gui_rect_contains((gui_rect_t){p.x+20,p.y+46+i*30,p.w-40,28},event->x,event->y)) {
            st->selected=index;modem_defaults(st);modem_load(st);modem_sync_buttons(st);
            snprintf(st->status,sizeof(st->status),"Seleccionado: PCI %02X:%02X.%u",st->modems[index].bus,st->modems[index].slot,st->modems[index].function);
            window->dirty=true;return true;
        }
    }
    return false;
}
static void modem_paint(gui_window_t *window,gui_surface_t *surface,void *context) {
    modem_cpl_state_t *st=context;gui_rect_t c=bk_gui_window_content_rect_raw(window),p=modem_page(st);char line[192];
    cpl_draw_tabs(surface,c,tabs,4,st->tab,p,12,8);
    if(st->tab==0) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,216},"Modems detectados");
        if(!st->listed) cpl_draw_text_wrapped(surface,(gui_rect_t){p.x+28,p.y+52,p.w-56,66},"No se detectaron modems PCI. Los dispositivos serie externos requieren un driver con deteccion AT.",CPL_TEXT,16,4);
        for(int i=0;i<6;i++) {
            int index=st->page*6+i;if(index>=st->listed) break;
            gui_rect_t row={p.x+20,p.y+46+i*30,p.w-40,28};bool selected=st->selected==index;
            if(selected) bk_gui_gfx_fill_rect(surface,row,CPL_BLUE);
            bk_pci_info_t *d=&st->modems[index];
            snprintf(line,sizeof(line),"PCI %02X:%02X.%u  %04X:%04X  IRQ %u",d->bus,d->slot,d->function,d->vendor_id,d->device_id,d->interrupt_line);
            cpl_draw_text_clipped(surface,cpl_inset(row,6),line,selected ? BK_THEME_SELECTION_TEXT : CPL_TEXT);
        }
        snprintf(line,sizeof(line),"Modems: %u  Controladores serie PCI: %u",st->modem_count,st->serial_count);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+286,p.w-56,18},line,CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+314,p.w-56,18},"Los controladores serie no se cuentan como modems.",BK_THEME_MUTED);
    } else if(st->tab==1) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,130},"Puerto y velocidad maxima");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+42,100,18},"Puerto:",CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+94,100,18},"Bits/s:",CPL_TEXT);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+172,p.w-32,170},"Formato de conexion");
        const char *parities[]={"Ninguna","Par","Impar"},*flows[]={"Ninguno","RTS/CTS","XON/XOFF"};
        snprintf(line,sizeof(line),"Actual: %u bits / %s / %u parada / %s",st->data_bits,parities[st->parity],st->stop_bits,flows[st->flow]);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+194,p.w-56,18},line,CPL_TEXT);
        const char *labels[]={"Datos","Paridad","Bits de parada","Control de flujo"};
        for(int i=0;i<4;i++) cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28+(i%2)*232,p.y+216+(i/2)*58,210,14},labels[i],CPL_TEXT);
    } else if(st->tab==2) {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,148},"Marcacion y altavoz");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+44,130,18},"Modo:",CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+88,130,18},"Altavoz:",CPL_TEXT);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+192,p.w-32,152},"Comandos del perfil");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+214,150,18},"Marcar / numero:",CPL_TEXT);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+274,150,18},"Inicializacion:",CPL_TEXT);
    } else {
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+20,p.w-32,174},"Estado del dispositivo");
        if(st->selected>=0) {
            bk_pci_info_t *d=&st->modems[st->selected];snprintf(line,sizeof(line),"Identificador PCI: %04X:%04X",d->vendor_id,d->device_id);
        } else bk_runtime_strcpy(line,"Perfil generico: sin dispositivo seleccionado");
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+46,p.w-56,18},line,CPL_TEXT);
        snprintf(line,sizeof(line),"Puerto configurado: COM%u  Velocidad: %u",st->port,st->baud);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+82,p.w-56,18},line,CPL_TEXT);
        cpl_draw_text_wrapped(surface,(gui_rect_t){p.x+28,p.y+122,p.w-56,48},"Driver de marcacion: no disponible. El perfil se guarda sin enviar comandos al dispositivo.",BK_THEME_MUTED,16,3);
        cpl_draw_group(surface,(gui_rect_t){p.x+16,p.y+218,p.w-32,120},"Preparacion de comandos AT");
        char prefix[32];bk_gui_widget_get_text(st->prefix_box,prefix,sizeof(prefix));bk_gui_widget_get_text(st->phone_box,st->phone,sizeof(st->phone));bk_gui_widget_get_text(st->init_box,st->init,sizeof(st->init));
        snprintf(line,sizeof(line),"Inicializar: %s  Altavoz: ATM%u",st->init,st->speaker?1U:0U);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+246,p.w-56,18},line,CPL_TEXT);
        snprintf(line,sizeof(line),"Marcar: %s%s",prefix,st->phone);
        cpl_draw_text_clipped(surface,(gui_rect_t){p.x+28,p.y+278,p.w-56,18},line,CPL_TEXT);
    }
    gui_rect_t status={c.x+8,c.y+c.h-24,c.w-16,20};cpl_draw_bevel(surface,status,CPL_FACE,true);
    cpl_draw_text_clipped(surface,cpl_inset(status,4),st->status,CPL_TEXT);
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    if(!desktop) return;
    modem_cpl_state_t *st=bk_sys_alloc_zero(sizeof(*st));if(!st) return;
    st->desktop=desktop;st->selected=-1;modem_defaults(st);
    st->window=bk_gui_create_window(desktop,112,40,548,484,"Propiedades de modem");if(!st->window){bk_sys_free(st);return;}
    st->window->resizable=false;bk_gui_set_window_content(st->window,modem_paint,st);bk_gui_set_window_event_handler(st->window,modem_event,st);
    st->window->owner_pid=bk_sys_getpid();bk_proc_bind_window(st->window);
    gui_rect_t c=bk_gui_window_content_rect_raw(st->window),p=cpl_local_rect(modem_page(st),c);
    st->port_choice=modem_register(st,bk_gui_widget_create_dropdown(desktop,st->window,(gui_rect_t){p.x+144,p.y+36,220,26},modem_select),1);
    bk_gui_widget_dropdown_add_item(st->port_choice,"COM1","1");bk_gui_widget_dropdown_add_item(st->port_choice,"COM2","2");
    st->speed_choice=modem_register(st,bk_gui_widget_create_dropdown(desktop,st->window,(gui_rect_t){p.x+144,p.y+88,220,26},modem_select),1);
    const char *speeds[]={"9600","33600","57600","115200"};
    for(int i=0;i<4;i++) bk_gui_widget_dropdown_add_item(st->speed_choice,speeds[i],speeds[i]);
    const char *choices[4][3]={{"7 bits","8 bits",NULL},{"Ninguna","Par","Impar"},{"1 bit","2 bits",NULL},{"Sin flujo","RTS/CTS","XON/XOFF"}};
    for(int i=0;i<4;i++) {
        int x=p.x+28+(i%2)*232,y=p.y+232+(i/2)*58;
        st->format_choices[i]=modem_register(st,bk_gui_widget_create_dropdown(desktop,st->window,(gui_rect_t){x,y,210,26},modem_framing),1);
        for(int j=0;j<3 && choices[i][j];j++) bk_gui_widget_dropdown_add_item(st->format_choices[i],choices[i][j],choices[i][j]);
    }
    st->tone_button=modem_register(st,bk_gui_create_choice(desktop,st->window,(gui_rect_t){p.x+144,p.y+38,270,26},"Marcar por tonos (sin cruz: pulsos)",false,modem_select),2);
    st->speaker_button=modem_register(st,bk_gui_create_choice(desktop,st->window,(gui_rect_t){p.x+144,p.y+82,270,26},"Activar altavoz",false,modem_select),2);
    st->tone_id=bk_gui_widget_id(st->tone_button);st->speaker_id=bk_gui_widget_id(st->speaker_button);
    st->prefix_box=modem_register(st,bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+28,p.y+238,96,26},"ATDT",31,NULL),2);
    st->phone_box=modem_register(st,bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+136,p.y+238,p.w-164,26},"",47,NULL),2);
    st->init_box=modem_register(st,bk_gui_create_textbox(desktop,st->window,(gui_rect_t){p.x+28,p.y+300,p.w-56,26},"ATZ",63,NULL),2);
    modem_register(st,bk_gui_create_button(desktop,st->window,(gui_rect_t){p.x+28,p.y+248,140,26},"Volver a detectar",modem_scan),0);
    modem_register(st,bk_gui_create_button(desktop,st->window,(gui_rect_t){p.x+176,p.y+248,140,26},"Configurar perfil",modem_configure),0);
    modem_register(st,bk_gui_create_button(desktop,st->window,(gui_rect_t){p.x+324,p.y+248,140,26},"Otra pagina",modem_next),0);
    bk_gui_create_button(desktop,st->window,(gui_rect_t){c.w-194,c.h-58,180,26},"Guardar perfil",modem_save);
    modem_detect(st);modem_load(st);modem_sync_buttons(st);modem_visibility(st);
    bk_runtime_strcpy(st->status,"Selecciona un modem o configura un perfil generico.");
    while(!bk_proc_exit_requested()&&st->window->listed) bk_sys_sleep_ticks(2);
    cpl_destroy_window(desktop,st->window);bk_sys_free(st);bk_proc_exit();
}
