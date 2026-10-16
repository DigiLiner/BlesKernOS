#!/usr/bin/env python3
"""Exercise production native image blits against compact buffers and canaries."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(source,name):
 pos=source.index(name+'(');start=source.rfind('\n',0,pos)+1
 opening=source.index('{',pos);depth=1;end=opening+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
api=(ROOT/'kernel/api/api.c').read_text();gfx=(ROOT/'gui/gfx.c').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t *pixels;uint16_t width,height,pitch;gui_rect_t clip;int16_t origin_x,origin_y;uint16_t storage_width,storage_height;} gui_surface_t;
typedef struct {uint32_t *pixels;uint16_t width,height;} gui_image_t;
static bool gui_rect_intersect(gui_rect_t a,gui_rect_t b,gui_rect_t *r){r->x=a.x>b.x?a.x:b.x;r->y=a.y>b.y?a.y:b.y;int x=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w;int y=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;r->w=x-r->x;r->h=y-r->y;return r->w>0&&r->h>0;}
static gui_rect_t gui_gfx_get_clip(const gui_surface_t*s){return s->clip;}
#define kmemcpy memcpy
'''
for name in ['bk_gui_max_int','bk_gui_min_int','bk_gui_image_blend','bk_gui_surface_draw_image']:
 code+=function(api,name)
for name in ['gui_gfx_storage_bounds','gui_gfx_draw_xrgb']:
 code+=function(gfx,name)
code+=r'''
#define CANARY 0xDEADBEEFU
static uint32_t arena[4096];
static gui_surface_t reset(void){for(unsigned i=0;i<4096;i++)arena[i]=CANARY;for(unsigned i=8;i<24;i++)arena[i]=0;return (gui_surface_t){arena+8,800,600,4,{0,0,800,600},100,50,4,4};}
static void guard(void){for(unsigned i=0;i<4096;i++)if(i<8||i>=24)assert(arena[i]==CANARY);}
int main(void){
 uint32_t argb[]={0xFFFF0000,0xFF00FF00,0xFF0000FF,0xFFFFFFFF};
 uint32_t rgb[]={0xFF0000,0x00FF00,0x0000FF,0xFFFFFF};
 gui_image_t image={argb,2,2};
 for(unsigned variant=0;variant<3;variant++)for(unsigned alpha=0;alpha<2;alpha++){
  gui_surface_t s=reset();int size=variant==0?2:variant==1?4:3;
  gui_rect_t d={100,50,size,size},c={0,0,800,600};
  if(alpha)bk_gui_surface_draw_image(&s,d,c,&image);else gui_gfx_draw_xrgb(&s,d,c,rgb,2,2);
  guard();assert(arena[8]==0xFF0000&&arena[8+(size-1)*4+(size-1)]==0xFFFFFF);
 }
 for(unsigned alpha=0;alpha<2;alpha++){
  gui_surface_t s=reset();gui_rect_t d={99,49,4,4},c={0,0,800,600};
  if(alpha)bk_gui_surface_draw_image(&s,d,c,&image);else gui_gfx_draw_xrgb(&s,d,c,rgb,2,2);
  guard();assert(arena[8]==0xFF0000&&arena[8+2*4+2]==0xFFFFFF);
 }
 gui_surface_t s=reset();argb[0]=0x80FF0000;
 bk_gui_surface_draw_image(&s,(gui_rect_t){100,50,2,2},s.clip,&image);
 guard();assert(arena[8]==0x800000);
 s=reset();bk_gui_surface_draw_image(&s,(gui_rect_t){200,100,2,2},s.clip,&image);guard();assert(!arena[8]);
 puts("PASS compact image/XRGB blits: native, 2x, general scaling, clipping, alpha and allocation canaries");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-blit-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
