#!/usr/bin/env python3
"""Check private browser primitives against host libc and the linked imports."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'programs/netsurf/string.c').read_text()
names=['memcpy','memmove','memset','memcmp','strlen','strcmp','strncmp','strncpy','strcasecmp','strncasecmp','strchr','strrchr','strstr']
for name in names:source=re.sub(r'\b'+name+r'\b','local_'+name,source)
checks=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
static int sign(int v){return (v>0)-(v<0);}
int main(void){
 unsigned char a[513],b[513],src[513];
 for(unsigned i=0;i<513;i++)src[i]=(unsigned char)(i*73);
 for(unsigned n=0;n<480;n++)for(unsigned off=0;off<17;off++){
  memset(a,0xa5,sizeof(a));memset(b,0xa5,sizeof(b));
  assert(local_memcpy(a+off,src,n)==a+off);memcpy(b+off,src,n);assert(!memcmp(a,b,sizeof(a)));
  local_memset(a+off,0x123,n);memset(b+off,0x123,n);assert(!memcmp(a,b,sizeof(a)));
 }
 for(unsigned from=0;from<20;from++)for(unsigned to=0;to<20;to++)for(unsigned n=0;n<90;n++){
  memcpy(a,src,sizeof(a));memcpy(b,src,sizeof(b));local_memmove(a+to,a+from,n);memmove(b+to,b+from,n);assert(!memcmp(a,b,sizeof(a)));
 }
 const char *samples[]={"","a","abcdef","abc","Á ñ UTF-8","\xff","Mixed CASE","mixed case"};
 for(unsigned i=0;i<8;i++)for(unsigned j=0;j<8;j++){
  const char *a=samples[i],*b=samples[j];assert(local_strlen(a)==strlen(a));
  assert(sign(local_strcmp(a,b))==sign(strcmp(a,b)));
  assert(sign(local_strcasecmp(a,b))==sign(strcasecmp(a,b)));
  for(unsigned n=0;n<8;n++){
   assert(sign(local_strncmp(a,b,n))==sign(strncmp(a,b,n)));
   assert(sign(local_strncasecmp(a,b,n))==sign(strncasecmp(a,b,n)));
   char x[12],y[12];memset(x,1,12);memset(y,1,12);local_strncpy(x,a,n);strncpy(y,a,n);assert(!memcmp(x,y,12));
  }
  for(int c=0;c<256;c++){
   assert(local_strchr(a,c)==strchr(a,c));assert(local_strrchr(a,c)==strrchr(a,c));
  }
  assert(local_strstr(a,b)==strstr(a,b));
 }
 for(unsigned n=0;n<513;n++){
  assert(sign(local_memcmp(src,src,n))==0);
  memcpy(a,src,513);a[n]=(unsigned char)(a[n]+1);assert(sign(local_memcmp(src,a,513))==sign(memcmp(src,a,513)));
 }
 puts("PASS: private memory/string functions match libc, including overlap, alignment, unsigned bytes and NUL");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-browser-libc-') as directory:
    tmp=Path(directory);(tmp/'test.c').write_text(source+checks)
    subprocess.run(['gcc','-std=c11','-O2','-fno-builtin','-Wall','-Wextra','-fsanitize=address,undefined','-fno-pie','-no-pie',str(tmp/'test.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
    # Exercise the actual 32-bit rep-movsl implementation without needing a
    # hosted 32-bit libc: this tiny harness exits through Linux int 0x80.
    native=r'''
typedef unsigned int size_t;
void *memcpy(void *,const void *,size_t);
void *memmove(void *,const void *,size_t);
static unsigned char a[600],s[600];
void _start(void){
 int result=0;
 for(unsigned n=0;n<500;n++)for(unsigned offset=0;offset<16;offset++){
  for(unsigned i=0;i<600;i++){a[i]=0xa5;s[i]=(unsigned char)(i*17);}
  memcpy(a+offset,s,n);
  for(unsigned i=0;i<600;i++)if(a[i]!=(i>=offset&&i<offset+n?s[i-offset]:0xa5))result=1;
 }
 __asm__ volatile("int $0x80"::"a"(1),"b"(result):"memory");
 __builtin_unreachable();
}
'''
    (tmp/'native.c').write_text(native)
    subprocess.run(['gcc','-m32','-Os','-fno-builtin','-ffreestanding','-fno-pie','-no-pie','-nostdlib','-Isdk/include',str(ROOT/'programs/netsurf/string.c'),str(tmp/'native.c'),'-o',str(tmp/'native')],cwd=ROOT,check=True)
    subprocess.run([str(tmp/'native')],check=True)
    print('PASS: actual i386 rep-movsl copies preserve bytes and guards')
obj=ROOT/'build/programs/netsurf.o'
if obj.exists():
    imports=subprocess.check_output(['nm','-u',str(obj)],text=True)
    for name in names:assert not re.search(r'\bU '+name+r'$',imports,re.M),name+' still imports a syscall'
    print('PASS: browser binds hot memory/string operations locally')
