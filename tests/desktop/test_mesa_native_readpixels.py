#!/usr/bin/env python3
"""Explicit GL readback resolves data without marking software rendering."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'libs/Mesa-3.5/src/OSmesa/osmesa.c').read_text();start=s.index('static void native_read_pixels(');end=s.index('\n#endif',start)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int GLint,GLsizei;typedef unsigned GLenum;typedef void GLvoid;typedef bool GLboolean;
#define GL_DEPTH_COMPONENT 0x1902
#define GL_OUT_OF_MEMORY 0x505
struct gl_pixelstore_attrib {int unused;};
typedef struct context {int unused;}GLcontext;
struct swrast_device_driver {void (*SpanRenderStart)(GLcontext *);};
static void original(GLcontext *c){assert(0);}
static struct swrast_device_driver driver={original};
static bool resolve_ok=true;static int colors,depths,reads,errors;
static bool bkmesa_gpu_resolve(GLcontext *c){depths++;return resolve_ok;}
static bool bkmesa_gpu_resolve_color(GLcontext *c){colors++;return resolve_ok;}
static void _mesa_error(GLcontext *c,unsigned code,const char *message){assert(code==GL_OUT_OF_MEMORY);errors++;}
static struct swrast_device_driver *_swrast_GetDeviceDriverReference(GLcontext *c){return &driver;}
static void _swrast_ReadPixels(GLcontext *ctx,int x,int y,int w,int h,unsigned format,unsigned type,const struct gl_pixelstore_attrib *pack,void *pixels){assert(!driver.SpanRenderStart);reads++;}
'''+s[start:end]+r'''
int main(void){GLcontext ctx={0};
 native_read_pixels(&ctx,0,0,1,1,0x1907,0,NULL,NULL);
 assert(colors==1 && !depths && reads==1 && driver.SpanRenderStart==original);
 native_read_pixels(&ctx,0,0,1,1,GL_DEPTH_COMPONENT,0,NULL,NULL);
 assert(depths==1 && reads==2 && driver.SpanRenderStart==original);
 resolve_ok=false;native_read_pixels(&ctx,0,0,1,1,0x1907,0,NULL,NULL);
 assert(errors==1 && reads==2 && driver.SpanRenderStart==original);
 puts("PASS explicit color/depth readback, strict-mode span isolation, failure and callback restoration");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-readpixels-') as d:
 c=Path(d)/'test.c';c.write_text(code);binary=Path(d)/'test'
 subprocess.run(['gcc','-O2',str(c),'-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
