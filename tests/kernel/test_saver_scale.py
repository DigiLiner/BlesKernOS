#!/usr/bin/env python3
"""Compare optimized saver expansion to its exact pixel mapping and benchmark it."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
code=r'''
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t *pixels;uint16_t width,height;uint32_t pitch;gui_rect_t clip;int16_t origin_x,origin_y;uint16_t storage_width,storage_height;} gui_surface_t;
#include "system/screensavers/ss_scale.h"
static uint32_t pattern(uint32_t n){return 0xff000000U|(n*37013U&0xffffffU);}
__attribute__((noinline)) static void old_scale(gui_surface_t *s,const uint32_t *p,uint16_t w,uint16_t h){
 for(uint32_t y=0;y<s->height;y++)
  for(uint32_t x=0;x<s->width;x++)
   s->pixels[y*s->pitch+x]=p[(y*h/s->height)*w+x*w/s->width]&0xffffffU;
}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void check(unsigned w,unsigned h,unsigned dw,unsigned dh){
 unsigned pitch=dw+7,n=pitch*dh;
 uint32_t *src=malloc(w*h*4),*dst=malloc((n+2)*4);
 assert(src&&dst);
 for(unsigned i=0;i<w*h;i++)src[i]=pattern(i);
 for(unsigned i=0;i<n+2;i++)dst[i]=0xdeadbeefU;
 gui_surface_t s={.pixels=dst+1,.width=dw,.height=dh,.pitch=pitch,.clip={0,0,dw,dh}};ss_scale_rgb(&s,src,w,h);
 for(unsigned y=0;y<dh;y++) {
  for(unsigned x=0;x<dw;x++)assert(s.pixels[y*pitch+x]==(src[y*h/dh*w+x*w/dw]&0xffffffU));
  for(unsigned x=dw;x<pitch;x++)assert(s.pixels[y*pitch+x]==0xdeadbeefU);
 }
 assert(dst[0]==0xdeadbeefU && dst[n+1]==0xdeadbeefU);free(src);free(dst);
}
static void check_offset(unsigned w,unsigned h,gui_rect_t dest,gui_rect_t clip){
 uint32_t src[137*113],guard[28*17+2];
 for(unsigned i=0;i<w*h;i++)src[i]=pattern(i);
 for(unsigned i=0;i<28*17+2;i++)guard[i]=0xdeadbeefU;
 gui_surface_t s={.pixels=guard+1,.width=200,.height=150,.pitch=28,.clip=clip,
                  .origin_x=11,.origin_y=17,.storage_width=21,.storage_height=17};
 ss_blit_rgb(&s,src,w,h,dest);
 for(int y=0;y<17;y++)for(int x=0;x<28;x++){
  int gx=x+11,gy=y+17;uint32_t expected=0xdeadbeefU;
  if(x<21 && gx>=clip.x && gy>=clip.y && gx<clip.x+clip.w && gy<clip.y+clip.h &&
     gx>=dest.x && gy>=dest.y && gx<dest.x+dest.w && gy<dest.y+dest.h)
   expected=src[(gy-dest.y)*h/dest.h*w+(gx-dest.x)*w/dest.w]&0xffffffU;
  assert(s.pixels[y*28+x]==expected);
 }
 assert(guard[0]==0xdeadbeefU && guard[28*17+1]==0xdeadbeefU);
}
int main(void){
 for(unsigned i=1;i<80;i++)check_offset(1+i*3%137,1+i*7%113,
  (gui_rect_t){(int)(i%19), (int)(i%23), 1+i*11%49,1+i*13%41},
  (gui_rect_t){8+(int)(i%13),12+(int)(i%17),1+i%31,1+i%29});
 unsigned dims[][4]={{400,300,1400,1050},{320,240,800,600},{317,239,1366,768},
 {400,300,200,150},{16,16,1,1},{1,1,33,47},{400,300,400,300},{400,300,1920,1080}};
 for(unsigned i=0;i<sizeof(dims)/sizeof(dims[0]);i++)check(dims[i][0],dims[i][1],dims[i][2],dims[i][3]);
 for(unsigned i=1;i<80;i++)check(1+i*3%137,1+i*7%113,1+i*11%179,1+i*13%127);
 uint32_t *src=malloc(400*300*4),*dst=malloc(1400*1050*4);
 for(unsigned i=0;i<400*300;i++)src[i]=pattern(i);
 gui_surface_t s={.pixels=dst,.width=1400,.height=1050,.pitch=1400,.clip={0,0,1400,1050}};double t=seconds();
 for(unsigned i=0;i<20;i++)old_scale(&s,src,400,300);
 double old=seconds()-t;t=seconds();
 for(unsigned i=0;i<20;i++)ss_scale_rgb(&s,src,400,300);
 double fast=seconds()-t;
 printf("PASS: exact pixels, alpha, guards, padded pitch, up/downscale; old=%.2fms new=%.2fms ratio=%.2fx (host)\n",old*1000,fast*1000,old/fast);
 free(src);free(dst);
}
'''
with tempfile.TemporaryDirectory(prefix='bk-scale-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 for flags in [['-fsanitize=address,undefined','-fno-pie','-no-pie'],['-DBLES_ARCH_ARM32_RPI3']]:
  subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-I'+str(ROOT),*flags,str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
