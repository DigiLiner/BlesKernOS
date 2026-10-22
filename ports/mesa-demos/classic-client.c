/* SPDX-License-Identifier: MIT */
#include "../gallium/include/bleskernos_gallium.h"
#include <GL/osmesa.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
extern int bkmesa_native_open(unsigned,unsigned,int);
extern int bkmesa_native_present(unsigned,const void *,unsigned,unsigned);
extern int bkmesa_native_poll(bles_gfx_event_t *);
extern void bkmesa_native_close(void);
extern int bkmesa_native_unpublish(void);
extern unsigned gfx3d_capabilities(void);
struct bkg_context {OSMesaContext mesa;uint32_t *pixels;unsigned w,h;int gpu;};
static int used;
bkg_context *bkg_create(unsigned w,unsigned h) {
    if(used || w<160 || h<90 || w>1024 || h>768)return NULL;
    used=1;
    bkg_context *c=calloc(1,sizeof(*c));if(!c)return NULL;
    c->w=w;c->h=h;c->pixels=calloc(w*h,4);
    c->mesa=OSMesaCreateContextExt(OSMESA_BGRA,16,0,0,NULL);
    const char *mode=getenv("BKG_MESA35_MODE");
    int software=mode && !strcmp(mode,"software");
    int automatic=mode && !strcmp(mode,"auto");
    if(!c->pixels || !c->mesa || !bkg_current(c)) {bkg_destroy(c);return NULL;}
    OSMesaPixelStore(OSMESA_Y_UP,0);
    if(!OSMesaBleskSetRenderer(c->mesa,software?1:automatic?0:3) ||
        (!software && !automatic && !OSMesaBleskGPUAvailable(c->mesa))) {bkg_destroy(c);return NULL;}
    c->gpu=!software && OSMesaBleskGPUAvailable(c->mesa) && (gfx3d_capabilities()&(1U<<9));
    if(!software)setenv("BLES_WINE_GL_TRACE","1",1);
    if(!bkmesa_native_open(w,h,c->gpu)){bkg_destroy(c);return NULL;}
    fprintf(stderr,"[MESA35:NATIVE] renderer=%s mode=%s gpu_present=%d\n",
        OSMesaBleskRendererName(c->mesa),software?"software":automatic?"auto":"strict",c->gpu);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    if(!bkg_present(c)){bkg_destroy(c);return NULL;}
    OSMesaBleskResetStats(c->mesa);
    return c;
}
void bkg_destroy(bkg_context *c) {
    if(!c)return;
    /* Vision stops sampling before Mesa frees the descriptor-owned target. */
    if(c->gpu)bkmesa_native_unpublish();
    if(c->mesa)OSMesaDestroyContext(c->mesa);
    bkmesa_native_close();
    free(c->pixels);free(c);
}
int bkg_current(bkg_context *c){return c && (OSMesaGetCurrentContext()==c->mesa || OSMesaMakeCurrent(c->mesa,c->pixels,GL_UNSIGNED_BYTE,c->w,c->h));}
int bkg_present(bkg_context *c) {
    if(!bkg_current(c))return 0;
    glFlush();
    if(OSMesaBleskStrictFailed(c->mesa)) {
        fprintf(stderr,"[MESA35:NATIVE] FAIL strict: %s\n",OSMesaBleskLastFallbackString(c->mesa));return 0;
    }
    if(c->gpu) {
        unsigned target=OSMesaBleskGPUSurface(c->mesa);
        return target && bkmesa_native_present(target,NULL,c->w,c->h);
    }
    return OSMesaBleskResolveColor(c->mesa) && bkmesa_native_present(0,c->pixels,c->w,c->h);
}
int bkg_poll_event(bkg_context *c,void *event){return c && bkmesa_native_poll(event)>0;}
int bkg_should_quit(bkg_context *c){bles_gfx_event_t e;while(bkg_poll_event(c,&e))if(e.type==BLES_GFX_CLOSE || (e.type==BLES_GFX_KEY && e.key==27))return 1;return 0;}
void *bkg_get_proc(const char *name) {
    static void *module;
    if(!module)module=dlopen("/SYSTEM/LIBS/GALLIUM/libbkmesa.so.1",RTLD_NOW|RTLD_LOCAL);
    return module?dlsym(module,name):NULL;
}
int bkg_classic_stats(bkg_context *c,OSMesaBleskStats *out){if(!c || !out)return 0;OSMesaBleskGetStats(c->mesa,out);return 1;}
