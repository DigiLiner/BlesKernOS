/* SPDX-License-Identifier: MIT */
#include "kernel/include/api.h"
#include "system/screensavers/jelly_settings.h"

typedef struct {
    gui_desktop_t *desktop;
    gui_program_t *program;
    uint32_t owner,worker,input_armed_ms;
    volatile uint32_t dismissed,done;
    int status;
    char arguments[96];
} ss_jelly_state;

static bool ss_jelly_input(const gui_event_t *event) {
    return event && (event->type==GUI_EVENT_KEY || event->type==GUI_EVENT_MOUSE_MOVE ||
        event->type==GUI_EVENT_MOUSE_DOWN || event->type==GUI_EVENT_MOUSE_UP ||
        event->type==GUI_EVENT_MOUSE_WHEEL);
}

static bool ss_jelly_event(gui_program_t *program,gui_desktop_t *desktop,const gui_event_t *event) {
    ss_jelly_state *state=program->state;(void)desktop;
    if(!ss_jelly_input(event))return false;
    if((int32_t)(bk_sys_uptime_ms()-state->input_armed_ms)>=0)
        __atomic_store_n(&state->dismissed,1U,__ATOMIC_RELEASE);
    return true; /* The dismissal gesture never reaches the underlying app. */
}

static void ss_jelly_message(gui_program_t *program,gui_desktop_t *desktop,gui_surface_t *surface) {
    ss_jelly_state *state=program->state;(void)desktop;
    /* Native Linux ELF exits can be encoded as wait statuses (exit << 8).
     * In particular, "Gallium no esta disponible" was shown for *any*
     * error, even a missing binary or a failed fullscreen request. */
    const char *message;
    if(state->status==-2)
        message="GLMARK2.ELF no encontrado";
    else if(state->status==-1)
        message="No se pudo iniciar GLMARK2";
    else if(state->status==2 || state->status==512)
        message="Error al crear contexto 3D";
    else if(state->status==3 || state->status==768)
        message="No se pudo cargar la medusa";
    else
        message="Jelly termino con un error";
    bk_gui_gfx_clear(surface,0x00000000);
    int width=0;while(message[width])width++;
    int x=((int)surface->width-width*8)/2;
    if(x<0)x=0;
    bk_gui_font_draw_string(surface,x,(int)surface->height/2-8,message,0x00FFFFFF,0,false);
}

static void ss_jelly_worker(void *argument) {
    ss_jelly_state *state=argument;
    state->status=0;
    if(!__atomic_load_n(&state->dismissed,__ATOMIC_ACQUIRE)) {
        vfs_dir_entry_t file;
        if(!bk_file_stat("/SYSTEM/PROGRAMS/GLMARK2.ELF",&file) ||
           file.type==VFS_NODE_DIR) {
            state->status=-2;
        } else {
            state->status=bk_app_run_sync("/SYSTEM/PROGRAMS/GLMARK2.ELF",
                                          state->arguments);
        }
        if(state->status)
            kprintf("[SSJELLY] GLMARK2 launch/exit status=%d args=%s\n",
                    state->status,state->arguments);
    }
    __atomic_store_n(&state->done,1U,__ATOMIC_RELEASE);
    bk_proc_exit();
}

/* The worker performing bk_app_run_sync is the only task that knows the
 * PID returned by elf_spawn_program_ex. Requesting its exit lets run_sync
 * cancel its exact child, without scanning unrelated processes by name. */
static void ss_jelly_stop_child(ss_jelly_state *state) {
    if (!state->worker) return;
    bool ok=bk_proc_request_exit(state->worker);
    kprintf("[SSJELLY] launcher cancellation worker=%u sent=%u\n",
            state->worker,ok?1U:0U);
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    if(!desktop)desktop=bk_gui_desktop();
    if(!desktop)return;
    uint32_t owner=bk_sys_getpid();
    for(uint32_t i=0;i<bk_proc_count();i++) {
        const task_t *task=bk_proc_get(i);
        if(task && task->process_id!=owner && task->state!=TASK_UNUSED && task->state!=TASK_ZOMBIE &&
           !bk_runtime_strcmp(task->name,"SSJELLY.SCV"))return;
    }
    ss_jelly_state *state=bk_sys_alloc_zero(sizeof(*state));
    if(!state)return;
    state->desktop=desktop;state->owner=owner;
    state->input_armed_ms=bk_sys_uptime_ms()+500U;
    /* Preserve the display aspect ratio while keeping saver GPU work modest. */
    unsigned width=640,height=desktop->surface.width ? (unsigned)desktop->surface.height*640U/desktop->surface.width : 480U;
    if(height>480U){width=width*480U/height;height=480U;}
    if(width<160U)width=160U;
    if(height<90U)height=90U;
    bk_jelly_settings_t settings;
    bk_jelly_load(&settings);
    snprintf(state->arguments,sizeof(state->arguments),
             "--bkg-screensaver --bkg-jellies=%u --bkg-variety=%u --size %ux%u",
             settings.count,settings.variation,width,height);
    kprintf("[SSJELLY] aquarium count=%u variation=%u\n",settings.count,settings.variation);
    /* Event-only registration protects the desktop without adding a CPU
     * overlay over the child's GPU viewport. Both processes retain ownership. */
    state->program=bk_gui_register_program(desktop,"ss_jelly_input",state,NULL,ss_jelly_event,NULL);
    if(!state->program){bk_sys_free(state);return;}
    int worker=bk_proc_spawn_thread("jelly-launch",ss_jelly_worker,state);
    if(worker<0){
        kprintf("[SSJELLY] no se pudo crear jelly-launch\n");
        state->status=-1;state->done=1;
    }else state->worker=(uint32_t)worker;
    bool stop_sent=false;
    while(!__atomic_load_n(&state->done,__ATOMIC_ACQUIRE)) {
        if(bk_proc_exit_requested())__atomic_store_n(&state->dismissed,1U,__ATOMIC_RELEASE);
        if(__atomic_load_n(&state->dismissed,__ATOMIC_ACQUIRE)) {
            if(!stop_sent) {
                stop_sent=true;
                kprintf("[SSJELLY] input dismissal; cancelling launcher\n");
                ss_jelly_stop_child(state);
            }
        }
        bk_sys_sleep_ms(10U);
    }
    bk_gui_unregister_program(desktop,state->program);state->program=NULL;
    if(!__atomic_load_n(&state->dismissed,__ATOMIC_ACQUIRE) && !bk_proc_exit_requested() && state->status) {
        state->input_armed_ms=bk_sys_uptime_ms()+500U;
        state->program=bk_gui_register_program(desktop,"ss_jelly_unavailable",state,ss_jelly_message,ss_jelly_event,NULL);
        bk_gui_invalidate_all(desktop);bk_gui_request_paint();
        while(state->program && !__atomic_load_n(&state->dismissed,__ATOMIC_ACQUIRE) && !bk_proc_exit_requested())
            bk_sys_sleep_ms(20U);
        if(state->program)bk_gui_unregister_program(desktop,state->program);
    }
    bk_sys_free(state);bk_gui_gfx_invalidate_front();bk_gui_invalidate_all(desktop);bk_gui_request_paint();
}
