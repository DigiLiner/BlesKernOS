#!/usr/bin/env python3
"""Test real ATAPI waits, capacity recovery, and delayed boot CD mounting."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
ata=(root/'kernel/drivers/storage/ata.c').read_text()
kernel=(root/'kernel/core/kernel.c').read_text()
def function(source,signature):
    a=source.index(signature);b=source.index('{',a);depth=1;b+=1
    while depth:
        if source[b]=='{':depth+=1
        elif source[b]=='}':depth-=1
        b+=1
    return source[a:b]+'\n'
base=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define ATA_STATUS_BSY 0x80
#define ATA_STATUS_DRQ 8
#define ATA_STATUS_ERR 1
#define ATA_STATUS_DF 0x20
#define ATA_REG_STATUS 7
#define ATA_TRANSPORT_ATAPI 1
#define ATAPI_DEFAULT_BLOCK_SIZE 2048
#define kmemset memset
'''
wait=base+r'''
static unsigned polls,busy_polls,yields;static uint8_t ready;
static uint32_t pit_get_ticks(void){return polls/1000;}
static uint32_t pit_get_frequency_hz(void){return 1000;}
static bool smp_scheduler_started(void){return true;}
static void task_yield(void){yields++;}
static uint8_t inb(unsigned port){assert(port==7);return polls++<busy_polls?0xa1:ready;}
'''
wait+=function(ata,'static bool ata_wait_packet_state(').replace('uint32_t flags; __asm__ volatile("pushfl; popl %0":"=r"(flags));','uint32_t flags = 0x200U;')
wait+=r'''
static void init(unsigned busy,uint8_t result){polls=yields=0;busy_polls=busy;ready=result;}
int main(void){
 init(150000,8);assert(ata_wait_packet_state(0,true));assert(polls==150001 && yields);
 init(150000,0);assert(ata_wait_packet_state(0,false));assert(polls==150001 && yields);
 init(0,1);assert(!ata_wait_packet_state(0,true));assert(polls==1);
 init(0,0x20);assert(!ata_wait_packet_state(0,false));
 init(0,0xff);assert(!ata_wait_packet_state(0,true));
 init(10000000,8);assert(!ata_wait_packet_state(0,true));assert(polls==5000000);
 puts("PASS packet waits ignore stale ERR/DF during BSY, tolerate delayed completion, yield and time out");
}
'''
capacity=base+r'''
typedef struct {unsigned transport,io,sectors;uint16_t block_size;} ata_device_t;
typedef struct {unsigned sector_count,sector_size;} block_device_t;
typedef unsigned kmutex_t;
static kmutex_t lock;static unsigned mode,reads,senses,resets;
static kmutex_t *ata_channel_lock_for(ata_device_t *d){(void)d;return &lock;}
static void kmutex_lock(kmutex_t *l){assert(!*l);*l=1;}
static void kmutex_unlock(kmutex_t *l){assert(*l);*l=0;}
static void ata_soft_reset_channel(ata_device_t *d){(void)d;assert(lock);resets++;}
static uint8_t ata_status(unsigned io){(void)io;return 1;}
static bool ata_atapi_packet_read(ata_device_t *d,const uint8_t *packet,uint16_t size,void *output){
 (void)d;assert(lock);uint8_t *p=output;memset(p,0,size);
 if(packet[0]==0x25){
  assert(size==8);reads++;
  if(mode==1 && reads==1)return false;
  if(mode==2 || mode==4)return false;
  if(mode==3 && reads<3)return false;
  unsigned last=mode==5?0xffffffffU:12344;
  unsigned bs=mode==6?0:2048;
  for(unsigned i=0;i<4;i++){p[i]=last>>(24-i*8);p[i+4]=bs>>(24-i*8);}return true;
 }
 assert(packet[0]==3 && packet[4]==18 && size==18);senses++;
 if(mode==4)return false;
 p[0]=0x70;p[2]=mode==1?6:2;p[12]=mode==2?0x3a:mode==1?0x29:4;return true;
}
'''
capacity+=function(ata,'static uint32_t read_be32(')+function(ata,'static bool ata_atapi_update_capacity(')
capacity+=r'''
int main(void){
 ata_device_t dev={1,0,0,2048};block_device_t block;
 for(mode=0;mode<7;mode++){
  reads=senses=resets=0;dev.sectors=999;block.sector_count=999;
  bool ok=ata_atapi_update_capacity(&dev,&block);assert(!lock);
  if(mode==0 || mode==1 || mode==3){assert(ok && dev.sectors==12345 && block.sector_count==12345 && block.sector_size==2048);}
  else {assert(!ok && !dev.sectors && !block.sector_count && block.sector_size==2048);}
  if(mode==1)assert(reads==2 && senses==1 && !resets);
  if(mode==2)assert(reads==1 && senses==1);
  if(mode==3)assert(reads==3 && senses==2);
  if(mode==4)assert(reads==3 && senses==3 && resets==3);
 }
 puts("PASS capacity recovers UNIT ATTENTION and becoming-ready, stops for empty tray and rejects invalid geometry");
}
'''
retry=base+r'''
#define UNUSED
#define KDOMAIN_VFS 2
static unsigned sleeps,mounts,lock,ready_after;static bool mounted;
static unsigned pit_get_frequency_hz(void){return 1000;}
static void task_sleep(unsigned ticks){assert(ticks==500 && !lock);sleeps++;}
static void kernel_domains_enter(unsigned mask){assert(mask==2 && !lock);lock=1;}
static void kernel_domains_exit(unsigned mask){assert(mask==2 && lock);lock=0;}
static bool vfs_has_cdrom(void){assert(lock);return mounted;}
static bool iso9660_mount_default(void){assert(lock);mounts++;return mounted=(mounts==ready_after);}
#define kprintf(...) ((void)0)
'''
retry+=function(kernel,'static void kernel_cdrom_mount_retry(')
retry+=r'''
int main(void){
 ready_after=3;kernel_cdrom_mount_retry(NULL);assert(mounts==3 && sleeps==3 && mounted && !lock);
 sleeps=mounts=0;kernel_cdrom_mount_retry(NULL);assert(!mounts && sleeps==1 && !lock);
 mounted=false;mounts=sleeps=0;ready_after=100;kernel_cdrom_mount_retry(NULL);assert(mounts==20 && sleeps==20 && !mounted && !lock);
 puts("PASS boot retries mount delayed media under VFS lock, stop on success and remain bounded");
}
'''
probe=base+r'''
static unsigned selections,found;
static uint8_t inb(unsigned port){(void)port;return 0xff;}
static void ata_probe_device(uint16_t io,uint16_t ctrl,uint8_t slave){
 assert(io==0x1f0 && ctrl==0x3f6);assert(slave==selections++);if(slave)found++;
}
'''
probe+=function(ata,'static void ata_probe(')
probe+=r'''
int main(void){ata_probe(0x1f0,0x3f6);assert(selections==2 && found==1);
 puts("PASS slave-only CD is probed even when initially selected empty position reports 0xff");}
'''
read=base+r'''
#define ATA_REG_ERROR 1
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_DATA 0
typedef struct {uint16_t io,ctrl,block_size;const char *name;} ata_device_t;
typedef struct {void *driver_data;} block_device_t;
typedef unsigned kmutex_t;
static kmutex_t lock;
static unsigned limit,remaining,phase,device_limit,phases,position;
static kmutex_t *ata_channel_lock_for(const ata_device_t *d){return &lock;}
static void kmutex_lock(kmutex_t *l){assert(!*l);*l=1;}
static void kmutex_unlock(kmutex_t *l){assert(*l);*l=0;}
static bool ata_atapi_begin_packet(const ata_device_t *d,uint16_t n){assert(lock);limit=n;return true;}
static void outsw(unsigned io,const void *data,unsigned words){
 const uint8_t *p=data;assert(words==6 && p[0]==0xa8);
 remaining=p[9]*2048U;position=phases=0;
}
static bool ata_atapi_wait_phase(const ata_device_t *d,bool *complete){
 *complete=!remaining;
 phase=remaining<limit?remaining:limit;
 if(phase>device_limit)phase=device_limit;
 return true;
}
static uint8_t inb(unsigned port){return port==4?(phase&255):port==5?(phase>>8):0;}
static void ata_atapi_read_bytes(unsigned io,uint8_t *out,uint16_t n){
 assert(n==phase && n<=remaining);phases++;
 for(unsigned i=0;i<n;i++)out[i]=(uint8_t)(position++*37U);
 remaining-=n;
}
static void ata_delay_400ns(unsigned ctrl){}
static uint8_t ata_status(unsigned io){return 0;}
static void ata_soft_reset_channel(ata_device_t *d){assert(!"unexpected retry");}
static void ata_select_packet(ata_device_t *d){}
static bool ata_wait_not_busy(unsigned io){return true;}
static void kprintf(const char *fmt,...){}
'''
read+=function(ata,'static bool ata_atapi_read_block(')
read+=r'''
int main(void){
 static uint8_t output[255*2048];ata_device_t dev={0,0,2048,"cd0"};
 block_device_t block={&dev};unsigned counts[]={1,16,32,255};
 for(unsigned mode=0;mode<2;mode++)for(unsigned i=0;i<4;i++){
  unsigned count=counts[i],bytes=count*2048U;device_limit=mode?2048:32768;
  assert(ata_atapi_read_block(&block,123,(uint8_t)count,output));
  assert(limit==(bytes>32768?32768:bytes) && !lock && !remaining);
  assert(phases==(bytes+device_limit-1)/device_limit);
  for(unsigned j=0;j<bytes;j++)assert(output[j]==(uint8_t)(j*37U));
 }
 puts("PASS multi-sector ATAPI phases reduce handshakes and tolerate devices returning smaller phases");
}
'''
reset=base+r''' 
#define ATA_CTRL_SRST 4
static unsigned elapsed, asserted_at, released_at;
static uint8_t value;
static void outb(unsigned port,unsigned v){assert(port==1);value=v;if(v==4)asserted_at=elapsed;else released_at=elapsed;}
static void ata_delay_400ns(unsigned ctrl){assert(ctrl==1);elapsed+=400;}
static uint8_t ata_status(unsigned io){assert(io==0);assert(elapsed-released_at>=2000000);return 0;}
'''
reset+=function(ata,'static void ata_soft_reset_ports(')
reset+=r''' 
int main(void){
 ata_soft_reset_ports(0,1);
 assert(!value && released_at-asserted_at>=5000 && elapsed-released_at>=2000000);
 puts("PASS SRST assertion and post-reset settling before zero-status detection");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-atapi-ready-') as temp:
    for name,code in [('wait',wait),('capacity',capacity),('retry',retry),('probe',probe),('read',read),('reset',reset)]:
        src=Path(temp)/(name+'.c');src.write_text(code)
        binary=Path(temp)/name
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
