#!/usr/bin/env python3
"""Check real shell composition order, clipped popup layers and publication."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'gui/desktop.c').read_text()
def function(name):
    import re
    m=re.search(r'^(?:static )?(?:void|bool) '+name+r'\([^;]+?\)\s*\{',s,re.M)
    assert m,name
    a=m.start();p=s.index('{',a);depth=1;b=p+1
    while depth:
        depth+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[a:b]+'\n'
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gui/color_palette.h"
#define krealloc realloc
static unsigned depth=32;
static unsigned gui_display_color_depth(void){return depth;}
static bool mm_set_allocation_owner(void *p,unsigned owner){(void)p;(void)owner;return true;}
#define kmemcpy memcpy
#define MM_SUBSYSTEM_GUI 1
static bool mm_set_allocation_subsystem(void *p,uint8_t tag){(void)p;(void)tag;return true;}
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t *pixels;uint16_t width,height,pitch;gui_rect_t clip;} gui_surface_t;
typedef struct gui_program {bool scene_owner,paint_ready,paint_requested,paint_pending,paint_cache_indexed;gui_surface_t paint_cache,paint_staging;uint32_t paint_overlay_count,staging_overlay_count;gui_rect_t paint_overlays[12],staging_overlays[12];struct gui_program *next;} gui_program_t;
typedef struct {gui_surface_t surface;gui_program_t *first_program;} gui_desktop_t;
static gui_rect_t gui_gfx_get_clip(const gui_surface_t *s){return s->clip;}
static void gui_gfx_set_clip(gui_surface_t *s,gui_rect_t r){s->clip=r;}
static bool gui_rect_intersect(gui_rect_t a,gui_rect_t b,gui_rect_t *out){int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y,r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,t=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;*out=(gui_rect_t){x,y,r-x,t-y};return r>x&&t>y;}
static gui_rect_t gui_rect_union(gui_rect_t a,gui_rect_t b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y,r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,t=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;return (gui_rect_t){x,y,r-x,t-y};}
static bool gui_program_surface_size(gui_surface_t *s,const gui_surface_t *source){return s->pixels&&s->width==source->width&&s->height==source->height&&s->pitch==source->pitch;}
'''+function('gui_program_cache_size')+function('gui_program_prepare_paint')+function('gui_program_composite_paint')+function('gui_program_finish_paint')+r'''
static void gui_desktop_paint_program_entry(gui_desktop_t *d,gui_program_t *p){
 if(p->scene_owner)gui_program_composite_paint(p,&d->surface);
 else d->surface.pixels[0]=0xffffff;
}
static void gui_desktop_paint_windows_layer(gui_desktop_t *d,gui_surface_t *s){(void)d;for(int y=s->clip.y;y<s->clip.y+s->clip.h;y++)for(int x=s->clip.x;x<s->clip.x+s->clip.w;x++)s->pixels[y*s->pitch+x]=0x0000ff;}
'''+function('gui_desktop_paint_shell_overlays')+function('gui_desktop_paint_programs')+r'''
int main(void){
 uint32_t cache[24],staging[24],output[24];
 for(int i=0;i<24;i++){cache[i]=0x00ff00;staging[i]=0xff0000;output[i]=0;}
 gui_rect_t clip={0,0,6,4};
 gui_program_t shell={.scene_owner=true,.paint_ready=true,.paint_overlay_count=1,.paint_overlays={{1,1,3,2}},
  .paint_cache={cache,6,4,6,{0,0,6,4}},.paint_staging={staging,6,4,6,{0,0,6,4}}};
 gui_desktop_t d={.surface={output,6,4,6,{0,0,6,4}},.first_program=&shell};
 gui_desktop_paint_programs(&d);
 for(int y=0;y<4;y++)for(int x=0;x<6;x++)assert(output[y*6+x]==(x>=1&&x<4&&y>=1&&y<3?0x00ff00:0x0000ff));
 assert(!memcmp(&d.surface.clip,&clip,sizeof(clip)));
 shell.staging_overlay_count=1;shell.staging_overlays[0]=(gui_rect_t){4,0,20,20};
 /* Pending geometry must not affect the already-published popup. */
 gui_desktop_paint_programs(&d);assert(output[7]==0x00ff00&&output[5]==0x0000ff);
 gui_program_finish_paint(&shell);assert(shell.paint_overlays[0].x==4);
 gui_desktop_paint_programs(&d);assert(output[7]==0x0000ff&&output[5]==0xff0000);
 d.surface.clip=(gui_rect_t){0,0,2,2};gui_desktop_paint_programs(&d);assert(output[5]==0xff0000);
 shell.paint_overlay_count=0;d.surface.clip=clip;gui_desktop_paint_programs(&d);
 for(int i=0;i<24;i++)assert(output[i]==0x0000ff);
 shell.paint_overlay_count=99;for(int i=0;i<12;i++)shell.paint_overlays[i]=(gui_rect_t){-10,-10,12,12};
 gui_desktop_paint_programs(&d);assert(output[0]==0xff0000&&output[23]==0x0000ff);
 gui_program_t saver={0};shell.next=&saver;gui_desktop_paint_programs(&d);assert(output[0]==0xffffff);
 shell.next=NULL;shell.paint_cache=(gui_surface_t){cache,6,4,6,clip};shell.paint_staging=(gui_surface_t){staging,6,4,6,clip};
 shell.paint_overlay_count=2;shell.paint_overlays[0]=(gui_rect_t){0,3,6,1};shell.paint_overlays[1]=(gui_rect_t){1,1,3,2};
 for(int i=0;i<24;i++){cache[i]=0x00ff00;staging[i]=0xff0000;output[i]=0x0000ff;}
 d.surface.clip=(gui_rect_t){0,0,1,1};gui_surface_t *borrowed=NULL;
 assert(gui_program_prepare_paint(&shell,&d.surface,&borrowed));
 assert(borrowed==&shell.paint_staging&&borrowed->clip.w==4&&borrowed->clip.h==3);
 for(int i=0;i<24;i++)assert(staging[i]==cache[i]);
 assert(shell.staging_overlay_count==0);
 gui_program_t indexed={0};depth=8;d.surface.clip=clip;
 assert(gui_program_cache_size(&indexed,&d.surface));
 assert(indexed.paint_cache_indexed&&!indexed.paint_ready);
 indexed.paint_staging=(gui_surface_t){staging,6,4,6,clip};
 for(int i=0;i<24;i++)staging[i]=content_rgb332_to_888((uint8_t)(16+i));
 gui_program_finish_paint(&indexed);
 memset(output,0,sizeof(output));gui_program_composite_paint(&indexed,&d.surface);
 for(int i=0;i<24;i++)assert(output[i]==staging[i]);
 assert(gui_program_prepare_paint(&indexed,&d.surface,&borrowed));
 for(int i=0;i<24;i++)assert(staging[i]==output[i]);
 depth=32;assert(gui_program_cache_size(&indexed,&d.surface));
 assert(!indexed.paint_cache_indexed&&!indexed.paint_ready);free(indexed.paint_cache.pixels);
 puts("PASS indexed cache publication, restoration, mode transition; shell menus above windows, delayed geometry publication, damage clipping, closed-menu cleanup, bounded overlays and screensaver order");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-shell-overlays-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Werror','-I'+str(root),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
