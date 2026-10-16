/* Gallium owns shader/state objects inside a separate host context. The
 * desktop's fixed-function context is never shared with these submissions.
 * Called under the core GFX3D lock. Async submission retains GPU work until
 * WAIT/TRANSFER or destruction establishes a host fence. */
typedef struct vg_raw_resource {
    struct vg_raw_resource *next;
    uint32_t id, bytes, width, height, depth, layers, levels, target;
    void *backing;
} vg_raw_resource_t;
typedef struct {
    uint32_t id, bytes;
    vg_raw_resource_t *resources;
    bool pending;
} vg_raw_context_t;
static vg_raw_context_t vg_raw_contexts[64];
static uint32_t vg_raw_next_context = 1;

static vg_raw_context_t *vg_raw_context(uint32_t id) {
    if (!id) return NULL;
    for (unsigned i=0;i<64;i++) if (vg_raw_contexts[i].id==id) return &vg_raw_contexts[i];
    return NULL;
}
static vg_raw_resource_t *vg_raw_resource(vg_raw_context_t *ctx,uint32_t id) {
    for (vg_raw_resource_t *r=ctx->resources;r;r=r->next) if(r->id==id)return r;
    return NULL;
}
static void vg_raw_free_resource(vg_raw_context_t *ctx,vg_raw_resource_t *r) {
    vg_raw_resource_t **link=&ctx->resources;
    while(*link && *link!=r)link=&(*link)->next;
    if(*link)*link=r->next;
    (void)virtio_gpu_context_resource(VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,r->id);
    (void)virtio_gpu_resource_unref(r->id);
    if(r->backing)kfree(r->backing);
    ctx->bytes-=r->bytes;kfree(r);
}
static bool vg_raw_caps(bles_virgl_t *a,void *data) {
    uint32_t size=g_virtio.virgl_capset_size;
    a->p[0]=BLES_VIRGL_ABI;a->p[1]=g_virtio.virgl_capset_id;
    a->p[2]=g_virtio.virgl_capset_version;a->p[3]=size;
    a->p[4]=BLES_VIRGL_FEATURE_ASYNC_SUBMIT|BLES_VIRGL_FEATURE_ASYNC_UPLOAD;
    if(!a->bytes)return true;
    if(!data || a->bytes<size || size>65536U)return false;
    uint32_t total=sizeof(virtio_gpu_ctrl_hdr_t)+size;
    void *response=kmalloc(total);if(!response)return false;
    virtio_gpu_get_capset_t request;
    kmemset(&request,0,sizeof(request));
    request.hdr.type=VIRTIO_GPU_CMD_GET_CAPSET;
    request.capset_id=g_virtio.virgl_capset_id;
    request.capset_version=g_virtio.virgl_capset_version;
    bool ok=virtio_gpu_control_command(&request,sizeof(request),response,total,
                                       VIRTIO_GPU_RESP_OK_CAPSET,false,NULL);
    if(ok)kmemcpy(data,(uint8_t *)response+sizeof(virtio_gpu_ctrl_hdr_t),size);
    kfree(response);return ok;
}
static bool vg_raw_create_context(bles_virgl_t *a) {
    vg_raw_context_t *ctx=NULL;
    for(unsigned i=0;i<64;i++)if(!vg_raw_contexts[i].id){ctx=&vg_raw_contexts[i];break;}
    if(!ctx || vg_raw_next_context==0xffffffffU)return false;
    virtio_gpu_ctx_create_t request;virtio_gpu_ctrl_hdr_t response;
    kmemset(&request,0,sizeof(request));
    request.hdr.type=VIRTIO_GPU_CMD_CTX_CREATE;
    request.hdr.ctx_id=++vg_raw_next_context;
    static const char name[]="BlesKernOS Gallium";
    request.nlen=sizeof(name)-1;kmemcpy(request.debug_name,name,sizeof(name)-1);
    if(g_virtio.context_init_available)request.context_init=g_virtio.virgl_capset_id;
    if(!virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),
                                   VIRTIO_GPU_RESP_OK_NODATA,true,NULL))return false;
    ctx->id=request.hdr.ctx_id;ctx->bytes=0;ctx->resources=NULL;ctx->pending=false;
    a->context=ctx->id;return true;
}
static bool vg_raw_create_resource(vg_raw_context_t *ctx,bles_virgl_t *a) {
    uint32_t *p=a->p,bytes=p[10];
    if(p[0]>8 || !p[1] || !p[3] || !p[4] || !p[5] || !p[6] || p[7]>14 || p[8]>16
       || bytes>BLES_VIRGL_MAX_DATA || bytes>256U*1024U*1024U-ctx->bytes
       || (p[0] && (p[3]>16384 || p[4]>16384 || p[5]>2048 || p[6]>2048)))return false;
    vg_raw_resource_t *r=kmalloc(sizeof(*r));if(!r)return false;
    kmemset(r,0,sizeof(*r));
    if(bytes){r->backing=kmalloc(bytes);if(!r->backing){kfree(r);return false;}
        kmemset(r->backing,0,bytes);}
    r->id=virtio_gpu_next_resource();r->bytes=bytes;r->width=p[3];r->height=p[4];
    r->depth=p[5];r->layers=p[6];r->levels=p[7]+1;r->target=p[0];
    virtio_gpu_resource_create_3d_t request;virtio_gpu_ctrl_hdr_t response;
    kmemset(&request,0,sizeof(request));request.hdr.type=VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    request.resource_id=r->id;request.target=p[0];request.format=p[1];request.bind=p[2];
    request.width=p[3];request.height=p[4];request.depth=p[5];request.array_size=p[6];
    request.last_level=p[7];request.nr_samples=p[8];request.flags=p[9];
    bool created=virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),
                                           VIRTIO_GPU_RESP_OK_NODATA,false,NULL);
    if(!created || (bytes && !virtio_gpu_resource_attach(r->id,r->backing,bytes))
       || !virtio_gpu_context_resource(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,r->id)) {
        if(created)(void)virtio_gpu_resource_unref(r->id);
        if(r->backing)kfree(r->backing);
        kfree(r);return false;
    }
    r->next=ctx->resources;ctx->resources=r;ctx->bytes+=bytes;a->p[0]=r->id;
    return true;
}
static bool vg_raw_transfer(vg_raw_context_t *ctx,bles_virgl_t *a,void *data) {
    uint32_t *p=a->p;vg_raw_resource_t *r=vg_raw_resource(ctx,p[0]);
    if(!r || !r->backing || !data || p[1]>=r->levels || p[10]>=r->bytes
       || p[11]>r->bytes || a->bytes>r->bytes-p[11]
       || !p[7] || !p[8] || !p[9])return false;
    uint32_t w=r->width>>p[1],h=r->height>>p[1],d=r->depth>>p[1];
    if(!w)w=1;
    if(!h)h=1;
    if(!d)d=1;
    if(r->target==4 || r->target==6 || r->target==7 || r->target==8)d=r->layers;
    if(p[4]>=w || p[7]>w-p[4] || p[5]>=h || p[8]>h-p[5]
       || p[6]>=d || p[9]>d-p[6])return false;
    bool write=a->op!=BLES_VIRGL_TRANSFER_READ;
    bool async=a->op==BLES_VIRGL_TRANSFER_WRITE_ASYNC;
    /* An unfenced ACK is not GPU completion. Keep the dependency pending;
     * Wine uses fenced uploads for streaming resources and waits before
     * overwriting buffers still referenced by submitted GPU work. */
    if(write)kmemcpy((uint8_t *)r->backing+p[11],data,a->bytes);
    virtio_gpu_transfer_host_3d_t request;virtio_gpu_ctrl_hdr_t response;
    kmemset(&request,0,sizeof(request));
    request.hdr.type=write?VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    request.hdr.ctx_id=ctx->id;request.resource_id=r->id;request.level=p[1];
    request.stride=p[2];request.layer_stride=p[3];request.offset=p[10];
    request.box=(virtio_gpu_box_t){p[4],p[5],p[6],p[7],p[8],p[9]};
    bool ok=virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),
                                      VIRTIO_GPU_RESP_OK_NODATA,!async,NULL);
    if(ok){ctx->pending=async;if(!write)kmemcpy(data,(uint8_t *)r->backing+p[11],a->bytes);}
    return ok;
}
static bool vg_raw_submit(vg_raw_context_t *ctx,bles_virgl_t *a,void *data) {
    if(!data || !a->bytes || (a->bytes&3) || a->bytes>BLES_VIRGL_MAX_COMMANDS)return false;
    /* Reject truncated packet streams before the host receives them. Shader
     * semantics and object references are validated by virglrenderer. */
    uint32_t *words=data,count=a->bytes/4;
    for(uint32_t i=0;i<count;){uint32_t n=words[i]>>16;
        if(n>=count-i)return false;
        i+=n+1;}
    uint32_t total=sizeof(virtio_gpu_cmd_submit_t)+a->bytes;
    virtio_gpu_cmd_submit_t *request=kmalloc(total);if(!request)return false;
    virtio_gpu_ctrl_hdr_t response;kmemset(request,0,sizeof(*request));
    request->hdr.type=VIRTIO_GPU_CMD_SUBMIT_3D;request->hdr.ctx_id=ctx->id;
    request->size=a->bytes;kmemcpy((uint8_t *)request+sizeof(*request),data,a->bytes);
    bool async=a->op==BLES_VIRGL_SUBMIT_ASYNC;
    bool ok=virtio_gpu_control_command(request,total,&response,sizeof(response),
                                      VIRTIO_GPU_RESP_OK_NODATA,!async,NULL);
    if(ok)ctx->pending=async;
    kfree(request);return ok;
}
static bool vg_raw_wait(vg_raw_context_t *ctx) {
    if(!ctx->pending)return true;
    /* A zero-length submission is an ordered host fence without changing GL
     * state or referring to a resource that might already have been released. */
    virtio_gpu_cmd_submit_t request;virtio_gpu_ctrl_hdr_t response;
    kmemset(&request,0,sizeof(request));
    request.hdr.type=VIRTIO_GPU_CMD_SUBMIT_3D;request.hdr.ctx_id=ctx->id;
    bool ok=virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),
                                      VIRTIO_GPU_RESP_OK_NODATA,true,NULL);
    if(ok)ctx->pending=false;
    return ok;
}
static bool vg_raw_call(bles_virgl_t *a,void *data) {
    if(!a || !g_virtio.virgl_commands_ready || a->abi!=BLES_VIRGL_ABI)return false;
    if(a->op==BLES_VIRGL_CAPS)return vg_raw_caps(a,data);
    if(a->op==BLES_VIRGL_CONTEXT_CREATE)return vg_raw_create_context(a);
    vg_raw_context_t *ctx=vg_raw_context(a->context);if(!ctx)return false;
    uint32_t saved=g_virtio.virgl_context_id;g_virtio.virgl_context_id=ctx->id;
    bool ok=false;
    switch(a->op) {
    case BLES_VIRGL_CONTEXT_DESTROY: {
        if(!vg_raw_wait(ctx))break;
        while(ctx->resources)vg_raw_free_resource(ctx,ctx->resources);
        virtio_gpu_ctrl_hdr_t request,response;kmemset(&request,0,sizeof(request));
        request.type=VIRTIO_GPU_CMD_CTX_DESTROY;request.ctx_id=ctx->id;
        ok=virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),
                                      VIRTIO_GPU_RESP_OK_NODATA,true,NULL);
        if(ok)ctx->id=0;
        break;
    }
    case BLES_VIRGL_RESOURCE_CREATE:ok=vg_raw_create_resource(ctx,a);break;
    case BLES_VIRGL_RESOURCE_DESTROY: {
        vg_raw_resource_t *r=vg_raw_resource(ctx,a->p[0]);
        if(r && vg_raw_wait(ctx)){vg_raw_free_resource(ctx,r);ok=true;}break;
    }
    case BLES_VIRGL_TRANSFER_WRITE:case BLES_VIRGL_TRANSFER_READ:
    case BLES_VIRGL_TRANSFER_WRITE_ASYNC:
        ok=vg_raw_transfer(ctx,a,data);break;
    case BLES_VIRGL_SUBMIT:case BLES_VIRGL_SUBMIT_ASYNC:ok=vg_raw_submit(ctx,a,data);break;
    case BLES_VIRGL_WAIT:ok=vg_raw_wait(ctx);break;
    }
    g_virtio.virgl_context_id=saved;return ok;
}
