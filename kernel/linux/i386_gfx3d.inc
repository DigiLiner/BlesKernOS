/* Mesa/Wine Linux graphics bridge. Resources belong to an open /dev/blesgfx
 * description and are reclaimed on its last close, including process exit. */
static int bles_3d_slot(bles_gfx_context *gfx,uint32_t handle) {
    if(!handle)return -1;
    for(unsigned i=0;i<128;i++)if(gfx->gpu_surfaces[i].handle==handle)return (int)i;
    return -1;
}
static void bles_gfx_3d_close(bles_gfx_context *gfx) {
    kernel_domains_enter(KDOMAIN_GFX);
    if(gfx->virgl_context) {
        bles_virgl_t a={.abi=BLES_VIRGL_ABI,.op=BLES_VIRGL_CONTEXT_DESTROY,
                        .context=gfx->virgl_context};
        (void)gfx3d_virgl(&a,NULL);gfx->virgl_context=0;
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
static int32_t bles_gfx_3d_ioctl(bles_gfx_context *gfx,uint32_t argument) {
    bles_gfx_3d_t a;
    if(!copy_from_user(&a,(void *)(uintptr_t)argument,sizeof(a)))return -BK_EFAULT;
    kernel_domains_enter(KDOMAIN_GFX);
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

static int32_t bles_gfx_virgl_ioctl(bles_gfx_context *gfx,uint32_t argument) {
    bles_virgl_t a;
    if(!copy_from_user(&a,(void *)(uintptr_t)argument,sizeof(a)))return -BK_EFAULT;
    if(a.abi!=BLES_VIRGL_ABI || a.op<BLES_VIRGL_CAPS || a.op>BLES_VIRGL_TRANSFER_WRITE_ASYNC
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
    if(!user_access_ok((void *)(uintptr_t)argument,sizeof(a),true)
       || (a.bytes && !user_access_ok((void *)(uintptr_t)a.buffer,a.bytes,output)))return -BK_EFAULT;
    void *data=a.bytes?kmalloc(a.bytes):NULL;
    if(a.bytes && !data)return -BK_ENOMEM;
    if(data) {
        if(output)kmemset(data,0,a.bytes);
        else if(!copy_from_user(data,(void *)(uintptr_t)a.buffer,a.bytes)){kfree(data);return -BK_EFAULT;}
    }
    kernel_domains_enter(KDOMAIN_GFX);
    a.context=gfx->virgl_context;
    bool valid=a.op==BLES_VIRGL_CAPS || (a.op==BLES_VIRGL_CONTEXT_CREATE?!a.context:a.context!=0);
    bool ok=valid && gfx3d_virgl(&a,data);
    if(ok && a.op==BLES_VIRGL_CONTEXT_CREATE)gfx->virgl_context=a.context;
    if(ok && a.op==BLES_VIRGL_CONTEXT_DESTROY)gfx->virgl_context=0;
    kernel_domains_exit(KDOMAIN_GFX);
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
