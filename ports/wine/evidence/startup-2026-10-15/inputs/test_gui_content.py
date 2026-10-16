#!/usr/bin/env python3
"""Verify production client staging/capture coordinates and bounded buffer reuse."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'gui/windows.c').read_text()
functions=s[s.index('static void content_staging_release_slot'):s.index('static uint32_t callback_owner')].replace('#include "color_palette.h"', '')
functions+=s[s.index('bool gui_window_capture_content('):s.index('bool gui_window_begin_content_paint(')]
gfx=(root/'gui/gfx.c').read_text()
functions+='\nstatic uint8_t g_output_depth;\n'+gfx[gfx.index('static void gui_gfx_quantize_output('):gfx.index('static bool gui_gfx_prepare_buffers')]
code='#include <stdint.h>\n#include "'+str(root/'gui/color_palette.h')+'"\n'+r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define GUI_CONTENT_STAGING_POOL_SIZE 4
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t *pixels;uint16_t width,height,pitch;gui_rect_t clip;int16_t origin_x,origin_y;uint16_t storage_width,storage_height;} gui_surface_t;
typedef struct {gui_surface_t surface;bool in_use;uint32_t capacity;} gui_content_staging_slot_t;
typedef struct {gui_rect_t bounds;bool content_staging_active;gui_rect_t content_staging_rect;int content_staging_slot;uint32_t *content_cache;uint16_t content_cache_width,content_cache_height;bool content_cache_rgb332,content_ready;} gui_window_t;
static gui_content_staging_slot_t g_content_staging_pool[4];
static int g_content_staging_lock;
static uint32_t physical=64U*1024U*1024U, last_allocation;
static uint32_t mm_physical_top(void){return physical;}
static bool compat_mode_is_low_memory(void){return false;}
static uint32_t kspin_lock_irqsave(int *p){(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;}
static void *kmalloc(uint32_t n){last_allocation=n;return malloc(n);}
static bool mm_set_allocation_owner(void *p,uint32_t owner){(void)p;(void)owner;return true;}
#define kfree free
#define kmemcpy memcpy
#define MM_SUBSYSTEM_GUI 1
static bool mm_set_allocation_subsystem(void *p,uint8_t tag){(void)p;(void)tag;return true;}
static gui_rect_t gui_window_content_rect(gui_window_t *w){return w->bounds;}
static bool gui_rect_intersect(gui_rect_t a,gui_rect_t b,gui_rect_t *r){r->x=a.x>b.x?a.x:b.x;r->y=a.y>b.y?a.y:b.y;int right=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w;int bottom=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;r->w=right-r->x;r->h=bottom-r->y;return r->w>0&&r->h>0;}
'''+functions+r'''
int main(void){
 uint32_t output[6]={BK_THEME_FACE,0x123456,0xFFFFFF,0x000000,BK_THEME_SHADOW,0x00FFFF};
 gui_surface_t output_surface={.pixels=output,.width=3,.height=2,.pitch=3};
 gui_rect_t output_rect={0,0,3,2};uint32_t original_output[6];memcpy(original_output,output,sizeof(output));
 g_output_depth=8;gui_gfx_quantize_output(&output_surface,output_rect);
 assert(output[0]==BK_THEME_FACE&&output[4]==BK_THEME_SHADOW);
 uint32_t first_output[6];memcpy(first_output,output,sizeof(output));
 gui_gfx_quantize_output(&output_surface,output_rect);assert(!memcmp(first_output,output,sizeof(output)));
 g_output_depth=16;gui_gfx_quantize_output(&output_surface,output_rect);
 memcpy(first_output,output,sizeof(output));gui_gfx_quantize_output(&output_surface,output_rect);assert(!memcmp(first_output,output,sizeof(output)));
 g_output_depth=4;gui_gfx_quantize_output(&output_surface,output_rect);
 assert(output[2]==0xFFFFFF&&output[3]==0);
 memcpy(output,original_output,sizeof(output));g_output_depth=0;gui_gfx_quantize_output(&output_surface,output_rect);assert(!memcmp(output,original_output,sizeof(output)));
 uint32_t dummy=0;gui_surface_t canvas={.pixels=&dummy,.width=800,.height=600,.pitch=800}, view;
 gui_rect_t region={100,50,3,2};
 int slot=content_staging_acquire(&canvas,&view,&region);
 assert(slot==0&&last_allocation==24&&view.pitch==3&&view.origin_x==100&&view.origin_y==50);
 assert(view.storage_width==3&&view.storage_height==2);
 gui_surface_t unavailable;assert(content_staging_acquire(&canvas,&unavailable,&region)==-1);
 uint32_t colors[6]={0xFF0000,0x00FF00,0x0000FF,0xFFFFFF,0x000000,0x808080};memcpy(view.pixels,colors,sizeof(colors));
 gui_window_t w={.bounds=region,.content_staging_active=true,.content_staging_rect=region,.content_staging_slot=slot};
 assert(gui_window_capture_content(&w,&view)&&w.content_ready&&w.content_cache_rgb332);
 for(unsigned i=0;i<6;i++)assert(((uint8_t *)w.content_cache)[i]==content_rgb888_to_332(colors[i]));
 assert(content_rgb332_to_888(content_rgb888_to_332(0xFFFFFF))==0xFFFFFF);
 for(unsigned i=0;i<16;i++)assert(content_rgb332_to_888(content_rgb888_to_332(content_cache_theme[i]))==content_cache_theme[i]);
 for(unsigned grey=0;grey<256;grey++) {
  uint32_t c=content_rgb332_to_888(content_rgb888_to_332(grey*0x010101U));
  assert(((c>>16)&255)==((c>>8)&255)&&((c>>8)&255)==(c&255));
  int error=(int)(c&255)-(int)grey;assert(error>=-6&&error<=6);
 }
 free(w.content_cache);w.content_cache=NULL;
 content_staging_release_slot(slot);uint32_t *original=view.pixels;
 gui_rect_t smaller={200,90,2,2};slot=content_staging_acquire(&canvas,&view,&smaller);
 assert(view.pixels==original&&view.pitch==2&&view.origin_x==200&&view.origin_y==90);
 content_staging_release_slot(slot);physical=256U*1024U*1024U;
 slot=content_staging_acquire(&canvas,&view,&region);memcpy(view.pixels,colors,sizeof(colors));
 assert(gui_window_capture_content(&w,&view)&&!w.content_cache_rgb332);
 assert(!memcmp(w.content_cache,colors,sizeof(colors)));
 puts("PASS compact client allocation, nonzero origins, indexed/full-color capture, exact theme/neutral greys, software output palettes, slot exclusion and retained-buffer reuse");
 free(w.content_cache);for(unsigned i=0;i<4;i++)free(g_content_staging_pool[i].surface.pixels);
}
'''
with tempfile.TemporaryDirectory(prefix='bk-gui-content-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
