/* SPDX-License-Identifier: MIT
 * Mesa Gallium VirGL winsys for BlesKernOS. No Linux DRM emulation is needed:
 * /dev/blesgfx owns a real isolated VirGL context and its backing resources.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "virgl/virgl_winsys.h"
#include "virgl/virgl_public.h"
#include "blesvirgl.h"

struct bk_winsys {
    struct virgl_winsys base;
    int fd;
    pthread_mutex_t lock;
    bool async, async_upload, failed;
    uint64_t submitted, completed;
};
struct virgl_hw_res {
    struct pipe_reference reference;
    uint32_t id, size;
    enum pipe_texture_target target;
    uint64_t last_submit;
    void *map;
};
struct bk_command {
    struct virgl_cmd_buf base;
    struct virgl_winsys *ws;
    struct virgl_hw_res **refs;
    unsigned count, capacity, words;
    bool failed;
};
struct bk_fence {struct pipe_reference reference;};

/* Serialize ioctl order and completion bookkeeping for all contexts/threads
 * using this screen. A fenced transfer completes every earlier submission. */
static int bk_ioctl(struct virgl_winsys *ws,bles_virgl_t *a) {
    struct bk_winsys *w=(struct bk_winsys *)ws;
    int rc=ioctl(w->fd,BLES_GFX_VIRGL,a);
    if(!rc){
        if(a->op==BLES_VIRGL_SUBMIT_ASYNC || a->op==BLES_VIRGL_TRANSFER_WRITE_ASYNC)w->submitted++;
        if(a->op==BLES_VIRGL_SUBMIT || a->op==BLES_VIRGL_WAIT ||
           a->op==BLES_VIRGL_TRANSFER_READ || a->op==BLES_VIRGL_TRANSFER_WRITE ||
           a->op==BLES_VIRGL_RESOURCE_DESTROY)w->completed=w->submitted;
    }
    return rc;
}
static int bk_call_locked(struct virgl_winsys *ws,bles_virgl_t *a,void *data,uint32_t bytes) {
    a->abi=BLES_VIRGL_ABI;a->buffer=(uintptr_t)data;a->bytes=bytes;
    static __thread int trace_initialized,trace;
    static __thread uint64_t since,counts[16],times[16],payload[16];
    if(!trace_initialized){const char *s=getenv("BLES_WINE_GL_TRACE");trace=s&&!strcmp(s,"1");trace_initialized=1;}
    if(!trace)return bk_ioctl(ws,a);
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    uint64_t start=(uint64_t)t.tv_sec*1000000U+t.tv_nsec/1000U;
    unsigned op=a->op;
    int rc=bk_ioctl(ws,a);
    clock_gettime(CLOCK_MONOTONIC,&t);
    uint64_t end=(uint64_t)t.tv_sec*1000000U+t.tv_nsec/1000U;
    if(!since)since=start;
    if(op<16){counts[op]++;times[op]+=end-start;payload[op]+=bytes;}
    if(end-since>=1000000U){
        fprintf(stderr,"[WINE:VIRGL-PERF] pid=%d elapsed_us=%llu submit=%llu/%lluus read=%llu/%lluus write=%llu/%lluus create=%llu destroy=%llu wait=%llu/%lluus write_bytes=%llu command_bytes=%llu\n",
            getpid(),(unsigned long long)(end-since),
            (unsigned long long)(counts[BLES_VIRGL_SUBMIT]+counts[BLES_VIRGL_SUBMIT_ASYNC]),
            (unsigned long long)(times[BLES_VIRGL_SUBMIT]+times[BLES_VIRGL_SUBMIT_ASYNC]),
            (unsigned long long)counts[BLES_VIRGL_TRANSFER_READ],(unsigned long long)times[BLES_VIRGL_TRANSFER_READ],
            (unsigned long long)(counts[BLES_VIRGL_TRANSFER_WRITE]+counts[BLES_VIRGL_TRANSFER_WRITE_ASYNC]),
            (unsigned long long)(times[BLES_VIRGL_TRANSFER_WRITE]+times[BLES_VIRGL_TRANSFER_WRITE_ASYNC]),
            (unsigned long long)counts[BLES_VIRGL_RESOURCE_CREATE],(unsigned long long)counts[BLES_VIRGL_RESOURCE_DESTROY],
            (unsigned long long)counts[BLES_VIRGL_WAIT],(unsigned long long)times[BLES_VIRGL_WAIT],
            (unsigned long long)(payload[BLES_VIRGL_TRANSFER_WRITE]+payload[BLES_VIRGL_TRANSFER_WRITE_ASYNC]),
            (unsigned long long)(payload[BLES_VIRGL_SUBMIT]+payload[BLES_VIRGL_SUBMIT_ASYNC]));
        memset(counts,0,sizeof(counts));memset(times,0,sizeof(times));memset(payload,0,sizeof(payload));since=end;
    }
    return rc;
}
static int bk_call(struct virgl_winsys *ws,bles_virgl_t *a,void *data,uint32_t bytes) {
    struct bk_winsys *w=(struct bk_winsys *)ws;
    pthread_mutex_lock(&w->lock);
    int rc=bk_call_locked(ws,a,data,bytes);
    pthread_mutex_unlock(&w->lock);return rc;
}
static bool bk_sync_locked(struct virgl_winsys *ws) {
    struct bk_winsys *w=(struct bk_winsys *)ws;
    if(w->submitted!=w->completed){
        bles_virgl_t a={.op=BLES_VIRGL_WAIT};
        if(bk_call_locked(ws,&a,NULL,0))w->failed=true;
    }
    return !w->failed;
}
static void bk_destroy(struct virgl_winsys *ws) {
    /* Last close releases the host context and every surviving resource. */
    close(((struct bk_winsys *)ws)->fd);
    pthread_mutex_destroy(&((struct bk_winsys *)ws)->lock);free(ws);
}
static int bk_fd(struct virgl_winsys *ws) {return ((struct bk_winsys *)ws)->fd;}
static struct virgl_hw_res *bk_create_resource(struct virgl_winsys *ws,
        enum pipe_texture_target target,const void *front,uint32_t format,uint32_t bind,
        uint32_t width,uint32_t height,uint32_t depth,uint32_t layers,uint32_t level,
        uint32_t samples,uint32_t flags,uint32_t size) {
    (void)front;
    struct virgl_hw_res *r=calloc(1,sizeof(*r));if(!r)return NULL;
    if(size){r->map=calloc(1,size);if(!r->map){free(r);return NULL;}}
    bles_virgl_t a={.op=BLES_VIRGL_RESOURCE_CREATE,.p={target,pipe_to_virgl_format(format),
                    bind,width,height,depth,layers,level,samples,flags,size}};
    if(bk_call(ws,&a,NULL,0)){free(r->map);free(r);return NULL;}
    pipe_reference_init(&r->reference,1);r->id=a.p[0];r->size=size;r->target=target;return r;
}
static void bk_reference(struct virgl_winsys *ws,struct virgl_hw_res **dst,struct virgl_hw_res *src) {
    struct virgl_hw_res *old=*dst;
    if(pipe_reference(old?&old->reference:NULL,src?&src->reference:NULL)) {
        bles_virgl_t a={.op=BLES_VIRGL_RESOURCE_DESTROY,.p={old->id}};
        (void)bk_call(ws,&a,NULL,0);free(old->map);free(old);
    }
    *dst=src;
}
static void *bk_map(struct virgl_winsys *ws,struct virgl_hw_res *r) {(void)ws;return r->map;}
static void bk_wait_resource(struct virgl_winsys *ws,struct virgl_hw_res *r) {
    struct bk_winsys *w=(struct bk_winsys *)ws;
    pthread_mutex_lock(&w->lock);
    if(r && r->last_submit>w->completed)(void)bk_sync_locked(ws);
    pthread_mutex_unlock(&w->lock);
}
static bool bk_busy(struct virgl_winsys *ws,struct virgl_hw_res *r) {
    struct bk_winsys *w=(struct bk_winsys *)ws;
    pthread_mutex_lock(&w->lock);
    bool busy=r && r->last_submit>w->completed;
    pthread_mutex_unlock(&w->lock);return busy;
}
static uint32_t bk_size(struct virgl_winsys *ws,struct virgl_hw_res *r) {(void)ws;return r->size;}
static int bk_transfer(struct virgl_winsys *ws,struct virgl_hw_res *r,const struct pipe_box *box,
                       uint32_t stride,uint32_t layer,uint32_t offset,uint32_t level,bool write) {
    if(!r->map || offset>=r->size || box->x<0 || box->y<0 || box->z<0 ||
       box->width<=0 || box->height<=0 || box->depth<=0)return -1;
    bles_virgl_t a={.op=write?BLES_VIRGL_TRANSFER_WRITE:BLES_VIRGL_TRANSFER_READ,
        .p={r->id,level,stride,layer,box->x,box->y,box->z,
            box->width,box->height,box->depth,offset,0}};
    void *data=r->map;uint32_t bytes=r->size;
    if(r->target==PIPE_BUFFER){
        /* Gallium's buffer offset is the first byte of the mapped/flushed
         * range. Host transfer offset (p[10]) and CPU backing offset (p[11])
         * must agree: copying the whole allocation for a few vertices wastes
         * both user->kernel and kernel->resource copies on every draw. */
        if(level || box->y || box->z || box->height!=1 || box->depth!=1 ||
           (uint32_t)box->x!=offset || (uint32_t)box->width>r->size-offset)return -1;
        a.p[11]=offset;
        data=(uint8_t *)r->map+offset;bytes=(uint32_t)box->width;
    }
    /* Texture layouts include mip/layer padding; retain their full backing
     * copy until a format-aware range calculation covers those layouts. */
    struct bk_winsys *w=(struct bk_winsys *)ws;
    pthread_mutex_lock(&w->lock);
    if(write && w->async_upload)a.op=BLES_VIRGL_TRANSFER_WRITE_ASYNC;
    int rc=bk_call_locked(ws,&a,data,bytes);
    if(!rc && write)r->last_submit=w->submitted;
    pthread_mutex_unlock(&w->lock);return rc;
}
static int bk_put(struct virgl_winsys *ws,struct virgl_hw_res *r,const struct pipe_box *box,
                   uint32_t stride,uint32_t layer,uint32_t offset,uint32_t level) {
    return bk_transfer(ws,r,box,stride,layer,offset,level,true);
}
static int bk_get(struct virgl_winsys *ws,struct virgl_hw_res *r,const struct pipe_box *box,
                   uint32_t stride,uint32_t layer,uint32_t offset,uint32_t level) {
    return bk_transfer(ws,r,box,stride,layer,offset,level,false);
}
static struct virgl_hw_res *bk_import(struct virgl_winsys *ws,struct winsys_handle *handle,
        struct pipe_resource *templ,uint32_t *plane,uint32_t *stride,uint32_t *offset,
        uint64_t *modifier,uint32_t *blob) {
    (void)ws;(void)handle;(void)templ;(void)plane;(void)stride;(void)offset;(void)modifier;(void)blob;
    return NULL; /* OSMesa does not import external DMA-BUF resources. */
}
static bool bk_export(struct virgl_winsys *ws,struct virgl_hw_res *r,uint32_t stride,
                       struct winsys_handle *handle) {
    (void)ws;(void)r;(void)stride;(void)handle;return false;
}
static void bk_release_commands(struct bk_command *c) {
    for(unsigned i=0;i<c->count;i++)bk_reference(c->ws,&c->refs[i],NULL);
    c->count=0;
}
static struct virgl_cmd_buf *bk_commands(struct virgl_winsys *ws,uint32_t words) {
    if(words>BLES_VIRGL_MAX_COMMANDS/4)return NULL;
    struct bk_command *c=calloc(1,sizeof(*c));if(!c)return NULL;
    c->base.buf=calloc(words,4);if(!c->base.buf){free(c);return NULL;}
    c->ws=ws;c->words=words;return &c->base;
}
static void bk_destroy_commands(struct virgl_cmd_buf *buf) {
    struct bk_command *c=(struct bk_command *)buf;bk_release_commands(c);
    free(c->refs);free(c->base.buf);free(c);
}
static bool bk_referenced(struct virgl_winsys *ws,struct virgl_cmd_buf *buf,struct virgl_hw_res *r) {
    (void)ws;struct bk_command *c=(struct bk_command *)buf;
    for(unsigned i=0;i<c->count;i++)if(c->refs[i]==r)return true;
    return false;
}
static void bk_emit(struct virgl_winsys *ws,struct virgl_cmd_buf *buf,struct virgl_hw_res *r,bool emit) {
    struct bk_command *c=(struct bk_command *)buf;
    if(emit){if(buf->cdw>=c->words){c->failed=true;return;}buf->buf[buf->cdw++]=r?r->id:0;}
    if(!r || bk_referenced(ws,buf,r))return;
    if(c->count==c->capacity) {
        unsigned n=c->capacity?c->capacity*2:64;
        void *p=realloc(c->refs,n*sizeof(*c->refs));if(!p){c->failed=true;return;}
        c->refs=p;c->capacity=n;
    }
    c->refs[c->count]=NULL;bk_reference(ws,&c->refs[c->count++],r);
}
static void bk_fence_reference(struct virgl_winsys *ws,struct pipe_fence_handle **dst,
                               struct pipe_fence_handle *src) {
    (void)ws;struct bk_fence *old=(struct bk_fence *)*dst,*next=(struct bk_fence *)src;
    if(pipe_reference(old?&old->reference:NULL,next?&next->reference:NULL))free(old);
    *dst=src;
}
static bool bk_fence_wait(struct virgl_winsys *ws,struct pipe_fence_handle *f,uint64_t timeout) {
    (void)ws;(void)timeout;return f!=NULL; /* created only after host completion */
}
static struct pipe_fence_handle *bk_fence(struct virgl_winsys *ws,int fd) {
    if(fd>=0)return NULL; /* no imported sync-file support */
    struct bk_winsys *w=(struct bk_winsys *)ws;
    pthread_mutex_lock(&w->lock);bool ok=bk_sync_locked(ws);pthread_mutex_unlock(&w->lock);
    if(!ok)return NULL;
    struct bk_fence *f=calloc(1,sizeof(*f));if(!f)return NULL;
    pipe_reference_init(&f->reference,1);return (struct pipe_fence_handle *)f;
}
static int bk_fence_fd(struct virgl_winsys *ws,struct pipe_fence_handle *f) {(void)ws;(void)f;return -1;}
static int bk_submit(struct virgl_winsys *ws,struct virgl_cmd_buf *buf,struct pipe_fence_handle **fence) {
    struct bk_command *c=(struct bk_command *)buf;
    struct bk_winsys *w=(struct bk_winsys *)ws;
    pthread_mutex_lock(&w->lock);
    int rc=c->failed || w->failed?-1:0;
    if(!rc && buf->cdw) {
        /* Exported Gallium fences remain already completed, preserving even
         * zero-timeout polling. Only submissions without a public fence run
         * ahead; map/wait, readback and destruction establish real barriers. */
        bles_virgl_t a={.op=w->async && !fence?BLES_VIRGL_SUBMIT_ASYNC:BLES_VIRGL_SUBMIT};
        rc=bk_call_locked(ws,&a,buf->buf,buf->cdw*4);
        if(!rc)for(unsigned i=0;i<c->count;i++)c->refs[i]->last_submit=w->submitted;
    }
    pthread_mutex_unlock(&w->lock);
    if(!rc && fence){*fence=bk_fence(ws,-1);if(!*fence)rc=-1;}
    bk_release_commands(c);buf->cdw=0;c->failed=false;return rc;
}
static int bk_caps(struct virgl_winsys *ws,struct virgl_drm_caps *caps) {
    bles_virgl_t a={.op=BLES_VIRGL_CAPS};
    if(bk_call(ws,&a,NULL,0))return -1;
    ((struct bk_winsys *)ws)->async=!!(a.p[4]&BLES_VIRGL_FEATURE_ASYNC_SUBMIT);
    ((struct bk_winsys *)ws)->async_upload=!!(a.p[4]&BLES_VIRGL_FEATURE_ASYNC_UPLOAD);
    uint32_t size=a.p[3];if(size>65536)return -1;
    void *data=calloc(1,size);if(!data)return -1;
    int rc=bk_call(ws,&a,data,size);
    if(!rc) {
        memset(caps,0,sizeof(*caps));virgl_ws_fill_new_caps_defaults(caps);
        memcpy(&caps->caps,data,size<sizeof(caps->caps)?size:sizeof(caps->caps));
        /* This winsys uses explicit copies, not a userspace mapping of host
         * backing. Encoded copy-transfers require coherent shared backing;
         * use real TRANSFER_{TO,FROM}_HOST_3D instead. Shader/FBO caps remain
         * exactly those supplied by the host. */
        caps->caps.v2.capability_bits &= ~VIRGL_CAP_COPY_TRANSFER;
        caps->caps.v2.capability_bits_v2 &= ~VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS;
    }
    free(data);return rc;
}

struct pipe_screen *bles_virgl_create_screen(void);
struct pipe_screen *bles_virgl_create_screen(void) {
    struct bk_winsys *w=calloc(1,sizeof(*w));if(!w)return NULL;
    if(pthread_mutex_init(&w->lock,NULL)){free(w);return NULL;}
    w->fd=open("/dev/blesgfx",O_RDWR|O_CLOEXEC);if(w->fd<0){pthread_mutex_destroy(&w->lock);free(w);return NULL;}
    bles_virgl_t a={.op=BLES_VIRGL_CONTEXT_CREATE};
    if(bk_call(&w->base,&a,NULL,0)){bk_destroy(&w->base);return NULL;}
    struct virgl_winsys *v=&w->base;
    v->has_private_staging=true;
    v->destroy=bk_destroy;v->get_fd=bk_fd;v->pci_id=0x1af41050;
    v->resource_create=bk_create_resource;v->resource_reference=bk_reference;
    v->resource_map=bk_map;v->resource_wait=bk_wait_resource;v->resource_is_busy=bk_busy;
    v->resource_create_from_handle=bk_import;v->resource_get_handle=bk_export;
    v->resource_get_storage_size=bk_size;v->transfer_put=bk_put;v->transfer_get=bk_get;
    v->cmd_buf_create=bk_commands;v->cmd_buf_destroy=bk_destroy_commands;
    v->emit_res=bk_emit;v->res_is_referenced=bk_referenced;v->submit_cmd=bk_submit;
    v->get_caps=bk_caps;v->cs_create_fence=bk_fence;v->fence_wait=bk_fence_wait;
    v->fence_reference=bk_fence_reference;v->fence_get_fd=bk_fence_fd;
    struct virgl_drm_caps caps;
    if(bk_caps(v,&caps)){bk_destroy(v);return NULL;}
    struct pipe_screen *screen=virgl_create_screen(v,NULL);
    if(!screen)bk_destroy(v);
    return screen;
}
