#!/usr/bin/env python3
"""Compare the production decoder against all upstream pixels and animate at rest."""
import importlib.util, json, struct, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('cursors',ROOT/'tools/build_retrosmart_cursors.py')
build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
s=(ROOT/'gui/compositor.c').read_text()
functions=s[s.index('typedef struct {'):s.index('/* BLESKERNOS_SVGA_HW_CURSOR_HELPER_BEGIN */')]
h=(ROOT/'gui/gui.h').read_text();enum=h[h.index('typedef enum {'):h.index('} gui_cursor_style_t;')+len('} gui_cursor_style_t;')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#define GUI_CURSOR_WIDTH 32
#define GUI_CURSOR_HEIGHT 32
typedef struct {int x,y,w,h;} gui_rect_t;
'''+enum+r'''
typedef struct {bool cursor_custom;gui_cursor_style_t cursor_style;int mouse_x,mouse_y;} gui_desktop_t;
static uint32_t pit_get_frequency_hz(void){return 100;}
static unsigned invalidations, reads;
static void gui_request_paint(void){}
static void gui_desktop_invalidate_rect(gui_desktop_t *d,gui_rect_t r){(void)d;assert(r.w==64&&r.h==64);invalidations++;}
#define kzalloc(n) calloc(1,n)
#define kfree free
static bool mm_set_allocation_owner(void *p,uint32_t owner){return p&&owner==0;}
static bool vfs_read_all(const char *p,void **data,uint32_t *size){(void)p;(void)data;(void)size;reads++;return false;}
'''+functions+r'''
static uint8_t *readfile(const char *p,uint32_t *size){FILE *f=fopen(p,"rb");assert(f);fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);uint8_t *b=malloc(*size);assert(fread(b,1,*size,f)==*size);fclose(f);return b;}
int main(int argc,char **argv){
 assert(argc==4);uint32_t size,n;uint8_t *data=readfile(argv[1],&size),*expect=readfile(argv[2],&n);
 system_cursor_t c={0};bool animation=atoi(argv[3])!=0;
 assert(animation?compositor_decode_animation(&c,data,size):compositor_decode_cursor(&c,data,size));
 unsigned count=animation?c.frame_count:1;assert(n==count*(8+4096));
 for(unsigned i=0;i<count;i++){
  if(animation){const uint8_t *e=data+8+i*12;assert(compositor_decode_cursor(&c,data+cur_le32(e+4),cur_le32(e+8)));}
  assert(c.width==32&&c.height==32);assert(c.hot_x==cur_le32(expect+i*4104)&&c.hot_y==cur_le32(expect+i*4104+4));
  assert(!memcmp(c.pixels,expect+i*4104+8,4096));
 }
 if(!animation){for(unsigned k=0;k<size;k++)assert(!compositor_decode_cursor(&c,data,k));}
 else {
  for(unsigned k=0;k<size;k+=17)assert(!compositor_decode_animation(&c,data,k));
  assert(compositor_decode_animation(&c,data,size));
  uint32_t duration=0;for(unsigned i=0;i<count;i++){
   assert(compositor_cursor_frame(&c,duration/10)==i);
   duration+=cur_le32(data+8+i*12);
  }
  assert(compositor_cursor_frame(&c,duration/10)==0);
  gui_desktop_t d={.cursor_style=GUI_CURSOR_WAIT};
  g_system_cursor_attempted[GUI_CURSOR_WAIT]=true;g_system_cursors[GUI_CURSOR_WAIT]=&c;
  unsigned before=reads;
  for(unsigned tick=0;tick<duration/10;tick++)gui_compositor_tick_cursor(&d,tick);
  assert(invalidations>=count-1&&reads==before);
  d.cursor_style=GUI_CURSOR_ARROW;
  gui_compositor_busy_begin();gui_compositor_busy_begin();assert(compositor_cursor_style(&d)==GUI_CURSOR_PROGRESS);
  gui_compositor_busy_end();assert(compositor_cursor_style(&d)==GUI_CURSOR_PROGRESS);
  d.cursor_style=GUI_CURSOR_SIZE_NS;assert(compositor_cursor_style(&d)==GUI_CURSOR_SIZE_NS);
  d.cursor_style=GUI_CURSOR_ARROW;gui_compositor_busy_end();assert(compositor_cursor_style(&d)==GUI_CURSOR_ARROW);
 }
 free(data);free(expect);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bk-cursors-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O1','-g','-Wall','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 manifest=json.loads((ROOT/'assets/cursors/INDEX.JSON').read_text())
 for item in manifest['cursors']:
  config=ROOT/'third_party/retrosmart/src'/ (item['role']+'.in')
  expected=[]
  for line in config.read_text().splitlines():
   fields=line.split()
   if fields[0]!='32':continue
   _,hx,hy,png,*delay=fields
   palette,rows=build.bitmap(ROOT/'third_party/retrosmart/src/bitmap.base'/Path(png).with_suffix('.xpm'))
   argb=[(a<<24)|(r<<16)|(g<<8)|b for row in rows for index in row for r,g,b,a in [palette[index]]]
   expected.append(struct.pack('<II1024I',int(hx),int(hy),*argb))
  for animation in ([False,True] if len(expected)>1 else [False]):
   asset=ROOT/'assets/cursors'/Path(item['file']).with_suffix('.BCA' if animation else '.CUR')
   (p/'expected').write_bytes(b''.join(expected if animation else expected[:1]))
   subprocess.run([str(p/'test'),str(asset),str(p/'expected'),str(int(animation))],check=True)
 print('PASS 48 native-size cursors and both animations: source pixels, alpha, hotspots, timing, stationary redraw, no tick reads, nested launch state, truncated files (ASan/UBSan)')
