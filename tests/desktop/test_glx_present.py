#!/usr/bin/env python3
"""Exercise the real swap path: CPU pixels must resolve before X11 reads them."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/wine/graphics/glx.c').read_text()
a=s.index('void glXSwapBuffers(');b=s.index('\nBool glXIsDirect',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stddef.h>
#include <GL/glx.h>
#include <X11/Xutil.h>
#include "GL/osmesa.h"
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8ca6
#undef DefaultVisual
#undef DefaultDepth
#undef DefaultScreen
#undef XDestroyImage
#define DefaultVisual(d,s) NULL
#define DefaultDepth(d,s) 24
#define DefaultScreen(d) 0
struct __GLXcontextRec {
 OSMesaContext mesa; Display *display; GLXDrawable draw,read;
 unsigned width,height; unsigned char *pixels; unsigned swaps;
 uint64_t trace_start,trace_resolve,trace_present,trace_last,trace_copy;
 GC present_gc; Display *gc_display;
};
static GLXContext current;
static int trace_gpu,step,presents,resolve_ok=1,gc_created,gc_freed;
static int offscreen;
static int offscreen_drawable(GLXContext c){(void)c;return offscreen;}
static unsigned char pixels[4];
static XImage image;
static void done(void){assert(step==0);step=1;}
static GLboolean resolve(OSMesaContext c){(void)c;assert(step==0);step=2;pixels[0]=42;return resolve_ok;}
static void (*finish)(void)=done;
static void (*flush)(void)=done;
static void (*getinteger)(GLenum,GLint *);
static GLboolean (*swapcolor)(OSMesaContext);
static int bles_glx_client_rendering(void){return 1;}
static int dimensions(Display *d,GLXDrawable draw,unsigned *w,unsigned *h){
 (void)d;(void)draw;*w=1;*h=1;return 1;
}
Bool glXMakeContextCurrent(Display *d,GLXDrawable draw,GLXDrawable read,GLXContext c){
 (void)d;(void)draw;(void)read;(void)c;assert(0);return False;
}
static GLboolean (*resolvecolor)(OSMesaContext)=resolve;
static void (*getstats)(OSMesaContext,OSMesaBleskStats *);
XImage *XCreateImage(Display *d,Visual *v,unsigned depth,int format,int off,char *data,
                    unsigned w,unsigned h,int pad,int pitch){
 (void)d;(void)v;(void)depth;(void)format;(void)off;(void)w;(void)h;(void)pad;(void)pitch;
 image.data=data;return &image;
}
GC XCreateGC(Display *d,Drawable draw,unsigned long mask,XGCValues *values){
 (void)d;(void)draw;(void)mask;(void)values;++gc_created;return (GC)1;
}
int XPutImage(Display *d,Drawable draw,GC gc,XImage *i,int sx,int sy,int dx,int dy,unsigned w,unsigned h){
 (void)d;(void)draw;(void)gc;(void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;
 assert((step==2 || (!resolvecolor && step==1)) && (unsigned char)i->data[0]==42);++presents;return 0;
}
int XFreeGC(Display *d,GC gc){(void)d;(void)gc;++gc_freed;return 0;}
int XFlush(Display *d){(void)d;return 0;}
static int XDestroyImage(XImage *i){assert(!i->data);return 0;}
''' + s[s.index('static uint64_t trace_time('):s.index('/* Wine explicitly declares offscreen composition.')]+s[s.index('static void present('):b] + r'''
int main(void){
 struct __GLXcontextRec ctx={.display=(Display *)1,.draw=7,.width=1,.height=1,.pixels=pixels};
 current=&ctx;
 glXSwapBuffers(ctx.display,8);assert(step==0 && !presents);
 glXSwapBuffers(ctx.display,7);assert(step==2 && presents==1);
 step=0;resolve_ok=0;glXSwapBuffers(ctx.display,7);assert(presents==1);

 step=0;resolvecolor=NULL;glXSwapBuffers(ctx.display,7);assert(step==1 && presents==2);
 assert(gc_created==1 && !gc_freed);
 assert(bles_glx_copy_front(ctx.display,7,9));assert(gc_created==1 && !gc_freed);
 presentation_gc(&ctx,(Display *)2,9);assert(gc_created==2 && gc_freed==1);
 presentation_gc(&ctx,ctx.display,7);assert(gc_created==3 && gc_freed==2);
 unsigned prior;
 offscreen=1;step=0;prior=presents;glXSwapBuffers(ctx.display,7);assert(presents==prior && step==1);
 assert(bles_glx_copy_front(ctx.display,7,9));assert(presents==prior+1);
 offscreen=0;step=0;glXSwapBuffers(ctx.display,7);assert(presents==prior+2);
 puts("PASS GPU color resolves before XPutImage; failed resolve and unrelated drawable never present stale pixels");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-glx-') as directory:
    c=Path(directory)/'test.c';c.write_text(code);binary=Path(directory)/'test'
    subprocess.run(['gcc','-O2','-Wall','-Werror','-I'+str(root/'libs/Mesa-3.5/include'),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
