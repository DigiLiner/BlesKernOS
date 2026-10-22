/* Mesa 3.5's native raster backend through the Linux i386 device ABI. */
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include "../../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
typedef uint8_t boolean;
typedef struct {uint16_t w,h;uint32_t format,flags;} desc;
typedef struct {int32_t x,y,w,h;} rect;
static int device=-1;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int profile;
static uint64_t io_ns[15];static unsigned io_calls[15];
static uint64_t clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec;}
static void open_device(void){device=open("/dev/blesgfx",O_RDWR|O_CLOEXEC);profile=getenv("BKG_MESA35_TRACE")!=NULL;}
static int call(bles_gfx_3d_t *a){pthread_once(&once,open_device);unsigned op=a->op;
    uint64_t start=profile?clock_ns():0;
    int result=device>=0?ioctl(device,BLES_GFX_3D,a):-1;
    if(profile && op<15){io_ns[op]+=clock_ns()-start;io_calls[op]++;}return result;}
uint32_t gfx3d_capabilities(void){bles_gfx_3d_t a={BLES_3D_CAPS,{0}};return call(&a)==0?a.p[0]:0;}
boolean gfx3d_available(void){return gfx3d_capabilities()!=0;}
const char *gfx3d_driver_name(void){return "BlesKernOS GFX3D";}
void bk_sys_log(const char *s){
    const char *trace=getenv("BLES_WINE_GL_TRACE");
    if(trace && !strcmp(trace,"1"))fputs(s,stderr);
}
boolean gfx3d_surface_create(const desc *d,uint32_t *out){
    bles_gfx_3d_t a={BLES_3D_CREATE,{d->w,d->h,d->format,d->flags}};
    if(call(&a)<0)return 0;*out=a.p[0];return 1;
}
boolean gfx3d_surface_destroy(uint32_t h){bles_gfx_3d_t a={BLES_3D_DESTROY,{h}};return call(&a)==0;}
static boolean transfer(uint32_t op,uint32_t h,const void *p,uint32_t pitch,const rect *r){
    bles_gfx_3d_t a={op,{h,(uint32_t)(uintptr_t)p,pitch,(uint32_t)(uintptr_t)r}};return call(&a)==0;
}
boolean gfx3d_surface_upload(uint32_t h,const uint32_t *p,uint32_t pitch,const rect *r){return transfer(BLES_3D_UPLOAD,h,p,pitch,r);}
boolean gfx3d_surface_download(uint32_t h,uint32_t *p,uint32_t pitch,const rect *r){return transfer(BLES_3D_DOWNLOAD,h,p,pitch,r);}
boolean gfx3d_depth_upload(uint32_t h,const uint16_t *p,uint32_t pitch,const rect *r){return transfer(BLES_3D_DEPTH_UPLOAD,h,p,pitch,r);}
boolean gfx3d_depth_download(uint32_t h,uint16_t *p,uint32_t pitch,const rect *r){return transfer(BLES_3D_DEPTH_DOWNLOAD,h,p,pitch,r);}
boolean gfx3d_surface_upload_mip(uint32_t h,uint32_t level,const uint32_t *p,uint32_t pitch){
    bles_gfx_3d_t a={BLES_3D_UPLOAD_MIP,{h,(uint32_t)(uintptr_t)p,pitch,level}};return call(&a)==0;
}
boolean gfx3d_surface_upload_region(uint32_t h,const uint32_t *p,uint32_t pitch,uint32_t x,uint32_t y,uint32_t w,uint32_t hh){
    bles_gfx_3d_t a={BLES_3D_UPLOAD_REGION,{h,(uint32_t)(uintptr_t)p,pitch,x,y,w,hh}};return call(&a)==0;
}
boolean gfx3d_begin(uint32_t h,uint32_t color,float depth,uint32_t flags){
    bles_gfx_3d_t a={BLES_3D_BEGIN,{h,color,0,flags}};memcpy(&a.p[2],&depth,4);return call(&a)==0;
}
boolean gfx3d_draw_triangles(uint32_t h,uint32_t texture,const void *v,uint32_t count,uint32_t flags,uint32_t *fence){
    bles_gfx_3d_t a={BLES_3D_DRAW,{h,texture,(uint32_t)(uintptr_t)v,count,flags}};
    if(call(&a)<0)return 0;if(fence)*fence=a.p[5];return 1;
}
boolean gfx3d_end(uint32_t h,uint32_t *fence){bles_gfx_3d_t a={BLES_3D_END,{h}};if(call(&a)<0)return 0;if(fence)*fence=a.p[1];return 1;}
boolean gfx3d_wait_fence(uint32_t f){bles_gfx_3d_t a={BLES_3D_WAIT,{f}};return call(&a)==0;}

/* Native drawable helpers share the rasterizer's descriptor/resource ownership.
 * One native window per process; Wine does not invoke these entry points. */
int bkmesa_native_open(unsigned w,unsigned h,int gpu) {
    pthread_once(&once,open_device);
    bles_gfx_open_t a={w,h,gpu?BLES_GFX_OPEN_GALLIUM:BLES_GFX_OPEN_NATIVE};
    return device>=0 && ioctl(device,BLES_GFX_OPEN,&a)==0;
}
int bkmesa_native_present(unsigned target,const void *pixels,unsigned w,unsigned h) {
    if(target){bles_gfx_3d_t a={BLES_3D_PRESENT,{target}};
        for(unsigned n=0;n<3000;n++){if(call(&a)==0)return 1;if(errno!=EAGAIN && errno!=EINTR)return 0;usleep(1000);}return 0;}
    bles_gfx_present_t a={(uint32_t)(uintptr_t)pixels,w,h};
    return device>=0 && ioctl(device,BLES_GFX_PRESENT,&a)==0;
}
int bkmesa_native_poll(bles_gfx_event_t *event) {
    return device>=0?ioctl(device,BLES_GFX_EVENT,event):0;
}
void bkmesa_native_close(void) {if(device>=0){close(device);device=-1;}
    if(profile)for(unsigned i=1;i<15;i++)if(io_calls[i])fprintf(stderr,"[MESA35:IO] op=%u calls=%u ms=%.3f\n",i,io_calls[i],io_ns[i]/1.e6);
}

int bkmesa_native_unpublish(void) {
    bles_gfx_3d_t a={BLES_3D_PRESENT,{0}};return call(&a)==0;
}
