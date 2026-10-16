#ifndef BK_DESKTOP_FILE_PROGRESS_H
#define BK_DESKTOP_FILE_PROGRESS_H
#include "system/desktop/file_ops.h"

/* Each worker owns copied paths and its window. It never retains a browser or
 * desktop icon pointer: either may be closed/rebuilt while the disk is busy. */
enum {BK_SHELL_JOB_MOVE,BK_SHELL_JOB_TRASH,BK_SHELL_JOB_DELETE,BK_SHELL_JOB_COPY};
#define BK_SHELL_JOB_MAX 128
#define BK_SHELL_JOB_UNITS_MAX 1000000U
typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_image_t icon;
    unsigned action,count,total,completed,percent,failed;
    uint32_t last_paint,current_bytes,current_size;
    char directory[VFS_MAX_PATH],current[VFS_MAX_PATH];
    char paths[BK_SHELL_JOB_MAX][VFS_MAX_PATH];
    bool preparing;
} bk_shell_job_t;

static void bk_shell_job_paint(gui_window_t *window,gui_surface_t *surface,void *context) {
    bk_shell_job_t *job=context;
    gui_rect_t c=bk_gui_window_content_rect_raw(window);
    bk_gui_gfx_fill_rect(surface,c,0x00D4D0C8);
    if(job->icon.pixels)bk_gui_surface_draw_image(surface,
        (gui_rect_t){c.x+14,c.y+16,40,40},c,&job->icon);
    const char *verb=job->action==BK_SHELL_JOB_MOVE?"Moviendo":
        job->action==BK_SHELL_JOB_COPY?"Copiando":"Eliminando";
    char text[320];
    snprintf(text,sizeof(text),"%s %s",job->preparing?"Preparando:":verb,
             bk_shell_basename(job->current));
    gui_rect_t label={c.x+68,c.y+16,c.w-82,40};
    bk_gui_font_draw_string_clipped(surface,label.x,label.y,text,0x00101010,label);
    snprintf(text,sizeof(text),"%u%%   (%u de %u elementos)",job->percent,job->completed,job->total);
    bk_gui_font_draw_string_clipped(surface,label.x,label.y+20,text,0x00505050,label);
    bk_gui_surface_draw_progress(surface,(gui_rect_t){c.x+14,c.y+68,c.w-28,22},
        job->percent,BK_GUI_PROGRESS_CLASSIC,0);
}
static bool bk_shell_job_event(gui_window_t *window UNUSED,const gui_event_t *event,void *context UNUSED) {
    /* Closing a progress view must not interrupt a staged filesystem operation. */
    return event && event->type==GUI_EVENT_CLOSE;
}
static void bk_shell_job_refresh(bk_shell_job_t *job,bool force) {
    uint32_t now=bk_sys_ticks();
    uint32_t interval=bk_sys_tick_frequency()/20U;
    if(!interval)interval=1;
    if(!force && now-job->last_paint<interval)return;
    job->last_paint=now;
    if(job->window)job->window->dirty=true;
    bk_gui_request_paint();
    bk_sys_sleep_ticks(1);
}
static void bk_shell_job_progress(void *context,const char *path,uint32_t bytes,bool completed) {
    bk_shell_job_t *job=context;
    if(bk_runtime_strcmp(job->current,path)) {
        bk_runtime_strncpy(job->current,path,sizeof(job->current)-1);
        job->current_bytes=0;job->current_size=0;
        vfs_dir_entry_t info;
        if(bk_file_stat(path,&info) && info.type!=VFS_NODE_DIR)job->current_size=info.size;
    }
    if(completed) {if(job->completed<job->total)++job->completed;job->current_bytes=0;}
    else if(bytes<=0xffffffffU-job->current_bytes)job->current_bytes+=bytes;
    unsigned partial=0;
    if(!completed && job->current_size) {
        uint32_t divisor=job->current_size/100U+1U;
        partial=job->current_bytes/divisor;
        if(partial>99)partial=99;
    }
    unsigned percent=job->total?(job->completed*100U+partial)/job->total:0;
    /* Publication/flush can still fail after the last write. */
    if(percent>99)percent=99;
    if(percent>job->percent)job->percent=percent;
    bk_shell_job_refresh(job,false);
}
static bool bk_shell_job_count(bk_shell_job_t *job,const char *path,unsigned depth) {
    vfs_dir_entry_t info;
    if(depth>16 || !bk_file_stat(path,&info) || job->total>=BK_SHELL_JOB_UNITS_MAX)return false;
    ++job->total;
    bk_runtime_strncpy(job->current,path,sizeof(job->current)-1);
    bk_shell_job_refresh(job,false);
    if(info.type!=VFS_NODE_DIR)return true;
    vfs_dir_entry_t *entries;uint32_t count;
    if(!bk_shell_read_directory(path,&entries,&count))return false;
    bool ok=true;
    for(uint32_t i=0;i<count && ok;++i) {
        if(!bk_runtime_strcmp(entries[i].name,".") || !bk_runtime_strcmp(entries[i].name,".."))continue;
        char child[VFS_MAX_PATH];
        ok=snprintf(child,sizeof(child),"%s/%s",path,entries[i].name)<(int)sizeof(child) &&
            bk_shell_job_count(job,child,depth+1);
    }
    bk_sys_free(entries);return ok;
}
static void bk_shell_job_task(void *argument) {
    bk_shell_job_t *job=argument;
    bk_shell_progress_t progress={bk_shell_job_progress,job};
    job->preparing=true;
    bk_runtime_strcpy(job->current,job->paths[0]);
    (void)bk_graphics_icon_load(job->action==BK_SHELL_JOB_DELETE || job->action==BK_SHELL_JOB_TRASH?
        "RecycleEmpty":"Folder",&job->icon);
    job->window=bk_gui_create_window(job->desktop,180,140,440,132,
        job->action==BK_SHELL_JOB_DELETE || job->action==BK_SHELL_JOB_TRASH?"Eliminando archivos":"Transferencia de archivos");
    if(!job->window) {
        bk_gui_error("Archivos","No se pudo crear la ventana de progreso.");
        goto cleanup;
    }
    job->window->owner_pid=bk_sys_getpid();
    bk_proc_bind_window(job->window);
    bk_gui_set_window_content(job->window,bk_shell_job_paint,job);
    bk_gui_window_set_extended_events(job->window,true);
    bk_gui_set_window_event_handler(job->window,bk_shell_job_event,job);
    job->window->resizable=false;
    bk_gui_focus_window(job->desktop,job->window);
    bk_shell_job_refresh(job,true);
    /* Count nodes before recursive copies/deletes. Renames and trashing are
       atomic per top-level item and do not need to scan entire directories. */
    for(unsigned i=0;i<job->count;++i) {
        bool recursive=job->action==BK_SHELL_JOB_DELETE || job->action==BK_SHELL_JOB_COPY ||
            (job->action==BK_SHELL_JOB_MOVE && bk_file_transfer_check(job->paths[i],job->directory)==0);
        if(recursive) {
            if(!bk_shell_job_count(job,job->paths[i],0)) {job->failed=job->count;goto finished;}
        } else ++job->total;
    }
    job->preparing=false;job->current[0]=0;
    bool changed=false;
    for(unsigned i=0;i<job->count;++i) {
        const char *path=job->paths[i];bool ok=false;
        bk_shell_job_progress(job,path,0,false);
        if(job->action==BK_SHELL_JOB_MOVE)ok=bk_shell_move_progress(path,job->directory,&progress);
        else if(job->action==BK_SHELL_JOB_TRASH) {
            ok=bk_shell_trash(path);
            if(ok)bk_shell_job_progress(job,path,0,true);
        } else if(job->action==BK_SHELL_JOB_DELETE) {
            ok=bk_shell_remove_tree_progress(path,0,&progress);
            if(ok)bk_shell_remove_trash_metadata(path);
        } else {
            char target[VFS_MAX_PATH];vfs_dir_entry_t info;
            ok=snprintf(target,sizeof(target),"%s/%s",job->directory,bk_shell_basename(path))<(int)sizeof(target) &&
                bk_file_transfer_check(path,job->directory)>=0 && !bk_file_stat(target,&info) &&
                bk_shell_copy_tree_progress(path,target,0,&progress);
        }
        if(ok)changed=true;else ++job->failed;
    }
    /* A failed recursive operation may have removed some children. Always
       refresh views after attempting it, including partial failures. */
    if(changed || job->count)bk_gui_files_changed();
finished:
    job->preparing=false;
    if(!job->failed){job->completed=job->total;job->percent=100;}
    bk_shell_job_refresh(job,true);
    bk_sys_sleep_ms(160);
cleanup:
    if(job->window) {
        bk_gui_destroy_window(job->desktop,job->window);
        bk_proc_bind_window(NULL);
    }
    if(job->icon.pixels)bk_gui_image_free(&job->icon);
    if(job->failed)bk_gui_error("Archivos","Algunos archivos no se pudieron procesar. Revisa el destino y el espacio libre.");
    bk_sys_free(job);bk_proc_exit();
}
static bk_shell_job_t *bk_shell_job_create(gui_desktop_t *desktop,unsigned action,const char *directory) {
    bk_shell_job_t *job=bk_sys_alloc_zero(sizeof(*job));
    if(!job)return NULL;
    job->desktop=desktop;job->action=action;
    if(directory)bk_runtime_strncpy(job->directory,directory,sizeof(job->directory)-1);
    return job;
}
static bool bk_shell_job_add(bk_shell_job_t *job,const char *path) {
    if(!job || !path || !path[0] || job->count>=BK_SHELL_JOB_MAX || bk_runtime_strlen(path)>=VFS_MAX_PATH)return false;
    bk_runtime_strcpy(job->paths[job->count++],path);return true;
}
/* Consumes job on both success and failure. */
static bool bk_shell_job_start(bk_shell_job_t *job) {
    if(!job)return false;
    if(!job->count || bk_proc_spawn_thread("file-operation",bk_shell_job_task,job)<0) {
        bk_sys_free(job);return false;
    }
    return true;
}
static bool bk_shell_job_one(gui_desktop_t *desktop,unsigned action,const char *path,const char *directory) {
    bk_shell_job_t *job=bk_shell_job_create(desktop,action,directory);
    if(!bk_shell_job_add(job,path)){bk_sys_free(job);return false;}
    return bk_shell_job_start(job);
}
#endif
