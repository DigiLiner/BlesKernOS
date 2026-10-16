/* Intel Gen2 fixed-function subset. Included in INTELGMA.DVR so 2D and 3D
 * share one ring and one owned GGTT arena. Packet definitions are from Mesa
 * i830 (MIT, notices in intel_i830_{regs,common}.h). No textures are advertised;
 * Mesa keeps unsupported GL states on its software renderer. */
#include "intel_i830_regs.h"
#define GMA3_CAPS (GFX3D_CAP_FIXED_FUNCTION|GFX3D_CAP_RENDER_TARGETS|\
    GFX3D_CAP_VERTEX_BUFFERS|GFX3D_CAP_PRESENT|GFX3D_CAP_TINYGL|\
    GFX3D_CAP_DEPTH_BUFFER|GFX3D_CAP_DEPTH_SURFACE_IO|GFX3D_CAP_DEPTH_FUNCS|\
    GFX3D_CAP_ALPHA_BLEND)
#define GMA3_FIRST_PAGE 18U /* 64-KiB ring + two scratch pages */
#define GMA3_PAGES (GMA_OWNED_BYTES/4096U)
#define GMA3_FLAGS (GFX3D_DRAW_DEPTH_TEST|GFX3D_DRAW_DEPTH_WRITE|GFX3D_DRAW_BLEND|\
    GFX3D_DRAW_CLEAR_COLOR|GFX3D_DRAW_CLEAR_DEPTH|GFX3D_DRAW_REVERSED_DEPTH|\
    GFX3D_DRAW_PREMULTIPLIED|GFX3D_DRAW_DEPTH_FUNC_VALID|GFX3D_DRAW_DEPTH_FUNC_MASK)
typedef struct {
    uint32_t handle,offset,depth,pitch,zpitch,page,pages;
    uint16_t w,h;
} gma3_surface;
static gma3_surface gma3_surfaces[64];
static uint32_t gma3_pages[GMA3_PAGES/32U],gma3_next=1;
static bool gma3_tested,gma3_ready;
static bool gma3_sync(void) {
    if(!g_gma.accel)return false;
    const uint32_t flush[2]={GMA_MI_FLUSH|1U,GMA_MI_NOOP};
    if(gma_emit(flush,2U) && gma_wait_idle())return true;
    gma_stop_accel("3D fence");gma3_ready=false;return false;
}
static gma3_surface *gma3_get(uint32_t handle) {
    for(unsigned i=0;i<64;i++)if(gma3_surfaces[i].handle==handle && handle)
        return &gma3_surfaces[i];
    return NULL;
}
static bool gma3_create(const gfx3d_surface_desc_t *d,uint32_t *out) {
    if(out)*out=0;
    if(!out || !d || !g_gma.accel || !g_gma.owned_raw ||
       !d->width || !d->height || d->width>2048 || d->height>2048 ||
       (d->format!=GFX3D_FORMAT_XRGB8888 && d->format!=GFX3D_FORMAT_ARGB8888) ||
       (d->flags&~(GFX3D_SURFACE_RENDER_TARGET|GFX3D_SURFACE_TEXTURE|
        GFX3D_SURFACE_DYNAMIC|GFX3D_SURFACE_WINDOW|GFX3D_SURFACE_TINYGL)))return false;
    gma3_surface *s=NULL;
    for(unsigned i=0;i<64;i++)if(!gma3_surfaces[i].handle){s=&gma3_surfaces[i];break;}
    if(!s || !gma3_next)return false;
    uint32_t pitch=ALIGN_UP((uint32_t)d->width*4U,64U);
    uint32_t zpitch=ALIGN_UP((uint32_t)d->width*2U,64U);
    uint32_t color=ALIGN_UP(pitch*d->height,4096U);
    uint32_t size=color+ALIGN_UP(zpitch*d->height,4096U),pages=size/4096U;
    uint32_t first=0,run=0;
    for(uint32_t i=GMA3_FIRST_PAGE;i<GMA3_PAGES;i++) {
        if(gma3_pages[i/32U]&(1U<<(i%32U))){run=0;continue;}
        if(!run)first=i;
        if(++run==pages)break;
    }
    if(run<pages)return false;
    for(uint32_t i=first;i<first+pages;i++)gma3_pages[i/32U]|=1U<<(i%32U);
    *s=(gma3_surface){.handle=gma3_next++,.offset=g_gma.owned_offset+first*4096U,
        .pitch=pitch,.zpitch=zpitch,.page=first,.pages=pages,.w=d->width,.h=d->height};
    s->depth=s->offset+color;
    kmemset((void *)(g_gma.aperture+s->offset),0,size);
    *out=s->handle;return true;
}
static bool gma3_destroy(uint32_t h) {
    gma3_surface *s=gma3_get(h);if(!s || !gma3_sync())return false;
    for(uint32_t i=s->page;i<s->page+s->pages;i++)gma3_pages[i/32U]&=~(1U<<(i%32U));
    kmemset(s,0,sizeof(*s));return true;
}
static bool gma3_rect(gma3_surface *s,const gfx_rect_t *r,gfx_rect_t *out) {
    if(!s || !out)return false;
    *out=r?*r:(gfx_rect_t){0,0,s->w,s->h};
    return out->x>=0 && out->y>=0 && out->w>0 && out->h>0 &&
        (uint32_t)out->x+(uint32_t)out->w<=s->w &&
        (uint32_t)out->y+(uint32_t)out->h<=s->h;
}
static bool gma3_transfer(uint32_t h,uint32_t *pixels,uint32_t pitch,
                           const gfx_rect_t *rect,bool upload) {
    gma3_surface *s=gma3_get(h);gfx_rect_t r;
    if(!pixels || !gma3_rect(s,rect,&r) || pitch<(uint32_t)r.x+(uint32_t)r.w ||
       !gma3_sync())return false;
    for(int y=r.y;y<r.y+r.h;y++) {
        void *gpu=(void *)(g_gma.aperture+s->offset+y*s->pitch+r.x*4U);
        uint32_t *cpu=pixels+(uint32_t)y*pitch+(uint32_t)r.x;
        if(upload)kmemcpy(gpu,cpu,(uint32_t)r.w*4U);
        else kmemcpy(cpu,gpu,(uint32_t)r.w*4U);
    }
    return true;
}
static bool gma3_upload(uint32_t h,const uint32_t *p,uint32_t pitch,const gfx_rect_t *r) {
    return gma3_transfer(h,(uint32_t *)p,pitch,r,true);
}
static bool gma3_download(uint32_t h,uint32_t *p,uint32_t pitch,const gfx_rect_t *r) {
    return gma3_transfer(h,p,pitch,r,false);
}
static bool gma3_depth(uint32_t h,uint16_t *pixels,uint32_t pitch,
                       const gfx_rect_t *rect,bool upload) {
    gma3_surface *s=gma3_get(h);gfx_rect_t r;
    /* Public ABI depth pitch is a count of uint16_t pixels. */
    if(!pixels || !gma3_rect(s,rect,&r) ||
       pitch<(uint32_t)r.x+(uint32_t)r.w || !gma3_sync())return false;
    for(int y=r.y;y<r.y+r.h;y++) {
        void *gpu=(void *)(g_gma.aperture+s->depth+y*s->zpitch+r.x*2U);
        uint16_t *cpu=pixels+(uint32_t)y*pitch+(uint32_t)r.x;
        if(upload)kmemcpy(gpu,cpu,(uint32_t)r.w*2U);
        else kmemcpy(cpu,gpu,(uint32_t)r.w*2U);
    }
    return true;
}
static bool gma3_depth_upload(uint32_t h,const uint16_t *p,uint32_t pitch,const gfx_rect_t *r) {
    return gma3_depth(h,(uint16_t *)p,pitch,r,true);
}
static bool gma3_depth_download(uint32_t h,uint16_t *p,uint32_t pitch,const gfx_rect_t *r) {
    return gma3_depth(h,p,pitch,r,false);
}
static bool gma3_clear(uint32_t h,uint32_t color,uint32_t *f) {
    if(f)*f=0;
    gma3_surface *s=gma3_get(h);
    return s && gma_color_blt(s->offset,s->pitch,0,0,s->w,s->h,color) && gma3_sync();
}
static bool gma3_begin(uint32_t h,uint32_t color,float depth,uint32_t flags) {
    gma3_surface *s=gma3_get(h);
    if(!s || (flags&~GMA3_FLAGS) || !(depth>=0.0f && depth<=1.0f) || !gma3_sync())return false;
    if((flags&GFX3D_DRAW_CLEAR_COLOR) && !gma3_clear(h,color,NULL))return false;
    if(flags&GFX3D_DRAW_CLEAR_DEPTH) {
        uint16_t z=(uint16_t)(depth*65535.0f+0.5f);
        for(uint32_t y=0;y<s->h;y++) {
            volatile uint16_t *row=(void *)(g_gma.aperture+s->depth+y*s->zpitch);
            for(uint32_t x=0;x<s->w;x++)row[x]=z;
        }
    }
    return true;
}
/* Complete state on every batch: a 2D blit may have run between 3D calls. */
static uint32_t gma3_state(uint32_t *c,gma3_surface *s,uint32_t flags) {
    uint32_t n=0;
#define G3(v) c[n++]=(v)
    G3(GMA_MI_FLUSH|1U);
    G3(_3DSTATE_DFLT_DIFFUSE_CMD);G3(0);G3(_3DSTATE_DFLT_SPEC_CMD);G3(0);
    G3(_3DSTATE_DFLT_Z_CMD);G3(0);
    G3(_3DSTATE_VERTEX_TRANS_CMD);G3(DISABLE_VIEWPORT_TRANSFORM|DISABLE_PERSP_DIVIDE);
    G3(_3DSTATE_W_STATE_CMD);G3(MAGIC_W_STATE_DWORD1);G3(0x3F800000U);
    G3(_3DSTATE_VFT0_CMD|VFT0_XYZ|VFT0_DIFFUSE);G3(_3DSTATE_VFT1_CMD);
    G3(_3DSTATE_ENABLES_1_CMD|DISABLE_LOGIC_OP|DISABLE_STENCIL_TEST|
        DISABLE_DEPTH_BIAS|DISABLE_SPEC_ADD|DISABLE_FOG|DISABLE_ALPHA_TEST|
        ((flags&GFX3D_DRAW_BLEND)?ENABLE_COLOR_BLEND:DISABLE_COLOR_BLEND)|
        ((flags&GFX3D_DRAW_DEPTH_TEST)?ENABLE_DEPTH_TEST:DISABLE_DEPTH_TEST));
    G3(_3DSTATE_ENABLES_2_CMD|DISABLE_STENCIL_WRITE|DISABLE_TEX_CACHE|
        DISABLE_DITHER|ENABLE_COLOR_MASK|ENABLE_COLOR_WRITE|
        ((flags&GFX3D_DRAW_DEPTH_WRITE)?ENABLE_DEPTH_WRITE:DISABLE_DEPTH_WRITE));
    /* Intel blend factors ZERO=1, ONE=2, SRC_ALPHA=5, INV_SRC_ALPHA=6. */
    G3(_3DSTATE_MODES_1_CMD|ENABLE_COLR_BLND_FUNC|ENABLE_SRC_BLND_FACTOR|
        SRC_BLND_FACT((flags&GFX3D_DRAW_PREMULTIPLIED)?2U:5U)|
        ENABLE_DST_BLND_FACTOR|DST_BLND_FACT(6U));
    static const uint8_t compare[]={1,2,3,4,5,6,7,0};
    G3(_3DSTATE_MODES_3_CMD|ENABLE_DEPTH_TEST_FUNC|
        DEPTH_TEST_FUNC(compare[gfx3d_draw_depth_func(flags)])|
        ENABLE_ALPHA_SHADE_MODE|ENABLE_FOG_SHADE_MODE|ENABLE_SPEC_SHADE_MODE|
        ENABLE_COLOR_SHADE_MODE|ENABLE_CULL_MODE|CULLMODE_NONE);
    G3(_3DSTATE_MODES_4_CMD|ENABLE_LOGIC_OP_FUNC|LOGIC_OP_FUNC(LOGICOP_COPY));
    G3(_3DSTATE_MODES_5_CMD|FLUSH_RENDER_CACHE|FLUSH_TEXTURE_CACHE|
        ENABLE_SPRITE_POINT_TEX|SPRITE_POINT_TEX_OFF);
    G3(_3DSTATE_INDPT_ALPHA_BLEND_CMD|DISABLE_INDPT_ALPHA_BLEND|ENABLE_ALPHA_BLENDFUNC);
    G3(_3DSTATE_STIPPLE);G3(0);
    G3(_3DSTATE_MAP_BLEND_OP_CMD(0)|TEXPIPE_COLOR|ENABLE_TEXOUTPUT_WRT_SEL|
        DISABLE_TEX_CNTRL_STAGE|TEXOP_MODIFY_PARMS|TEXOP_LAST_STAGE|TEXBLENDOP_ARG1);
    G3(_3DSTATE_MAP_BLEND_OP_CMD(0)|TEXPIPE_ALPHA|ENABLE_TEXOUTPUT_WRT_SEL|
        TEXOP_MODIFY_PARMS|TEXBLENDOP_ARG1);
    G3(_3DSTATE_MAP_BLEND_ARG_CMD(0)|TEXPIPE_COLOR|TEXBLEND_ARG1|
        TEXBLENDARG_MODIFY_PARMS|TEXBLENDARG_DIFFUSE);
    G3(_3DSTATE_MAP_BLEND_ARG_CMD(0)|TEXPIPE_ALPHA|TEXBLEND_ARG1|
        TEXBLENDARG_MODIFY_PARMS|TEXBLENDARG_DIFFUSE);
    G3(_3DSTATE_BUF_INFO_CMD);G3(BUF_3D_ID_COLOR_BACK|s->pitch);G3(s->offset);
    G3(_3DSTATE_BUF_INFO_CMD);G3(BUF_3D_ID_DEPTH|s->zpitch);G3(s->depth);
    G3(_3DSTATE_DST_BUF_VARS_CMD);
    G3(DSTORG_HORT_BIAS(8)|DSTORG_VERT_BIAS(8)|COLR_BUF_ARGB8888|DEPTH_FRMT_16_FIXED);
    G3(_3DSTATE_SCISSOR_ENABLE_CMD|DISABLE_SCISSOR_RECT);
    G3(_3DSTATE_DRAW_RECT_CMD);G3(0);G3(0);
    G3(((uint32_t)(s->h-1U)<<16)|(s->w-1U));G3(0);
    G3(_3DSTATE_RASTER_RULES_CMD|ENABLE_POINT_RASTER_RULE|OGL_POINT_RASTER_RULE);
#undef G3
    return n;
}
static bool gma3_draw(uint32_t h,uint32_t texture,const gfx3d_vertex_t *v,
                      uint32_t count,uint32_t flags,uint32_t *fence) {
    if(fence)*fence=0;
    gma3_surface *s=gma3_get(h);
    if(!s || texture || !v || !count || count%3U || count>65535U ||
       (flags&~GMA3_FLAGS) || !g_gma.accel)return false;
    /* Validate the entire call before submitting anything. IEEE comparisons
     * reject NaN/Inf as well as coordinates outside the Gen2 guard band. */
    for(uint32_t i=0;i<count;i++)if(!(v[i].x>=-2048.0f && v[i].x<=2047.0f &&
        v[i].y>=-2048.0f && v[i].y<=2047.0f && v[i].z>=0.0f && v[i].z<=1.0f))return false;
    for(uint32_t at=0;at<count;) {
        uint32_t c[64U+96U*4U],n=gma3_state(c,s,flags);
        uint32_t take=count-at;if(take>96U)take=96U;
        c[n++]=_3DPRIMITIVE|PRIM_INLINE|PRIM3D_TRILIST|(take*4U-1U);
        for(uint32_t i=0;i<take;i++) {
            union {float f;uint32_t u;} bits;
            bits.f=v[at+i].x;c[n++]=bits.u;bits.f=v[at+i].y;c[n++]=bits.u;
            bits.f=v[at+i].z;c[n++]=bits.u;c[n++]=v[at+i].color;
        }
        if(n&1U)c[n++]=GMA_MI_NOOP;
        if(!gma_emit(c,n) || !gma3_sync())return false;
        at+=take;
    }
    return true;
}
static bool gma3_end(uint32_t h,uint32_t *f) {if(f)*f=0;return gma3_get(h)&&gma3_sync();}
static bool gma3_wait(uint32_t f) {(void)f;return gma3_sync();}
static bool gma3_present(uint32_t h,const gfx_rect_t *rect,uint32_t *f) {
    if(f)*f=0;
    gma3_surface *s=gma3_get(h);gfx_rect_t r;
    if(!gma3_rect(s,rect,&r) || r.x+r.w>g_gma.mode.width ||
       r.y+r.h>g_gma.mode.height || !gma3_sync())return false;
    /* XY_SRC_COPY_BLT accepts independent source/destination strides. */
    uint32_t c[10]={GMA_XY_SRC_COPY_BLT|GMA_BLT_WRITE_ALPHA|GMA_BLT_WRITE_RGB,
        GMA_BLT_DEPTH_32|GMA_ROP_COPY|g_gma.mode.pitch,
        ((uint32_t)r.y<<16)|r.x,((uint32_t)(r.y+r.h)<<16)|(r.x+r.w),
        g_gma.fb_offset,((uint32_t)r.y<<16)|r.x,s->pitch,s->offset,GMA_MI_FLUSH,0};
    return gma_emit(c,10U)&&gma3_sync();
}
static bool gma3_selftest(uint32_t *fence) {
    if(fence)*fence=0;
    uint32_t h=0;
    gfx3d_surface_desc_t d={32,32,GFX3D_FORMAT_ARGB8888,GFX3D_SURFACE_RENDER_TARGET};
    if(!gma3_create(&d,&h))return false;
    gma3_surface *s=gma3_get(h);
    const gfx3d_vertex_t triangle[3]={{2,2,0.25f,1,0xFFFF0000U,0,0},
        {26,2,0.25f,1,0xFFFF0000U,0,0},{2,26,0.25f,1,0xFFFF0000U,0,0}};
    bool ok=gma3_begin(h,0xFF102030U,1.0f,GFX3D_DRAW_CLEAR_COLOR|GFX3D_DRAW_CLEAR_DEPTH) &&
        gma3_draw(h,0,triangle,3,GFX3D_DRAW_DEPTH_TEST|GFX3D_DRAW_DEPTH_WRITE,NULL);
    volatile uint32_t *inside=(void *)(g_gma.aperture+s->offset+6*s->pitch+6*4U);
    volatile uint32_t *outside=(void *)(g_gma.aperture+s->offset+30*s->pitch+30*4U);
    ok=ok && (*inside&0xFFFFFFU)==0xFF0000U && (*outside&0xFFFFFFU)==0x102030U;
    /* A farther triangle must fail the depth test. */
    gfx3d_vertex_t behind[3];kmemcpy(behind,triangle,sizeof(behind));
    for(unsigned i=0;i<3;i++){behind[i].z=0.75f;behind[i].color=0xFF00FF00U;}
    ok=ok && gma3_draw(h,0,behind,3,GFX3D_DRAW_DEPTH_TEST|GFX3D_DRAW_DEPTH_WRITE,NULL) &&
        (*inside&0xFFFFFFU)==0xFF0000U;
    /* Blend is advertised only if the actual GPU mixes source/destination. */
    for(unsigned i=0;i<3;i++)behind[i].color=0x800000FFU;
    ok=ok && gma3_draw(h,0,behind,3,GFX3D_DRAW_BLEND,NULL);
    uint32_t mixed=*inside;
    ok=ok && ((mixed>>16)&255U)>=126U && ((mixed>>16)&255U)<=129U &&
        (mixed&255U)>=126U && (mixed&255U)<=129U;
    (void)gma3_destroy(h);
    return ok;
}
static bool gma3_probe(gfx3d_info_t *info) {
    if(!g_gma.active || !g_gma.accel || g_gma.chip->generation!=2U ||
       g_gma.mode.bpp!=32U || !g_gma.owned_raw)return false;
    if(!gma3_tested) {
        gma3_tested=true;gma3_ready=gma3_selftest(NULL);
        kprintf("[INTELGMA:3D] triangle/depth/blend selftest %s\n",gma3_ready?"PASS":"FAIL; Mesa software");
    }
    if(!gma3_ready)return false;
    if(info)*info=(gfx3d_info_t){true,"intel_852_855_3d","Gen2 ring + owned GGTT",
        GMA3_CAPS,2U,1U,1U};
    return true;
}
static void gma3_reset(void) {
    if(!gma3_sync())return;
    kmemset(gma3_surfaces,0,sizeof(gma3_surfaces));kmemset(gma3_pages,0,sizeof(gma3_pages));
}
GMA_LOCKED(gma3_locked_probe,(gfx3d_info_t *i),gma3_probe(i))
GMA_LOCKED(gma3_locked_create,(const gfx3d_surface_desc_t *d,uint32_t *h),gma3_create(d,h))
GMA_LOCKED(gma3_locked_destroy,(uint32_t h),gma3_destroy(h))
GMA_LOCKED(gma3_locked_upload,(uint32_t h,const uint32_t *p,uint32_t pitch,const gfx_rect_t *r),gma3_upload(h,p,pitch,r))
GMA_LOCKED(gma3_locked_download,(uint32_t h,uint32_t *p,uint32_t pitch,const gfx_rect_t *r),gma3_download(h,p,pitch,r))
GMA_LOCKED(gma3_locked_clear,(uint32_t h,uint32_t c,uint32_t *f),gma3_clear(h,c,f))
GMA_LOCKED(gma3_locked_present,(uint32_t h,const gfx_rect_t *r,uint32_t *f),gma3_present(h,r,f))
GMA_LOCKED(gma3_locked_begin,(uint32_t h,uint32_t c,float z,uint32_t f),gma3_begin(h,c,z,f))
GMA_LOCKED(gma3_locked_draw,(uint32_t h,uint32_t t,const gfx3d_vertex_t *v,uint32_t n,uint32_t f,uint32_t *o),gma3_draw(h,t,v,n,f,o))
GMA_LOCKED(gma3_locked_end,(uint32_t h,uint32_t *f),gma3_end(h,f))
GMA_LOCKED(gma3_locked_wait,(uint32_t f),gma3_wait(f))
GMA_LOCKED(gma3_locked_test,(uint32_t *f),gma3_selftest(f))
GMA_LOCKED(gma3_locked_depth_up,(uint32_t h,const uint16_t *p,uint32_t pitch,const gfx_rect_t *r),gma3_depth_upload(h,p,pitch,r))
GMA_LOCKED(gma3_locked_depth_down,(uint32_t h,uint16_t *p,uint32_t pitch,const gfx_rect_t *r),gma3_depth_download(h,p,pitch,r))
static void gma3_locked_reset(void) {
    kmutex_lock(&gma_engine_mutex);gma3_reset();kmutex_unlock(&gma_engine_mutex);
}
static bool gma3_register(void) {
    static const gfx3d_driver_ops_t ops={
        .abi_version=BK_GFX3D_DRIVER_ABI_VERSION,.descriptor_size=sizeof(gfx3d_driver_ops_t),
        .name="intel_852_855_3d",.priority=235,.capabilities=GMA3_CAPS,
        .probe=gma3_locked_probe,.reset=gma3_locked_reset,.surface_create=gma3_locked_create,
        .surface_destroy=gma3_locked_destroy,.surface_upload=gma3_locked_upload,
        .surface_download=gma3_locked_download,.surface_clear=gma3_locked_clear,
        .surface_present=gma3_locked_present,.begin=gma3_locked_begin,.draw_triangles=gma3_locked_draw,
        .end=gma3_locked_end,.wait_fence=gma3_locked_wait,.selftest=gma3_locked_test,
        .depth_upload=gma3_locked_depth_up,.depth_download=gma3_locked_depth_down};
    return gfx3d_register_driver(&ops);
}
