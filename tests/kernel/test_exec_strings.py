#!/usr/bin/env python3
"""Check production exec string copying against page boundaries and ABI limits."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linux/i386_exec.inc').read_text()
part=s[s.index('static int32_t linux_exec_strings'):s.index('static int32_t sys_linux_execve')]
header=(root/'kernel/include/linux_exec.h').read_text()
args_type=header[header.index('typedef struct'):header.index('bool task_linux_exec_stack')]
code=r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define BK_EFAULT 14
'''+args_type+r'''
static uint32_t lo,hi,calls;
static bool copy_from_user(void *dst,const void *src,uint32_t n){
 uint32_t a=(uint32_t)(uintptr_t)src;calls++;
 if(a<lo || a>=hi || n>hi-a)return false;
 memcpy(dst,src,n);return true;
}
'''+part+r'''
int main(void){
 char *p=mmap(NULL,32768,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(p!=MAP_FAILED);lo=(uint32_t)(uintptr_t)p;hi=lo+28672;
 assert(!mprotect(p+28672,4096,PROT_NONE));
 uint32_t *v=(uint32_t *)p;linux_exec_args_t args={0};
 v[0]=lo+4090;v[1]=lo+10000;v[2]=0;
 memset(p+4090,'a',4000);p[8090]=0;strcpy(p+10000,"ENV=value");
 assert(!linux_exec_strings(&args,lo,false));assert(args.argc==2 && args.used==4011 && calls==6);
 assert(strlen(args.data)==4000 && !strcmp(args.data+args.offsets[1],"ENV=value"));
 v[0]=lo+10000;v[1]=0;assert(!linux_exec_strings(&args,lo,true) && args.envc==1 && args.argc==2);
 memset(&args,0,sizeof args);v[0]=hi-1;p[28671]=0;
 assert(!linux_exec_strings(&args,lo,false) && args.used==1);
 memset(&args,0,sizeof args);p[28671]='a';assert(linux_exec_strings(&args,lo,false)==-BK_EFAULT);
 memset(&args,0,sizeof args);v[0]=lo+4096;memset(p+4096,'x',16384);
 assert(linux_exec_strings(&args,lo,false)==-7);
 memset(&args,0,sizeof args);v[0]=1;assert(linux_exec_strings(&args,lo,false)==-BK_EFAULT);
 memset(&args,0,sizeof args);v[0]=0;assert(!linux_exec_strings(&args,lo,false) && args.argc==1 && args.used==1);
 memset(&args,0,sizeof args);for(unsigned i=0;i<128;i++)v[i]=lo+1000;p[1000]=0;v[128]=0;
 assert(linux_exec_strings(&args,lo,false)==-7);
 munmap(p,32768);puts("PASS exec argv/envp: page batching, cross-page strings, last-byte NUL, EFAULT, E2BIG and vector limits");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-exec-string-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-I'+str(root),'-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
