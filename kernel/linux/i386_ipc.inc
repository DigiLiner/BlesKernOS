/* IPC local de Linux. Los handles internos conservan referencias a la
 * descripcion abierta y se heredan/duplican igual que los archivos VFS. */
#define LINUX_PIPE_BUF 4096U
/* X11 PutImage streams carry megabytes; do not force a peer wakeup per 4 KiB.
 * Pipes retain their capacity and PIPE_BUF atomicity. Socket storage is freed
 * with the pair, including on descriptor-passing cycle collection. */
#define LINUX_SOCKET_BUFFER 65536U
typedef struct {uint32_t offset,count;int fd[8];} linux_rights;
typedef struct {
    uint32_t refs,inflight,flags,pid,read_pos,write_pos,used,read_seq,write_seq;
    bool read_closed,write_closed,listen,bound;
    uint8_t name[110];uint32_t name_length;
    int backlog[8];uint32_t pending,backlog_limit;
    linux_rights rights[8];
    uint8_t *buffer;
} linux_endpoint;
typedef struct {
    void *allocation;uint32_t size,capacity;
    bool pipe,connected;
    uint32_t inode;
    linux_endpoint end[2];
} linux_io_pair;
static linux_io_pair *linux_pairs[LINUX_IO_PAIRS];
typedef struct {uint32_t length,inode;int raw;uint8_t address[110];} linux_unix_name;
static linux_unix_name *linux_names;
static uint32_t linux_io_inode=0x10000000;
static kspinlock_t linux_io_lock=KSPINLOCK_INITIALIZER;
static void linux_unix_gc(void);
static void linux_io_yield(void) {
    kernel_domain_snapshot_t snapshot;
    kernel_domains_drop_current(&snapshot);task_yield();kernel_domains_restore(&snapshot);
}

/* A private event per pipe/socket pair avoids waking every Wine thread
 * for unrelated RPC traffic. Poll/select sets spanning several pairs keep
 * the global event. Stable tag addresses survive endpoint destruction. */
static uint32_t linux_io_event_tag;
static uint32_t linux_io_pair_tags[LINUX_IO_PAIRS];
static uint32_t linux_io_event_key(void) {
    return (uint32_t)(uintptr_t)&linux_io_event_tag;
}
static uint32_t linux_io_pair_key(int raw) {
    if(raw<LINUX_IO_BASE || (uint32_t)(raw-LINUX_IO_BASE)>=2U*LINUX_IO_PAIRS)
        return linux_io_event_key();
    return (uint32_t)(uintptr_t)&linux_io_pair_tags[(raw-LINUX_IO_BASE)/2];
}
static bool linux_io_wait_arm(uint32_t key) {
    return task_event_wait_prepare(key);
}
static void linux_io_wait_cancel(bool armed,uint32_t key) {
    if(armed)task_event_wait_cancel(key);
}
static void linux_io_notify(int raw) {
    uint32_t key=linux_io_pair_key(raw);
    if(key!=linux_io_event_key())(void)task_event_signal(key);
    (void)task_event_signal(linux_io_event_key());
}
static void linux_io_wait(uint32_t *empty_rounds,bool armed,uint32_t key,uint32_t ticks) {
    kernel_domain_snapshot_t snapshot;
    if(armed) {
        W3P_SCOPE(W3P_IPC_SLEEP,0U);
        kernel_domains_drop_current(&snapshot);
        if(task_event_wait_commit(key,ticks))*empty_rounds=0U;
        else if(*empty_rounds!=0xffffffffU)++*empty_rounds;
        kernel_domains_restore(&snapshot);
        return;
    }
    W3P_SCOPE(W3P_IPC_YIELD,0U);
    if(*empty_rounds<16U) {++*empty_rounds;linux_io_yield();return;}
    kernel_domains_drop_current(&snapshot);
    task_sleep(1U);
    kernel_domains_restore(&snapshot);
}

/* Event-backed single-pair polls can sleep until their producer or deadline.
 * Sets with other resources retain periodic checks of their readiness. */
static uint32_t linux_io_poll_ticks(uint32_t key,uint32_t start,int64_t limit_us) {
    if(key==linux_io_event_key())return 1U;
    uint32_t hz=pit_get_frequency_hz();
    if(!hz)return 1U;
    if(limit_us<0)return hz;
    uint64_t deadline=((uint64_t)limit_us*hz+999999U)/1000000U;
    uint32_t elapsed=pit_get_ticks()-start;
    if(deadline<=elapsed)return 1U;
    uint64_t remaining=deadline-elapsed;
    return remaining<hz?(uint32_t)remaining:hz;
}

static linux_endpoint *linux_endpoint_for(int fd) {
    uint32_t n=(uint32_t)(fd-LINUX_IO_BASE);
    if(n>=LINUX_IO_PAIRS*2 || !linux_pairs[n/2])return NULL;
    return &linux_pairs[n/2]->end[n&1];
}
#define BLES_GFX_MAX_SURFACES 32U

typedef struct bles_gfx_context bles_gfx_context;
typedef struct {
    bles_gfx_context *owner;
    gui_window_t *window;
    uint32_t surface_id;
    uint32_t parent_id;
    uint32_t z_index;
    uint16_t source_x, source_y, source_w, source_h;
    int16_t host_offset_x, host_offset_y;
    uint32_t flags;
    bool active;
} bles_gfx_surface;

struct bles_gfx_context {
    gui_window_t *window;
    uint32_t *pixels;
    uint32_t *scratch;
    uint32_t scratch_capacity;
    uint32_t instance_id;
    uint16_t width, height;
    uint16_t view_x, view_y, view_width, view_height;
    bool force_present;
    bool gallium_window;
    bool native_window;
    bool saver_window;
    bool saver_close_reported;
    bool launch_busy;
    kmutex_t launch_lock;
    kmutex_t virgl_lock;
    bool gallium_failed;
    struct { uint32_t handle, width, height, flags; } gpu_surfaces[128];
    uint32_t gpu_fence;
    uint32_t virgl_context;
    uint32_t gallium_surfaces[BLES_VIRGL_NATIVE_SLOTS], gallium_readers[BLES_VIRGL_NATIVE_SLOTS], gallium_next;
    uint32_t gallium_serial[BLES_VIRGL_NATIVE_SLOTS], gallium_sequence;
    uint32_t native_wait_limit,native_wait_readers,native_wait_ready;
    uint32_t native_queue_limit;
    uint8_t gallium_states[BLES_VIRGL_NATIVE_SLOTS]; /* 0 free, 1 GPU capture, 2 desktop readers */
    volatile uint32_t native_consumers;
    bles_gfx_surface surfaces[BLES_GFX_MAX_SURFACES];
    bles_gfx_event_t events[64];
    uint8_t read_pos, write_pos;
    bles_gfx_surface_event_t surface_events[64];
    uint8_t surface_read_pos, surface_write_pos;
};

typedef struct {
    uint32_t refs,flags; unsigned kind; bles_gfx_context *gfx;
    uint8_t *pcm; uint32_t pcm_rate,pcm_format,pcm_channels,pcm_read,pcm_count,pcm_inflight,pcm_serial;
    struct linux_inotify *inotify;
} linux_device;
static linux_device linux_devices[128];
static uint32_t linux_pcm_serial;
/* Allocate a graphics context only when /dev/blesgfx is opened. */

#include "i386_oss.inc"

static linux_device *linux_device_for(int raw) {
    unsigned i=(unsigned)(raw-LINUX_DEVICE_BASE);
    return i<128 && linux_devices[i].refs?&linux_devices[i]:NULL;
}

static int linux_device_open(unsigned kind,uint32_t flags) {
    bles_gfx_context *gfx=NULL;
    if(kind==3) {
        gfx=kzalloc(sizeof(*gfx));
        if(!gfx)return -BK_ENOMEM;
        (void)mm_set_allocation_owner(gfx,0U);
    }
    uint8_t *pcm=NULL;
    if(kind==4) {
        if(!sound_pcm_available() || !(flags&3))return -19;
        pcm=kmalloc(BK_OSS_QUEUE);if(!pcm)return -BK_ENOMEM;
        if(__sync_bool_compare_and_swap(&linux_pcm_worker_started,0,1)) {
            if(task_create_kernel("wine-pcm",linux_pcm_worker,NULL)<0) {
                linux_pcm_worker_started=0;kfree(pcm);return -BK_ENOMEM;
            }
        }
    }
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    int raw=-BK_EMFILE;
    for(unsigned i=0;i<128;i++) if(!linux_devices[i].refs) {
        linux_devices[i]=(linux_device){.refs=1,.flags=flags,.kind=kind,
            .gfx=gfx,.pcm=pcm,.pcm_rate=44100,
            .pcm_format=BK_OSS_S16,.pcm_channels=2,.pcm_serial=++linux_pcm_serial};
        raw=LINUX_DEVICE_BASE+i;break;
    }
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    if(raw<0 && pcm)kfree(pcm);
    if(raw<0 && gfx)kfree(gfx);
    return raw;
}

#include "i386_inotify.inc"

static bles_gfx_context *bles_gfx_find_instance(uint32_t instance_id) {
    if(!instance_id)return NULL;
    for(unsigned i=0;i<128;i++) {
        bles_gfx_context *gfx=linux_devices[i].gfx;
        if(linux_devices[i].refs && gfx && gfx->width &&
           gfx->instance_id==instance_id)
            return gfx;
    }
    return NULL;
}

static void bles_gfx_push_input(bles_gfx_context *gfx,const gui_event_t *event,
                                int origin_x,int origin_y) {
    uint8_t next;
    bles_gfx_event_t *out;
    if(!gfx || !event)return;
    next=(uint8_t)((gfx->write_pos+1U)&63U);
    if(next==gfx->read_pos)return;
    out=&gfx->events[gfx->write_pos];
    out->type=event->type;
    out->x=event->x+origin_x;out->y=event->y+origin_y;
    out->dx=event->dx;out->dy=event->dy;
    out->buttons=event->buttons;out->button=event->button;
    out->key=(uint8_t)event->key;
    out->shift=event->shift;out->ctrl=event->ctrl;out->alt=event->alt;
    gfx->write_pos=next;
}

static void bles_gfx_push_surface_event(bles_gfx_surface *surface,
                                        uint32_t type) {
    bles_gfx_context *gfx;
    bles_gfx_surface_event_t *out;
    gui_rect_t content;
    uint8_t next;
    if(!surface || !(gfx=surface->owner) || !surface->window)return;
    next=(uint8_t)((gfx->surface_write_pos+1U)&63U);
    if(next==gfx->surface_read_pos)return;
    content=gui_window_content_rect(surface->window);
    out=&gfx->surface_events[gfx->surface_write_pos];
    kmemset(out,0,sizeof(*out));
    out->instance_id=gfx->instance_id;
    out->surface_id=surface->surface_id;
    out->type=type;
    out->x=surface->window->bounds.x;
    out->y=surface->window->bounds.y;
    if(type==BLES_GFX_SURFACE_MOVE) {
        out->x-=surface->host_offset_x;
        out->y-=surface->host_offset_y;
    }
    out->width=content.w>0?(uint32_t)content.w:0U;
    out->height=content.h>0?(uint32_t)content.h:0U;
    gfx->surface_write_pos=next;
}

static bool bles_gfx_on_event(gui_window_t *window,const gui_event_t *event,void *opaque) {
    bles_gfx_context *gfx=(bles_gfx_context *)opaque;
    bool handled=false;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    if(gfx && gfx->window==window) {
        gui_rect_t content=gui_window_content_rect(window);
        bles_gfx_push_input(gfx,event,
                            gfx->view_x-content.x,
                            gfx->view_y-content.y);
        /* The owner drains CLOSE and releases its descriptor/window. Keep
         * the window alive until then, just like the rootless surface path. */
        handled=gfx->native_window && event->type==GUI_EVENT_CLOSE;
        if(handled)kprintf("[GFX:NATIVE] CLOSE owner=%u\n",gfx->instance_id);
    }
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    return handled;
}

static bool bles_gfx_surface_on_event_locked(gui_window_t *window,
                                      const gui_event_t *event,void *opaque) {
    bles_gfx_surface *surface=(bles_gfx_surface *)opaque;
    bles_gfx_context *gfx=surface?surface->owner:NULL;
    bool handled=false;
    if(surface && gfx && surface->active && surface->window==window) {
        if(event->type==GUI_EVENT_CLOSE) {
            bles_gfx_push_surface_event(surface,BLES_GFX_SURFACE_CLOSE);
            handled=true;
        } else if(event->type==GUI_EVENT_RESIZE) {
            bles_gfx_push_surface_event(surface,BLES_GFX_SURFACE_RESIZE);
            handled=true;
        } else if(event->type==GUI_EVENT_FOCUS_GAIN) {
            bles_gfx_push_surface_event(surface,BLES_GFX_SURFACE_FOCUS);
        } else if(event->type==GUI_EVENT_FOCUS_LOST) {
            bles_gfx_push_surface_event(surface,BLES_GFX_SURFACE_BLUR);
        } else {
            /* X can clear focus while a Wine modal dialog is withdrawn,
             * even though Vision still considers the parent focused. A
             * click must reactivate the X input queue in that case too. */
            if(event->type==GUI_EVENT_MOUSE_DOWN && window->input_enabled)
                bles_gfx_push_surface_event(surface,BLES_GFX_SURFACE_FOCUS);
            gui_rect_t content=gui_window_content_rect(window);
            bles_gfx_push_input(gfx,event,
                                (int)surface->source_x-content.x,
                                (int)surface->source_y-content.y);
        }
    }
    return handled;
}

static bool bles_gfx_surface_on_event(gui_window_t *window,const gui_event_t *event,void *opaque) {
    uint32_t flags=kspin_lock_irqsave(&linux_io_lock);
    bool result=bles_gfx_surface_on_event_locked(window,event,opaque);
    kspin_unlock_irqrestore(&linux_io_lock,flags);return result;
}
bool syscall_gfx_window_event(gui_window_t *window,const gui_event_t *event,bool *handled) {
    if(!window || !event || !handled)return false;
    uint32_t flags=kspin_lock_irqsave(&linux_io_lock);
    for(uint32_t i=0;i<128U;i++) {
        bles_gfx_context *gfx=linux_devices[i].gfx;
        if(!linux_devices[i].refs || !gfx)continue;
        if(gfx->window==window) {
            gui_rect_t content=gui_window_content_rect(window);
            bles_gfx_push_input(gfx,event,gfx->view_x-content.x,gfx->view_y-content.y);
            *handled=false;kspin_unlock_irqrestore(&linux_io_lock,flags);return true;
        }
        for(uint32_t n=0;n<BLES_GFX_MAX_SURFACES;n++)
            if(gfx->surfaces[n].active && gfx->surfaces[n].window==window) {
                *handled=bles_gfx_surface_on_event_locked(window,event,&gfx->surfaces[n]);
                kspin_unlock_irqrestore(&linux_io_lock,flags);return true;
            }
    }
    kspin_unlock_irqrestore(&linux_io_lock,flags);return false;
}

/* The TinyX framebuffer contains the composited X screen, not a separate
 * pixmap for each X window. Keep a lower native window's previous pixels
 * where a higher X window covers it, otherwise the higher window becomes
 * permanently copied into the lower window's backing surface. */
/* Recompute modal ownership before closing a dialog, so focus can return to
 * its parent. Surface slots can be reused, and multiple dialogs can coexist. */
static void bles_gfx_modal_inputs(bles_gfx_context *gfx) {
    for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++) {
        bles_gfx_surface *parent=&gfx->surfaces[i];
        if(!parent->active || !parent->window)continue;
        bool blocked=false;
        for(unsigned j=0;j<BLES_GFX_MAX_SURFACES;j++) {
            bles_gfx_surface *child=&gfx->surfaces[j];
            if(child->active && (child->flags&BLES_GFX_SURFACE_DIALOG) &&
               child->parent_id==parent->surface_id) {blocked=true;break;}
        }
        parent->window->input_enabled=!blocked;
    }
}

static void bles_gfx_preserve_occluded(bles_gfx_context *gfx,
                                      const bles_gfx_surface *surface,
                                      gui_window_t *window,
                                      uint32_t sx,uint32_t sy,
                                      uint32_t sw,uint32_t sh,
                                      uint32_t z_index) {
    for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++) {
        bles_gfx_surface *above=&gfx->surfaces[i];
        uint32_t ax,ay,aw,ah,az;
        uint32_t x0,y0,x1,y1;
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        bool covers=above!=surface && above->active && above->window &&
                    above->z_index>z_index;
        ax=above->source_x;ay=above->source_y;
        aw=above->source_w;ah=above->source_h;az=above->z_index;
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        if(!covers || az<=z_index)continue;
        x0=sx>ax?sx:ax;y0=sy>ay?sy:ay;
        x1=sx+sw<ax+aw?sx+sw:ax+aw;
        y1=sy+sh<ay+ah?sy+sh:ay+ah;
        if(x0>=x1 || y0>=y1)continue;
        for(uint32_t y=y0;y<y1;y++) {
            uint32_t offset=(y-sy)*sw+(x0-sx);
            uint32_t count=x1-x0;
            if(window->stream_ready && window->stream_pixels &&
               window->stream_width==sw && window->stream_height==sh)
                kmemcpy(gfx->scratch+offset,window->stream_pixels+offset,count*4U);
            else for(uint32_t x=0;x<count;x++)
                gfx->scratch[offset+x]=0x00c0c0c0U;
        }
    }
}

#include "i386_gfx3d.inc"
#include "../../ports/gallium/include/blesgfx-native.h"

static int32_t bles_gfx_native_fullscreen(bles_gfx_context *gfx,uint32_t argument) {
    gui_desktop_t *desktop=gui_get_desktop();
    if(argument || !gfx->native_window || !gfx->window || !desktop ||
       desktop->surface.width<64 || desktop->surface.height<64)return -BK_EINVAL;
    bk_gui_window_set_borderless(gfx->window,true,0);
    return bk_gui_window_set_bounds(gfx->window,(gui_rect_t){0,0,
        desktop->surface.width,desktop->surface.height})?0:-BK_EINVAL;
}

/* The control descriptor owns its busy reference, including failure/exit.
 * Called outside linux_io_lock: cursor invalidation may enter the GUI. */
static void bles_gfx_launch_busy(bles_gfx_context *gfx, bool busy) {
    kmutex_lock(&gfx->launch_lock);
    if (gfx->launch_busy != busy) {
        gfx->launch_busy = busy;
        if (busy) gui_compositor_busy_begin();
        else gui_compositor_busy_end();
    }
    kmutex_unlock(&gfx->launch_lock);
}

static int32_t bles_gfx_ioctl_pinned(linux_device *device,uint32_t request,uint32_t argument) {
    bles_gfx_context *gfx=device->gfx;
    if(!gfx)return -BK_ENOTTY;
    if(request==BLES_GFX_NATIVE_FULLSCREEN)return bles_gfx_native_fullscreen(gfx,argument);
    if(request==BLES_GFX_NATIVE_EXIT_REQUEST) {
        /* glmark2 calls this from its own render thread each frame. The
         * cancellation request must be observed by that task; this trace
         * differentiates a failed stop from a presentation/cleanup problem. */
        bool stop = gfx->saver_window && task_exit_requested();
        if(stop && !gfx->saver_close_reported) {
            gfx->saver_close_reported=true;
            kprintf("[GALLIUM:SAVER] renderer observed exit pid=%u\n",task_current_pid());
        }
        return stop?1:0;
    }
    if(request==BLES_GFX_LAUNCH_BUSY) {
        if(argument>1U)return -BK_EINVAL;
        bles_gfx_launch_busy(gfx,argument!=0U);
        return 0;
    }
    if(request==BLES_GFX_3D)return bles_gfx_3d_ioctl(gfx,argument);
    if(request==BLES_GFX_VIRGL)return bles_gfx_virgl_ioctl(gfx,argument);
    if(request==BLES_GFX_WINE_BOOT_READY) {
        int32_t status;
        if(!g_wine_boot_pid || task_current_process_id()!=g_wine_boot_pid)return -13;
        if(!copy_from_user(&status,(void *)(uintptr_t)argument,sizeof(status)))return -BK_EFAULT;
        __sync_bool_compare_and_swap(&g_wine_boot_result,0,status==0?1:-1);
        return 0;
    }
    if(request==BLES_GFX_HEAP_SUBSYSTEMS) {
        bles_gfx_heap_subsystems_t a;
        mm_subsystem_usage_t usage[MM_SUBSYSTEM_COUNT];
        mm_get_subsystem_usage(usage);
        for(unsigned i=0;i<8;i++) {a.bytes[i]=usage[i].bytes;a.blocks[i]=usage[i].blocks;}
        return copy_to_user((void *)(uintptr_t)argument,&a,sizeof(a))?0:-BK_EFAULT;
    }
    if(request==BLES_GFX_WINE_PAGER_PERF) {
        bles_gfx_wine_pager_perf_t a;
        if(!copy_from_user(&a,(void *)(uintptr_t)argument,sizeof(a)))return -BK_EFAULT;
        pager_stats_t stats;pager_stats(a.pid,&stats);
        a.anonymous_pages=stats.anonymous_pages;a.file_private_pages=stats.file_private_pages;
        a.file_clean_pages=stats.file_clean_pages;a.borrowed_pages=stats.borrowed_pages;
        return copy_to_user((void *)(uintptr_t)argument,&a,sizeof(a))?0:-BK_EFAULT;
    }
    if(request==BLES_GFX_WINE_MEMORY) {
        bles_gfx_wine_memory_t a;
        if(!copy_from_user(&a,(void *)(uintptr_t)argument,sizeof(a)))return -BK_EFAULT;
        uint32_t pid=a.pid;
        kmemset(&a,0,sizeof(a));a.pid=pid;
        system_memory_info_t total;mm_get_system_info(&total);
        a.system_allocated_bytes=total.used_bytes;
        mm_resident_pages(&a.heap_resident_pages,&a.vm_allocated_pages);
        if(pid) {
            a.process_heap_bytes=mm_get_process_usage(pid);
            kernel_domains_enter(KDOMAIN_VFS);
            syscall_resources_t *r=resource_for(pid,false);
            if(r && r->vm_initialized) {
                a.brk_capacity=r->vm.brk_capacity;
                a.brk_committed=r->vm.brk_current-r->vm.brk_base;
                if(!sys_vm_memory_totals(&r->vm,&a.virtual_bytes,&a.committed_bytes)) {
                    kernel_domains_exit(KDOMAIN_VFS);return -BK_ENOMEM;
                }
            }
            kernel_domains_exit(KDOMAIN_VFS);
            pager_stats_t stats;pager_stats(pid,&stats);
            a.paged_virtual_pages=stats.virtual_pages;
            a.paged_resident_pages=stats.resident_pages;a.swapped_pages=stats.swapped_pages;
            if(!paging_memory_pages(pid,&a.mapped_pages,&a.resident_pages,
                &a.writable_pages,&a.cow_pages,&a.table_pages))return -BK_ENOMEM;
        }
        return copy_to_user((void *)(uintptr_t)argument,&a,sizeof(a))?0:-BK_EFAULT;
    }
    if(request==BLES_GFX_WINE_CACHE_PERF) {
        bles_gfx_wine_cache_perf_t args;
        kernel_domains_enter(KDOMAIN_VFS);
        vfs_cache_perf(&args.cache_entries,&args.cache_bytes,&args.cache_evictions,
                       &args.vfs_handles_peak,&args.vfs_handles,&args.private_fds);
        kernel_domains_exit(KDOMAIN_VFS);
        return copy_to_user((void *)(uintptr_t)argument,&args,sizeof(args))?0:-BK_EFAULT;
    }
    if(request==BLES_GFX_WINE_VM_PERF) {
        bles_gfx_wine_vm_perf_t args;
        syscall_resources_t *resource=NULL;
        uint32_t hz;
        if(!copy_from_user(&args,(const void *)(uintptr_t)argument,sizeof(args)))return -BK_EFAULT;
        if(args.pid) {
            resource=resource_for(args.pid,false);
            if(!resource)return -BK_EINVAL;
            args.mmap_calls=resource->vm_perf_mmap_calls;
            args.file_maps=resource->vm_perf_file_maps;
            args.cached_file_maps=resource->vm_perf_cached_file_maps;
            args.cache_hits=resource->vm_perf_cache_hits;
            args.cache_misses=resource->vm_perf_cache_misses;
            args.fallback_reads=resource->vm_perf_fallback_reads;
            args.file_io_kib=resource->vm_perf_file_io_kib;
            hz=pit_get_frequency_hz();
            args.file_io_ms=hz?(uint32_t)(((uint64_t)resource->vm_perf_file_io_ticks*1000U)/hz):0U;
        } else {
            /* pid=0 is a cumulative kernel-wide snapshot. Unlike a per-PID
             * resource slot it survives exec helpers that exit before the
             * first X surface is visible. */
            args.mmap_calls=g_vm_perf_totals.mmap_calls;
            args.file_maps=g_vm_perf_totals.file_maps;
            args.cached_file_maps=g_vm_perf_totals.cached_file_maps;
            args.cache_hits=g_vm_perf_totals.cache_hits;
            args.cache_misses=g_vm_perf_totals.cache_misses;
            args.fallback_reads=g_vm_perf_totals.fallback_reads;
            args.file_io_kib=g_vm_perf_totals.file_io_kib;
            hz=pit_get_frequency_hz();
            args.file_io_ms=hz?(uint32_t)(((uint64_t)g_vm_perf_totals.file_io_ticks*1000U)/hz):0U;
        }
        return copy_to_user((void *)(uintptr_t)argument,&args,sizeof(args))?0:-BK_EFAULT;
    }
    if(request==BLES_GFX_OPEN) {
        bles_gfx_open_t args;
        if(!copy_from_user(&args,(const void *)(uintptr_t)argument,sizeof(args)))return -BK_EFAULT;
        if(gfx->width || args.width<160U || args.height<90U || args.width>1024U || args.height>768U ||
           (args.flags & ~(BLES_GFX_OPEN_ROOTLESS|BLES_GFX_OPEN_GALLIUM|BLES_GFX_OPEN_NATIVE|BLES_GFX_OPEN_PIPELINED|BLES_GFX_OPEN_SAVER)) ||
           ((args.flags&BLES_GFX_OPEN_SAVER) && !(args.flags&BLES_GFX_OPEN_GALLIUM)) ||
           ((args.flags&BLES_GFX_OPEN_PIPELINED) && !(args.flags&BLES_GFX_OPEN_GALLIUM)) ||
           ((args.flags&BLES_GFX_OPEN_ROOTLESS) && args.flags!=BLES_GFX_OPEN_ROOTLESS))return -BK_EINVAL;
        uint32_t bytes=args.width*args.height*4U;
        bool rootless=(args.flags&BLES_GFX_OPEN_ROOTLESS)!=0U;
        bool gpu_native=(args.flags&BLES_GFX_OPEN_GALLIUM)!=0U;
        uint32_t *pixels=(rootless||gpu_native)?NULL:(uint32_t *)kmalloc(bytes);
        uint32_t *scratch=(rootless||gpu_native)?NULL:(uint32_t *)kmalloc(bytes);
        if(!rootless && !gpu_native && (!pixels || !scratch)) {
            if(pixels)kfree(pixels);
            if(scratch)kfree(scratch);
            return -BK_ENOMEM;
        }
        if(pixels)kmemset(pixels,0,bytes);
        if(scratch)kmemset(scratch,0,bytes);
        gui_window_t *window=NULL;
        if(!(args.flags&BLES_GFX_OPEN_ROOTLESS)) {
            window=bk_gui_create_window(NULL,32,40,(int)args.width,(int)args.height,
                (args.flags&(BLES_GFX_OPEN_GALLIUM|BLES_GFX_OPEN_NATIVE))?"OpenGL / BlesKernOS":"TinyX / BlesKernOS");
            if(!window){kfree(pixels);kfree(scratch);return -BK_ENOMEM;}
            if(args.flags&(BLES_GFX_OPEN_GALLIUM|BLES_GFX_OPEN_NATIVE)) {
                gui_rect_t bounds,content;
                if(bk_gui_window_bounds(window,&bounds) && bk_gui_window_content_rect(window,&content)) {
                    bounds.w+=(int)args.width-content.w;
                    bounds.h+=(int)args.height-content.h;
                    /* Keep the requested client pixels at 1:1 when the
                     * decorated window fits; larger images may still scale. */
                    (void)bk_gui_window_set_bounds(window,bounds);
                }
            }
        }
        gfx->width=(uint16_t)args.width;gfx->height=(uint16_t)args.height;
        gfx->view_x=0;gfx->view_y=0;
        gfx->view_width=(uint16_t)args.width;gfx->view_height=(uint16_t)args.height;
        gfx->instance_id=task_current_process_id();
        gfx->pixels=pixels;gfx->scratch=scratch;gfx->scratch_capacity=(rootless||gpu_native)?0U:bytes;gfx->window=window;
        gfx->force_present=true;
        gfx->gallium_window=(args.flags&BLES_GFX_OPEN_GALLIUM)!=0U;
        gfx->native_queue_limit=(args.flags&BLES_GFX_OPEN_PIPELINED)?3U:1U;
        gfx->native_window=(args.flags&(BLES_GFX_OPEN_GALLIUM|BLES_GFX_OPEN_NATIVE))!=0U;
        gfx->saver_window=(args.flags&BLES_GFX_OPEN_SAVER)!=0U;
        /* Fullscreen is part of window creation, not a second best-effort
         * ioctl after the GPU has already published its first grey window. */
        if(gfx->saver_window && window) {
            gui_desktop_t *desktop=gui_get_desktop();
            if(!desktop || !desktop->surface.width || !desktop->surface.height) {
                kprintf("[GALLIUM:SAVER] no desktop geometry\n");
                return -BK_EINVAL;
            }
            bk_gui_window_set_borderless(window,true,0U);
            if(!bk_gui_window_set_bounds(window,(gui_rect_t){0,0,
                     desktop->surface.width,desktop->surface.height})) {
                kprintf("[GALLIUM:SAVER] fullscreen bounds failed\n");
                return -BK_EINVAL;
            }
            /* The gui_window belongs to Vision in Ring 3.  Never write its
             * fields from the Linux syscall context: communicate via IPC. */
            bk_vision_request_t saver_request={0};
            uint32_t saver_result=0U;
            saver_request.operation=VISION_IPC_GPU_SAVER;
            saver_request.window=(uint32_t)(uintptr_t)window;
            saver_request.values[0]=1U;
            if(!vision_ipc_call(&saver_request,&saver_result) || !saver_result) {
                kprintf("[GALLIUM:SAVER] Vision saver ownership failed\n");
                return -BK_EIO;
            }
            kprintf("[GALLIUM:SAVER] fullscreen %ux%u (GPU canvas %ux%u)\n",
                    desktop->surface.width,desktop->surface.height,args.width,args.height);
        }
        for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++)gfx->surfaces[i].owner=gfx;
        if(window) {
            bk_gui_window_set_streaming(window,true);
            gui_window_set_extended_events(window,true);
            gui_window_set_event_handler(window,bles_gfx_on_event,gfx);
            bk_gui_focus_window(NULL,window);
        }
        return 0;
    }
    if(request==BLES_GFX_VIEWPORT) {
        bles_gfx_viewport_t args;
        bles_gfx_context *target=NULL;
        gui_rect_t content,bounds;
        int chrome_w,chrome_h;
        if(!copy_from_user(&args,(const void *)(uintptr_t)argument,sizeof(args)))return -BK_EFAULT;
        if(!args.instance_id || !args.width || !args.height)return -BK_EINVAL;
        {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            target=bles_gfx_find_instance(args.instance_id);
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        if(!target || args.x>=target->width || args.y>=target->height ||
           args.width>target->width-args.x || args.height>target->height-args.y)
            return -BK_EINVAL;
        if(!target->window)return 0;
        content=gui_window_content_rect(target->window);
        bounds=target->window->bounds;
        chrome_w=bounds.w-content.w;chrome_h=bounds.h-content.h;
        bounds.w=(int)args.width+chrome_w;bounds.h=(int)args.height+chrome_h;
        if(bounds.w<64)bounds.w=64;
        if(bounds.h<64)bounds.h=64;
        if(!bk_gui_window_set_bounds(target->window,bounds))return -BK_EINVAL;
        {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            target->view_x=(uint16_t)args.x;target->view_y=(uint16_t)args.y;
            target->view_width=(uint16_t)args.width;target->view_height=(uint16_t)args.height;
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        return 0;
    }
    if(request==BLES_GFX_SURFACE_UPSERT) {
        bles_gfx_surface_t args;
        bles_gfx_context *target=NULL;
        bles_gfx_surface *surface=NULL,*free_slot=NULL,*parent_surface=NULL;
        gui_rect_t bounds,content;
        int chrome_w=0,chrome_h=0;
        bool new_mapping=false;
        bool stack_changed=false;
        if(!copy_from_user(&args,(const void *)(uintptr_t)argument,sizeof(args)))return -BK_EFAULT;
        args.title[sizeof(args.title)-1]='\0';
        if(!args.instance_id || !args.surface_id || !args.width || !args.height)return -BK_EINVAL;
        {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            target=bles_gfx_find_instance(args.instance_id);
            if(target)for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++) {
                bles_gfx_surface *candidate=&target->surfaces[i];
                if(candidate->active && candidate->surface_id==args.surface_id)surface=candidate;
                if(args.parent_id && candidate->active && candidate->surface_id==args.parent_id)
                    parent_surface=candidate;
                if(!candidate->active && !free_slot)free_slot=candidate;
            }
            if(!surface){surface=free_slot;new_mapping=true;}
            if(surface)stack_changed=new_mapping || surface->z_index!=args.z_index;
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        if(!target || !surface || args.x>=target->width || args.y>=target->height ||
           args.width>target->width-args.x || args.height>target->height-args.y)
            return -BK_EINVAL;
        if(!surface->window) {
            int host_x=32+(int)args.x,host_y=40+(int)args.y;
            if(parent_surface && parent_surface->window) {
                host_x=parent_surface->window->bounds.x+
                    (int)args.x-(int)parent_surface->source_x;
                host_y=parent_surface->window->bounds.y+
                    (int)args.y-(int)parent_surface->source_y;
            }
            surface->window=bk_gui_create_window(NULL,host_x,host_y,
                (int)args.width,(int)args.height,args.title[0]?args.title:"Windows application");
            if(!surface->window)return -BK_ENOMEM;
            bk_gui_window_set_streaming(surface->window,true);
            gui_window_set_extended_events(surface->window,true);
            gui_window_set_event_handler(surface->window,bles_gfx_surface_on_event,surface);
            bk_gui_window_set_owner(surface->window,target->instance_id);
        } else {
            gui_window_restore(surface->window);
        }
        content=gui_window_content_rect(surface->window);
        bounds=surface->window->bounds;
        if(new_mapping) {
            bounds.x=32+(int)args.x;bounds.y=40+(int)args.y;
            if(parent_surface && parent_surface->window) {
                bounds.x=parent_surface->window->bounds.x+
                    (int)args.x-(int)parent_surface->source_x;
                bounds.y=parent_surface->window->bounds.y+
                    (int)args.y-(int)parent_surface->source_y;
            }
        } else if(bounds.x==(int)surface->source_x+surface->host_offset_x &&
                  bounds.y==(int)surface->source_y+surface->host_offset_y) {
            /* The X client moved the window. Keep the native chrome aligned.
             * A native drag is handled through SURFACE_MOVE below. */
            bounds.x=(int)args.x+surface->host_offset_x;
            bounds.y=(int)args.y+surface->host_offset_y;
        }
        chrome_w=bounds.w-content.w;chrome_h=bounds.h-content.h;
        bounds.w=(int)args.width+chrome_w;bounds.h=(int)args.height+chrome_h;
        if(bounds.w<80)bounds.w=80;
        if(bounds.h<GUI_TITLEBAR_HEIGHT+30)bounds.h=GUI_TITLEBAR_HEIGHT+30;
        if(new_mapping && surface->window->desktop) {
            int max_x=surface->window->desktop->surface.width-bounds.w;
            int max_y=surface->window->desktop->surface.height-30-bounds.h;
            if(max_x<0)max_x=0;
            if(max_y<0)max_y=0;
            if(bounds.x>max_x)bounds.x=max_x;
            if(bounds.y>max_y)bounds.y=max_y;
        }
        if(bounds.x!=surface->window->bounds.x ||
           bounds.y!=surface->window->bounds.y ||
           bounds.w!=surface->window->bounds.w ||
           bounds.h!=surface->window->bounds.h)
            (void)bk_gui_window_set_bounds(surface->window,bounds);
        surface->window->resizable=(args.flags&BLES_GFX_SURFACE_FIXED)==0U;
        {
            const char *new_title=args.title[0]?args.title:"Windows application";
            bool title_changed=kstrcmp(surface->window->title,new_title)!=0;
            kstrncpy(surface->window->title,new_title,sizeof(surface->window->title)-1U);
            surface->window->title[sizeof(surface->window->title)-1U]='\0';
            surface->window->dirty=true;
            if(title_changed && surface->window->desktop)
                gui_desktop_shell_window_state_changed(surface->window->desktop);
        }
        {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            surface->owner=target;surface->surface_id=args.surface_id;
            surface->parent_id=args.parent_id;surface->flags=args.flags;
            surface->z_index=args.z_index;
            surface->source_x=(uint16_t)args.x;surface->source_y=(uint16_t)args.y;
            surface->source_w=(uint16_t)args.width;surface->source_h=(uint16_t)args.height;
            if(new_mapping) {
                surface->host_offset_x=(int16_t)(surface->window->bounds.x-(int)args.x);
                surface->host_offset_y=(int16_t)(surface->window->bounds.y-(int)args.y);
            }
            surface->active=true;
            target->force_present=true;
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        if(target->window && target->window->visible)gui_window_close(target->window);
        bles_gfx_modal_inputs(target);
        if(stack_changed && surface->window->desktop)
            gui_desktop_raise_window(surface->window->desktop,surface->window);
        if(new_mapping || (args.flags&BLES_GFX_SURFACE_DIALOG))
            bk_gui_focus_window(NULL,surface->window);
        return 0;
    }
    if(request==BLES_GFX_SURFACE_REMOVE) {
        bles_gfx_surface_t args;
        bles_gfx_context *target=NULL;
        bles_gfx_surface *surface=NULL,*parent_surface=NULL;
        uint32_t parent_id=0U;
        if(!copy_from_user(&args,(const void *)(uintptr_t)argument,sizeof(args)))return -BK_EFAULT;
        {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            target=bles_gfx_find_instance(args.instance_id);
            if(target)for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++)
                if(target->surfaces[i].active && target->surfaces[i].surface_id==args.surface_id) {
                    surface=&target->surfaces[i];parent_id=surface->parent_id;surface->active=false;break;
                }
            if(target && parent_id)for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++)
                if(target->surfaces[i].active && target->surfaces[i].surface_id==parent_id) {
                    parent_surface=&target->surfaces[i];break;
                }
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        if(!surface)return 0;
        /* Restore the parent before GUI chooses the next focused window. */
        bles_gfx_modal_inputs(target);
        if(parent_surface && parent_surface->window && parent_surface->window->input_enabled)
            bk_gui_focus_window(NULL,parent_surface->window);
        if(surface->window)gui_window_close(surface->window);
        /* Closing an X dialog can clear X input focus after the native parent
         * has already gained focus. Reconcile it even when GUI focus did not
         * change; otherwise the launcher never receives another focus event. */
        if(parent_surface && parent_surface->window && parent_surface->window->input_enabled) {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            if(parent_surface->active)
                bles_gfx_push_surface_event(parent_surface,BLES_GFX_SURFACE_FOCUS);
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        target->force_present=true;
        return 0;
    }
    if(request==BLES_GFX_SURFACE_EVENT) {
        bles_gfx_surface_event_t args,event;
        bles_gfx_context *target=NULL;
        if(!copy_from_user(&args,(const void *)(uintptr_t)argument,sizeof(args)))return -BK_EFAULT;
        {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            target=bles_gfx_find_instance(args.instance_id);
            if(target)for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++) {
                bles_gfx_surface *surface=&target->surfaces[i];
                if(!surface->active || !surface->window)continue;
                int32_t x=surface->window->bounds.x-surface->host_offset_x;
                int32_t y=surface->window->bounds.y-surface->host_offset_y;
                if(x!=(int32_t)surface->source_x || y!=(int32_t)surface->source_y) {
                    int32_t max_x=(int32_t)target->width-surface->source_w;
                    int32_t max_y=(int32_t)target->height-surface->source_h;
                    if(x<0)x=0;
                    if(y<0)y=0;
                    if(x>max_x)x=max_x;
                    if(y>max_y)y=max_y;
                    if(((target->surface_write_pos+1U)&63U)==target->surface_read_pos)
                        continue;
                    surface->host_offset_x=(int16_t)(surface->window->bounds.x-x);
                    surface->host_offset_y=(int16_t)(surface->window->bounds.y-y);
                    bles_gfx_push_surface_event(surface,BLES_GFX_SURFACE_MOVE);
                    surface->source_x=(uint16_t)x;
                    surface->source_y=(uint16_t)y;
                }
            }
            if(!target || target->surface_read_pos==target->surface_write_pos) {
                kspin_unlock_irqrestore(&linux_io_lock,lock);return 0;
            }
            event=target->surface_events[target->surface_read_pos];
            target->surface_read_pos=(uint8_t)((target->surface_read_pos+1U)&63U);
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        return copy_to_user((void *)(uintptr_t)argument,&event,sizeof(event))?1:-BK_EFAULT;
    }
    if(request==BLES_GFX_PRESENT) {
        if(gfx->gallium_window)return -BK_EINVAL;
        bles_gfx_present_t args;
        bool have_rootless=false;
        if(!copy_from_user(&args,(const void *)(uintptr_t)argument,sizeof(args)))return -BK_EFAULT;
        if(!gfx->width || args.width!=gfx->width || args.height!=gfx->height)return -BK_EINVAL;
        if(!gfx->window) {
            bool visible=false;
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++)
                if(gfx->surfaces[i].active && gfx->surfaces[i].window) { visible=true; break; }
            kspin_unlock_irqrestore(&linux_io_lock,lock);
            if(!visible){gfx->force_present=false;return 0;}
        }
        {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            gfx->force_present=false;
            kspin_unlock_irqrestore(&linux_io_lock,lock);
        }
        uint32_t framebuffer_bytes=(uint32_t)gfx->width*gfx->height*4U;
        if(!paging_user_range_permitted((void *)(uintptr_t)args.pixels,framebuffer_bytes,false))return -BK_EFAULT;
        if(gfx->pixels && !copy_from_user(gfx->pixels,(const void *)(uintptr_t)args.pixels,
                                           framebuffer_bytes))return -BK_EFAULT;
        for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++) {
            bles_gfx_surface *surface=&gfx->surfaces[i];
            gui_window_t *window;
            uint32_t sx,sy,sw,sh,z_index;
            {
                uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
                if(!surface->active || !surface->window){kspin_unlock_irqrestore(&linux_io_lock,lock);continue;}
                window=surface->window;sx=surface->source_x;sy=surface->source_y;
                sw=surface->source_w;sh=surface->source_h;
                z_index=surface->z_index;
                kspin_unlock_irqrestore(&linux_io_lock,lock);
            }
            if(!sw || !sh || sx+sw>gfx->width || sy+sh>gfx->height)continue;
            have_rootless=true;
            uint32_t surface_bytes=sw*sh*4U;
            if(surface_bytes>gfx->scratch_capacity) {
                uint32_t *scratch=kmalloc(surface_bytes);
                if(!scratch)return -BK_ENOMEM;
                if(gfx->scratch)kfree(gfx->scratch);
                gfx->scratch=scratch;gfx->scratch_capacity=surface_bytes;
            }
            uint32_t offset=(sy*(uint32_t)gfx->width+sx)*4U;
            if(gfx->pixels) {
                for(uint32_t row=0;row<sh;row++)
                    kmemcpy(gfx->scratch+row*sw,
                            (uint8_t *)gfx->pixels+offset+row*gfx->width*4U,sw*4U);
            } else if(!copy_rows_from_user(gfx->scratch,
                    (const void *)(uintptr_t)(args.pixels+offset),sw*4U,sh,gfx->width*4U)) {
                /* Under memory pressure retain the bounded per-row path. */
                for(uint32_t row=0;row<sh;row++)
                    if(!copy_from_user(gfx->scratch+row*sw,
                        (const void *)(uintptr_t)(args.pixels+offset+row*gfx->width*4U),sw*4U))return -BK_EFAULT;
            }
            bles_gfx_preserve_occluded(gfx,surface,window,sx,sy,sw,sh,z_index);
            (void)bk_gui_window_present_xrgb(window,gfx->scratch,(uint16_t)sw,(uint16_t)sh);
        }
        if(have_rootless || !gfx->window)return 0;
        {
            uint32_t view_w,view_h,view_x,view_y;
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            view_x=gfx->view_x;view_y=gfx->view_y;
            view_w=gfx->view_width?gfx->view_width:gfx->width;
            view_h=gfx->view_height?gfx->view_height:gfx->height;
            kspin_unlock_irqrestore(&linux_io_lock,lock);
            if(view_x==0U && view_y==0U && view_w==gfx->width && view_h==gfx->height)
                return bk_gui_window_present_xrgb(gfx->window,gfx->pixels,gfx->width,gfx->height)?0:-BK_EIO;
            for(uint32_t row=0;row<view_h;row++)
                kmemcpy(gfx->scratch+row*view_w,
                        gfx->pixels+(view_y+row)*(uint32_t)gfx->width+view_x,
                        view_w*4U);
            return bk_gui_window_present_xrgb(gfx->window,gfx->scratch,
                                               (uint16_t)view_w,(uint16_t)view_h)?0:-BK_EIO;
        }
    }
    if(request==BLES_GFX_EVENT) {
        bles_gfx_event_t event;
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        if(gfx->read_pos==gfx->write_pos){kspin_unlock_irqrestore(&linux_io_lock,lock);return 0;}
        event=gfx->events[gfx->read_pos];
        gfx->read_pos=(uint8_t)((gfx->read_pos+1U)&63U);
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        return copy_to_user((void *)(uintptr_t)argument,&event,sizeof(event))?1:-BK_EFAULT;
    }
    if(request==BLES_GFX_NEEDS_PRESENT) {
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        bool needed=gfx->force_present;
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        return needed?1:0;
    }
    return -BK_ENOTTY;
}

static int32_t bles_gfx_ioctl(linux_device *device,uint32_t request,uint32_t argument) {
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    bles_gfx_context *gfx=device->gfx;
    if(!device->refs || device->refs==0xffffffffU || device->kind!=3U || !gfx) {
        kspin_unlock_irqrestore(&linux_io_lock,lock);return -BK_EBADF;
    }
    __sync_add_and_fetch(&gfx->native_consumers,1U);
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    int32_t result=bles_gfx_ioctl_pinned(device,request,argument);
    __sync_sub_and_fetch(&gfx->native_consumers,1U);
    return result;
}
static bool sys_raw_retain(int fd) {
    linux_device *d=linux_device_for(fd);
    if(d) {
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        bool ok=d->refs && d->refs!=0xffffffffU;
        if(ok)d->refs++;
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        return ok;
    }
    if(fd<LINUX_IO_BASE)return vfs_retain(fd);
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    linux_endpoint *e=linux_endpoint_for(fd);bool ok=e && e->refs;
    if(ok)++e->refs;
    kspin_unlock_irqrestore(&linux_io_lock,lock);return ok;
}
static bool sys_raw_close(int fd) {
    linux_device *d=linux_device_for(fd);
    if(d) {
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        if(d->refs==0xffffffffU){kspin_unlock_irqrestore(&linux_io_lock,lock);return false;}
        bool last=d->refs==1U;
        d->refs=last && d->gfx?0xffffffffU:d->refs-1U;
        bles_gfx_context *gfx=last?d->gfx:NULL;
        uint8_t *pcm=last?d->pcm:NULL;
        struct linux_inotify *inotify=last?d->inotify:NULL;
        if(inotify) {
            d->inotify=NULL;
            if(!--linux_inotify_instances)vfs_observe_changes(NULL);
        }
        if(last)d->pcm=NULL;
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        if(inotify)kfree(inotify);
        if(pcm)kfree(pcm);
        if(gfx) {
            bles_gfx_launch_busy(gfx,false);
            bles_gfx_3d_close(gfx);
            if(gfx->window) {
                gui_window_set_event_handler(gfx->window,NULL,NULL);
                gui_window_close(gfx->window);
                gfx->window=NULL;
            }
            for(unsigned i=0;i<BLES_GFX_MAX_SURFACES;i++) {
                bles_gfx_surface *surface=&gfx->surfaces[i];
                if(surface->window) {
                    gui_window_set_event_handler(surface->window,NULL,NULL);
                    gui_window_close(surface->window);
                    surface->window=NULL;
                }
                surface->active=false;
            }
            if(gfx->pixels){kfree(gfx->pixels);gfx->pixels=NULL;}
            if(gfx->scratch){kfree(gfx->scratch);gfx->scratch=NULL;}
        }
        if(gfx) {
            lock=kspin_lock_irqsave(&linux_io_lock);
            d->refs=0;d->kind=0;d->gfx=NULL;
            kspin_unlock_irqrestore(&linux_io_lock,lock);
            kfree(gfx);
        }
        return true;
    }
    if(fd<LINUX_IO_BASE)return vfs_close(fd);
    int discarded[72];uint32_t count=0;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    linux_endpoint *e=linux_endpoint_for(fd);
    if(!e || !e->refs){kspin_unlock_irqrestore(&linux_io_lock,lock);return false;}
    linux_io_pair *pair=linux_pairs[(fd-LINUX_IO_BASE)/2];
    bool endpoint_closed=false;
    if(!--e->refs) {
        endpoint_closed=true;
        if(linux_names)for(unsigned i=0;i<32;i++)if(linux_names[i].length && linux_names[i].raw==fd)linux_names[i].raw=-1;
        e->read_closed=e->write_closed=true;
        for(unsigned i=0;i<8;i++) {
            for(unsigned j=0;j<e->rights[i].count;j++)discarded[count++]=e->rights[i].fd[j];
            e->rights[i].count=0;
        }
        while(e->pending)discarded[count++]=e->backlog[--e->pending];
        if(!e->name[2])e->name_length=0; /* los nombres abstractos desaparecen */
    }
    for(unsigned i=0;i<count;i++){linux_endpoint *target=linux_endpoint_for(discarded[i]);if(target)target->inflight--;}
    /* Dropping a temporary read/write reference cannot make a cycle
     * unreachable while another external reference still roots this endpoint.
     * Avoid tracing the whole socket graph after every Wine RPC transfer. */
    bool collect=e->refs==e->inflight;
    bool release=!pair->end[0].refs && !pair->end[1].refs;
    if(release)linux_pairs[(fd-LINUX_IO_BASE)/2]=NULL;
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    for(unsigned i=0;i<count;i++)sys_raw_close(discarded[i]);
    if(release)mm_free_guarded_stack(pair->allocation,pair,pair->size);
    if(endpoint_closed)linux_io_notify(fd);
    if(collect)linux_unix_gc();
    return true;
}

/* Recolector de ciclos SCM_RIGHTS. Las referencias en colas son aristas;
 * descriptores instalados y operaciones activas constituyen las raices. */
static void linux_unix_gc(void) {
    W3P_SCOPE(W3P_IPC_GC, 0U);
    static bool running;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    if(running){kspin_unlock_irqrestore(&linux_io_lock,lock);return;}running=true;
    for(;;) {
        bool marked[LINUX_IO_ENDPOINTS]={0},changed;int discarded[8];uint32_t count=0;
        for(unsigned i=0;i<LINUX_IO_ENDPOINTS;i++) {linux_endpoint *e=linux_endpoint_for(LINUX_IO_BASE+i);marked[i]=e && e->refs>e->inflight;}
        do {
            changed=false;
            for(unsigned i=0;i<LINUX_IO_ENDPOINTS;i++)if(marked[i]) {
                linux_endpoint *e=linux_endpoint_for(LINUX_IO_BASE+i);if(!e)continue;
                for(unsigned j=0;j<8;j++)for(unsigned k=0;k<e->rights[j].count;k++) {
                    unsigned n=e->rights[j].fd[k]-LINUX_IO_BASE;
                    if(n<LINUX_IO_ENDPOINTS && !marked[n]){marked[n]=true;changed=true;}
                }
                for(unsigned j=0;j<e->pending;j++) {unsigned n=e->backlog[j]-LINUX_IO_BASE;
                    if(n<LINUX_IO_ENDPOINTS && !marked[n]){marked[n]=true;changed=true;}}
            }
        }while(changed);
        for(unsigned i=0;i<LINUX_IO_ENDPOINTS && !count;i++)if(!marked[i]) {
            linux_endpoint *e=linux_endpoint_for(LINUX_IO_BASE+i);if(!e)continue;
            for(unsigned j=0;j<8;j++)if(e->rights[j].count) {
                count=e->rights[j].count;kmemcpy(discarded,e->rights[j].fd,count*4);e->rights[j].count=0;break;
            }
            if(!count && e->pending)discarded[count++]=e->backlog[--e->pending];
        }
        if(!count)break;
        for(unsigned i=0;i<count;i++){linux_endpoint *e=linux_endpoint_for(discarded[i]);if(e)e->inflight--;}
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        for(unsigned i=0;i<count;i++)sys_raw_close(discarded[i]);
        lock=kspin_lock_irqsave(&linux_io_lock);
    }
    running=false;kspin_unlock_irqrestore(&linux_io_lock,lock);
}
static int linux_install_fd(syscall_resources_t *r,int raw,uint32_t flags) {
    for(unsigned fd=3;fd<SYSCALL_FD_SLOTS+3;fd++)if(resource_fd(r,fd)<0) {
        r->fds[fd-3]=raw;r->linux_fd_flags[fd]=(flags&0x80000)?1:0;
        r->linux_open_flags[fd]=flags&~0x80000U;return (int)fd;
    }
    return -BK_EMFILE;
}
static int linux_pair_create(bool pipe,bool connected,uint32_t flags) {
    void *allocation;uint32_t size;
    uint32_t capacity=pipe?LINUX_PIPE_BUF:LINUX_SOCKET_BUFFER;
    linux_io_pair *p=mm_alloc_guarded_stack(sizeof(*p)+2U*capacity,false,&allocation,&size);
    if(!p)return -BK_ENOMEM;
    kmemset(p,0,sizeof(*p));p->allocation=allocation;p->size=size;p->pipe=pipe;p->connected=connected;p->capacity=capacity;
    for(unsigned i=0;i<2;i++)p->end[i].buffer=(uint8_t *)(p+1)+i*capacity;
    for(unsigned i=0;i<2;i++){p->end[i].refs=connected||!i?1:0;p->end[i].flags=flags&0x800;
        p->end[i].pid=task_current_process_id();}
    if(pipe){p->end[0].write_closed=true;p->end[1].read_closed=true;}
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);int result=-BK_EMFILE;
    for(unsigned i=0;i<LINUX_IO_PAIRS;i++)if(!linux_pairs[i]){linux_pairs[i]=p;p->inode=linux_io_inode;linux_io_inode+=2;result=LINUX_IO_BASE+i*2;break;}
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    if(result<0) {
        kprintf("[LINUX:IPC] pair table full pid=%u capacity=%u\n",
                task_current_process_id(), LINUX_IO_PAIRS);
        mm_free_guarded_stack(allocation,p,size);
    }
    return result;
}

static int32_t linux_make_pair(uint32_t pointer,uint32_t flags,bool pipe) {
    if(flags&~(0x80000U|0x800U))return -BK_EINVAL;
    if(!user_access_ok((void *)pointer,8,true))return -BK_EFAULT;
    syscall_resources_t *r=resource_for(task_current_process_id(),true);
    if(!r)return -BK_ENOMEM;
    int raw=linux_pair_create(pipe,true,flags);if(raw<0)return raw;
    int fds[2];fds[0]=linux_install_fd(r,raw,flags|(pipe?0:2));
    fds[1]=fds[0]<0?-BK_EMFILE:linux_install_fd(r,raw+1,flags|(pipe?1:2));
    if(fds[0]<0 || fds[1]<0 || !copy_to_user((void *)pointer,fds,8)) {
        for(unsigned i=0;i<2;i++){if(fds[i]>=0)r->fds[fds[i]-3]=-1;sys_raw_close(raw+i);}
        return fds[0]<0?fds[0]:fds[1]<0?fds[1]:-BK_EFAULT;
    }
    return 0;
}

/* IO no conserva locks mientras duerme. PIPE_BUF mantiene atomicidad para
 * escrituras pequenas; los streams permiten transferencias parciales. */
static int32_t linux_io(int fd,void *buffer,uint32_t length,bool write,uint32_t flags,
                         linux_rights *control) {
    W3P_SCOPE(write ? W3P_IPC_WRITE : W3P_IPC_READ, length);
    if(!length)return 0;
    if(flags&~(0x40U|0x4000U|0x40000000U))return -BK_ENOSYS;
    /* Prefault/pin before the endpoint spinlock. copy_*_user itself can wait
     * for the pager mutex; calling it with IF=0 freezes a CPU when a peer
     * faults or swaps concurrently. The pin remains held across queue waits. */
    uint32_t io_pid=task_current_process_id();
    if(!pager_pin(io_pid,(uint32_t)(uintptr_t)buffer,length,!write))return -BK_EFAULT;
    if(!user_access_ok(buffer,length,!write)) {
        pager_unpin(io_pid,(uint32_t)(uintptr_t)buffer,length);return -BK_EFAULT;
    }
    if(!sys_raw_retain(fd)) {
        pager_unpin(io_pid,(uint32_t)(uintptr_t)buffer,length);return -BK_EBADF;
    }
    int32_t result;int discarded[64];uint32_t discarded_count=0;
    uint32_t empty_rounds=0;
    bool changed=false;
    uint32_t wait_key=linux_io_pair_key(fd);
    for(;;) {
        /* Arm before inspecting the queue. If a peer changes it after this
         * point, task_event_signal() is latched and commit() cannot miss it. */
        bool wait_armed=linux_io_wait_arm(wait_key);
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        linux_endpoint *e=linux_endpoint_for(fd),*peer=linux_endpoint_for(fd^1);
        linux_io_pair *p=linux_pairs[(fd-LINUX_IO_BASE)/2];
        if(!p->connected){result=-107;goto unlock;}
        if((write && p->pipe && e->write_closed) || (!write && p->pipe && e->read_closed)){result=-BK_EBADF;goto unlock;}
        if(write && (e->write_closed || peer->read_closed || !peer->refs)){result=-32;goto unlock;}
        if(!write && e->read_closed){result=0;goto unlock;}
        uint32_t n=write?p->capacity-peer->used:e->used;
        if(n>length)n=length;
        if(!write)for(unsigned i=0;i<8;i++)if(e->rights[i].count) {
            uint32_t distance=e->rights[i].offset-e->read_seq;
            if(distance && distance<n)n=distance;
        }
        if(write && p->pipe && length<=LINUX_PIPE_BUF && n<length)n=0;
        int rights_slot=-1;
        if(write && control && control->count) {
            for(unsigned i=0;i<8;i++)if(!peer->rights[i].count){rights_slot=i;break;}
            if(rights_slot<0)n=0;
        }
        if(n) {
            linux_endpoint *in=write?peer:e;
            uint32_t pos=write?in->write_pos:in->read_pos;
            uint32_t first=p->capacity-pos;if(first>n)first=n;
            if(write) {
                kmemcpy(in->buffer+pos,buffer,first);
                kmemcpy(in->buffer,(uint8_t *)buffer+first,n-first);
            } else {
                kmemcpy(buffer,in->buffer+pos,first);
                kmemcpy((uint8_t *)buffer+first,in->buffer,n-first);
            }
            if(write) {
                if(rights_slot>=0){peer->rights[rights_slot]=*control;peer->rights[rights_slot].offset=in->write_seq;
                    for(unsigned j=0;j<control->count;j++){linux_endpoint *target=linux_endpoint_for(control->fd[j]);if(target)target->inflight++;}
                    control->count=0;}
                in->write_pos=(pos+n)%p->capacity;in->used+=n;in->write_seq+=n;
            } else {
                /* Una lectura termina en la primera frontera de control.
                 * Las referencias pasan a recvmsg o se descartan en read. */
                if(control)control->count=0;
                for(unsigned i=0;i<8;i++)if(in->rights[i].count &&
                    (uint32_t)(in->rights[i].offset-in->read_seq)<n) {
                    for(unsigned j=0;j<in->rights[i].count;j++){linux_endpoint *target=linux_endpoint_for(in->rights[i].fd[j]);if(target)target->inflight--;}
                    if(control && !control->count)*control=in->rights[i];
                    else {
                        /* Cerrar fuera del spinlock (puede liberar VFS). */
                        for(unsigned j=0;j<in->rights[i].count;j++)
                            discarded[discarded_count++]=in->rights[i].fd[j];
                    }
                    in->rights[i].count=0;
                }
                in->read_pos=(pos+n)%p->capacity;in->used-=n;in->read_seq+=n;
            }
            changed=true;
            result=n;goto unlock;
        }
        if(!write && (!peer->refs || peer->write_closed)){result=0;goto unlock;}
        if((flags&0x40) || (e->flags&0x800)){result=-BK_EAGAIN;goto unlock;}
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        if(task_linux_signal_pending()){
            linux_io_wait_cancel(wait_armed,wait_key);
            result=-4;break;
        }
        /* Data, EOF, shutdown and signals all wake this subscription. Keep a
         * bounded safety timeout without polling every idle Wine thread at
         * the PIT frequency. Timed poll/select retain their one-tick check. */
        linux_io_wait(&empty_rounds,wait_armed,wait_key,pit_get_frequency_hz());continue;
unlock:
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        linux_io_wait_cancel(wait_armed,wait_key);
        break;
    }
    if(changed)linux_io_notify(fd);
    for(unsigned i=0;i<discarded_count;i++)sys_raw_close(discarded[i]);
    sys_raw_close(fd);
    if(result==-32 && !(flags&0x4000)) {
        registers_t signal={.eax=270,.ebx=task_current_process_id(),.ecx=task_current_tid(),.edx=13};
        task_linux_signal_syscall(&signal);
    }
    pager_unpin(io_pid,(uint32_t)(uintptr_t)buffer,length);
    return result;
}
static int32_t sys_raw_read(int fd,void *buffer,uint32_t length) {
    linux_device *d=linux_device_for(fd);
    if(d) {
        if((d->flags&3)==1) return -BK_EBADF;
        if(d->kind==6)return linux_inotify_read(fd,buffer,length);
        if(d->kind==2) return 0;
        if(d->kind==3 || d->kind==4 || d->kind==5) return -BK_EINVAL;
        kernel_domain_snapshot_t snapshot;kernel_domains_drop_current(&snapshot);
        int32_t result=vga_console_input_read(buffer,length);
        kernel_domains_restore(&snapshot);return result;
    }
    return fd<LINUX_IO_BASE?vfs_read(fd,buffer,length):linux_io(fd,buffer,length,false,0,NULL);
}
static int32_t sys_raw_write(int fd,const void *buffer,uint32_t length) {
    linux_device *d=linux_device_for(fd);
    if(d) {
        if(!(d->flags&3)) return -BK_EBADF;
        if(d->kind==3 || d->kind==5) return -BK_EINVAL;
        if(d->kind==4)return linux_pcm_write(d,buffer,length);
        if(d->kind!=2) for(unsigned i=0;i<length;i++) vga_putchar(((const char *)buffer)[i]);
        return length;
    }
    return fd<LINUX_IO_BASE?vfs_write(fd,buffer,length):linux_io(fd,(void *)buffer,length,true,0,NULL);
}

static uint32_t linux_poll_raw(int raw,uint32_t requested) {
    linux_device *d=linux_device_for(raw);
    if(d) {
        if(d->kind==6)return d->inotify && d->inotify->used?(requested&1U):0U;
        uint32_t ready=(d->flags&3)?4U:(d->kind==2?1U:0U);
        /* KDrive can watch the native input queue alongside its X sockets.
         * Level readiness lasts until BLES_GFX_EVENT drains the queue. */
        if(d->kind==3 && d->gfx && d->gfx->read_pos!=d->gfx->write_pos)
            ready|=1U;
        return requested & ready;
    }
    if(raw<0)return 32;
    if(raw<LINUX_IO_BASE)return requested&(1|4);
    linux_endpoint *e=linux_endpoint_for(raw),*peer=linux_endpoint_for(raw^1);
    if(!e || !e->refs)return 32;
    linux_io_pair *p=linux_pairs[(raw-LINUX_IO_BASE)/2];uint32_t events=0;
    if(e->listen)return e->pending?(requested&1):0;
    if(!p->connected)return 16;
    if(!e->read_closed && (e->used || !peer->refs || peer->write_closed))events|=requested&1;
    if(!e->write_closed && !peer->read_closed && peer->refs && peer->used<p->capacity)events|=requested&4;
    if((!p->pipe || !(raw&1)) && (!peer->refs || peer->write_closed))events|=16;
    if(!e->write_closed && (!peer->refs || peer->read_closed))events|=8;
    return events;
}

static int32_t linux_poll(uint32_t pointer,uint32_t count,int32_t timeout) {
    struct {int32_t fd;uint16_t events,revents;} fds[SYSCALL_FD_SLOTS+3];
    if(count>SYSCALL_FD_SLOTS+3)return -BK_EINVAL;
    if(!copy_from_user(fds,(void *)pointer,count*8) || !user_access_ok((void *)pointer,count*8,true))return -BK_EFAULT;
    syscall_resources_t *r=resource_for(task_current_process_id(),false);
    uint32_t start=pit_get_ticks(),hz=pit_get_frequency_hz();
    uint32_t empty_rounds=0;
    for(;;) {
        uint32_t wait_key=0U;
        for(unsigned i=0;i<count;i++)if(fds[i].fd>=0) {
            uint32_t key=linux_io_pair_key(resource_fd(r,fds[i].fd));
            if(!wait_key)wait_key=key;
            else if(wait_key!=key) {wait_key=linux_io_event_key();break;}
        }
        if(!wait_key)wait_key=linux_io_event_key();
        bool wait_armed=linux_io_wait_arm(wait_key);
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);int result=0;
        for(unsigned i=0;i<count;i++) {
            fds[i].revents=fds[i].fd<0?0:linux_poll_raw(resource_fd(r,fds[i].fd),fds[i].events);
            if(fds[i].revents)result++;
        }
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        if(result || (timeout>=0 && (uint64_t)(pit_get_ticks()-start)*1000>=(uint64_t)(uint32_t)timeout*hz)) {
            linux_io_wait_cancel(wait_armed,wait_key);
            return copy_to_user((void *)pointer,fds,count*8)?result:-BK_EFAULT;
        }
        if(task_linux_signal_pending()){
            linux_io_wait_cancel(wait_armed,wait_key);
            return -4;
        }
        linux_io_wait(&empty_rounds,wait_armed,wait_key,
                      linux_io_poll_ticks(wait_key,start,timeout<0?-1:(int64_t)timeout*1000));
    }
}

static int32_t linux_select(registers_t *r) {
    enum { set_words = (SYSCALL_FD_SLOTS + 3U + 31U) / 32U };
    uint32_t input[3][set_words],output[3][set_words],pointers[3]={r->ecx,r->edx,r->esi};
    if(r->ebx>SYSCALL_FD_SLOTS+3)return -BK_EINVAL;
    uint32_t bytes=((r->ebx+31)/32)*4;
    struct {int32_t sec,usec;} tv;
    int64_t limit=-1;
    if(r->edi){if(!copy_from_user(&tv,(void *)r->edi,8))return -BK_EFAULT;
        if(tv.sec<0 || tv.usec<0 || tv.usec>=(r->eax==308?1000000000:1000000))return -BK_EINVAL;
        limit=(int64_t)tv.sec*1000000+(r->eax==308?(tv.usec+999)/1000:tv.usec);}
    kmemset(input,0,sizeof(input));
    for(unsigned i=0;i<3;i++)if(pointers[i] &&
        (!copy_from_user(input[i],(void *)pointers[i],bytes) || !user_access_ok((void *)pointers[i],bytes,true)))return -BK_EFAULT;
    uint32_t start=pit_get_ticks(),hz=pit_get_frequency_hz();
    syscall_resources_t *resource=resource_for(task_current_process_id(),false);
    uint32_t empty_rounds=0;
    for(;;) {
        uint32_t wait_key=0U;
        for(unsigned fd=0;fd<r->ebx;fd++) {
            if(!((input[0][fd/32]|input[1][fd/32]|input[2][fd/32])&(1U<<(fd%32))))continue;
            uint32_t key=linux_io_pair_key(resource_fd(resource,fd));
            if(!wait_key)wait_key=key;
            else if(wait_key!=key) {wait_key=linux_io_event_key();break;}
        }
        if(!wait_key)wait_key=linux_io_event_key();
        bool wait_armed=linux_io_wait_arm(wait_key);
        kmemset(output,0,sizeof(output));int result=0;
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        for(unsigned fd=0;fd<r->ebx;fd++) {
            uint32_t bit=1U<<(fd%32),word=fd/32;
            if(!((input[0][word]|input[1][word]|input[2][word])&bit))continue;
            uint32_t events=linux_poll_raw(resource_fd(resource,fd),1|4|2);
            if(events&32){
                kspin_unlock_irqrestore(&linux_io_lock,lock);
                linux_io_wait_cancel(wait_armed,wait_key);
                return -BK_EBADF;
            }
            for(unsigned i=0;i<3;i++)if((input[i][word]&bit) && (events&(i==0?(1|16|8):i==1?(4|8):2))) {
                output[i][word]|=bit;result++;
            }
        }
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        int64_t elapsed=(int64_t)(pit_get_ticks()-start)*1000000/hz;
        if(result || (limit>=0 && elapsed>=limit)) {
            linux_io_wait_cancel(wait_armed,wait_key);
            for(unsigned i=0;i<3;i++)if(pointers[i] && !copy_to_user((void *)pointers[i],output[i],bytes))return -BK_EFAULT;
            if(r->edi){int64_t left=limit>elapsed?limit-elapsed:0;tv.sec=left/1000000;tv.usec=left%1000000;
                if(r->eax==308)tv.usec*=1000;
                if(!copy_to_user((void *)r->edi,&tv,8))return -BK_EFAULT;}
            return result;
        }
        if(task_linux_signal_pending()){
            linux_io_wait_cancel(wait_armed,wait_key);
            return -4;
        }
        linux_io_wait(&empty_rounds,wait_armed,wait_key,
                      linux_io_poll_ticks(wait_key,start,limit));
    }
}

static int32_t linux_pselect(registers_t *r) {
    uint32_t maskarg[2]={0};uint64_t mask,old=0;
    if(r->ebp && !copy_from_user(maskarg,(void *)r->ebp,8))return -BK_EFAULT;
    if(maskarg[0]) {
        if(maskarg[1]!=8)return -BK_EINVAL;
        if(!copy_from_user(&mask,(void *)maskarg[0],8))return -BK_EFAULT;
        old=task_linux_mask_swap(mask);
    }
    int32_t result=linux_select(r);
    if(maskarg[0])task_linux_mask_swap(old);
    return result;
}

static int32_t linux_raw_flags(int raw) {
    linux_device *d=linux_device_for(raw);if(d)return d->flags;
    if(raw<LINUX_IO_BASE)return vfs_status_flags(raw);
    linux_endpoint *e=linux_endpoint_for(raw);if(!e || !e->refs)return -BK_EBADF;
    linux_io_pair *p=linux_pairs[(raw-LINUX_IO_BASE)/2];
    return e->flags|(p->pipe?(raw&1):2);
}

static int32_t linux_unix_address(uint32_t pointer,uint32_t length,uint8_t *out) {
    if(length<3 || length>110)return -BK_EINVAL;
    kmemset(out,0,110);
    if(!copy_from_user(out,(void *)pointer,length))return -BK_EFAULT;
    if(*(uint16_t *)out!=1)return -97;
    if(out[2]) {
        uint32_t n=2;while(n<length && out[n])n++;
        if(n==110)return -36;
        out[n]=0;char full[VFS_MAX_PATH];
        if(!vfs_normalize_path((char *)out+2,full))return -BK_EINVAL;
        n=kstrlen(full);if(n>107)return -36;
        kmemcpy(out+2,full,n+1);length=n+3;
    }
    return length;
}
static int linux_unix_find(const uint8_t *address,uint32_t length) {
    if(address[2]) {
        if(linux_names)for(unsigned i=0;i<32;i++)if(linux_names[i].length==length &&
            !kmemcmp(linux_names[i].address,address,length))return linux_names[i].raw;
    } else for(unsigned i=0;i<LINUX_IO_PAIRS;i++)if(linux_pairs[i])
        for(unsigned j=0;j<2;j++) {
            linux_endpoint *e=&linux_pairs[i]->end[j];
            if(e->refs && e->bound && e->name_length==length && !kmemcmp(e->name,address,length))return LINUX_IO_BASE+i*2+j;
        }
    return -1;
}
static bool linux_unix_output(linux_endpoint *e,uint32_t address,uint32_t length_pointer) {
    if(!address)return true;
    uint32_t capacity,length=e->name_length?e->name_length:2;
    uint8_t empty[2]={1,0};
    if(!copy_from_user(&capacity,(void *)length_pointer,4))return false;
    if(capacity>length)capacity=length;
    return copy_to_user((void *)address,e->name_length?e->name:empty,capacity) &&
        copy_to_user((void *)length_pointer,&length,4);
}

/* Los nodos de socket con pathname persisten hasta unlink, aun tras close.
 * Son objetos del namespace Linux en RAM; nunca archivos FAT ficticios. */
static int32_t linux_unix_unlink(const char *path) {
    if(!linux_names)return -BK_ENOENT;
    char full[VFS_MAX_PATH];if(!vfs_normalize_path(path,full))return -BK_EINVAL;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);int32_t result=-BK_ENOENT;
    for(unsigned i=0;i<32;i++)if(linux_names[i].length && !kstrcmp((char *)linux_names[i].address+2,full)) {
        linux_names[i].length=0;result=0;break;
    }
    kspin_unlock_irqrestore(&linux_io_lock,lock);return result;
}

static bool linux_unix_stat(const char *path,linux_i386_stat64_t *status) {
    if(!linux_names)return false;
    char full[VFS_MAX_PATH];if(!vfs_normalize_path(path,full))return false;
    for(unsigned i=0;i<32;i++)if(linux_names[i].length && !kstrcmp((char *)linux_names[i].address+2,full)) {
        linux_fill_stat64(status,0,false);status->device=9;status->mode=0140777;
        status->inode=status->inode_legacy=linux_names[i].inode;return true;
    }
    return false;
}
static bool linux_io_stat(int raw,linux_i386_stat64_t *status) {
    linux_device *d=linux_device_for(raw);
    if(d) {linux_fill_stat64(status,0,false);status->mode=0020666;status->device=1;
        status->inode=status->inode_legacy=raw;return true;}
    linux_endpoint *e=linux_endpoint_for(raw);if(!e || !e->refs)return false;
    linux_io_pair *pair=linux_pairs[(raw-LINUX_IO_BASE)/2];
    linux_fill_stat64(status,0,false);status->device=pair->pipe?13:9;
    status->mode=pair->pipe?0010600:0140777;
    status->inode=status->inode_legacy=pair->inode+(pair->pipe?0:(raw&1));return true;
}

static int32_t linux_unix_control(int raw,uint32_t op,uint32_t *a) {
    uint8_t address[110];int32_t length=0;
    if(op==2 || op==3) {length=linux_unix_address(a[1],a[2],address);if(length<0)return length;}
    if(op==2 && address[2] && !linux_names) {
        linux_names=mm_alloc_guarded_stack(sizeof(*linux_names)*32,false,NULL,NULL);
        if(!linux_names)return -BK_ENOMEM;
    }
    if(op==2 && address[2]) {
        char parent[108];kstrcpy(parent,(char *)address+2);
        uint32_t n=kstrlen(parent);while(n>1 && parent[n-1]!='/')n--;parent[n>1?n-1:1]=0;
        vfs_dir_entry_t entry;
        if(!vfs_stat(parent,&entry))return -BK_ENOENT;
        if(entry.type!=VFS_NODE_DIR)return -20;
        if(vfs_stat((char *)address+2,&entry))return -98;
    }
    if(!sys_raw_retain(raw))return -BK_EBADF;
    int32_t result=0;
    uint32_t empty_rounds=0;
    bool changed=false;
    uint32_t wait_key=linux_io_event_key();
    int listener_raw=-1;
    for(;;) {
        bool wait_armed=linux_io_wait_arm(wait_key);
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        linux_endpoint *e=linux_endpoint_for(raw);
        linux_io_pair *pair=linux_pairs[(raw-LINUX_IO_BASE)/2];
        if(pair->pipe){result=-88;goto done;}
        if(op==2) {
            if(e->bound || pair->connected){result=-BK_EINVAL;goto done;}
            if(address[2]) {
                int free_slot=-1;
                for(unsigned i=0;i<32;i++) {
                    if(!linux_names[i].length){if(free_slot<0)free_slot=i;continue;}
                    if(linux_names[i].length==(uint32_t)length && !kmemcmp(linux_names[i].address,address,length)){result=-98;goto done;}
                }
                if(free_slot<0){result=-BK_ENOMEM;goto done;}
                linux_names[free_slot].length=length;linux_names[free_slot].raw=raw;
                linux_names[free_slot].inode=pair->inode+(raw&1);
                kmemcpy(linux_names[free_slot].address,address,length);
            } else if(linux_unix_find(address,length)>=0){result=-98;goto done;}
            kmemcpy(e->name,address,length);e->name_length=length;e->bound=true;changed=true;
        } else if(op==4) {
            if(pair->connected || !e->bound){result=-BK_EINVAL;goto done;}
            e->listen=true;e->backlog_limit=a[1]>8?8:a[1]?a[1]:1;e->pid=task_current_process_id();changed=true;
        } else if(op==3) {
            if(pair->connected){result=-106;goto done;}
            listener_raw=linux_unix_find(address,length);
            linux_endpoint *server=linux_endpoint_for(listener_raw);
            if(!server || !server->refs || !server->listen){result=-111;goto done;}
            if(server->pending<server->backlog_limit) {
                linux_endpoint *accepted=linux_endpoint_for(raw^1);
                accepted->refs=accepted->inflight=1;accepted->pid=server->pid;
                accepted->name_length=server->name_length;kmemcpy(accepted->name,server->name,server->name_length);
                e->pid=task_current_process_id();pair->connected=true;server->backlog[server->pending++]=raw^1;changed=true;
            } else if(e->flags&0x800){result=-BK_EAGAIN;goto done;}
            else goto wait;
        } else if(op==5 || op==18) {
            if(!e->listen || (op==18 && (a[3]&~(0x80000U|0x800U)))){result=-BK_EINVAL;goto done;}
            if(e->pending) {
                int accepted=e->backlog[0];linux_endpoint *child=linux_endpoint_for(accepted);
                if(!linux_unix_output(linux_endpoint_for(accepted^1),a[1],a[2])){result=-BK_EFAULT;goto done;}
                result=linux_install_fd(resource_for(task_current_process_id(),true),accepted,(op==18?a[3]:0)|2U);
                if(result<0)goto done;
                child->inflight--;
                child->flags=op==18?(a[3]&0x800):0;
                for(unsigned i=1;i<e->pending;i++)e->backlog[i-1]=e->backlog[i];
                e->pending--;changed=true;
            } else if(e->flags&0x800){result=-BK_EAGAIN;goto done;}
            else goto wait;
        } else if(op==6 || op==7) {
            if(op==7 && !pair->connected){result=-107;goto done;}
            if(!linux_unix_output(op==6?e:linux_endpoint_for(raw^1),a[1],a[2]))result=-BK_EFAULT;
        } else result=-BK_ENOSYS;
done:
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        linux_io_wait_cancel(wait_armed,wait_key);
        break;
wait:
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        if(task_linux_signal_pending()){
            linux_io_wait_cancel(wait_armed,wait_key);
            result=-4;break;
        }
        linux_io_wait(&empty_rounds,wait_armed,wait_key,1U);
    }
    if(changed) {linux_io_notify(raw);if(listener_raw>=0)linux_io_notify(listener_raw);}
    sys_raw_close(raw);return result;
}

static int32_t linux_message(int raw,uint32_t pointer,uint32_t flags,bool send) {
    struct {uint32_t name,namelen,iov,iovlen,control,controllen,flags;} msg;
    linux_rights rights={0};int32_t result=0;
    syscall_resources_t *r=resource_for(task_current_process_id(),true);
    if(!r)return -BK_ENOMEM;
    if(!copy_from_user(&msg,(void *)pointer,sizeof(msg)) ||
        (!send && !user_access_ok((void *)pointer,sizeof(msg),true)))return -BK_EFAULT;
    if(msg.iovlen>1024 || msg.controllen>4096)return -BK_EINVAL;
    if(msg.name && msg.namelen && send)return -106;
    if(!send && msg.controllen && !user_access_ok((void *)msg.control,msg.controllen,true))return -BK_EFAULT;
    if(send && msg.controllen) {
        for(uint32_t off=0;off+12<=msg.controllen;) {
            uint32_t header[3];
            if(!copy_from_user(header,(void *)(msg.control+off),12)){result=-BK_EFAULT;goto done;}
            uint32_t size=header[0];
            if(size<12 || size>msg.controllen-off || header[1]!=1 || header[2]!=1 || ((size-12)&3)) {
                result=-BK_EINVAL;goto done;
            }
            uint32_t count=(size-12)/4;
            if(count>8-rights.count){result=-BK_EINVAL;goto done;}
            for(unsigned i=0;i<count;i++) {
                uint32_t fd;
                if(!copy_from_user(&fd,(void *)(msg.control+off+12+i*4),4)){result=-BK_EFAULT;goto done;}
                int item=resource_fd(r,fd);
                if(item<0 || !sys_raw_retain(item)){kprintf("[SCM:RIGHTS] pid=%u userfd=%u raw=%d invalid\n",task_current_process_id(),fd,item);result=-BK_EBADF;goto done;}
                rights.fd[rights.count++]=item;
            }
            off+=(size+3)&~3U;
        }
    }
    for(uint32_t i=0;i<msg.iovlen;i++) {
        uint32_t iov[2];
        if(!copy_from_user(iov,(void *)(msg.iov+i*8),8)){if(!result)result=-BK_EFAULT;break;}
        if(iov[1]>SYSCALL_WRITE_MAX){if(!result)result=-BK_EINVAL;break;}
        if(!iov[1])continue;
        int32_t n=linux_io(raw,(void *)iov[0],iov[1],send,flags|(result?0x40U:0),&rights);
        if(n<0){if(!result)result=n;break;}
        result+=n;
        if((uint32_t)n<iov[1] || (!send && rights.count))break;
    }
    if(!send && result>=0) {
        int output[11];uint32_t n=0,capacity=msg.controllen>=16?(msg.controllen-12)/4:0;
        msg.flags=0;msg.namelen=0;
        for(unsigned i=0;i<rights.count;i++) {
            int fd=n<capacity?linux_install_fd(r,rights.fd[i],linux_raw_flags(rights.fd[i])|
                ((flags&0x40000000)?0x80000:0)):-BK_EMFILE;
            if(fd<0){msg.flags|=8;sys_raw_close(rights.fd[i]);}
            else output[3+n++]=fd;
        }
        rights.count=0;msg.controllen=n?12+n*4:0;
        output[0]=msg.controllen;output[1]=1;output[2]=1;
        if((n && !copy_to_user((void *)msg.control,output,msg.controllen)) ||
            !copy_to_user((void *)pointer,&msg,sizeof(msg))) {
            for(unsigned i=0;i<n;i++)sys_close(output[3+i]);
            result=-BK_EFAULT;
        }
    }
done:
    for(unsigned i=0;i<rights.count;i++)sys_raw_close(rights.fd[i]);
    return result;
}

static int32_t linux_socket_operation_locked(uint32_t op,uint32_t *a) {
    syscall_resources_t *r=resource_for(task_current_process_id(),true);
    if(!r)return -BK_ENOMEM;
    if(op==8 || op==1) {
        if(a[0]!=1)return -97;
        if((a[1]&~(0x80000U|0x800U))!=1 || a[2])return -94;
        if(op==8)return linux_make_pair(a[3],a[1]&~1U,false);
        int raw=linux_pair_create(false,false,a[1]);if(raw<0)return raw;
        int fd=linux_install_fd(r,raw,(a[1]&~1U)|2U);if(fd<0)sys_raw_close(raw);return fd;
    }
    int raw=resource_fd(r,a[0]);linux_endpoint *e=linux_endpoint_for(raw);
    if(!e || !e->refs)return raw<0?-BK_EBADF:-88;
    if((op>=2 && op<=7) || op==18)return linux_unix_control(raw,op,a);
    if(op==16 || op==17)return linux_message(raw,a[1],a[2],op==16);
    if(op==15) {
        uint32_t values[3],size=4,capacity;
        if(a[1]!=1)return -92;
        switch(a[2]) {
            case 3:values[0]=1;break; /* SO_TYPE */
            case 4:values[0]=0;break; /* SO_ERROR: connect sin operacion asincrona */
            case 7:case 8:values[0]=linux_pairs[(raw-LINUX_IO_BASE)/2]->capacity;break;
            case 17:
                if(!linux_pairs[(raw-LINUX_IO_BASE)/2]->connected)return -107;
                values[0]=linux_endpoint_for(raw^1)->pid;values[1]=values[2]=0;size=12;break;
            default:return -92;
        }
        if(!copy_from_user(&capacity,(void *)a[4],4))return -BK_EFAULT;
        if(size>capacity)size=capacity;
        return copy_to_user((void *)a[3],values,size) && copy_to_user((void *)a[4],&size,4)?0:-BK_EFAULT;
    }
    if(op==13) {
        if(a[1]>2)return -BK_EINVAL;
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        if(a[1]!=1)e->read_closed=true;
        if(a[1]!=0)e->write_closed=true;
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        linux_io_notify(raw);return 0;
    }
    if(op==9 || op==10 || op==11 || op==12) {
        if((op==11 || op==12) && a[4])return -BK_ENOSYS;
        return linux_io(raw,(void *)a[1],a[2],op==9 || op==11,a[3],NULL);
    }
    return -BK_ENOSYS;
}

static int32_t linux_socket_operation(uint32_t op,uint32_t *a) {
    kernel_domains_enter(KDOMAIN_VFS);
    int32_t result=linux_socket_operation_locked(op,a);
    kernel_domains_exit(KDOMAIN_VFS);return result;
}

static int32_t linux_socketcall(uint32_t op,uint32_t pointer) {
    static const uint8_t counts[]={0,3,3,3,2,3,3,3,4,4,4,6,6,2,5,5,3,3,4};
    uint32_t args[6]={0};
    if(!op || op>=sizeof(counts))return -BK_EINVAL;
    if(!copy_from_user(args,(void *)pointer,counts[op]*4))return -BK_EFAULT;
    return linux_socket_operation(op,args);
}
