#!/usr/bin/env python3
"""Check production CPU cell geometry for every supported core count."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'programs/processmanager.c').read_text()
function=s[s.index('static gui_rect_t cpu_cell('):s.index('/* Classic monitor frames:')]
code='''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef struct {int x,y,w,h;} gui_rect_t;
'''+function+'''
int main(void){
 for(int h=400;h<=900;h+=100)for(int w=640;w<=1400;w+=100)for(uint32_t count=1;count<=16;count++){
  gui_rect_t upper={118,128,w-36,h/2-56},cells[16];
  for(uint32_t i=0;i<count;i++){
   cells[i]=cpu_cell(upper,count,i);gui_rect_t r=cells[i];
   assert(r.w>8&&r.h>20&&r.x>=upper.x&&r.y>=upper.y);
   assert(r.x+r.w<=upper.x+upper.w&&r.y+r.h<=80+h/2);
   for(uint32_t j=0;j<i;j++)assert(r.x+r.w<=cells[j].x||cells[j].x+cells[j].w<=r.x||r.y+r.h<=cells[j].y||cells[j].y+cells[j].h<=r.y);
  }
 }
 puts("PASS 1..16 CPU panels remain separated and above the window midpoint");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-cpu-layout-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
