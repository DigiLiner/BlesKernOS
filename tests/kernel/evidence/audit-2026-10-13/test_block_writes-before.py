#!/usr/bin/env python3
"""Production block routing: durable filesystem writes and volatile swap writes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source='\n'.join(line for line in (root/'kernel/drivers/core/block.c').read_text().splitlines() if not line.startswith('#include'))
code=r'''#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "kernel/include/block.h"
typedef int kmutex_t;typedef int krwlock_t;
#define KMUTEX_INITIALIZER 0
#define KRWLOCK_INITIALIZER 0
#define W3P_SCOPE(a,b) ((void)0)
#define kmemset memset
#define kstrncpy strncpy
#define kstrcmp strcmp
static void kmutex_init(kmutex_t *p){*p=0;}
static void kmutex_lock(kmutex_t *p){assert(!*p);*p=1;}
static void kmutex_unlock(kmutex_t *p){assert(*p);*p=0;}
static void krwlock_init(krwlock_t *p){*p=0;}
static void krwlock_write_lock(krwlock_t *p){assert(!*p);*p=1;}
static void krwlock_write_unlock(krwlock_t *p){assert(*p);*p=0;}
#define krwlock_read_lock krwlock_write_lock
#define krwlock_read_unlock krwlock_write_unlock
static void bootsplash_pulse(void){}
static uint64_t perfmon_scope_begin(void){return 0;}
static void perfmon_block_complete(uint32_t t,bool w,uint32_t c,bool r,uint64_t s){(void)t;(void)w;(void)c;(void)r;(void)s;}
'''+source+r'''
static unsigned durable,temporary,last_lba;static bool fail;
static bool read_cb(block_device_t *d,uint32_t l,uint8_t n,void *b){(void)d;(void)l;(void)n;(void)b;return true;}
static bool stable(block_device_t *d,uint32_t l,uint8_t n,const void *b){(void)d;(void)n;(void)b;durable++;last_lba=l;return !fail;}
static bool swap_cb(block_device_t *d,uint32_t l,uint8_t n,const void *b){(void)d;(void)n;(void)b;temporary++;last_lba=l;return !fail;}
int main(void){char page[4096]={0};block_init();assert(block_register("ata0",BLOCK_DEVICE_ATA,1000,0,read_cb));assert(block_set_writer("ata0",stable));block_device_t *disk=block_get("ata0");
 assert(block_write_volatile(disk,4,8,page));assert(durable==1 && temporary==0);
 assert(block_set_volatile_writer("ata0",swap_cb));assert(block_write(disk,5,8,page));assert(durable==2 && temporary==0);
 assert(block_write_volatile(disk,6,8,page));assert(temporary==1 && last_lba==6);
 assert(block_register_view("part",disk,100,200));block_device_t *part=block_get("part");assert(block_write(part,1,8,page));assert(durable==3 && last_lba==101);assert(block_write_volatile(part,2,8,page));assert(temporary==2 && last_lba==102);
 assert(!block_write_volatile(part,198,8,page));assert(!block_write(part,200,1,page));assert(temporary==2 && durable==3);
 part->read_only=true;assert(!block_write(part,0,1,page));assert(!block_write_volatile(part,0,1,page));part->read_only=false;
 part->base_lba=0xfffffffeU;assert(!block_write_volatile(part,1,8,page));assert(!block_write(part,2,1,page));
 fail=true;assert(!block_write(disk,0,1,page));assert(!block_write_volatile(disk,0,1,page));
 assert(!block_write_volatile(0,0,1,page));assert(!block_write(disk,0,0,page));block_init();assert(block_count()==0);
 puts("PASS filesystem durability routing, volatile fallback, partition translation, bounds, read-only protection and I/O errors");}
'''
with tempfile.TemporaryDirectory(prefix='bk-block-') as directory:
 d=Path(directory);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror','-I'+str(root),str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
