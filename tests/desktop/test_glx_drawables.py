#!/usr/bin/env python3
"""Exercise production GLX drawable ownership and independent read bindings."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/wine/graphics/glx.c').read_text()
structs=s[s.index('struct bles_drawable {'):s.index('static __thread GLXContext current;')]
functions=s[s.index('/* contexts_lock protects'):s.index('Bool glXMakeCurrent(')]
functions += s[s.index('void bles_glx_set_offscreen('):s.index('static int offscreen_drawable(')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <GL/glx.h>
#include "GL/osmesa.h"
''' +structs+r'''
static GLXContext current;
static pthread_mutex_t contexts_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned created,destroyed,width=32,geometry_calls;
struct gpu {unsigned color;};
static struct gpu *draw_gpu,*read_gpu;
static int reject,bind_calls;
static void *create_gpu(OSMesaContext c,void *pixels,GLenum t,GLsizei w,GLsizei h){
 (void)c;assert(pixels && t==GL_UNSIGNED_BYTE && w>0 && h>0);created++;return calloc(1,sizeof(struct gpu));
}
static void destroy_gpu(void *p){destroyed++;free(p);}
static GLboolean bind_gpu(OSMesaContext c,void *d,void *r){
 bind_calls++;if(reject)return GL_FALSE;
 assert(!c || (d && r));draw_gpu=d;read_gpu=r;return GL_TRUE;
}
static void store(GLint p,GLint v){assert(p==OSMESA_Y_UP && !v);}
static void *(*create_drawable)(OSMesaContext,void *,GLenum,GLsizei,GLsizei)=create_gpu;
static void (*destroy_drawable)(void *)=destroy_gpu;
static GLboolean (*bind_drawables)(OSMesaContext,void *,void *)=bind_gpu;
static void (*pixelstore)(GLint,GLint)=store;
static void (*detach)(void *,void *,void *);
static GLboolean (*bind)(OSMesaContext,void *,GLenum,GLsizei,GLsizei);
static int dimensions(Display *d,GLXDrawable x,unsigned *w,unsigned *h){
 (void)d;geometry_calls++;*w=width;*h=32;return x>0 && x<100;
}
''' +functions+r'''
int main(void){
 Display *d=(Display *)1;
 struct __GLXcontextRec c={.mesa=(OSMesaContext)1,.doublebuffer=1},other={.mesa=(OSMesaContext)2,.doublebuffer=1};
 assert(glXMakeContextCurrent(d,11,11,&c));assert(geometry_calls==1);draw_gpu->color=0xff0000;
 int bound=bind_calls;assert(glXMakeContextCurrent(d,11,11,&c));assert(bind_calls==bound && draw_gpu->color==0xff0000);
 assert(glXMakeContextCurrent(d,12,12,&c));draw_gpu->color=0x0000ff;
 assert(glXMakeContextCurrent(d,11,12,&c));assert(draw_gpu->color==0xff0000 && read_gpu->color==0xff);
 draw_gpu->color=0x00ff00;
 assert(glXMakeContextCurrent(d,11,11,&other));assert(draw_gpu->color==0x00ff00 && created==2);
 bles_glx_set_offscreen(d,11,True);assert(other.draw_buffer->offscreen);
 bles_glx_set_offscreen((Display *)2,11,False);assert(other.draw_buffer->offscreen);
 bles_glx_set_offscreen(d,11,False);assert(!other.draw_buffer->offscreen);
 struct gpu *old=draw_gpu;reject=1;
 assert(!glXMakeContextCurrent(d,12,12,&c));assert(current==&other && draw_gpu==old);
 reject=0;assert(!glXMakeContextCurrent(d,12,999,&c));assert(current==&other && draw_gpu==old);
 retire_drawable(d,11);assert(destroyed==0 && old->color==0x00ff00);
 assert(glXMakeContextCurrent(d,11,11,&c));assert(draw_gpu->color==0 && created==3 && destroyed==1);
 width=64;assert(glXMakeContextCurrent(d,11,12,&c));assert(c.width==64 && created==5 && destroyed==3);
 retire_drawable(d,11);retire_drawable(d,12);assert(destroyed==3);
 assert(glXMakeContextCurrent(d,0,0,NULL));assert(destroyed==created && !drawables && !current);
 bind_drawables=NULL;assert(!glXMakeContextCurrent(d,11,12,&c));
 puts("PASS: drawable persistence, independent read, cross-context storage, rollback, resize and deferred destruction");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-glx-drawables-') as directory:
 c=Path(directory)/'test.c';c.write_text(code)
 for bits in [[],['-m32']]:
  binary=Path(directory)/('test32' if bits else 'test')
  subprocess.run(['gcc',*bits,'-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-no-pie','-pthread','-I'+str(root/'libs/Mesa-3.5/include'),str(c),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
