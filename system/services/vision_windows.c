#define BK_API_IMPLEMENTATION
#include "kernel/include/api.h"
#include "kernel/include/vision_ipc.h"
#include "kernel/include/vision_resources.h"
#include "kernel/include/about_dialog.h"
bool vision_file_dialog_open(const bk_vision_request_t *request);
bool vision_about_attach(gui_window_t *,gui_desktop_t *,const bk_about_info_t *);
void vision_about_show(gui_desktop_t *,const bk_about_info_t *);
void vision_about_detach(gui_window_t *);
/* Messages contain copied scalars/strings. Foreign process pointers never
 * become title strings or return buffers in the graphical server. */
static gui_window_t *owned_window(gui_desktop_t *desktop,uint32_t handle,uint32_t pid) {
    for(gui_window_t *w=desktop->first_window;w;w=w->next)
        if((uint32_t)(uintptr_t)w==handle && (w->owner_pid==pid || w->controller_pid==pid))return w;
    return NULL;
}
static bool set_bounds(gui_desktop_t *desktop,gui_window_t *window,gui_rect_t bounds) {
    if(bounds.w<64 || bounds.h<64 || bounds.w>desktop->surface.width ||
       bounds.h>desktop->surface.height)return false;
    if(bounds.x<0)bounds.x=0;
    if(bounds.y<0)bounds.y=0;
    if(bounds.x>desktop->surface.width-bounds.w)bounds.x=desktop->surface.width-bounds.w;
    if(bounds.y>desktop->surface.height-bounds.h)bounds.y=desktop->surface.height-bounds.h;
    gui_rect_t old=window->bounds;window->bounds=bounds;
    window->mode_restore_valid=false;window->content_ready=false;
    window->stream_ready=false;window->dirty=true;
    gui_desktop_invalidate_rect(desktop,old);gui_desktop_invalidate_rect(desktop,bounds);
    gui_request_paint();return true;
}
void vision_windows_dispatch(gui_desktop_t *desktop) {
    bk_vision_request_t request;
    for(uint32_t count=0;count<32U && bk_vision_ipc_take(&request);count++) {
        gui_window_t *window=owned_window(desktop,request.window,request.process_id);
        uint32_t result=0U;gui_window_t *created=NULL;
        if(request.operation==VISION_IPC_PROGRAM_ADD) {
            request.title[63]=0;
            gui_program_t *program=gui_desktop_register_program(desktop,request.title,
                (void *)(uintptr_t)request.values[0],(gui_program_paint_t)(uintptr_t)request.values[1],
                (gui_program_event_t)(uintptr_t)request.values[2],(gui_program_destroy_t)(uintptr_t)request.values[3]);
            if(program){program->callback_pid=request.thread_id;(void)task_set_affinity_mask(request.thread_id,1U);bk_vision_resource_owner(program,request.process_id);result=(uint32_t)(uintptr_t)program;}
        } else if(request.operation==VISION_IPC_PROGRAM_REMOVE) {
            for(gui_program_t *program=desktop->first_program;program;program=program->next)
                if((uint32_t)(uintptr_t)program==request.window && program->callback_pid==request.thread_id) {
                    gui_desktop_unregister_program(desktop,program);result=1;break;
                }
        } else if(request.operation==VISION_IPC_FILE_DIALOG) {
            request.title[63]=0;request.path[259]=0;request.extension[15]=0;
            result=vision_file_dialog_open(&request);
        } else if(request.operation==VISION_IPC_ABOUT_SHOW || request.operation==VISION_IPC_ABOUT_ATTACH) {
            request.about.name[47]=0;request.about.version[23]=0;request.about.description[127]=0;
            request.about.copyright[63]=0;request.about.icon_path[63]=0;
            bk_about_info_t info={request.about.name,request.about.version,request.about.description,request.about.copyright,request.about.icon_path};
            if(request.operation==VISION_IPC_ABOUT_SHOW){vision_about_show(desktop,&info);result=1;}
            else if(window)result=vision_about_attach(window,desktop,&info);
        } else if(request.operation==VISION_IPC_CREATE) {
            request.title[sizeof(request.title)-1]=0;
            if(request.bounds.w>0 && request.bounds.h>0 &&
               request.bounds.w<=desktop->surface.width &&
               request.bounds.h<=desktop->surface.height) {
                created=gui_desktop_create_window(desktop,request.bounds.x,request.bounds.y,
                    request.bounds.w,request.bounds.h,request.title);
                if(created) {created->owner_pid=request.process_id;created->controller_pid=request.process_id;bk_vision_resource_owner(created,request.process_id);result=(uint32_t)(uintptr_t)created;}
            }
        } else if(window) {
            switch(request.operation) {
            case VISION_IPC_MENU_ADD:
                request.title[63]=0;{int menu=gui_window_add_menu(window,request.title);result=menu<0?0:(uint32_t)menu+1U;}break;
            case VISION_IPC_MENU_ITEM:
                request.title[63]=0;
                result=gui_window_add_menu_item(window,(int)request.values[0],request.values[1],request.title,
                    (gui_menu_callback_t)(uintptr_t)request.values[2],(void *)(uintptr_t)request.values[3]);
                if(result){gui_menu_t *menu=&window->menus[request.values[0]];menu->items[menu->item_count-1U].callback_pid=request.thread_id;if(request.process_id!=bk_sys_getpid())(void)task_set_affinity_mask(request.thread_id,1U);}
                break;
            case VISION_IPC_DESTROY:vision_about_detach(window);gui_desktop_remove_window(desktop,window);gui_window_destroy(window);result=1U;break;
            case VISION_IPC_OWNER:window->owner_pid=request.values[0];bk_vision_resource_owner(window,request.values[0]);result=1U;break;
            case VISION_IPC_CONTENT:
                window->content_paint=(gui_window_content_paint_t)(uintptr_t)request.values[0];
                window->content_context=(void *)(uintptr_t)request.values[1];
                window->content_pid=request.values[0]?(request.process_id==bk_sys_getpid()?request.process_id:request.thread_id):0;
                if(window->content_pid!=bk_sys_getpid() && window->content_pid)(void)task_set_affinity_mask(window->content_pid,1U);
                window->content_ready=false;window->dirty=true;gui_request_paint();result=1U;break;
            case VISION_IPC_EVENT:
                window->event_handler=(gui_window_event_t)(uintptr_t)request.values[0];
                window->event_context=(void *)(uintptr_t)request.values[1];
                window->event_pid=request.values[0]?(request.process_id==bk_sys_getpid()?request.process_id:request.thread_id):0;
                if(window->event_pid!=bk_sys_getpid() && window->event_pid)(void)task_set_affinity_mask(window->event_pid,1U);
                result=1U;break;
            case VISION_IPC_EXTENDED:gui_window_set_extended_events(window,request.values[0]!=0);result=1U;break;
            case VISION_IPC_COMPACT:window->compact_content=request.values[0]!=0;result=1U;break;
            case VISION_IPC_STREAMING:window->streaming_content=request.values[0]!=0;window->dirty=true;gui_request_paint();result=1U;break;
            case VISION_IPC_CLOSE:gui_window_close(window);result=1U;break;
            case VISION_IPC_FOCUS:gui_desktop_focus_window(desktop,window);result=1U;break;
            case VISION_IPC_BOUNDS:result=set_bounds(desktop,window,request.bounds);break;
            case VISION_IPC_MIN_SIZE:gui_window_set_min_size(window,(int)request.values[0],(int)request.values[1]);result=1U;break;
            case VISION_IPC_BORDERLESS:gui_window_set_borderless(window,request.values[0]!=0U,(uint8_t)request.values[1]);gui_request_paint();result=1U;break;
            default:break;
            }
        }
        if(!bk_vision_ipc_reply(request.cookie,result) && created) {
            gui_desktop_remove_window(desktop,created);gui_window_destroy(created);
        }
    }
}
