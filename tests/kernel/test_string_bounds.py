#!/usr/bin/env python3
"""Production kstrncpy must not write past n after an early terminator."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/memory.c').read_text()
body=s[s.index('char *kstrncpy('):s.index('char *kstrcat(')]
code='#include <stddef.h>\n#include <assert.h>\n#include <string.h>\n#include <stdio.h>\n'+body+r'''
int main(void){
 char output[32];
 for(size_t n=0;n<24;n++)for(size_t length=0;length<24;length++){
  char input[25];memset(input,'A',length);input[length]=0;
  memset(output,0x7e,sizeof(output));assert(kstrncpy(output+1,input,n)==output+1);
  assert(output[0]==0x7e && output[n+1]==0x7e);
  for(size_t i=0;i<n;i++)assert(output[i+1]==(i<length?'A':0));
 }
 assert(kstrncpy(NULL,NULL,0)==NULL);
 puts("PASS production kstrncpy: early NUL, truncation, padding, zero length and destination redzones");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-string-bounds-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 service=(root/'system/services/vision_memory.c').read_text()
 service_body=service[service.index('char *kstrncpy('):service.index('char *kstrcat(')]
 (p/'service.c').write_text(code.replace(body,service_body))
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(p/'service.c'),'-o',str(p/'service')],check=True)
 subprocess.run([str(p/'service')],check=True)
