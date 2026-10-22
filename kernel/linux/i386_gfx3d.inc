/* Mesa/Wine Linux graphics bridge. Resources belong to an open /dev/blesgfx
 * description and are reclaimed on its last close, including process exit. */
#include "../include/vision_ipc.h"
static volatile uint32_t bles_native_pending_count;
bool linux_native_gpu_pending(void) {return __atomic_load_n(&bles_native_pending_count,__ATOMIC_ACQUIRE)!=0U;}
bool bk_vision_native_pending(void) {
    return task_current_process_id()==gui_vision_owner_pid() && linux_native_gpu_pending();
}
static void bles_native_fail(bles_gfx_context *gfx) {
    gfx->gallium_failed=true;
    for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)if(gfx->gallium_states[i]==1U) {
        gfx->gallium_states[i]=0;__sync_sub_and_fetch(&bles_native_pending_count,1U);
    }
}
/* Called only with the graphics domain held. Last close marks its device
 * closing before destroying any GPU surfaces or freeing the context. */
static bles_gfx_context *bles_native_window(gui_window_t *window) {
    bles_gfx_context *found=NULL;uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    for(unsigned i=0;i<128;i++) {
        linux_device *d=&linux_devices[i];
        if(d->refs && d->refs!=0xffffffffU && d->gfx && d->gfx->gallium_window &&
           !d->gfx->gallium_failed && d->gfx->window==window){
            found=d->gfx;__sync_add_and_fetch(&found->native_consumers,1U);break;
        }
    }
    kspin_unlock_irqrestore(&linux_io_lock,lock);return found;
}
static int bles_native_ready(bles_gfx_context *gfx,unsigned slot) {
    bles_virgl_t q={.abi=BLES_VIRGL_ABI,.op=BLES_VIRGL_PRESENT,.context=gfx->virgl_context,
        .p={0,gfx->gallium_surfaces[slot],gfx->gallium_readers[slot],gfx->gallium_states[slot]==1U?2U:3U}};
    if(!gfx3d_virgl(&q,NULL)){bles_native_fail(gfx);return -1;}
    return q.p[0]?1:0;
}
static int bles_native_reserve(bles_gfx_context *gfx) {
    /* Interactive windows keep one capture ahead. Throughput benchmarks can
     * explicitly request three to overlap host polling with draws. In both cases a
     * completed capture still belongs to Vision: never recycle it merely
     * because its producer fence completed. The remaining pool slots protect
     * unfinished desktop readers. */
    unsigned pending=0,limit=gfx->native_queue_limit?gfx->native_queue_limit:1U;
    for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)pending+=gfx->gallium_states[i]==1U;
    if(pending>=limit){
        gfx->native_wait_limit++;
        for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)if(gfx->gallium_states[i]==1U) {
            int ready=bles_native_ready(gfx,i);
            if(ready<0)return -BK_EIO;
            if(ready){
                gfx->native_wait_ready++;vision_ipc_notify_native_gpu();break;
            }
        }
        return -BK_EAGAIN;
    }
    for(unsigned n=0;n<BLES_VIRGL_NATIVE_SLOTS;n++) {
        unsigned i=(gfx->gallium_next+n)%BLES_VIRGL_NATIVE_SLOTS;
        if(!gfx->gallium_states[i])return (int)i;
        if(gfx->gallium_states[i]==1U)continue;
        int ready=bles_native_ready(gfx,i);
        if(ready<0)return -BK_EIO;
        if(ready) {
            gfx->gallium_states[i]=0;return (int)i;
        }
    }
    gfx->native_wait_readers++;return -BK_EAGAIN;
}
static int bles_native_latest(bles_gfx_context *gfx) {
    int selected=-1;
    for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)if(gfx->gallium_states[i]==1U) {
        int ready=bles_native_ready(gfx,i);
        if(ready<0)return -BK_EIO;
        if(ready && (selected<0 || (int32_t)(gfx->gallium_serial[i]-gfx->gallium_serial[selected])>0))selected=(int)i;
    }
    return selected;
}
static int bles_3d_slot(bles_gfx_context *gfx,uint32_t handle) {
    if(!handle)return -1;
    for(unsigned i=0;i<128;i++)if(gfx->gpu_surfaces[i].handle==handle)return (int)i;
    return -1;
}
static void bles_gfx_3d_close(bles_gfx_context *gfx) {
    /* Last close prevents new consumers through the device's closing marker.
     * A service transaction can yield inside a driver call, so its lifetime
     * is pinned independently of domain locks. */
    while(__atomic_load_n(&gfx->native_consumers,__ATOMIC_ACQUIRE))task_sleep(1U);
    if(gfx->gallium_window && gfx->gallium_sequence)
        kprintf("[NATIVE:QUEUE] captures=%u limit_waits=%u reader_waits=%u ready_waits=%u\n",
            gfx->gallium_sequence,gfx->native_wait_limit,gfx->native_wait_readers,gfx->native_wait_ready);
    if(gfx->gallium_window && gfx->window)
        bk_gui_window_clear_gpu_viewport(gfx->window);
    kernel_domains_enter(KDOMAIN_GFX);
    bles_native_fail(gfx);
    if(gfx->virgl_context) {
        bles_virgl_t a={.abi=BLES_VIRGL_ABI,.op=BLES_VIRGL_CONTEXT_DESTROY,
                        .context=gfx->virgl_context};
        (void)gfx3d_virgl(&a,NULL);gfx->virgl_context=0;
    }
    for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)if(gfx->gallium_surfaces[i]) {
        gfx3d_surface_destroy(gfx->gallium_surfaces[i]);gfx->gallium_surfaces[i]=0;
    }
    if(gfx->gpu_fence)gfx3d_wait_fence(gfx->gpu_fence);
    for(unsigned i=0;i<128;i++)if(gfx->gpu_surfaces[i].handle) {
        gfx3d_surface_destroy(gfx->gpu_surfaces[i].handle);
        gfx->gpu_surfaces[i].handle=0;
    }
    gfx->gpu_fence=0;
    kernel_domains_exit(KDOMAIN_GFX);
}
static int32_t bles_gfx_3d_locked(bles_gfx_context *gfx,bles_gfx_3d_t *a) {
    uint32_t *p=a->p, fence=0; int slot;
    bool ok=false;
    if(a->op==BLES_3D_CAPS) {p[0]=gfx3d_capabilities();return 0;}
    if(a->op==BLES_3D_CREATE) {
        if(!p[0] || !p[1] || p[0]>2048 || p[1]>2048 ||
           p[2]!=GFX3D_FORMAT_ARGB8888 || (p[3]&~(7U|0xf00U)))return -BK_EINVAL;
        for(slot=0;slot<128 && gfx->gpu_surfaces[slot].handle;slot++);
        if(slot==128)return -BK_ENOMEM;
        gfx3d_surface_desc_t desc={(uint16_t)p[0],(uint16_t)p[1],p[2],p[3]};
        uint32_t handle=0;
        if(!gfx3d_surface_create(&desc,&handle))return -BK_ENOMEM;
        gfx->gpu_surfaces[slot].handle=handle;
        gfx->gpu_surfaces[slot].width=p[0];gfx->gpu_surfaces[slot].height=p[1];
        gfx->gpu_surfaces[slot].flags=p[3];p[0]=handle;return 0;
    }
    if(a->op==BLES_3D_WAIT) {
        /* Barrier for this description's own ordered submissions. Waiting on
         * the newest issued fence also covers older contexts' pending work.
         * Never pass a caller-supplied fence to another owner's device work. */
        if(!gfx->gpu_fence)return 0;
        return gfx3d_wait_fence(gfx->gpu_fence)?0:-BK_EIO;
    }
    slot=bles_3d_slot(gfx,p[0]);if(slot<0)return -BK_EINVAL;
    if(a->op==BLES_3D_DESTROY) {
        ok=gfx3d_surface_destroy(p[0]);
        if(ok)gfx->gpu_surfaces[slot].handle=0;
    } else if(a->op==BLES_3D_BEGIN) {
        float depth;kmemcpy(&depth,&p[2],sizeof(depth));
        if(!(depth>=0.0f && depth<=1.0f))return -BK_EINVAL;
        ok=gfx3d_begin(p[0],p[1],depth,p[3]);
    } else if(a->op==BLES_3D_END) {
        ok=gfx3d_end(p[0],&fence);p[1]=fence;
    } else if(a->op==BLES_3D_DRAW) {
        if((p[1] && bles_3d_slot(gfx,p[1])<0) || !p[3] || p[3]>16384 || p[3]%3)
            return -BK_EINVAL;
        uint32_t bytes=p[3]*sizeof(gfx3d_vertex_t);
        gfx3d_vertex_t *v=kmalloc(bytes);if(!v)return -BK_ENOMEM;
        if(!copy_from_user(v,(void *)(uintptr_t)p[2],bytes)){kfree(v);return -BK_EFAULT;}
        for(unsigned i=0;i<p[3];i++) {
            /* Also rejects NaN and infinity before a device converts vertices. */
            if(!(v[i].x>=-65536 && v[i].x<=65536 && v[i].y>=-65536 && v[i].y<=65536 &&
                 v[i].z>=-65536 && v[i].z<=65536 && v[i].rhw>=-65536 && v[i].rhw<=65536 &&
                 v[i].u>=-65536 && v[i].u<=65536 && v[i].v>=-65536 && v[i].v<=65536)) {
                kfree(v);return -BK_EINVAL;
            }
        }
        ok=gfx3d_draw_triangles(p[0],p[1],v,p[3],p[4],&fence);kfree(v);p[5]=fence;
    } else if(a->op>=BLES_3D_UPLOAD && a->op<=BLES_3D_DEPTH_DOWNLOAD) {
        uint32_t width=gfx->gpu_surfaces[slot].width,height=gfx->gpu_surfaces[slot].height;
        uint32_t level=a->op==BLES_3D_UPLOAD_MIP?p[3]:0;
        if(level>11 || (level && level>=((gfx->gpu_surfaces[slot].flags>>8)&15U)+1))return -BK_EINVAL;
        width>>=level;height>>=level;if(!width)width=1;if(!height)height=1;
        gfx_rect_t rect={0,0,(int32_t)width,(int32_t)height};
        if(a->op==BLES_3D_UPLOAD_REGION)rect=(gfx_rect_t){p[3],p[4],p[5],p[6]};
        else if(a->op!=BLES_3D_UPLOAD_MIP && p[3] &&
            !copy_from_user(&rect,(void *)(uintptr_t)p[3],sizeof(rect)))return -BK_EFAULT;
        if(rect.x<0 || rect.y<0 || rect.w<=0 || rect.h<=0 ||
           (uint32_t)rect.x>=width || (uint32_t)rect.y>=height ||
           (uint32_t)rect.w>width-(uint32_t)rect.x || (uint32_t)rect.h>height-(uint32_t)rect.y)
            return -BK_EINVAL;
        bool region=a->op==BLES_3D_UPLOAD_REGION;
        uint32_t rows=region?(uint32_t)rect.h:height;
        uint32_t min_pitch=region?(uint32_t)rect.w:width;
        bool depth=a->op==BLES_3D_DEPTH_UPLOAD || a->op==BLES_3D_DEPTH_DOWNLOAD;
        if(p[2]<min_pitch || p[2]>4096)return -BK_EINVAL;
        uint32_t bytes=p[2]*rows*(depth?2U:4U);
        void *buffer=kmalloc(bytes);if(!buffer)return -BK_ENOMEM;
        bool read=a->op==BLES_3D_DOWNLOAD || a->op==BLES_3D_DEPTH_DOWNLOAD;
        /* Readback modifies only a rectangle. Preserve the caller's other rows. */
        if(!copy_from_user(buffer,(void *)(uintptr_t)p[1],bytes)){kfree(buffer);return -BK_EFAULT;}
        switch(a->op) {
        case BLES_3D_UPLOAD:ok=gfx3d_surface_upload(p[0],buffer,p[2],&rect);break;
        case BLES_3D_DOWNLOAD:ok=gfx3d_surface_download(p[0],buffer,p[2],&rect);break;
        case BLES_3D_UPLOAD_MIP:ok=gfx3d_surface_upload_mip(p[0],level,buffer,p[2]);break;
        case BLES_3D_UPLOAD_REGION:ok=gfx3d_surface_upload_region(p[0],buffer,p[2],rect.x,rect.y,rect.w,rect.h);break;
        case BLES_3D_DEPTH_UPLOAD:ok=gfx3d_depth_upload(p[0],buffer,p[2],&rect);break;
        case BLES_3D_DEPTH_DOWNLOAD:ok=gfx3d_depth_download(p[0],buffer,p[2],&rect);break;
        }
        if(ok && read && !copy_to_user((void *)(uintptr_t)p[1],buffer,bytes)) {
            kfree(buffer);return -BK_EFAULT;
        }
        kfree(buffer);
    } else return -BK_EINVAL;
    if(fence)gfx->gpu_fence=fence;
    return ok?0:-BK_EIO;
}
static int32_t bles_gfx_3d_ioctl_locked(bles_gfx_context *gfx,uint32_t argument) {
    bles_gfx_3d_t a;
    if(!copy_from_user(&a,(void *)(uintptr_t)argument,sizeof(a)))return -BK_EFAULT;
    kernel_domains_enter(KDOMAIN_GFX);
    if(a.op==BLES_3D_PRESENT) {
        if(!a.p[0]) {
            bool native=gfx->gallium_window && gfx->window;
            kernel_domains_exit(KDOMAIN_GFX);
            if(!native)return -BK_EINVAL;
            bk_gui_window_clear_gpu_viewport(gfx->window);
            return 0;
        }
        int slot=bles_3d_slot(gfx,a.p[0]);
        bool valid=gfx->gallium_window && gfx->window && slot>=0 &&
            gfx->gpu_surfaces[slot].width==gfx->width && gfx->gpu_surfaces[slot].height==gfx->height &&
            (gfx->gpu_surfaces[slot].flags&GFX3D_SURFACE_TEXTURE);
        if(!valid){kernel_domains_exit(KDOMAIN_GFX);return -BK_EINVAL;}
        bles_virgl_t caps={.abi=BLES_VIRGL_ABI,.op=BLES_VIRGL_CAPS};
        if(gfx3d_virgl(&caps,NULL)) {
            if(gfx->gallium_failed){kernel_domains_exit(KDOMAIN_GFX);return -BK_EIO;}
            int selected=bles_native_reserve(gfx);
            if(selected<0){kernel_domains_exit(KDOMAIN_GFX);return selected;}
            unsigned slot=(unsigned)selected;
            if(!gfx->gallium_surfaces[slot]) {
                gfx3d_surface_desc_t d={gfx->width,gfx->height,GFX3D_FORMAT_ARGB8888,
                    GFX3D_SURFACE_RENDER_TARGET|GFX3D_SURFACE_TEXTURE|GFX3D_SURFACE_WINDOW};
                if(!gfx3d_surface_create(&d,&gfx->gallium_surfaces[slot])) {
                    kernel_domains_exit(KDOMAIN_GFX);return -BK_ENOMEM;
                }
            }
            bles_virgl_t capture={.abi=BLES_VIRGL_ABI,.op=BLES_VIRGL_PRESENT,
                .p={a.p[0],gfx->gallium_surfaces[slot],0,4U}};
            bool ok=gfx3d_end(a.p[0],&gfx->gpu_fence) && gfx3d_virgl(&capture,NULL);
            if(ok) {
                gfx->gallium_readers[slot]=capture.p[2];gfx->gallium_states[slot]=1U;
                if(!++gfx->gallium_sequence)++gfx->gallium_sequence;
                gfx->gallium_serial[slot]=gfx->gallium_sequence;gfx->gallium_next=(slot+1U)%BLES_VIRGL_NATIVE_SLOTS;
                __sync_add_and_fetch(&bles_native_pending_count,1U);
            } else bles_native_fail(gfx);
            kernel_domains_exit(KDOMAIN_GFX);
            if(ok)vision_ipc_notify_native_gpu();
            return ok?0:-BK_EIO;
        }
        bool ok=gfx3d_end(a.p[0],&gfx->gpu_fence);
        kernel_domains_exit(KDOMAIN_GFX);
        gui_rect_t rect;
        ok=ok && bk_gui_window_content_rect(gfx->window,&rect) &&
            bk_gui_window_set_gpu_viewport(gfx->window,a.p[0],gfx->width,gfx->height,rect);
        return ok?0:-BK_EIO;
    }
    int32_t result=bles_gfx_3d_locked(gfx,&a);
    kernel_domains_exit(KDOMAIN_GFX);
    if(result<0)return result;
    if(!copy_to_user((void *)(uintptr_t)argument,&a,sizeof(a))) {
        if(a.op==BLES_3D_CREATE) { /* Roll back resources if the result is unwritable. */
            kernel_domains_enter(KDOMAIN_GFX);
            int slot=bles_3d_slot(gfx,a.p[0]);
            if(slot>=0){gfx3d_surface_destroy(a.p[0]);gfx->gpu_surfaces[slot].handle=0;}
            kernel_domains_exit(KDOMAIN_GFX);
        }
        return -BK_EFAULT;
    }
    return 0;
}
static int32_t bles_gfx_3d_ioctl(bles_gfx_context *gfx,uint32_t argument) {
    kmutex_lock(&gfx->virgl_lock);
    int32_t result=bles_gfx_3d_ioctl_locked(gfx,argument);
    kmutex_unlock(&gfx->virgl_lock);return result;
}

static int32_t bles_gfx_virgl_ioctl_locked(bles_gfx_context *gfx,uint32_t argument) {
    bles_virgl_t a;
    if(!copy_from_user(&a,(void *)(uintptr_t)argument,sizeof(a)))return -BK_EFAULT;
    if(a.abi!=BLES_VIRGL_ABI || a.op<BLES_VIRGL_CAPS || a.op>BLES_VIRGL_PRESENT
       || a.bytes>BLES_VIRGL_MAX_DATA
       || ((a.op==BLES_VIRGL_SUBMIT || a.op==BLES_VIRGL_SUBMIT_ASYNC) &&
           (a.bytes>BLES_VIRGL_MAX_COMMANDS || (a.bytes&3)))
       || (a.op==BLES_VIRGL_CAPS && a.bytes>65536U))return -BK_EINVAL;
    bool output=a.op==BLES_VIRGL_CAPS || a.op==BLES_VIRGL_TRANSFER_READ;
    if(a.bytes && a.op!=BLES_VIRGL_CAPS && a.op!=BLES_VIRGL_TRANSFER_WRITE
       && a.op!=BLES_VIRGL_TRANSFER_WRITE_ASYNC
       && a.op!=BLES_VIRGL_TRANSFER_READ && a.op!=BLES_VIRGL_SUBMIT
       && a.op!=BLES_VIRGL_SUBMIT_ASYNC)return -BK_EINVAL;
    /* Validate writable results before creating any host resources. */
    /* A valid staging allocation may still contain demand-paged pages.
     * Preflight its permissions here; copy_{from,to}_user faults and pins
     * the pages before touching them outside the graphics domain. */
    if(!paging_user_range_permitted((void *)(uintptr_t)argument,sizeof(a),true)
       || (a.bytes && !paging_user_range_permitted((void *)(uintptr_t)a.buffer,a.bytes,output)))return -BK_EFAULT;
    void *data=a.bytes?kmalloc(a.bytes):NULL;
    if(a.bytes && !data)return -BK_ENOMEM;
    if(data) {
        if(output)kmemset(data,0,a.bytes);
        else if(!copy_from_user(data,(void *)(uintptr_t)a.buffer,a.bytes)){kfree(data);return -BK_EFAULT;}
    }
    kernel_domains_enter(KDOMAIN_GFX);
    a.context=gfx->virgl_context;
    if(a.op==BLES_VIRGL_PRESENT) {
        if(gfx->gallium_failed) {
            kernel_domains_exit(KDOMAIN_GFX);if(data)kfree(data);return -BK_EIO;
        }
        /* Never accept a caller-supplied GFX3D handle: the snapshot belongs
         * to this description. The driver checks ownership of the source. */
        if(!gfx->window || !gfx->width || !gfx->height || a.bytes || a.p[1] || a.p[2] || a.p[3]) {
            kernel_domains_exit(KDOMAIN_GFX);if(data)kfree(data);return -BK_EINVAL;
        }
        int selected=bles_native_reserve(gfx);
        if(selected<0){kernel_domains_exit(KDOMAIN_GFX);if(data)kfree(data);return selected;}
        unsigned slot=(unsigned)selected;
        if(!gfx->gallium_surfaces[slot]) {
            gfx3d_surface_desc_t d={gfx->width,gfx->height,GFX3D_FORMAT_ARGB8888,
                GFX3D_SURFACE_RENDER_TARGET|GFX3D_SURFACE_TEXTURE|GFX3D_SURFACE_WINDOW};
            if(!gfx3d_surface_create(&d,&gfx->gallium_surfaces[slot])) {
                kernel_domains_exit(KDOMAIN_GFX);if(data)kfree(data);return -BK_ENOMEM;
            }
        }
        a.p[1]=gfx->gallium_surfaces[slot];
        a.p[2]=0;
        gfx->gallium_next=slot;
    }
    bool valid=a.op==BLES_VIRGL_CAPS || (a.op==BLES_VIRGL_CONTEXT_CREATE?!a.context:a.context!=0);
    bool ok=valid && gfx3d_virgl(&a,data);
    if(ok && a.op==BLES_VIRGL_CONTEXT_CREATE)gfx->virgl_context=a.context;
    if(ok && a.op==BLES_VIRGL_CONTEXT_DESTROY)gfx->virgl_context=0;
    if(a.op==BLES_VIRGL_PRESENT) {
        if(ok) {
            unsigned slot=gfx->gallium_next;
            gfx->gallium_readers[slot]=a.p[2];gfx->gallium_states[slot]=1U;
            if(!++gfx->gallium_sequence)++gfx->gallium_sequence;
            gfx->gallium_serial[slot]=gfx->gallium_sequence;
            gfx->gallium_next=(slot+1U)%BLES_VIRGL_NATIVE_SLOTS;
            __sync_add_and_fetch(&bles_native_pending_count,1U);
        } else bles_native_fail(gfx);
        a.p[1]=a.p[2]=a.p[3]=0;
    }
    kernel_domains_exit(KDOMAIN_GFX);
    if(ok && a.op==BLES_VIRGL_PRESENT)vision_ipc_notify_native_gpu();
    int32_t result=ok?0:-BK_EIO;
    if(ok && ((output && a.bytes && !copy_to_user((void *)(uintptr_t)a.buffer,data,a.bytes))
               || !copy_to_user((void *)(uintptr_t)argument,&a,sizeof(a)))) {
        /* A concurrent unmap can still invalidate the validated result. */
        if(a.op==BLES_VIRGL_CONTEXT_CREATE || a.op==BLES_VIRGL_RESOURCE_CREATE) {
            kernel_domains_enter(KDOMAIN_GFX);
            a.op=a.op==BLES_VIRGL_CONTEXT_CREATE?BLES_VIRGL_CONTEXT_DESTROY:BLES_VIRGL_RESOURCE_DESTROY;
            (void)gfx3d_virgl(&a,NULL);
            if(a.op==BLES_VIRGL_CONTEXT_DESTROY)gfx->virgl_context=0;
            kernel_domains_exit(KDOMAIN_GFX);
        }
        result=-BK_EFAULT;
    }
    if(data)kfree(data);
    return result;
}
static int32_t bles_gfx_virgl_ioctl(bles_gfx_context *gfx,uint32_t argument) {
    /* Publication drops domains while Vision runs; keep this description's
     * producer operations serialized across that interval as well. */
    kmutex_lock(&gfx->virgl_lock);
    int32_t result=bles_gfx_virgl_ioctl_locked(gfx,argument);
    kmutex_unlock(&gfx->virgl_lock);
    return result;
}

/* One protected consumer transaction. Its explicit lifetime pin also covers
 * scheduler safe points inside driver calls. No lease escapes to Ring 3. */
int32_t bk_vision_native_frame(gui_window_t *window,uint32_t destination,uint32_t *width,uint32_t *height) {
    if(!gui_vision_owner_pid() || task_current_process_id()!=gui_vision_owner_pid())return -BK_EACCES;
    if(!paging_user_range_permitted(width,sizeof(*width),true) ||
       !paging_user_range_permitted(height,sizeof(*height),true))return -BK_EFAULT;
    uint32_t w=0,h=0;int32_t result=0;
    kernel_domains_enter(KDOMAIN_GFX);
    bles_gfx_context *gfx=bles_native_window(window);
    int selected=gfx?bles_native_latest(gfx):-1;
    if(selected==-BK_EIO)result=-BK_EIO;
    if(gfx && selected>=0 && !gfx->gallium_failed) {
        w=gfx->width;h=gfx->height;result=1;
        if(destination) {
            unsigned slot=(unsigned)selected;
            /* Both handles are kernel-owned desktop-context surfaces. An
             * exact GPU blit preserves RGBA, including transparent pixels,
             * without a clear, textured quad, VBO upload and separate seal.
             * Its real completion fence protects the snapshot's reader. */
            bles_virgl_t copy={.abi=BLES_VIRGL_ABI,.op=BLES_VIRGL_PRESENT,
                .p={gfx->gallium_surfaces[slot],destination,0,4U}};
            bool ok=gfx3d_virgl(&copy,NULL);
            if(ok) {
                gfx->gallium_readers[slot]=copy.p[2];gfx->gallium_states[slot]=2U;
                __sync_sub_and_fetch(&bles_native_pending_count,1U);
                uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
                gfx->force_present=false;kspin_unlock_irqrestore(&linux_io_lock,lock);
                /* Older completed captures need no desktop reader fence:
                 * mailbox presentation deliberately coalesces them. */
                for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)if(gfx->gallium_states[i]==1U &&
                    (int32_t)(gfx->gallium_serial[slot]-gfx->gallium_serial[i])>0 && bles_native_ready(gfx,i)>0) {
                    gfx->gallium_states[i]=0;__sync_sub_and_fetch(&bles_native_pending_count,1U);
                }
            } else {bles_native_fail(gfx);result=-BK_EIO;}
        }
    }
    kernel_domains_exit(KDOMAIN_GFX);
    if(gfx)__sync_sub_and_fetch(&gfx->native_consumers,1U);
    if(result>0 && (!copy_to_user(width,&w,sizeof(w)) || !copy_to_user(height,&h,sizeof(h))))return -BK_EFAULT;
    return result;
}
