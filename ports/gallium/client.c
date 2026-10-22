/* SPDX-License-Identifier: MIT */
#include "include/bleskernos_gallium.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
struct bkg_context {void *mesa;uint32_t *pixels;unsigned width,height;};
static void *library;
static void *(*create_context)(const int *,void *);
static void (*destroy_context)(void *);
static unsigned char (*bind_context)(void *,void *,unsigned,int,int);
static void *(*current_context)(void);
static unsigned char (*native_mode)(void *,unsigned char);
static unsigned char (*present_native)(void *);
static int (*native_event)(void *);
static int (*native_ready)(void *);
static int (*native_poll)(void *,void *);
static void *(*lookup)(const char *);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int loaded;
static void load(void) {
    library=dlopen("/SYSTEM/LIBS/GALLIUM/libbkvirgl.so.8",RTLD_NOW|RTLD_LOCAL);
    if(!library){fprintf(stderr,"[GALLIUM] load: %s\n",dlerror());return;}
#define LOAD(var,sym) do {*(void **)(&var)=dlsym(library,sym);if(!var){fprintf(stderr,"[GALLIUM] missing %s\n",sym);return;}}while(0)
    LOAD(create_context,"OSMesaBleskCreateContextAttribs");
    LOAD(destroy_context,"OSMesaDestroyContext");
    LOAD(bind_context,"OSMesaMakeCurrent");
    LOAD(current_context,"OSMesaGetCurrentContext");
    LOAD(native_mode,"OSMesaBleskNativeMode");
    LOAD(present_native,"OSMesaBleskPresentNative");
    LOAD(native_event,"OSMesaBleskNativeEvent");
    LOAD(native_ready,"OSMesaBleskNativeReady");
    LOAD(native_poll,"OSMesaBleskNativePoll");
    LOAD(lookup,"OSMesaGetProcAddress");
    loaded=1;
#undef LOAD
}
bkg_context *bkg_create(unsigned width,unsigned height) {
    pthread_once(&once,load);
    if(!loaded || width<160 || height<90 || width>1024 || height>768)return NULL;
    /* BGRA, depth24/stencil8, compatibility OpenGL 2.0; no silent software
     * driver exists in this build. API/version failures propagate to caller. */
    const int attributes[]={0x22,0x1,0x30,24,0x31,8,0x32,0,0x33,0x35,0x36,2,0x37,0,0};
    bkg_context *c=calloc(1,sizeof(*c));if(!c)return NULL;
    c->pixels=calloc(width*height,4);
    c->mesa=create_context(attributes,NULL);c->width=width;c->height=height;
    if(!c->pixels || !c->mesa || !native_mode(c->mesa,1) || !bkg_current(c)) {
        bkg_destroy(c);return NULL;
    }
    /* Create and attach the native drawable during context setup, as other
     * window-system adapters do. The first measured benchmark frame must
     * not include window creation, compositor allocation and activation. */
    void (*clear_color)(float,float,float,float)=lookup("glClearColor");
    void (*clear)(unsigned)=lookup("glClear");
    if(!clear_color || !clear) {bkg_destroy(c);return NULL;}
    clear_color(0,0,0,1);clear(0x4000);
    if(!present_native(c->mesa)) {bkg_destroy(c);return NULL;}
    unsigned tries=0;
    while(!native_ready(c->mesa) && tries++<3000)usleep(1000);
    if(tries>=3000){bkg_destroy(c);return NULL;}
    /* A screensaver covers the desktop. Keep its default backbuffer opaque:
     * otherwise a clear with alpha=0 exposes the grey GUI window beneath
     * the composited GPU viewport even while frames are arriving. */
    const char *saver=getenv("BKG_FULLSCREEN");
    clear_color(0,0,0,(saver && saver[0]=='1' && saver[1]==0)?1.0f:0.0f);
    return c;
}
void bkg_destroy(bkg_context *c) {
    if(!c)return;
    if(c->mesa)destroy_context(c->mesa);
    free(c->pixels);free(c);
}
int bkg_current(bkg_context *c) {
    return c && (current_context()==c->mesa || bind_context(c->mesa,c->pixels,0x1401,c->width,c->height));
}
int bkg_present(bkg_context *c) {return c && bkg_current(c) && present_native(c->mesa);}
int bkg_should_quit(bkg_context *c) {return c && native_event(c->mesa);}
void *bkg_get_proc(const char *name) {
    if(!loaded || !name)return NULL;
    return lookup(name);
}

int bkg_poll_event(bkg_context *c, void *event) {return c && native_poll(c->mesa,event)>0;}
