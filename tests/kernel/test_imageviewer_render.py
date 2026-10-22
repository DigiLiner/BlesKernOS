#!/usr/bin/env python3
"""Compare production raster output with the original sampler, including guards."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t *pixels;int width,height,pitch;gui_rect_t clip;int origin_x,origin_y,storage_width,storage_height;} gui_surface_t;
typedef struct {uint32_t *pixels;int width,height;} gui_image_t;
static __attribute__((unused)) bool gui_rect_intersect(gui_rect_t a,gui_rect_t b,gui_rect_t*out){int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y,r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,t=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;*out=(gui_rect_t){x,y,r-x,t-y};return r>x&&t>y;}
#include "programs/imageviewer_render.h"
static uint32_t seed=41;
static int pick(int n){seed=seed*1664525U+1013904223U;return (seed>>8)%n;}
int main(void){
 uint32_t src[53*41],actual[67*47+8],expected[67*47+8];
 for(int i=0;i<53*41;i++)src[i]=(i%7?0xff000000U:0)|(i*371U&0xffffff);
 for(int test=0;test<1000;test++){
  for(unsigned i=0;i<sizeof(actual)/sizeof(*actual);i++)actual[i]=expected[i]=0x12345678;
  gui_surface_t s={actual+4,800,600,67,{100+pick(40),70+pick(30),1+pick(60),1+pick(40)},100,70,61,47};
  gui_rect_t area={90+pick(40),60+pick(30),1+pick(100),1+pick(80)};
  gui_rect_t dest={60+pick(120),40+pick(100),1+pick(424),1+pick(328)};
  gui_image_t image={src,53,41};
  viewer_draw_checker(&s,area);viewer_blit_image(&s,area,dest,&image);
  for(int y=70;y<117;y++)for(int x=100;x<161;x++){
   if(x<s.clip.x||y<s.clip.y||x>=s.clip.x+s.clip.w||y>=s.clip.y+s.clip.h||x<area.x||y<area.y||x>=area.x+area.w||y>=area.y+area.h)continue;
   uint32_t color=((((x-area.x)/10)+((y-area.y)/10))&1)?0x383838:0x484848;
   if(x>=dest.x&&y>=dest.y&&x<dest.x+dest.w&&y<dest.y+dest.h){uint32_t p=src[((y-dest.y)*41/dest.h)*53+(x-dest.x)*53/dest.w];if(p>>24)color=p&0xffffff;}
   expected[4+(y-70)*67+x-100]=color;
  }
  assert(!memcmp(actual,expected,sizeof(actual)));
 }
 puts("PASS: 1000 clipped checker/image samples, transparency, scale, pan, offset storage and padded guards");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root),'-idirafter',str(root/'sdk/include'),str(root/'sdk/lbs/image_raster.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
