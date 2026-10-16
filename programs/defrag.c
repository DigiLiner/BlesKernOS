#include "system_tools_common.h"

typedef struct {
    bk_gui_desktop_t *desktop;
    bk_gui_window_t *window;
    bk_gui_widget_t *analyze, *optimize, *stop, *close;
    bk_gui_rect_t client;
    bk_volume_info_t volume;
    bk_defrag_info_t info;
    uint32_t pending, last_refresh;
    bool available, running;
    char status[160];
} defrag_state_t;
static defrag_state_t *state;
static const uint32_t colors[] = {0x00A0A0A0,0x00FFFFFF,0x000000C0,0x00FF0000,0x00505050};

static void text(bk_gui_surface_t *surface,int x,int y,int width,const char *value) {
    char line[160];
    uint32_t count=width>0 ? (uint32_t)width/7 : 0;
    if (count>=sizeof(line)) count=sizeof(line)-1;
    st_copy(line,count+1,value);
    bk_gui_surface_draw_text(surface,x,y,line,ST_TEXT,0,false);
}
static void inset(bk_gui_surface_t *surface,bk_gui_rect_t r,uint32_t face) {
    bk_gui_surface_fill_rect(surface,r,face);
    bk_gui_surface_draw_rect(surface,r,ST_LIGHT);
    bk_gui_surface_fill_rect(surface,(bk_gui_rect_t){r.x,r.y,r.w-1,1},ST_DARK);
    bk_gui_surface_fill_rect(surface,(bk_gui_rect_t){r.x,r.y,1,r.h-1},ST_DARK);
    bk_gui_surface_draw_rect(surface,(bk_gui_rect_t){r.x+1,r.y+1,r.w-3,r.h-3},ST_SHADOW);
}
static uint32_t progress(const bk_defrag_info_t *info) {
    if (info->state==BK_DEFRAG_COMPLETE) return 100;
    if (!info->work_blocks) return 0;
    uint32_t value=(uint32_t)((uint64_t)info->processed_blocks*100/info->work_blocks);
    return value>100 ? 100 : value;
}
static void paint(bk_gui_window_t *w,bk_gui_surface_t *surface,void *context) {
    (void)w;
    defrag_state_t *s=context;
    bk_gui_rect_t c=s->client;
    bk_gui_surface_fill_rect(surface,c,ST_FACE);
    bk_gui_rect_t details={c.x+10,c.y+10,c.w-20,48};
    inset(surface,details,ST_FACE);
    char line[160],number[24];
    st_copy(line,sizeof(line),"Unidad: ");
    st_append(line,sizeof(line),s->volume.device_name);
    st_append(line,sizeof(line),"   Sistema: ");
    st_append(line,sizeof(line),s->volume.filesystem);
    text(surface,details.x+8,details.y+8,details.w-16,line);
    st_copy(line,sizeof(line),"Fragmentados: ");
    st_u32(number,sizeof(number),s->info.fragmented_before);st_append(line,sizeof(line),number);
    st_append(line,sizeof(line),"   Reorganizados: ");
    st_u32(number,sizeof(number),s->info.moved_files);st_append(line,sizeof(line),number);
    st_append(line,sizeof(line),"   Omitidos: ");
    st_u32(number,sizeof(number),s->info.skipped_files);st_append(line,sizeof(line),number);
    text(surface,details.x+8,details.y+28,details.w-16,line);
    text(surface,c.x+12,c.y+66,c.w-24,"Mapa del disco - cada cuadro agrupa bloques de 4 KiB");
    bk_gui_rect_t map={c.x+10,c.y+84,c.w-20,c.h-208};
    inset(surface,map,ST_LIGHT);
    int cell_w=(map.w-12)/32, cell_h=(map.h-12)/16;
    for (uint32_t i=0;i<BK_DEFRAG_CELLS;i++) {
        uint32_t value=s->info.cells[i];
        if (value>BK_DEFRAG_RESERVED) value=BK_DEFRAG_PENDING;
        bk_gui_surface_fill_rect(surface,
            (bk_gui_rect_t){map.x+6+(int)(i%32)*cell_w,map.y+6+(int)(i/32)*cell_h,
                            cell_w-2,cell_h-2},colors[value]);
    }
    const char *legend[]={"Pendiente","Libre","Procesado","Error","Sistema"};
    int legend_width=(c.w-24)/5;
    for (int i=0;i<5;i++) {
        int x=c.x+12+i*legend_width,y=map.y+map.h+12;
        bk_gui_surface_fill_rect(surface,(bk_gui_rect_t){x,y,10,10},colors[i]);
        bk_gui_surface_draw_rect(surface,(bk_gui_rect_t){x,y,10,10},ST_SHADOW);
        text(surface,x+15,y+1,legend_width-17,legend[i]);
    }
    uint32_t percent=progress(&s->info);
    st_u32(line,sizeof(line),percent);st_append(line,sizeof(line),"%   ");
    st_append(line,sizeof(line),s->info.current_file);
    text(surface,c.x+12,c.y+c.h-88,c.w-24,line);
    bk_gui_rect_t bar={c.x+10,c.y+c.h-68,c.w-20,18};
    inset(surface,bar,ST_LIGHT);
    bk_gui_surface_fill_rect(surface,(bk_gui_rect_t){bar.x+3,bar.y+3,
        (int)((uint32_t)(bar.w-6)*percent/100),bar.h-6},0x000000C0);
    text(surface,c.x+12,c.y+c.h-44,c.w-24,s->status);
}
static void action(bk_gui_window_t *w,uint32_t id) {
    defrag_state_t *s=state;
    if (!s) return;
    if (id==bk_gui_widget_id(s->close)) bk_gui_close_window(w);
    else if (id==bk_gui_widget_id(s->stop)) s->pending=3;
    else if (!s->running) s->pending=id==bk_gui_widget_id(s->analyze) ? 1 : 2;
}
static void layout(defrag_state_t *s) {
    bk_gui_window_content_rect(s->window,&s->client);
    int y=s->client.h-30;
    bk_gui_widget_set_bounds(s->window,s->analyze,(bk_gui_rect_t){10,y,105,24});
    bk_gui_widget_set_bounds(s->window,s->optimize,(bk_gui_rect_t){121,y,135,24});
    bk_gui_widget_set_bounds(s->window,s->stop,(bk_gui_rect_t){262,y,100,24});
    bk_gui_widget_set_bounds(s->window,s->close,(bk_gui_rect_t){s->client.w-110,y,100,24});
}
static void describe(defrag_state_t *s) {
    if (s->info.state==BK_DEFRAG_COMPLETE) {
        st_copy(s->status,sizeof(s->status),s->info.optimize ? "Desfragmentacion terminada."
            : "Analisis terminado: los bloques azules se verificaron por lectura.");
        if (s->info.optimize && s->info.fragmented_after) {
            st_copy(s->status,sizeof(s->status),"Terminado: quedan archivos fragmentados; revise omitidos y espacio libre.");
        }
    } else if (s->info.state==BK_DEFRAG_CANCELLED)
        st_copy(s->status,sizeof(s->status),"Operacion detenida. Se conservan los archivos y los cambios ya completados.");
    else if (s->info.state==BK_DEFRAG_ERROR)
        st_copy(s->status,sizeof(s->status),s->info.error==BK_DEFRAG_IO_ERROR ?
            "Error de lectura/escritura: operacion detenida. Consulte el visor de eventos."
            : "No se pudo continuar. Compruebe el volumen y la memoria disponible.");
}
void bleskernos_program_main(bk_gui_desktop_t *desktop) {
    if (bk_sys_api_version()<36) return;
    defrag_state_t *s=bk_sys_alloc(sizeof(*s));
    if (!s) return;
    st_zero(s,sizeof(*s));state=s;s->desktop=desktop;
    bk_device_volume_info(&s->volume);
    s->available=bk_device_defrag_status(&s->info);
    s->window=bk_gui_create_window(desktop,70,45,660,455,"Desfragmentador de disco");
    if (!s->window) {state=NULL;bk_sys_free(s);return;}
    bk_proc_bind_window(s->window);
    bk_gui_set_window_min_size(s->window,640,420);
    s->analyze=bk_gui_create_button(desktop,s->window,(bk_gui_rect_t){10,0,105,24},"Analizar",action);
    s->optimize=bk_gui_create_button(desktop,s->window,(bk_gui_rect_t){121,0,135,24},"Desfragmentar",action);
    s->stop=bk_gui_create_button(desktop,s->window,(bk_gui_rect_t){262,0,100,24},"Detener",action);
    s->close=bk_gui_create_button(desktop,s->window,(bk_gui_rect_t){540,0,100,24},"Cerrar",action);
    layout(s);
    bk_gui_window_set_compact_content(s->window,true);
    bk_gui_set_window_content(s->window,paint,s);
    st_copy(s->status,sizeof(s->status),s->available ? "Seleccione Analizar o Desfragmentar. La unidad activa usa BFS4."
        : "Esta version admite el volumen BFS4 activo; FAT y CD-ROM no se desfragmentan.");
    while (!bk_proc_exit_requested() && bk_gui_window_is_open(s->window)) {
        if (s->pending) {
            uint32_t request=s->pending;s->pending=0;
            if (request==3) {if (s->running) bk_device_defrag_cancel();s->running=false;}
            else if (!s->available)
                st_copy(s->status,sizeof(s->status),"Volumen no compatible: solo BFS4 activo.");
            else {
                s->running=bk_device_defrag_begin(request==2);
                st_copy(s->status,sizeof(s->status),s->running ?
                    (request==2 ? "Copiando y verificando bloques antes de reorganizar archivos..." : "Leyendo y verificando bloques...")
                    : "No se pudo iniciar: otra operacion activa, volumen protegido o inconsistente.");
            }
            if (s->available) bk_device_defrag_status(&s->info);
            describe(s);bk_gui_window_invalidate(s->window);
        }
        if (s->running) {
            bool ok=bk_device_defrag_step();
            bk_device_defrag_status(&s->info);
            s->running=ok && s->info.state==BK_DEFRAG_RUNNING;
            describe(s);
        }
        if (bk_sys_uptime_ms()-s->last_refresh>=100) {
            s->last_refresh=bk_sys_uptime_ms();layout(s);
            bk_gui_window_invalidate(s->window);
        }
        bk_sys_sleep_ms(s->running ? 10 : 30);
    }
    if (s->running) bk_device_defrag_cancel();
    state=NULL;bk_gui_destroy_window(desktop,s->window);bk_sys_free(s);
}
