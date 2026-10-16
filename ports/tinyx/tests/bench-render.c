/* SPDX-License-Identifier: MIT
 * XRender bilinear throughput, not game FPS or visible refresh rate.
 * Run on the guest; output hashes must match across server builds. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

static unsigned long long micros(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec*1000000+t.tv_nsec/1000;
}
int main(void) {
    setbuf(stdout,NULL);
    setenv("LD_LIBRARY_PATH","/WINEGATE/lib",1);
    pid_t server=fork();
    if(server<0)return 1;
    if(!server){execl("/WINEGATE/bin/XFBDEV","XFBDEV",":30","-ac","-nolock",
                      "-dumbSched","-fp","/WINEGATE/fonts/misc",NULL);_exit(127);}
    Display *d=NULL;
    for(int i=0;i<100 && !d;i++){usleep(50000);d=XOpenDisplay(":30");}
    int result=1;
    if(!d)goto done;
    for(int alpha=0;alpha<2;alpha++) {
        int depth=alpha?32:24;
        XRenderPictFormat *format=XRenderFindStandardFormat(d,alpha?PictStandardARGB32:PictStandardRGB24);
        if(!format)goto close;
        Pixmap src=XCreatePixmap(d,DefaultRootWindow(d),640,480,depth);
        Pixmap dst=XCreatePixmap(d,DefaultRootWindow(d),800,600,32);
        GC gc=XCreateGC(d,src,0,NULL);
        XImage *input=XCreateImage(d,DefaultVisual(d,DefaultScreen(d)),depth,ZPixmap,0,
                                  calloc(640*480,4),640,480,32,640*4);
        if(!input)goto close;
        for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++) {
            unsigned a=alpha?128+(x&127):255;
            XPutPixel(input,x,y,(a<<24)|(((x*a/640)&255)<<16)|(((y*a/480)&255)<<8)|((x+y)*a/1120));
        }
        XPutImage(d,src,gc,input,0,0,0,0,640,480);XDestroyImage(input);
        Picture source=XRenderCreatePicture(d,src,format,0,NULL);
        Picture target=XRenderCreatePicture(d,dst,XRenderFindStandardFormat(d,PictStandardARGB32),0,NULL);
        XTransform transform={{{52429,0,0},{0,52429,0},{0,0,65536}}};
        XRenderSetPictureTransform(d,source,&transform);
        XRenderSetPictureFilter(d,source,FilterBilinear,NULL,0);
        for(unsigned i=0;i<4;i++)XRenderComposite(d,PictOpSrc,source,None,target,0,0,0,0,0,0,800,600);
        XSync(d,False);
        printf("[RENDER-BENCH] BEGIN alpha=%d frames=80 size=800x600\n",alpha);
        unsigned long long begin=micros();
        for(unsigned i=0;i<80;i++) {
            XRenderComposite(d,PictOpSrc,source,None,target,0,0,0,0,0,0,800,600);
            XSync(d,False);
        }
        unsigned long long elapsed=micros()-begin;
        printf("[RENDER-BENCH] END alpha=%d us=%llu\n",alpha,elapsed);
        XImage *output=XGetImage(d,dst,0,0,800,600,AllPlanes,ZPixmap);
        if(!output)goto close;
        uint32_t hash=2166136261U;
        for(unsigned y=0;y<600;y++)for(unsigned x=0;x<800;x++)hash=(hash^(uint32_t)XGetPixel(output,x,y))*16777619U;
        printf("[RENDER-BENCH] HASH alpha=%d value=%08x\n",alpha,hash);
        XDestroyImage(output);XRenderFreePicture(d,source);XRenderFreePicture(d,target);
        XFreeGC(d,gc);XFreePixmap(d,src);XFreePixmap(d,dst);
    }
    result=0;
close:
    XCloseDisplay(d);
done:
    kill(server,SIGTERM);waitpid(server,NULL,0);
    printf("[RENDER-BENCH] %s\n",result?"FAIL":"PASS");
    return result;
}
