#!/usr/bin/env python3
"""Native fixed-function publication must enforce descriptor ownership/shape."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linux/i386_gfx3d.inc').read_text();start=s.index('    if(a.op==BLES_3D_PRESENT) {');end=s.index('    int32_t result=bles_gfx_3d_locked',start)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"
#define BLES_3D_PRESENT 14
#define BK_EINVAL 22
#define BK_EIO 5
#define BK_ENOMEM 12
#define GFX3D_FORMAT_ARGB8888 2
#define GFX3D_SURFACE_RENDER_TARGET 1
#define GFX3D_SURFACE_WINDOW 8
#define KDOMAIN_GFX 1
#define GFX3D_SURFACE_TEXTURE 2
struct gfx {bool gallium_window;void *window;uint32_t width,height,gpu_fence;bool gallium_failed;uint32_t gallium_surfaces[3],gallium_readers[3],gallium_states[3],gallium_serial[3],gallium_next,gallium_sequence;struct{uint32_t handle,width,height,flags;}gpu_surfaces[2];};
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct{unsigned short w,h;unsigned format,flags;}gfx3d_surface_desc_t;
static unsigned bles_native_pending_count;
static bool async_mode,capture_ok=true;static int reserve_result=0;
static int domain,clear_calls,bind_calls,end_calls;static bool end_ok=true,bind_ok=true;
static int bles_3d_slot(struct gfx *g,uint32_t h){for(int i=0;i<2;i++)if(h && g->gpu_surfaces[i].handle==h)return i;return -1;}
static void kernel_domains_exit(int d){assert(domain==1);domain=0;}
static void bk_gui_window_clear_gpu_viewport(void *w){assert(!domain);clear_calls++;}
static bool gfx3d_end(uint32_t h,uint32_t *f){assert(domain);end_calls++;*f=3;return end_ok;}
static bool bk_gui_window_content_rect(void *w,gui_rect_t *r){assert(!domain);*r=(gui_rect_t){0,0,640,480};return true;}
static bool bk_gui_window_set_gpu_viewport(void *w,uint32_t h,uint32_t width,uint32_t height,gui_rect_t r){assert(!domain);bind_calls++;return bind_ok;}
static bool gfx3d_virgl(bles_virgl_t *a,void *unused){if(a->op==BLES_VIRGL_CAPS)return async_mode;assert(a->op==BLES_VIRGL_PRESENT && a->p[3]==4 && !a->context);a->p[2]=41;return capture_ok;}
static int bles_native_reserve(struct gfx *gfx){return reserve_result;}
static bool gfx3d_surface_create(const gfx3d_surface_desc_t *d,unsigned *h){*h=99;return true;}
static void bles_native_fail(struct gfx *gfx){gfx->gallium_failed=true;}
static void vision_ipc_notify_native_gpu(void){assert(!domain);}
static int present(struct gfx *gfx,uint32_t target){struct{unsigned op,p[12];}a={BLES_3D_PRESENT,{target}};domain=1;
'''+s[start:end]+r'''
return -99;}
int main(void){
 struct gfx g={.gallium_window=true,.window=(void *)1,.width=640,.height=480,.gpu_surfaces={{7,640,480,2}}};
 assert(present(&g,7)==0 && end_calls==1 && bind_calls==1 && !domain);
 assert(present(&g,8)==-22 && end_calls==1 && bind_calls==1);
 g.gpu_surfaces[0].width=320;assert(present(&g,7)==-22);g.gpu_surfaces[0].width=640;
 g.gpu_surfaces[0].flags=0;assert(present(&g,7)==-22);g.gpu_surfaces[0].flags=2;
 end_ok=false;assert(present(&g,7)==-5 && bind_calls==1);end_ok=true;
 bind_ok=false;assert(present(&g,7)==-5);bind_ok=true;
 assert(present(&g,0)==0 && clear_calls==1);
 g.gallium_window=false;assert(present(&g,7)==-22 && present(&g,0)==-22 && clear_calls==1);
 g.gallium_window=true;async_mode=true;
 assert(present(&g,7)==0 && g.gallium_surfaces[0]==99 && g.gallium_states[0]==1 && g.gallium_readers[0]==41 && bles_native_pending_count==1 && !domain);
 reserve_result=-11;assert(present(&g,7)==-11 && bles_native_pending_count==1);
 reserve_result=0;capture_ok=false;assert(present(&g,7)==-5 && g.gallium_failed && !domain);
 puts("PASS native target ownership, shape/type, failed submission/bind and unpublication");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-mesa-present-') as d:
 c=Path(d)/'test.c';c.write_text(code);binary=Path(d)/'test'
 subprocess.run(['gcc','-O2','-I'+str(root),str(c),'-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
