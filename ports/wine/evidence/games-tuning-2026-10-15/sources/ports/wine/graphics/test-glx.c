/* SPDX-License-Identifier: MIT
 * Guest integration test: local GLX -> GPU -> TinyX, front flush/back swap. */
#define GL_GLEXT_PROTOTYPES
#include <GL/glx.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/wait.h>
extern Bool bles_glx_copy_front(Display *,GLXDrawable,Pixmap);
extern void bles_glx_set_offscreen(Display *,GLXDrawable,Bool);
static void (*clear_color)(GLfloat,GLfloat,GLfloat,GLfloat);
static void (*clear)(GLbitfield);
static void (*draw_buffer)(GLenum);
static void (*finish)(void);
static unsigned long pixel(Display *d,Window w){
    XSync(d,False);XImage *image=XGetImage(d,w,0,0,1,1,AllPlanes,ZPixmap);
    if(!image)return ~0UL;
    unsigned long p=XGetPixel(image,0,0);XDestroyImage(image);return p&0xffffff;
}
int main(void){
    setbuf(stdout,NULL);setbuf(stderr,NULL);
    setenv("LD_LIBRARY_PATH","/WINEGATE/lib",1);setenv("BLES_WINE_GL","virgl",1);
    void *gpu=dlopen("libbkvirgl.so.8",RTLD_NOW|RTLD_LOCAL);
    if(!gpu){fprintf(stderr,"[GLX-PROBE] GPU load FAILED: %s\n",dlerror());return 10;}
    dlclose(gpu);
    pid_t server=fork();
    if(server<0){perror("fork");return 1;}
    if(!server){execl("/WINEGATE/bin/XFBDEV","XFBDEV",":30","-ac","-nolock","-dumbSched","-fp","/WINEGATE/fonts/misc",NULL);_exit(127);}
    Display *d=NULL;for(int i=0;i<100&&!d;i++){usleep(50000);d=XOpenDisplay(":30");}
    int result=1;
    if(!d)goto done;
    clear_color=(void *)glXGetProcAddress((const GLubyte *)"glClearColor");
    clear=(void *)glXGetProcAddress((const GLubyte *)"glClear");
    draw_buffer=(void *)glXGetProcAddress((const GLubyte *)"glDrawBuffer");
    finish=(void *)glXGetProcAddress((const GLubyte *)"glFinish");
    if(!clear_color || !clear || !draw_buffer || !finish)goto close;
    int count=0;GLXFBConfig *configs=glXGetFBConfigs(d,DefaultScreen(d),&count);
    if(!configs || count!=2)goto close;
    Window w=XCreateSimpleWindow(d,DefaultRootWindow(d),0,0,32,32,0,0,0);
    if(!glXCreateWindow(d,configs[1],w,NULL))goto close;
    XMapWindow(d,w);XSync(d,False);
    GLXContext c=glXCreateNewContext(d,configs[1],GLX_RGBA_TYPE,NULL,True);free(configs);
    if(!c || !glXMakeCurrent(d,w,c))goto close;
    draw_buffer(GL_FRONT);clear_color(1,0,0,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long red=pixel(d,w);
    draw_buffer(GL_BACK);clear_color(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long held=pixel(d,w);
    glXSwapBuffers(d,w);unsigned long blue=pixel(d,w);
    printf("[GLX-PROBE] front=%06lx held=%06lx swap=%06lx\n",red,held,blue);
    result=red==0xff0000 && held==red && blue==0x0000ff?0:2;
    XUnmapWindow(d,w);XSync(d,False);
    draw_buffer(GL_FRONT);clear_color(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);finish();
    Pixmap backing=XCreatePixmap(d,DefaultRootWindow(d),32,32,DefaultDepth(d,DefaultScreen(d)));
    Bool copied=bles_glx_copy_front(d,w,backing);
    unsigned long green=pixel(d,backing);
    printf("[GLX-PROBE] hidden front copied=%d pixel=%06lx\n",copied,green);
    if(!copied || green!=0x00ff00)result=3;
    XFreePixmap(d,backing);
    /* Wine can resize a hidden client without rebinding its GL context. */
    XResizeWindow(d,w,64,48);XSync(d,False);
    glXSwapBuffers(d,w); /* resized storage has undefined initial contents */
    draw_buffer(GL_FRONT);clear_color(1,1,0,1);clear(GL_COLOR_BUFFER_BIT);finish();
    backing=XCreatePixmap(d,DefaultRootWindow(d),64,48,DefaultDepth(d,DefaultScreen(d)));
    copied=bles_glx_copy_front(d,w,backing);
    XSync(d,False);
    XImage *resized=XGetImage(d,backing,0,0,64,48,AllPlanes,ZPixmap);
    unsigned long corner=resized?XGetPixel(resized,63,47)&0xffffff:~0UL;
    if(resized)XDestroyImage(resized);
    printf("[GLX-PROBE] resized current drawable copied=%d corner=%06lx\n",copied,corner);
    if(!copied || corner!=0xffff00)result=4;
    XFreePixmap(d,backing);
    /* A formerly hidden Wine client must resume normal window uploads. */
    XMapWindow(d,w);XSync(d,False);
    clear_color(1,0,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long remapped=pixel(d,w);
    printf("[GLX-PROBE] remapped front=%06lx\n",remapped);
    if(remapped!=0xff00ff)result=5;
    /* Offscreen composition can use a mapped client under Wine's dummy parent. */
    bles_glx_set_offscreen(d,w,True);clear_color(0,1,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long held_offscreen=pixel(d,w);
    backing=XCreatePixmap(d,DefaultRootWindow(d),64,48,DefaultDepth(d,DefaultScreen(d)));
    copied=bles_glx_copy_front(d,w,backing);unsigned long composed=pixel(d,backing);XFreePixmap(d,backing);
    bles_glx_set_offscreen(d,w,False);clear_color(1,1,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long resumed=pixel(d,w);
    printf("[GLX-PROBE] explicit offscreen held=%06lx composed=%06lx resumed=%06lx\n",held_offscreen,composed,resumed);
    if(held_offscreen!=0xff00ff || !copied || composed!=0x00ffff || resumed!=0xffffff)result=12;
    /* Wine can also bind a legacy raw X Window without glXCreateWindow. */
    Window legacy=XCreateSimpleWindow(d,DefaultRootWindow(d),70,0,32,32,0,0,0);
    XMapWindow(d,legacy);XSync(d,False);glXMakeCurrent(d,legacy,c);draw_buffer(GL_FRONT);
    clear_color(0,1,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long legacy_color=pixel(d,legacy);
    XUnmapWindow(d,legacy);XSync(d,False);clear_color(1,1,0,1);clear(GL_COLOR_BUFFER_BIT);finish();
    backing=XCreatePixmap(d,DefaultRootWindow(d),32,32,DefaultDepth(d,DefaultScreen(d)));
    copied=bles_glx_copy_front(d,legacy,backing);unsigned long legacy_hidden=pixel(d,backing);
    XFreePixmap(d,backing);XMapWindow(d,legacy);XSync(d,False);
    clear_color(1,0,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long legacy_remap=pixel(d,legacy);
    printf("[GLX-PROBE] legacy window=%06lx hidden=%06lx remap=%06lx\n",legacy_color,legacy_hidden,legacy_remap);
    if(legacy_color!=0x00ffff || !copied || legacy_hidden!=0xffff00 || legacy_remap!=0xff00ff)result=10;
    glXMakeCurrent(d,w,c);XDestroyWindow(d,legacy);
    /* Equal-sized drawables must retain different GPU attachments. */
    int attrs[]={GLX_PBUFFER_WIDTH,32,GLX_PBUFFER_HEIGHT,32,None};
    int n=0;GLXFBConfig *fb=glXGetFBConfigs(d,DefaultScreen(d),&n);
    GLXPbuffer pa=glXCreatePbuffer(d,fb[1],attrs),pb=glXCreatePbuffer(d,fb[1],attrs);
    GLXContext other=glXCreateNewContext(d,fb[1],GLX_RGBA_TYPE,c,True);free(fb);
    void (*read_buffer)(GLenum)=(void *)glXGetProcAddress((const GLubyte *)"glReadBuffer");
    void (*read_pixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void *)=(void *)glXGetProcAddress((const GLubyte *)"glReadPixels");
    GLenum (*get_error)(void)=(void *)glXGetProcAddress((const GLubyte *)"glGetError");
    if(!pa || !pb || !other || !read_buffer || !read_pixels || !get_error)return 6;
    unsigned char sample[4]={0};
    glXMakeCurrent(d,pa,c);draw_buffer(GL_FRONT);read_buffer(GL_FRONT);
    clear_color(1,0,0,1);clear(GL_COLOR_BUFFER_BIT);
    glXMakeCurrent(d,pb,c);draw_buffer(GL_FRONT);read_buffer(GL_FRONT);clear_color(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);
    glXMakeCurrent(d,pa,c);read_buffer(GL_FRONT);read_pixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sample);
    printf("[GLX-PROBE] retained drawable=%u,%u,%u\n",sample[0],sample[1],sample[2]);
    if(sample[0]!=255 || sample[1] || sample[2])result=6;
    /* Draw into A while glReadPixels must still read B. */
    if(!glXMakeContextCurrent(d,pa,pb,c))result=7;
    draw_buffer(GL_FRONT);read_buffer(GL_FRONT);
    clear_color(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);
    read_pixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sample);
    printf("[GLX-PROBE] independent read=%u,%u,%u\n",sample[0],sample[1],sample[2]);
    if(sample[0] || sample[1] || sample[2]!=255)result=7;
    /* A second context observes the same drawable, not a private buffer. */
    if(!glXMakeCurrent(d,pa,other))result=8;
    read_buffer(GL_FRONT);read_pixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sample);
    printf("[GLX-PROBE] second context=%u,%u,%u\n",sample[0],sample[1],sample[2]);
    if(sample[0] || sample[1]!=255 || sample[2] || get_error()!=GL_NO_ERROR)result=8;
    /* Destroy while current: attachments live until the binding is released. */
    glXDestroyPbuffer(d,pa);glXDestroyPbuffer(d,pb);
    read_pixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sample);
    if(sample[1]!=255)result=9;
    glXMakeCurrent(d,None,NULL);
    fb=glXGetFBConfigs(d,DefaultScreen(d),&n);
    Pixmap xp=XCreatePixmap(d,DefaultRootWindow(d),32,32,DefaultDepth(d,DefaultScreen(d)));
    GLXPixmap gp=glXCreatePixmap(d,fb[1],xp,NULL);free(fb);
    if(!gp || !glXMakeCurrent(d,gp,c))result=11;
    draw_buffer(GL_FRONT);clear_color(0,1,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
    unsigned long pixmap_color=pixel(d,xp);
    printf("[GLX-PROBE] GLXPixmap front=%06lx\n",pixmap_color);
    if(pixmap_color!=0x00ffff)result=11;
    glXMakeCurrent(d,None,NULL);glXDestroyPixmap(d,gp);XFreePixmap(d,xp);
    glXDestroyContext(d,other);
    glXDestroyWindow(d,w);
    glXMakeCurrent(d,None,NULL);glXDestroyContext(d,c);XDestroyWindow(d,w);
close:
    XCloseDisplay(d);
done:
    kill(server,15);waitpid(server,NULL,0);
    puts(result?"[GLX-PROBE] FAILED":"[GLX-PROBE] PASS GPU front flush and back swap presented to TinyX");
    return result;
}
