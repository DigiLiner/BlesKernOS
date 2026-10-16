#!/usr/bin/env python3
"""Check preview clipping and the selected tab/page join without booting a VM."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(source,name):
 start=source.index(name);start=source.rfind('\n',0,start)+1
 brace=source.index('{',start);end=brace;depth=0
 for end in range(brace,len(source)):
  if source[end]=='{':depth+=1
  elif source[end]=='}':
   depth-=1
   if not depth:return source[start:end+1]+'\n'
common=(ROOT/'system/control/control_common.h').read_text()
preview=(ROOT/'system/control/display_props_shared.h').read_text()
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "bleskernos_theme.h"
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t pixels[256*256];} gui_surface_t;
typedef struct {const char *label;int width;} cpl_tab_spec_t;
#define CPL_FACE 0xc0c0c0
#define CPL_LIGHT 0xffffff
#define CPL_DARK 0x404040
#define CPL_SHADOW 0x808080
#define CPL_TEXT 0x101010
#define DESK_WALLPAPER_TILE 0
#define DESK_WALLPAPER_CENTER 1
#define DESK_WALLPAPER_STRETCH 2
#define DESK_WALLPAPER_FIT 3
#define DESK_WALLPAPER_FILL 4
typedef struct {uint32_t preview_color;uint32_t *wallpaper_pixels;uint16_t wallpaper_preview_w,wallpaper_preview_h;uint8_t wallpaper_mode;} display_props_state_t;
static void bk_gui_gfx_putpixel(gui_surface_t *s,int x,int y,uint32_t color) {assert(x>=0&&x<256&&y>=0&&y<256);s->pixels[y*256+x]=color;}
static void bk_gui_gfx_fill_rect(gui_surface_t *s,gui_rect_t r,uint32_t c){for(int y=r.y;y<r.y+r.h;++y)for(int x=r.x;x<r.x+r.w;++x)bk_gui_gfx_putpixel(s,x,y,c);}
static void bk_gui_gfx_draw_line(gui_surface_t *s,int x,int y,int ex,int ey,uint32_t c){assert(x==ex||y==ey);if(x==ex){for(;y<=ey;++y)bk_gui_gfx_putpixel(s,x,y,c);}else for(;x<=ex;++x)bk_gui_gfx_putpixel(s,x,y,c);}
static void bk_gui_gfx_draw_rect(gui_surface_t *s,gui_rect_t r,uint32_t c){bk_gui_gfx_draw_line(s,r.x,r.y,r.x+r.w-1,r.y,c);bk_gui_gfx_draw_line(s,r.x,r.y,r.x,r.y+r.h-1,c);bk_gui_gfx_draw_line(s,r.x,r.y+r.h-1,r.x+r.w-1,r.y+r.h-1,c);bk_gui_gfx_draw_line(s,r.x+r.w-1,r.y,r.x+r.w-1,r.y+r.h-1,c);}
static int bk_gui_font_text_width(const char *t){return strlen(t)*8;}
static void bk_gui_font_draw_string_clipped(gui_surface_t *s,int x,int y,const char *t,uint32_t c,gui_rect_t clip){(void)s;(void)x;(void)y;(void)t;(void)c;(void)clip;}
'''
code+='\n#define gui_gfx_fill_rect bk_gui_gfx_fill_rect\n#define gui_font_text_width bk_gui_font_text_width\n#define gui_font_draw_string_clipped bk_gui_font_draw_string_clipped\n'
widget=(ROOT/'gui/widget.c').read_text()
code+=function(widget,'bk_gui_surface_draw_tab(')
for name in ['cpl_draw_bevel(', 'cpl_tab_rect(', 'cpl_draw_tab_face(', 'cpl_draw_tabs(']:code+=function(common,name)
code+=function(preview,'display_props_draw_preview_background(')
code+=r'''
int main(void){
 static gui_surface_t surface;uint32_t tile[35];for(int i=0;i<35;++i)tile[i]=0x112233;
 display_props_state_t state={.preview_color=0x334455,.wallpaper_pixels=tile,.wallpaper_preview_w=7,.wallpaper_preview_h=5};
 gui_rect_t screen={32,25,111,89};
 for(int mode=0;mode<5;++mode){
  state.wallpaper_mode=mode;for(int i=0;i<256*256;++i)surface.pixels[i]=0xabcdef;
  display_props_draw_preview_background(&state,&surface,screen);
  for(int y=0;y<256;++y)for(int x=0;x<256;++x)
   if(x<screen.x||x>=screen.x+screen.w||y<screen.y||y>=screen.y+screen.h)assert(surface.pixels[y*256+x]==0xabcdef);
 }
 cpl_tab_spec_t tabs[]={{"Uno",60},{"Dos",60},{"Tres",60}};
 gui_rect_t page={4,32,200,100},client={0,0,210,200};
 cpl_draw_tabs(&surface,client,tabs,3,0,page,12,8);
 assert(surface.pixels[6*256+15]==CPL_LIGHT); /* Active tab rises above others. */
 assert(surface.pixels[32*256+25]==CPL_FACE); /* Page top is open below active tab. */
 assert(surface.pixels[32*256+95]==CPL_LIGHT); /* Inactive tab keeps the page edge. */
 puts("PASS: preview bounds in five modes; raised active tab and continuous page join");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-visuals-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-Wall','-Wextra','-iquote'+str(ROOT/'sdk/include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
