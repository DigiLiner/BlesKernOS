#!/usr/bin/env python3
"""Exercise bounded syscall I/O batches and page fallback on failed pins."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/api/syscall.c').read_text()
part=s[s.index('static int32_t sys_paged_io('):s.index('static int32_t sys_read(',s.index('static int32_t sys_paged_io('))]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define BK_EFAULT 14
#define SYSCALL_FILE_IO_MAX (32U*1024U*1024U)
#define SYSCALL_WRITE_MAX 65536U
#define LINUX_IO_BASE 1000
#define ROOT_ADDRESS 0x10000000U
typedef int registers_t;typedef int syscall_resources_t;
static unsigned pins,unpins,held,calls,progress,valid_bytes,source_bytes,max_pin;
static bool pressure,special_fd;
static uint32_t task_current_process_id(void){return 1;}
static syscall_resources_t *resource_for(unsigned pid,bool make){(void)pid;(void)make;static int r;return &r;}
static int resource_fd(syscall_resources_t *r,unsigned fd){(void)r;(void)fd;return special_fd?1000:7;}
static bool pager_pin(unsigned pid,unsigned addr,unsigned n,bool write){
 (void)pid;(void)write;pins++;assert(!held);
 if(addr<ROOT_ADDRESS || addr-ROOT_ADDRESS>=valid_bytes || n>valid_bytes-(addr-ROOT_ADDRESS) || (pressure && n>4096))return false;
 held=n;if(n>max_pin)max_pin=n;return true;
}
static void pager_unpin(unsigned pid,unsigned addr,unsigned n){(void)pid;(void)addr;assert(held==n);held=0;unpins++;}
static int32_t transfer(unsigned fd,void *p,unsigned n){
 (void)fd;(void)p;assert(!n || held==n);calls++;
 unsigned got=source_bytes-progress;if(got>n)got=n;progress+=got;return got;
}
static int32_t sys_read_resident(const registers_t *r,unsigned fd,void *p,unsigned n){(void)r;return transfer(fd,p,n);}
static int32_t sys_write_resident(const registers_t *r,unsigned fd,const void *p,unsigned n){(void)r;return transfer(fd,(void *)p,n);}
'''+part+r'''
static void reset(void){pins=unpins=held=calls=progress=max_pin=0;valid_bytes=source_bytes=256*1024;pressure=special_fd=false;}
int main(void){
 for(unsigned read=0;read<2;read++){
  reset();assert(sys_paged_io(NULL,7,(void *)ROOT_ADDRESS,192*1024,read)==192*1024 && calls==3 && pins==3 && unpins==3 && max_pin==65536);
  reset();valid_bytes=8192;assert(sys_paged_io(NULL,7,(void *)(ROOT_ADDRESS+17),16384,read)==8192-17 && progress==8192-17 && !held && unpins==2);
  reset();pressure=true;assert(sys_paged_io(NULL,7,(void *)ROOT_ADDRESS,16384,read)==16384 && calls==4 && unpins==4 && !held);
  reset();source_bytes=6000;assert(sys_paged_io(NULL,7,(void *)ROOT_ADDRESS,65536,read)==6000 && calls==1 && !held);
  reset();valid_bytes=0;assert(sys_paged_io(NULL,7,(void *)ROOT_ADDRESS,100,read)==-BK_EFAULT && !calls && !held);
  reset();special_fd=true;assert(sys_paged_io(NULL,7,(void *)ROOT_ADDRESS,4096,read)==4096 && calls==1 && pins==1);
  reset();special_fd=true;pressure=true;assert(sys_paged_io(NULL,7,(void *)ROOT_ADDRESS,8192,read)==-BK_EFAULT && !calls && !held);
  reset();assert(sys_paged_io(NULL,7,(void *)ROOT_ADDRESS,0,read)==0 && !pins);
  assert(sys_paged_io(NULL,7,(void *)0xfffff000U,8192,read)==-BK_EFAULT && !pins);
 }
 puts("PASS production paged I/O: 64 KiB batches, partial inaccessible spans, low-RAM fallback, EOF, balanced pins, socket/pipe atomicity and overflow");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-io-batch-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
