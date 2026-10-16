#!/usr/bin/env python3
"""Rasterize production shared tabs: selection joins the page, without moving hit areas."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'system/control/control_common.h').read_text()
def function(name):
 start=source.rfind('\n',0,source.index(name+'('))+1
 opening=source.index('{',start);depth=1;end=opening+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bleskernos_theme.h"
#define CPL_FACE BK_THEME_FACE
#define CPL_LIGHT BK_THEME_LIGHT
#define CPL_SHADOW BK_THEME_SHADOW
#define CPL_DARK BK_THEME_DARK
#define CPL_TEXT BK_THEME_TEXT
struct rect {int x,y,w,h;};typedef struct rect gui_rect_t;
typedef struct {uint32_t p[120*80];} gui_surface_t;
typedef struct {const char *label;int width;} cpl_tab_spec_t;
static void bk_gui_gfx_fill_rect(gui_surface_t*s,gui_rect_t r,uint32_t c){for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)if(x>=0&&x<120&&y>=0&&y<80)s->p[y*120+x]=c;}
static void bk_gui_gfx_draw_line(gui_surface_t*s,int x,int y,int xx,int yy,uint32_t c){int dx=(xx>x)-(xx<x),dy=(yy>y)-(yy<y);for(;;){bk_gui_gfx_fill_rect(s,(gui_rect_t){x,y,1,1},c);if(x==xx&&y==yy)break;x+=dx;y+=dy;}}
static int bk_gui_font_text_width(const char*t){return strlen(t)*7;}
static void bk_gui_font_draw_string_clipped(gui_surface_t*s,int x,int y,const char*t,uint32_t c,gui_rect_t r){(void)s;(void)x;(void)y;(void)t;(void)c;(void)r;}
'''
code+='\n#define gui_gfx_fill_rect bk_gui_gfx_fill_rect\n#define gui_font_text_width bk_gui_font_text_width\n#define gui_font_draw_string_clipped bk_gui_font_draw_string_clipped\n'
widget=(root/'gui/widget.c').read_text()
saved_source=source;source=widget;code+=function('bk_gui_surface_draw_tab');source=saved_source
for name in ['cpl_draw_bevel','cpl_tab_rect','cpl_draw_tab_face','cpl_draw_tabs']:code+=function(name)
code+=r'''
int main(void){gui_surface_t s;gui_rect_t client={0,0,120,80},page={8,30,104,42};cpl_tab_spec_t tabs[]={{"A",40},{"B",40}};
 for(int active=0;active<2;active++){
  memset(&s,0,sizeof(s));cpl_draw_tabs(&s,client,tabs,2,active,page,16,6);
  int selected_x=16+40*active+10,other_x=16+40*(1-active)+10;
  assert(s.p[4*120+selected_x]==BK_THEME_LIGHT);
  assert(s.p[8*120+other_x]==BK_THEME_LIGHT);
  assert(s.p[30*120+selected_x]==BK_THEME_FACE);
  assert(s.p[30*120+other_x]==BK_THEME_LIGHT);
  assert(s.p[15*120+other_x]==BK_THEME_TAB_INACTIVE);
  gui_rect_t hit=cpl_tab_rect(client,tabs,active,16,6);assert(hit.y==6&&hit.h==BK_THEME_TAB_HEIGHT);
 }
 /* Inactive tabs cannot paint over any part of the projected page. */
 for(int i=0;i<120*80;i++)s.p[i]=0x123456;
 bk_gui_surface_draw_tab(&s,(gui_rect_t){16,6,40,24},"A",false,page);
 assert(s.p[8*120+16]==0x123456); /* softened top corner */
 for(int y=page.y;y<80;y++)for(int x=0;x<120;x++)assert(s.p[y*120+x]==0x123456);
 bk_gui_surface_draw_tab(&s,(gui_rect_t){16,6,40,24},"A",true,page);
 for(int y=0;y<80;y++)assert(s.p[y*120+56]==0x123456); /* neighbour slot untouched */
 cpl_draw_bevel(&s,(gui_rect_t){10,40,30,20},BK_THEME_PANEL,true);
 assert(s.p[40*120+20]==BK_THEME_DARK);
 assert(s.p[41*120+20]==BK_THEME_SHADOW);
 assert(s.p[59*120+20]==BK_THEME_LIGHT);
 assert(s.p[58*120+20]==BK_THEME_MIDLIGHT);
 puts("PASS shared tabs and global bevel: elevated selection, inset inactive tabs, continuous page join and stable hit geometry");}
'''
with tempfile.TemporaryDirectory(prefix='bk-classic-theme-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-Wall','-Werror','-iquote'+str(root/'sdk/include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
