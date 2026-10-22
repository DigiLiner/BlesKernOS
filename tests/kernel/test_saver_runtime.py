#!/usr/bin/env python3
"""Verify production Mesa-saver memory helpers, including overlap and guards."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define memcpy saver_memcpy
#define memset saver_memset
#define memmove saver_memmove
#include "system/screensavers/ss_runtime.c"
#undef memcpy
#undef memset
#undef memmove
int main(void){
 uint8_t source[600],expected[600],actual[600];
 for(unsigned i=0;i<600;i++)source[i]=(uint8_t)(i*37);
 for(unsigned n=0;n<=512;n++)for(unsigned a=0;a<8;a++)for(unsigned b=0;b<8;b++){
  memset(actual,0xa5,sizeof(actual));memset(expected,0xa5,sizeof(expected));
  assert(saver_memcpy(actual+8+a,source+8+b,n)==actual+8+a);
  memcpy(expected+8+a,source+8+b,n);assert(!memcmp(actual,expected,sizeof(actual)));
 }
 for(unsigned n=0;n<=512;n++)for(unsigned a=0;a<8;a++)for(int c=-1;c<=256;c+=17){
  memset(actual,0xa5,sizeof(actual));memset(expected,0xa5,sizeof(expected));
  assert(saver_memset(actual+8+a,c,n)==actual+8+a);
  memset(expected+8+a,c,n);assert(!memcmp(actual,expected,sizeof(actual)));
 }
 for(unsigned n=0;n<128;n++)for(unsigned a=0;a<24;a++)for(unsigned b=0;b<24;b++){
  memcpy(actual,source,sizeof(actual));memcpy(expected,source,sizeof(expected));
  assert(saver_memmove(actual+8+a,actual+8+b,n)==actual+8+a);
  memmove(expected+8+a,expected+8+b,n);assert(!memcmp(actual,expected,sizeof(actual)));
 }
 puts("PASS: local memcpy/memset/memmove, alignment, overlap, zero length and guards");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-saver-runtime-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 for extra in [[],['-DBLES_ARCH_ARM32_RPI3']]:
  subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fno-builtin','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT),*extra,str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
