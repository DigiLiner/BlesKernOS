#!/usr/bin/env python3
"""Gen2 production allocation, ABI transfers and packet validation (not a GPU emulator)."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/drivers/graphics/intel_gma9xx_dvr.c').read_text()
start=s.index('static bool gma_uncached_range(');end=s.index('\n}',start)+2
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#define TYPES_H
#define PACKED __attribute__((packed))
#include "kernel/include/gfx3d_driver.h"
#define kmemset memset
#define kmemcpy memcpy
#define kprintf printf
#define ALIGN_UP(v,a) (((v)+(a)-1U)&~((a)-1U))
#define GMA_OWNED_BYTES (8U*1024U*1024U)
static struct {bool accel,active;uint8_t *aperture;void *owned_raw;uint32_t owned_offset,fb_offset;
 gfx_info_t mode;struct {unsigned generation;} *chip;} g_gma;
static unsigned map_calls,emits,last_count;static uint32_t last[512];
static bool paging_kernel_mmio_uncached(uint32_t address,uint32_t length){
 assert(length && (address>>22)==((address+length-1)>>22));map_calls++;return true;
}
'''+s[start:end]+'\n'+'\n'.join(line for line in s.splitlines() if line.startswith('#define GMA_') and not line.startswith(('#define GMA_OWNED_BYTES','#define GMA_LOCKED')))+r'''
static bool gma_emit(const uint32_t *words,uint32_t count){
 assert(count && count<=512 && !(count&1U));
 if(count!=2){memcpy(last,words,count*4);last_count=count;emits++;}return true;
}
static bool gma_wait_idle(void){return true;}
static void gma_stop_accel(const char *why){(void)why;g_gma.accel=false;}
static bool gfx3d_register_driver(const gfx3d_driver_ops_t *ops);
'''
# The production prototype is external, so the mock uses external linkage too.
code=code.replace('static bool gfx3d_register_driver(', 'bool gfx3d_register_driver(')
for signature in ['static bool gma_color_blt(', 'static bool gma_copy_blt(']:
 start=s.index(signature);end=s.index('\n}',start)+2
 code+=s[start:end]+'\n'
a=s.index('#define GMA_LOCKED(');b=s.index('GMA_LOCKED(gma_locked_activate',a)
code+=r'''
static unsigned lock_depth,acquires,gma_engine_mutex;
static void kmutex_lock(unsigned *m){(void)m;assert(!lock_depth);lock_depth++;acquires++;}
static void kmutex_unlock(unsigned *m){(void)m;assert(lock_depth==1);lock_depth--;}
static const gfx3d_driver_ops_t *registered;
''' +s[a:b]+'\n'
code+='#include "kernel/drivers/graphics/intel_i830_3d.inc"\n'
code+=r'''
bool gfx3d_register_driver(const gfx3d_driver_ops_t *ops){
 registered=ops;
 assert(ops->depth_upload && ops->depth_download && ops->draw_triangles && ops->selftest);
 assert(!(ops->capabilities&GFX3D_CAP_TEXTURES));return true;
}
int main(void){
 assert(gma_uncached_range(0xe7800000U,8U*1024*1024) && map_calls==2);
 assert(gma_uncached_range(0xe03ff000U,8192) && map_calls==4);
 assert(!gma_uncached_range(0xfffff000U,8192));
 g_gma.mode.bpp=32;
 assert(gma_color_blt(4096,128,1,2,3,4,0xff123456));
 assert(last[0]==0x54300004U && last[1]==0x03cc0080U && last[4]==4096 && last[5]==0xff123456U);
 assert(gma_copy_blt(4096,128,1,2,3,4,5,6));assert(last[0]==0x54f00006U && last[6]==128 && last[7]==4096);
 g_gma.mode.bpp=16;assert(gma_color_blt(4096,64,0,0,8,8,0x1234));
 assert(last[0]==0x54000004U && last[1]==0x01cc0040U);
 g_gma.mode.bpp=32;
 g_gma.accel=g_gma.active=true;g_gma.aperture=calloc(1,GMA_OWNED_BYTES);assert(g_gma.aperture);
 g_gma.owned_raw=g_gma.aperture;
 gfx3d_surface_desc_t d={31,17,GFX3D_FORMAT_ARGB8888,GFX3D_SURFACE_RENDER_TARGET};
 uint32_t h;assert(gma3_create(&d,&h));gma3_surface *surface=gma3_get(h);
 assert(surface && surface->offset>=18U*4096 && surface->depth+surface->zpitch*d.height<=GMA_OWNED_BYTES);
 uint32_t pixels[37*17],copy[37*17];
 for(unsigned i=0;i<37*17;i++)pixels[i]=0xff000000U+i;
 memset(copy,0,sizeof copy);assert(gma3_upload(h,pixels,37,NULL));assert(gma3_download(h,copy,37,NULL));
 for(unsigned y=0;y<17;y++)for(unsigned x=0;x<31;x++)assert(copy[y*37+x]==pixels[y*37+x]);
 uint16_t z[37*17],zc[37*17];for(unsigned i=0;i<37*17;i++)z[i]=(uint16_t)i;
 memset(zc,0,sizeof zc);assert(gma3_depth_upload(h,z,37,NULL));assert(gma3_depth_download(h,zc,37,NULL));
 for(unsigned y=0;y<17;y++)for(unsigned x=0;x<31;x++)assert(zc[y*37+x]==z[y*37+x]);
 assert(!gma3_depth_download(h,zc,30,NULL));
 gfx_rect_t invalid={1,0,31,17};assert(!gma3_upload(h,pixels,37,&invalid));
 gfx3d_vertex_t v[3]={{2,2,.5f,1,0xff123456,0,0},{20,2,.5f,1,0xff123456,0,0},{2,15,.5f,1,0xff123456,0,0}};
 assert(gma3_draw(h,0,v,3,GFX3D_DRAW_DEPTH_TEST|GFX3D_DRAW_DEPTH_WRITE,NULL));
 unsigned prim=0,buffers=0;
 for(unsigned i=0;i<last_count;i++){
  if(last[i]==0x7f00000bU){prim++;assert(last[i+4]==0xff123456U);}
  if(last[i]==0x7d8e0001U){buffers++;assert(last[i+2]==(buffers==1?surface->offset:surface->depth));}
 }
 assert(prim==1 && buffers==2);
 gfx3d_vertex_t many[99];for(unsigned i=0;i<99;i++)many[i]=v[i%3];
 unsigned batches=emits;assert(gma3_draw(h,0,many,99,0,NULL) && emits==batches+2);
 unsigned before=emits;
 assert(!gma3_draw(h,1,v,3,0,NULL));assert(!gma3_draw(h,0,v,2,0,NULL));
 v[2].x=NAN;assert(!gma3_draw(h,0,v,3,0,NULL));v[2].x=2;v[2].z=INFINITY;
 assert(!gma3_draw(h,0,v,3,0,NULL));assert(emits==before);
 uint32_t old=h;assert(gma3_destroy(h));assert(gma3_create(&d,&h) && h!=old && !gma3_get(old));assert(gma3_destroy(h));
 uint32_t handles[64];for(unsigned i=0;i<64;i++)assert(gma3_create(&d,&handles[i]));
 h=99;assert(!gma3_create(&d,&h) && !h);
 for(unsigned i=0;i<64;i++)assert(gma3_destroy(handles[i]));
 for(unsigned i=0;i<GMA3_PAGES/32;i++)assert(!gma3_pages[i]);
 d.width=65535;assert(!gma3_create(&d,&h));assert(gma3_register());
 unsigned locked_before=acquires;assert(!registered->surface_create(&d,&h));
 assert(acquires==locked_before+1 && !lock_depth);
 d.width=31;assert(registered->surface_create(&d,&h));assert(registered->surface_destroy(h));
 assert(acquires==locked_before+3 && !lock_depth);free(g_gma.aperture);
 puts("PASS production Gen2 MMIO chunking, surface bounds/exhaustion/reuse, color/depth pitches, command headers and reject-before-submit; hardware rasterization remains untested");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-intel-gen2-') as folder:
 path=Path(folder)/'test.c';path.write_text(code)
 for bits in [[],['-m32']]:
  binary=Path(folder)/('test32' if bits else 'test')
  subprocess.run(['gcc',*bits,'-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-no-pie','-I'+str(root),'-I'+str(root/'kernel/drivers/graphics'),str(path),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
