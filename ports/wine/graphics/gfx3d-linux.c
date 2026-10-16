/* Mesa 3.5's native raster backend through the Linux i386 device ABI. */
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
typedef uint8_t boolean;
typedef struct {uint16_t w,h;uint32_t format,flags;} desc;
typedef struct {int32_t x,y,w,h;} rect;
static int device=-1;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void open_device(void){device=open("/dev/blesgfx",O_RDWR|O_CLOEXEC);}
static int call(bles_gfx_3d_t *a){pthread_once(&once,open_device);return device>=0?ioctl(device,BLES_GFX_3D,a):-1;}
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
