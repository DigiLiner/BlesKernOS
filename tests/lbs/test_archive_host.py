#!/usr/bin/env python3
"""Exercise production BKZ1 code against round trips and malformed streams."""
import subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)
 (p/'bleskernos_archive_lbs.h').write_text('''#include <stdint.h>
#include <stdlib.h>
#define BK_ARCHIVE_MAX_BYTES (16U*1024U*1024U)
#define BK_LBS_INVALID -22
#define BK_LBS_NOMEM -12
#define bk_sys_alloc malloc
''')
 (p/'test.c').write_text(r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
int bk_archive_pack(const void*,uint32_t,void**,uint32_t*);
int bk_archive_unpack(const void*,uint32_t,void**,uint32_t*);
int main(void) {
 uint8_t input[8192]; uint32_t seed=19;
 for (unsigned t=0;t<1000;t++) {
  unsigned n=t<300?t:(t*31)%sizeof(input);
  for(unsigned i=0;i<n;i++){seed=seed*1664525+1013904223;input[i]=t&1?seed>>24:(i/300)&255;}
  void *packed=0,*decoded=0;uint32_t pn=0,dn=0;
  assert(!bk_archive_pack(input,n,&packed,&pn));
  assert(!bk_archive_unpack(packed,pn,&decoded,&dn));
  assert(dn==n && !memcmp(input,decoded,n));free(decoded);
  if(pn>8){assert(bk_archive_unpack(packed,pn-1,&decoded,&dn)<0);assert(!decoded&&!dn);}
  free(packed);
 }
 uint8_t invalid[][12]={{'B','K','Z','1',1,0,0,0,0,42},
 {'B','K','Z','1',1,0,0,0,2,42}, {'B','K','Z','1',0,0,0,0,1,42},
 {'B','K','Z','1',0,0,0,2}};
 for(unsigned i=0;i<4;i++){void *o=(void*)1;uint32_t n=9;assert(bk_archive_unpack(invalid[i],12,&o,&n)<0);assert(!o&&!n);}
 puts("PASS: production ARCHIVE, 1000 round trips, truncation, invalid runs, trailing data and limits");
}
''')
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(p),str(ROOT/'sdk/lbs/archive.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
