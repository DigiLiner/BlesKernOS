#!/usr/bin/env python3
"""Exercise production ATI upload strips with a deferred fake GPU and VRAM guards."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/graphics/ati_rage128_dvr.c').read_text()
def extract(sig):
 a=s.index(sig);return s[a:s.index('\n}',a)+2]+'\n'
code='''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define TYPES_H
#include "kernel/include/rgb565.h"
'''+ '\n'.join(re.findall(r'^#define R128_[^\n]+',s,re.M))+'\n'
code+='''
typedef struct {uint16_t width,height;} gfx_info_t;
typedef struct {int x,y,w,h;} gfx_rect_t;
#define GFX_ROP_COPY 0
static struct {
 bool active,accel_ok,overlay_active,surface_used,cursor_ready,engine_busy;
 uint8_t bpp;uint16_t width,height,pitch;
 uint32_t fb_offset,vram_size,cursor_offset,stat_present_cpu_bytes,
 stat_present_vram_calls,stat_present_staging_calls;
 uintptr_t fb_bar;
} g_r128;
static uint32_t r128_align_up(uint32_t n,uint32_t a){return (n+a-1)&~(a-1);}
'''
for sig in ['static uint32_t r128_bytes_per_pixel(', 'static uint32_t r128_pitch_units(', 'static uint32_t r128_visible_end(', 'static bool r128_surface_layout(', 'static bool r128_staging_tile_layout(', 'static bool r128_clip_rect(']:code+=extract(sig)
code+='''
static struct {bool valid;uint32_t offset;int sx,sy,dx,dy,w,h;} pending;
static unsigned submits,waits,fail_at;
static bool r128_wait_idle(void){
 waits++;
 if(pending.valid){
  uint8_t *base=(uint8_t*)g_r128.fb_bar;uint32_t bytes=r128_bytes_per_pixel();
  for(int y=0;y<pending.h;y++)memcpy(base+g_r128.fb_offset+(pending.dy+y)*g_r128.pitch+pending.dx*bytes,
   base+pending.offset+(pending.sy+y)*g_r128.pitch+pending.sx*bytes,pending.w*bytes);
  pending.valid=false;
 }
 g_r128.engine_busy=false;return true;
}
static void r128_copy_dwords_to_vram(volatile uint32_t *d,const uint32_t *p,uint32_t n){
 assert(!pending.valid);memcpy((void*)d,p,n*4);
}
static bool r128_emit_bitblt(const gfx_info_t *info,uint32_t po,int sx,int sy,int dx,int dy,int w,int h,int rop,bool same){
 (void)info;(void)rop;assert(!same && !pending.valid);submits++;
 if(submits==fail_at)return false;
 pending.valid=true;pending.offset=(po&0x1fffffU)<<5;pending.sx=sx;pending.sy=sy;
 pending.dx=dx;pending.dy=dy;pending.w=w;pending.h=h;g_r128.engine_busy=true;return true;
}
'''
code+=extract('static bool r128_present_tiles(')
code+=extract('static uint32_t r128_bridge_reserved_vram_end(')
code+='''
int main(void){
 uint8_t *vram=malloc(8*1024*1024+32);assert(vram);memset(vram,0x5a,8*1024*1024+32);
 g_r128.active=true;g_r128.accel_ok=true;g_r128.bpp=32;
 g_r128.width=1400;g_r128.height=1050;g_r128.pitch=5600;g_r128.fb_offset=4096;
 g_r128.vram_size=8*1024*1024;g_r128.cursor_ready=true;g_r128.cursor_offset=g_r128.vram_size-1024;
 g_r128.fb_bar=(uintptr_t)vram;
 uint32_t offset,rows;assert(r128_staging_tile_layout(&offset,&rows));assert(rows==128);
 assert(offset==4096+5600*1050);assert(offset+rows*5600<g_r128.cursor_offset);
 assert(r128_bridge_reserved_vram_end()==offset+rows*5600);
 uint32_t *src=malloc(1431*1050*4);assert(src);
 for(unsigned i=0;i<1431*1050;i++)src[i]=i*37013U;
 gfx_info_t info={1400,1050};gfx_rect_t full={0,0,1400,1050};
 assert(r128_present_tiles(&info,src,1431,&full,1));assert(submits==9 && waits==8);
 r128_wait_idle();uint32_t *scanout=(uint32_t*)(vram+4096);
 for(unsigned y=0;y<1050;y++)for(unsigned x=0;x<1400;x++)assert(scanout[y*1400+x]==src[y*1431+x]);
 assert(g_r128.stat_present_cpu_bytes==5600*1050);
 for(unsigned i=g_r128.cursor_offset;i<g_r128.vram_size+32;i++)assert(vram[i]==0x5a);
 for(unsigned i=0;i<4096;i++)assert(vram[i]==0x5a);
 gfx_rect_t edges[]={{-17,-8,100,150},{1390,1040,300,500}};
 assert(r128_present_tiles(&info,src,1431,edges,2));r128_wait_idle();
 fail_at=submits+2;assert(!r128_present_tiles(&info,src,1431,&full,1));r128_wait_idle();
 g_r128.overlay_active=true;assert(!r128_staging_tile_layout(NULL,NULL));g_r128.overlay_active=false;
 g_r128.bpp=8;assert(!r128_staging_tile_layout(NULL,NULL));g_r128.bpp=32;
 g_r128.cursor_offset=offset+5599;assert(!r128_staging_tile_layout(NULL,NULL));
 g_r128.cursor_offset=offset+5600;assert(r128_staging_tile_layout(NULL,&rows)&&rows==1);
 g_r128.cursor_offset=g_r128.vram_size-1024;g_r128.bpp=16;g_r128.pitch=2816;fail_at=0;
 memset(vram,0x5a,g_r128.vram_size+32);g_r128.stat_present_cpu_bytes=0;
 assert(r128_staging_tile_layout(&offset,&rows));assert(rows==128);
 assert(r128_present_tiles(&info,src,1431,&full,1));r128_wait_idle();
 for(unsigned y=0;y<1050;y++) {
  uint16_t *line=(uint16_t*)(vram+4096+y*2816);
  for(unsigned x=0;x<1400;x++)assert(line[x]==bk_rgb565_encode(src[y*1431+x]));
  for(unsigned x=2800;x<2816;x++)assert(vram[4096+y*2816+x]==0x5a);
 }
 assert(g_r128.stat_present_cpu_bytes==1400*1050*2);
 gfx_rect_t odd={1,3,1397,271};assert(r128_present_tiles(&info,src,1431,&odd,1));r128_wait_idle();
 for(unsigned i=g_r128.cursor_offset;i<g_r128.vram_size+32;i++)assert(vram[i]==0x5a);
 free(src);free(vram);puts("PASS: 8 MiB SXGA strips, deferred-GPU barriers, source pitch, clipping, cursor guards, failure and one-row limit");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-tiles-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-I'+str(ROOT),'-std=c11','-Wall','-Wextra','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
