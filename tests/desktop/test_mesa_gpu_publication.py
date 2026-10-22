#!/usr/bin/env python3
"""Exercise production Mesa target publication/coherence and cache lifetime."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2];mesa=root/'libs/Mesa-3.5'
code=r'''
#include <assert.h>
#include <stdio.h>
#include "bkmesa_gpu.c"
static unsigned create_fail,upload_fail,end_fail,creates,destroys,texture_destroys,uploads;
static unsigned last_pixel;
bk_bool gfx3d_available(void){return 1;}
bk_u32 gfx3d_capabilities(void){return BK_CAP_FIXED_FUNCTION|BK_CAP_RENDER_TARGETS|BK_CAP_VERTEX_BUFFERS|BK_CAP_ORDERED_SUBMIT;}
bk_bool gfx3d_surface_create(const bk_surface_desc *d,bk_surface *out){creates++;if(create_fail){create_fail--;return 0;}*out=100+creates;return 1;}
bk_bool gfx3d_surface_destroy(bk_surface h){destroys++;if(h==77)texture_destroys++;return 1;}
bk_bool gfx3d_surface_upload(bk_surface h,const bk_u32 *p,bk_u32 pitch,const bk_rect *r){uploads++;last_pixel=*p;return !upload_fail;}
bk_bool gfx3d_surface_download(bk_surface h,bk_u32 *p,bk_u32 pitch,const bk_rect *r){return 1;}
bk_bool gfx3d_end(bk_surface h,bk_u32 *f){*f=0;return !end_fail;}
bk_bool gfx3d_wait_fence(bk_u32 f){return 1;}
bk_bool gfx3d_draw_triangles(bk_surface h,bk_surface t,const bk_vertex *v,bk_u32 n,bk_u32 flags,bk_u32 *f){*f=0;return 1;}
void bk_sys_log(const char *s){}
int main(void) {
    GLcontext ctx={0};bkmesa_gpu_context gpu={0};unsigned pixels[64]={0xff123456};
    gpu.ctx=&ctx;gpu.requested_renderer=BKMESA_RENDERER_AUTO;contexts=&gpu;
    bkmesa_gpu_bind(&ctx,pixels,8,8,8,OSMESA_BGRA,GL_FALSE);
    assert(gpu.supported && gpu.cpu_dirty);
    unsigned target=bkmesa_gpu_surface(&ctx);
    assert(target && uploads==1 && last_pixel==0xff123456 && !gpu.cpu_dirty);
    /* AUTO ends with a software operation, without another GPU draw. */
    pixels[0]=0xffaabbcc;gpu.cpu_dirty=GL_TRUE;
    assert(bkmesa_gpu_surface(&ctx)==target && uploads==2 && last_pixel==0xffaabbcc);
    upload_fail=1;gpu.cpu_dirty=GL_TRUE;
    assert(!bkmesa_gpu_surface(&ctx) && gpu.cpu_dirty);
    upload_fail=0;gpu.cpu_dirty=GL_FALSE;gpu.draw_open=GL_TRUE;end_fail=1;
    assert(!bkmesa_gpu_surface(&ctx));end_fail=0;
    /* Drawable dimensions change; persistent texture must survive. */
    gpu.textures[0].surface=77;gpu.gpu_dirty=GL_FALSE;
    bkmesa_gpu_bind(&ctx,pixels,4,8,8,OSMESA_BGRA,GL_FALSE);
    assert(gpu.supported && gpu.textures[0].surface==77 && !texture_destroys);
    /* A failed target allocation may evict once and retry. */
    create_fail=1;unsigned before=creates;
    bkmesa_gpu_bind(&ctx,pixels,4,4,8,OSMESA_BGRA,GL_FALSE);
    assert(gpu.supported && creates==before+2 && texture_destroys==1 && !gpu.textures[0].surface);
    contexts=NULL;
    puts("PASS coherent GPU publication, failed upload/end rejection, texture lifetime and VRAM pressure retry");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-mesa-publish-') as d:
    c=Path(d)/'test.c';c.write_text(code);binary=Path(d)/'test'
    includes=['include','src','src/math','src/swrast','src/swrast_setup','src/tnl','src/array_cache','src/BlesKernOS']
    subprocess.run(['gcc','-m32','-O2','-DMESA_BLESKERNOS=1','-ffunction-sections','-fdata-sections',*['-I'+str(mesa/p) for p in includes],str(c),'-Wl,--gc-sections','-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
