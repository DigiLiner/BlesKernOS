#!/usr/bin/env python3
"""Compare optimized saver expansion to its exact pixel mapping and benchmark it."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
code=r'''
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
typedef struct {uint32_t *pixels;uint16_t width,height;uint32_t pitch;} gui_surface_t;
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
 gui_surface_t s={dst+1,dw,dh,pitch};ss_scale_rgb(&s,src,w,h);
 for(unsigned y=0;y<dh;y++) {
  for(unsigned x=0;x<dw;x++)assert(s.pixels[y*pitch+x]==(src[y*h/dh*w+x*w/dw]&0xffffffU));
  for(unsigned x=dw;x<pitch;x++)assert(s.pixels[y*pitch+x]==0xdeadbeefU);
 }
 assert(dst[0]==0xdeadbeefU && dst[n+1]==0xdeadbeefU);free(src);free(dst);
}
int main(void){
 unsigned dims[][4]={{400,300,1400,1050},{320,240,800,600},{317,239,1366,768},
 {400,300,200,150},{16,16,1,1},{1,1,33,47},{400,300,400,300},{400,300,1920,1080}};
 for(unsigned i=0;i<sizeof(dims)/sizeof(dims[0]);i++)check(dims[i][0],dims[i][1],dims[i][2],dims[i][3]);
 for(unsigned i=1;i<80;i++)check(1+i*3%137,1+i*7%113,1+i*11%179,1+i*13%127);
 uint32_t *src=malloc(400*300*4),*dst=malloc(1400*1050*4);
 for(unsigned i=0;i<400*300;i++)src[i]=pattern(i);
 gui_surface_t s={dst,1400,1050,1400};double t=seconds();
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
 for flags in [['-fsanitize=address,undefined','-fno-pie','-no-pie'],[]]:
  subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-I'+str(ROOT),*flags,str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
