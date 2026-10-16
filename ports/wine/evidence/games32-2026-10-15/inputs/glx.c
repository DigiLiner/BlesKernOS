/* Local GLX-compatible drawable backend for TinyX, which has no GLX server.
 * Rendering uses the system's Mesa/GFX3D implementation; X11 only presents
 * pixels. This does not invent shader or Direct3D capabilities. */
#define _GNU_SOURCE
#include <GL/glx.h>
#include <GL/glxext.h>
#include "blesmesa.h"
#include "../../../libs/Mesa-3.5/include/GL/osmesa.h"
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"

struct __GLXFBConfigRec {int doublebuffer;};
static struct __GLXFBConfigRec configs[2]={{0},{1}};
struct __GLXcontextRec {
    OSMesaContext mesa;
    Display *display;
    GLXDrawable draw,read;
    unsigned width,height;
    unsigned char *pixels;
    unsigned swaps;
    uint64_t trace_start, trace_resolve, trace_present, trace_last, trace_copy;
    GC present_gc;
    Display *gc_display;
};
static __thread GLXContext current;
static void *backend;
static int software, trace_gpu, context_flags;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static pthread_mutex_t contexts_lock=PTHREAD_MUTEX_INITIALIZER;
static OSMesaContext (*create)(GLenum,GLint,GLint,GLint,OSMesaContext);
static OSMesaContext (*create_attribs)(const int *,OSMesaContext);
static void (*destroy)(OSMesaContext);
static GLboolean (*bind)(OSMesaContext,void *,GLenum,GLsizei,GLsizei);
static void (*pixelstore)(GLint,GLint);
static void (*finish)(void);
static void (*flush)(void);
static void (*getinteger)(GLenum,GLint *);
static GLboolean (*swapcolor)(OSMesaContext);
static void (*detach)(void *,void *,void *);
static const GLubyte *(*getstring)(GLenum);
static void *(*lookup)(const char *);
static int (*setrenderer)(OSMesaContext,GLint);
static GLboolean (*resolvecolor)(OSMesaContext);
static void (*getstats)(OSMesaContext,OSMesaBleskStats *);
static void initialize(void) {
    trace_gpu=getenv("BLES_WINE_GL_TRACE") && !strcmp(getenv("BLES_WINE_GL_TRACE"),"1");
    software=getenv("BLES_WINE_GL") && !strcmp(getenv("BLES_WINE_GL"),"software");
    const char *selection=getenv("BLES_WINE_GL");
    if(!software && (!selection || !strcmp(selection,"native"))){
        int fd=open("/dev/blesgfx",O_RDWR|O_CLOEXEC);
        bles_virgl_t caps={.abi=BLES_VIRGL_ABI,.op=BLES_VIRGL_CAPS};
        int available=fd>=0 && !ioctl(fd,BLES_GFX_VIRGL,&caps);
        if(fd>=0)close(fd);
        if(available)backend=dlopen("libbkvirgl.so.8",RTLD_NOW|RTLD_LOCAL);
    }
    if(!backend)backend=dlopen(software?"libOSMesa.so.8":
        selection && !strcmp(selection,"virgl")?"libbkvirgl.so.8":"libbkmesa.so.1",RTLD_NOW|RTLD_LOCAL);
    if(!backend)return;
    create_attribs=dlsym(backend,"OSMesaBleskCreateContextAttribs");
    context_flags=create_attribs!=NULL;
    if(!create_attribs)create_attribs=dlsym(backend,"OSMesaCreateContextAttribs");
    create=dlsym(backend,"OSMesaCreateContextExt");destroy=dlsym(backend,"OSMesaDestroyContext");
    bind=dlsym(backend,"OSMesaMakeCurrent");pixelstore=dlsym(backend,"OSMesaPixelStore");
    flush=dlsym(backend,"glFlush");getinteger=dlsym(backend,"glGetIntegerv");
    swapcolor=dlsym(backend,"OSMesaBleskSwapBuffers");
    finish=dlsym(backend,"glFinish");getstring=dlsym(backend,"glGetString");
    lookup=dlsym(backend,"OSMesaGetProcAddress");
    if(!lookup)lookup=dlsym(backend,"_glapi_get_proc_address");
    detach=dlsym(backend,"_mesa_make_current");
    setrenderer=dlsym(backend,"OSMesaBleskSetRenderer");
    resolvecolor=dlsym(backend,"OSMesaBleskResolveColor");
    getstats=dlsym(backend,"OSMesaBleskGetStats");
}
/* Checked by Wine's local backend patch instead of asking TinyX for GLX. */
int bles_glx_client_rendering(void){pthread_once(&once,initialize);return create && destroy && bind && finish && getstring;}
const GLubyte *glGetString(GLenum name){return bles_glx_client_rendering()?getstring(name):NULL;}
XVisualInfo *glXChooseVisual(Display *d,int screen,int *attributes) {
    (void)attributes;
    XVisualInfo match={0};int count;
    match.visualid=XVisualIDFromVisual(DefaultVisual(d,screen));match.screen=screen;
    return XGetVisualInfo(d,VisualIDMask|VisualScreenMask,&match,&count);
}
XVisualInfo *glXGetVisualFromFBConfig(Display *d,GLXFBConfig config){(void)config;return glXChooseVisual(d,DefaultScreen(d),NULL);}
GLXFBConfig *glXGetFBConfigs(Display *d,int screen,int *count){
    (void)d;(void)screen;
    GLXFBConfig *list=malloc(sizeof(*list)*2);*count=list?2:0;
    if(list){list[0]=&configs[0];list[1]=&configs[1];}return list;
}
int glXGetFBConfigAttrib(Display *d,GLXFBConfig config,int attribute,int *value){
    if(config!=&configs[0] && config!=&configs[1])return GLX_BAD_VISUAL;
    *value=0;
    switch(attribute){
    case GLX_FBCONFIG_ID:*value=config->doublebuffer+1;break;
    case GLX_VISUAL_ID:*value=XVisualIDFromVisual(DefaultVisual(d,DefaultScreen(d)));break;
    case GLX_BUFFER_SIZE:*value=DefaultDepth(d,DefaultScreen(d));break;
    case GLX_RENDER_TYPE:*value=GLX_RGBA_BIT;break;
    case GLX_DRAWABLE_TYPE:*value=GLX_WINDOW_BIT|GLX_PIXMAP_BIT|GLX_PBUFFER_BIT;break;
    case GLX_X_RENDERABLE:case GLX_USE_GL:case GLX_RGBA:*value=True;break;
    case GLX_DOUBLEBUFFER:*value=config->doublebuffer;break;
    case GLX_RED_SIZE:case GLX_GREEN_SIZE:case GLX_BLUE_SIZE:case GLX_ALPHA_SIZE:*value=8;break;
    case GLX_DEPTH_SIZE:*value=16;break;
    case GLX_STENCIL_SIZE:*value=8;break;
    case GLX_X_VISUAL_TYPE:*value=GLX_TRUE_COLOR;break;
    case GLX_CONFIG_CAVEAT:case GLX_TRANSPARENT_TYPE:*value=GLX_NONE;break;
    case GLX_MAX_PBUFFER_WIDTH:case GLX_MAX_PBUFFER_HEIGHT:*value=2048;break;
    case GLX_MAX_PBUFFER_PIXELS:*value=2048*2048;break;
    case GLX_LEVEL:case GLX_STEREO:case GLX_AUX_BUFFERS:
    case GLX_ACCUM_RED_SIZE:case GLX_ACCUM_GREEN_SIZE:case GLX_ACCUM_BLUE_SIZE:case GLX_ACCUM_ALPHA_SIZE:
    case GLX_SAMPLE_BUFFERS_ARB:case GLX_SAMPLES_ARB:break;
    default:return GLX_BAD_ATTRIBUTE;
    }return 0;
}
int glXGetConfig(Display *d,XVisualInfo *visual,int attribute,int *value){(void)visual;return glXGetFBConfigAttrib(d,&configs[1],attribute,value);}
GLXFBConfig *glXChooseFBConfig(Display *d,int screen,const int *attributes,int *count){
    GLXFBConfig *all=glXGetFBConfigs(d,screen,count);int found=0;
    for(int i=0;i<*count;i++){
        int valid=1;
        for(const int *p=attributes;p && *p;p+=2){
            int actual;
            if(glXGetFBConfigAttrib(d,all[i],p[0],&actual)){valid=0;break;}
            if(p[1]==(int)GLX_DONT_CARE)continue;
            if(p[0]==GLX_DRAWABLE_TYPE || p[0]==GLX_RENDER_TYPE)valid=(actual&p[1])==p[1];
            else if(p[0]==GLX_RED_SIZE || p[0]==GLX_GREEN_SIZE || p[0]==GLX_BLUE_SIZE ||
                    p[0]==GLX_ALPHA_SIZE || p[0]==GLX_DEPTH_SIZE || p[0]==GLX_STENCIL_SIZE)valid=actual>=p[1];
            else valid=actual==p[1];
            if(!valid)break;
        }
        if(valid)all[found++]=all[i];
    }
    *count=found;if(!found){free(all);return NULL;}return all;
}
GLXContext glXCreateContextAttribsARB(Display *,GLXFBConfig,GLXContext,Bool,const int *);
GLXContext glXCreateContext(Display *d,XVisualInfo *v,GLXContext share,Bool direct){
    (void)d;(void)v;(void)direct;
    if(!bles_glx_client_rendering())return NULL;
    if(swapcolor)return glXCreateContextAttribsARB(d,&configs[1],share,direct,NULL);
    GLXContext c=calloc(1,sizeof(*c));if(!c)return NULL;
    pthread_mutex_lock(&contexts_lock);
    c->mesa=create(OSMESA_BGRA,16,8,0,share?share->mesa:NULL);
    pthread_mutex_unlock(&contexts_lock);
    if(!c->mesa){free(c);return NULL;}
    if(setrenderer)setrenderer(c->mesa,0); /* native GPU with correct CPU fallback */
    return c;
}
GLXContext glXCreateNewContext(Display *d,GLXFBConfig config,int type,GLXContext share,Bool direct){
    if(type!=GLX_RGBA_TYPE)return NULL;
    if(bles_glx_client_rendering() && swapcolor)return glXCreateContextAttribsARB(d,config,share,direct,NULL);
    return glXCreateContext(d,NULL,share,direct);
}
/* Stable OSMesa attribute ABI, including newer versions than the legacy
 * Mesa header retained for OSMesaBleskStats. Gallium validates the requested
 * GL version/profile against the real host GPU capabilities. */
GLXContext glXCreateContextAttribsARB(Display *d,GLXFBConfig config,GLXContext share,
                                    Bool direct,const int *attributes){
    (void)d;(void)direct;
    if(!bles_glx_client_rendering() || !create_attribs ||
       (config!=&configs[0] && config!=&configs[1]))return NULL;
    int major=1,minor=0,profile=0x35,flags=0;
    for(const int *p=attributes;p && *p;p+=2){
        switch(p[0]){
        case GLX_CONTEXT_MAJOR_VERSION_ARB:major=p[1];break;
        case GLX_CONTEXT_MINOR_VERSION_ARB:minor=p[1];break;
        case GLX_CONTEXT_PROFILE_MASK_ARB:
            if(p[1]==GLX_CONTEXT_CORE_PROFILE_BIT_ARB)profile=0x34;
            else if(p[1]==GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB)profile=0x35;
            else return NULL;
            break;
        case GLX_CONTEXT_FLAGS_ARB:
            flags=p[1];
            if((flags & ~3) || (flags && !context_flags))return NULL;
            break;
        default:return NULL;
        }
    }
    if(major<1 || minor<0)return NULL;
    const int attrs[]={0x22,OSMESA_BGRA,0x30,16,0x31,8,
                       0x33,profile,0x36,major,0x37,minor,
                       context_flags?OSMESA_BLES_CONTEXT_FLAGS:0,flags,
                       swapcolor?OSMESA_BLES_DOUBLE_BUFFER:0,config->doublebuffer,0};
    GLXContext c=calloc(1,sizeof(*c));if(!c)return NULL;
    pthread_mutex_lock(&contexts_lock);
    c->mesa=create_attribs(attrs,share?share->mesa:NULL);
    pthread_mutex_unlock(&contexts_lock);
    if(!c->mesa){free(c);return NULL;}return c;
}
static const char *context_extensions(void){
    pthread_once(&once,initialize);
    return create_attribs?
        "GLX_ARB_get_proc_address GLX_ARB_create_context GLX_ARB_create_context_profile":
        "GLX_ARB_get_proc_address";
}
static int dimensions(Display *d,GLXDrawable drawable,unsigned *w,unsigned *h){
    Window root;int x,y;unsigned border,depth;
    return XGetGeometry(d,drawable,&root,&x,&y,w,h,&border,&depth) && *w && *h && *w<=2048 && *h<=2048;
}
Bool glXMakeContextCurrent(Display *d,GLXDrawable draw,GLXDrawable read,GLXContext c){
    if(!c){if(detach)detach(NULL,NULL,NULL);else if(bind)bind(NULL,NULL,GL_UNSIGNED_BYTE,0,0);current=NULL;return True;}
    unsigned w,h;if(!draw || !dimensions(d,draw,&w,&h))return False;
    if(c->width!=w || c->height!=h){
        unsigned char *p=calloc(w*h,4);if(!p)return False;
        if(!bind(c->mesa,p,GL_UNSIGNED_BYTE,w,h)){free(p);return False;}
        free(c->pixels);c->pixels=p;c->width=w;c->height=h;
    }else if(!bind(c->mesa,c->pixels,GL_UNSIGNED_BYTE,w,h))return False;
    pixelstore(OSMESA_Y_UP,0);current=c;c->display=d;c->draw=draw;c->read=read;return True;
}
Bool glXMakeCurrent(Display *d,GLXDrawable draw,GLXContext c){return glXMakeContextCurrent(d,draw,draw,c);}
GLXContext glXGetCurrentContext(void){return current;}
GLXDrawable glXGetCurrentDrawable(void){return current?current->draw:0;}
GLXDrawable glXGetCurrentReadDrawable(void){return current?current->read:0;}
Display *glXGetCurrentDisplay(void){return current?current->display:NULL;}
void glXDestroyContext(Display *d,GLXContext c){if(!c)return;if(current==c)glXMakeCurrent(d,0,NULL);pthread_mutex_lock(&contexts_lock);destroy(c->mesa);pthread_mutex_unlock(&contexts_lock);if(c->present_gc)XFreeGC(c->gc_display,c->present_gc);free(c->pixels);free(c);}
static uint64_t trace_time(void){
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000U+t.tv_nsec/1000U;
}
static GC presentation_gc(GLXContext c,Display *d,Drawable drawable){
    /* All presentation images use this display's default visual/depth.
     * One GC works for both windows and pixmaps of that depth, including
     * replacement front pixmaps and resized/rebound drawables. */
    if(c->present_gc && c->gc_display!=d){
        XFreeGC(c->gc_display,c->present_gc);c->present_gc=NULL;
    }
    if(!c->present_gc){c->present_gc=XCreateGC(d,drawable,0,NULL);c->gc_display=d;}
    return c->present_gc;
}
static void present(Display *d,GLXDrawable draw,int swap){
    GLXContext c=current;if(!c || c->draw!=draw || c->display!=d)return;
    /* X windows can resize while their context remains current. Native GLX
     * updates drawable storage automatically; OSMesa needs an explicit rebind.
     * Contents across a resize are undefined, but subsequent draws must use
     * the new extent and Wine's compositor must see the same dimensions. */
    unsigned w,h;
    if(!dimensions(d,draw,&w,&h))return;
    if(w!=c->width || h!=c->height){
        if(!glXMakeContextCurrent(d,draw,c->read,c))return;
    }
    uint64_t start=trace_gpu?trace_time():0;
    if(trace_gpu && !c->trace_start)c->trace_start=start;
    /* Native resolve flushes queued draws and waits for its readback fence.
     * An extra glFinish would add a redundant host GPU round trip. */
    if(swap && swapcolor){if(!swapcolor(c->mesa))return;}
    else if(resolvecolor){if(!resolvecolor(c->mesa))return;}
    else finish();
    uint64_t resolved=trace_gpu?trace_time():0;
    XImage *image=XCreateImage(d,DefaultVisual(d,DefaultScreen(d)),DefaultDepth(d,DefaultScreen(d)),
                              ZPixmap,0,(char *)c->pixels,c->width,c->height,32,c->width*4);
    if(!image)return;
    GC gc=presentation_gc(c,d,draw);
    if(gc){XPutImage(d,draw,gc,image,0,0,0,0,c->width,c->height);XFlush(d);}
    image->data=NULL;XDestroyImage(image);
    if(trace_gpu){
        uint64_t end=trace_time();
        if(!getstats && (++c->swaps<=8 || c->swaps%120==0))
            fprintf(stderr,"[WINE:GL] present=%u swap=%d drawable=%lu size=%ux%u pixel=%u,%u,%u resolve_us=%llu present_us=%llu\n",
                    c->swaps,swap,(unsigned long)draw,c->width,c->height,
                    c->pixels[0],c->pixels[1],c->pixels[2],
                    (unsigned long long)(resolved-start),(unsigned long long)(end-resolved));
        c->trace_resolve+=resolved-start;c->trace_present+=end-resolved;
        if(end-c->trace_last>=1000000U){
            fprintf(stderr,"[WINE:FRAME-PERF] frames=%u drawable=%lu size=%ux%u elapsed_us=%llu resolve_us=%llu present_us=%llu copy_us=%llu\n",
                c->swaps,(unsigned long)draw,c->width,c->height,
                (unsigned long long)(end-c->trace_start),
                (unsigned long long)c->trace_resolve,(unsigned long long)c->trace_present,
                (unsigned long long)c->trace_copy);
            c->trace_last=end;
        }
        if(getstats && (++c->swaps == 1 || c->swaps % 120 == 0)){
            OSMesaBleskStats stats={0};getstats(c->mesa,&stats);
            fprintf(stderr,"[WINE:GL] swaps=%u gpu_draws=%u triangles=%u texture_uploads=%u uploads=%u downloads=%u fallbacks=%u elapsed_ms=%llu resolve_ms=%llu present_ms=%llu\n",
                    c->swaps,stats.draw_calls,stats.triangles,stats.texture_uploads,stats.uploads,stats.downloads,stats.fallbacks,
                    (unsigned long long)((end-c->trace_start)/1000),
                    (unsigned long long)(c->trace_resolve/1000),
                    (unsigned long long)(c->trace_present/1000));
        }
    }
}

/* Wine's offscreen client windows are hidden on TinyX. Their X window
 * storage is clipped away; copy the resolved GPU front into a persistent
 * pixmap so Wine's existing compositor can scale/clip it correctly. */
Bool bles_glx_copy_front(Display *d,GLXDrawable source,Pixmap target){
    GLXContext c=current;
    uint64_t started=trace_gpu?trace_time():0;
    if(!c || c->display!=d || c->draw!=source || !c->pixels){
        if(trace_gpu){static __thread unsigned misses;
            if(misses++<4)fprintf(stderr,"[WINE:GL] front-copy context mismatch source=%lu current=%lu\n",
                (unsigned long)source,(unsigned long)(c?c->draw:0));}
        return False;
    }
    unsigned w,h;if(!dimensions(d,target,&w,&h) || w!=c->width || h!=c->height){
        if(trace_gpu){static __thread unsigned misses;
            if(misses++<4)fprintf(stderr,"[WINE:GL] front-copy size mismatch target=%lu current=%ux%u\n",
                (unsigned long)target,c->width,c->height);}
        return False;
    }
    if(trace_gpu){static __thread unsigned copies;
        if(++copies<=4 || copies%120==0){
            unsigned nonzero=0;size_t n=(size_t)w*h;
            for(size_t i=0;i<n;i++)nonzero+=!!(c->pixels[i*4]|c->pixels[i*4+1]|c->pixels[i*4+2]);
            fprintf(stderr,"[WINE:GL] front-copy=%u source=%lu target=%lu size=%ux%u colored_pixels=%u\n",
                copies,(unsigned long)source,(unsigned long)target,w,h,nonzero);
        }
    }
    XImage *image=XCreateImage(d,DefaultVisual(d,DefaultScreen(d)),DefaultDepth(d,DefaultScreen(d)),
                              ZPixmap,0,(char *)c->pixels,w,h,32,w*4);
    if(!image)return False;
    GC gc=presentation_gc(c,d,target);
    if(gc){XPutImage(d,target,gc,image,0,0,0,0,w,h);XFlush(d);}
    image->data=NULL;XDestroyImage(image);
    if(trace_gpu)c->trace_copy+=trace_time()-started;
    return gc!=NULL;
}
void glXSwapBuffers(Display *d,GLXDrawable draw){present(d,draw,1);}
/* Front-buffer rendering is visible at flush/finish, including Wine's GDI
 * surface upload path. FBOs and back-buffer draws wait for explicit swap. */
static void present_front(void){
    if(!current || !getinteger)return;
    GLint buffer=0,framebuffer=0;
    if(swapcolor)getinteger(GL_DRAW_FRAMEBUFFER_BINDING,&framebuffer);
    if(framebuffer)return;
    getinteger(GL_DRAW_BUFFER,&buffer);
    if(trace_gpu){static __thread unsigned n;
        if(n++<8)fprintf(stderr,"[WINE:GL] flush drawable=%lu framebuffer=%d buffer=%x\n",(unsigned long)current->draw,framebuffer,buffer);
    }
    if(buffer==GL_FRONT || buffer==GL_FRONT_LEFT || buffer==GL_FRONT_AND_BACK)
        present(current->display,current->draw,0);
}
void glFlush(void){if(bles_glx_client_rendering() && flush){flush();present_front();}}
void glFinish(void){if(bles_glx_client_rendering()){finish();present_front();}}

Bool glXIsDirect(Display *d,GLXContext c){(void)d;return c!=NULL;}
Bool glXQueryVersion(Display *d,int *major,int *minor){(void)d;*major=1;*minor=3;return True;}
const char *glXGetClientString(Display *d,int name){(void)d;return name==GLX_VENDOR?"BlesKernOS":name==GLX_VERSION?"1.3":context_extensions();}
const char *glXQueryServerString(Display *d,int screen,int name){(void)screen;return glXGetClientString(d,name);}
const char *glXQueryExtensionsString(Display *d,int screen){(void)d;(void)screen;return context_extensions();}
GLXWindow glXCreateWindow(Display *d,GLXFBConfig config,Window w,const int *attributes){(void)d;(void)config;(void)attributes;return w;}
void glXDestroyWindow(Display *d,GLXWindow w){(void)d;(void)w;}
GLXPixmap glXCreatePixmap(Display *d,GLXFBConfig config,Pixmap p,const int *attributes){(void)d;(void)config;(void)attributes;return p;}
void glXDestroyPixmap(Display *d,GLXPixmap p){(void)d;(void)p;}
GLXPbuffer glXCreatePbuffer(Display *d,GLXFBConfig config,const int *attributes){
    (void)config;unsigned w=1,h=1;
    for(const int *p=attributes;p && *p;p+=2){if(*p==GLX_PBUFFER_WIDTH)w=p[1];if(*p==GLX_PBUFFER_HEIGHT)h=p[1];}
    if(!w || !h || w>2048 || h>2048)return 0;
    return XCreatePixmap(d,RootWindow(d,DefaultScreen(d)),w,h,DefaultDepth(d,DefaultScreen(d)));
}
void glXDestroyPbuffer(Display *d,GLXPbuffer p){if(p)XFreePixmap(d,p);}
void glXQueryDrawable(Display *d,GLXDrawable draw,int attribute,unsigned *value){
    unsigned w=0,h=0;dimensions(d,draw,&w,&h);
    *value=attribute==GLX_WIDTH?w:attribute==GLX_HEIGHT?h:0;
}
void glXCopyContext(Display *d,GLXContext source,GLXContext dest,unsigned long mask){
    (void)d;
    void (*copy)(void *,void *,unsigned)=dlsym(backend,"_mesa_copy_context");
    if(source && dest && copy)copy(source->mesa,dest->mesa,(unsigned)mask);
}
void glXWaitGL(void){glFinish();}
void glXWaitX(void){if(current)XSync(current->display,False);}
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name){
    if(!name || !bles_glx_client_rendering())return NULL;
    if(!strcmp((const char *)name,"glXCreateContextAttribsARB"))return (__GLXextFuncPtr)glXCreateContextAttribsARB;
    if(!strcmp((const char *)name,"glFlush"))return (__GLXextFuncPtr)glFlush;
    if(!strcmp((const char *)name,"glFinish"))return (__GLXextFuncPtr)glFinish;
    if(!strncmp((const char *)name,"glX",3))return (__GLXextFuncPtr)dlsym(RTLD_DEFAULT,(const char *)name);
    void *p=lookup?lookup((const char *)name):NULL;
    return (__GLXextFuncPtr)(p?p:dlsym(backend,(const char *)name));
}
__GLXextFuncPtr glXGetProcAddress(const GLubyte *name){return glXGetProcAddressARB(name);}
