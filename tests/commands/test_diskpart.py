#!/usr/bin/env python3
"""Actual partition editor: owned in-memory MBRs only; verify rejection is inert."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/api/command_api.inc').read_text();s=s[s.index('static void command_put32('):]
h=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define BLOCK_MAX_DEVICES 24
#define BFS4_BOOT_SECTORS 4096U
#define BFS4_MAGIC 0x34534642U
#define kmemcpy memcpy
#define kmemcmp memcmp
#define kmemset memset
#define kstrlen strlen
#define kstrncpy strncpy
#define kstrncmp strncmp
struct block_device;
typedef int (*read_fn)(void);
typedef struct block_device {char name[8];uint32_t base_lba,sector_size,sector_count;bool read_only;void *driver_data;read_fn read,write;} block_device_t;
typedef struct {block_device_t *device;} fat_fs_t;
static block_device_t devices[24];static uint8_t disk[512];static uint32_t writes,count=2;static bool busy,read_fail,write_fail,bad_verify,bfs_volume;static block_device_t *active;
static int transport(void){return 0;}
static block_device_t *block_get(const char *name){for(unsigned i=0;i<count;i++)if(!strcmp(devices[i].name,name))return &devices[i];return NULL;}
static uint32_t block_count(void){return count;}
static block_device_t *block_at(uint32_t i){return i<count?&devices[i]:NULL;}
static bool block_read(block_device_t *d,uint32_t lba,uint8_t n,void *out){assert((lba==0||lba==4096)&&n==1);if(read_fail)return false;if(lba==4096){memset(out,0,512);if(bfs_volume){uint32_t magic=BFS4_MAGIC;memcpy(out,&magic,4);}return true;}memcpy(out,disk,512);if(bad_verify&&writes)((uint8_t*)out)[50]^=1;return true;}
static bool block_write(block_device_t *d,uint32_t lba,uint8_t n,const void *in){assert(lba==0&&n==1);if(write_fail)return false;memcpy(disk,in,512);writes++;return true;}
static bool block_register_view(const char *name,block_device_t *d,uint32_t first,uint32_t sectors){if(count>=24)return false;block_device_t *v=&devices[count++];*v=*d;strcpy(v->name,name);v->base_lba=first;v->sector_count=sectors;return true;}
static bool block_unregister(const char *name){block_device_t *v=block_get(name);if(!v)return false;v->name[0]=0;return true;}
static bool vfs_get_fs_info(fat_fs_t *f){f->device=active;return active!=NULL;}
static bool vfs_volume_busy(const char *name){return busy;}
static bool vfs_sync_all(void){return true;}
static uint32_t public_read_le32(const uint8_t *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
'''
tests=r'''
static void reset(void){memset(devices,0,sizeof(devices));memset(disk,0,sizeof(disk));count=2;writes=0;busy=read_fail=write_fail=bad_verify=bfs_volume=false;active=NULL;for(int i=0;i<2;i++){snprintf(devices[i].name,8,"ata%d",i);devices[i].sector_size=512;devices[i].sector_count=262144;devices[i].read=devices[i].write=transport;devices[i].driver_data=(void*)(uintptr_t)(i+1);}}
#define REJECT(expr) do {uint8_t before[512];memcpy(before,disk,512);unsigned n=writes;assert((expr)<0);assert(writes==n&&!memcmp(before,disk,512));}while(0)
int main(void){
 reset();REJECT(bk_device_partition_edit("missing",1,2048,5000,12));
 REJECT(bk_device_partition_edit("ata1",0,2048,5000,12));
 REJECT(bk_device_partition_edit("ata1",1,2048,UINT32_MAX,12));
 REJECT(bk_device_partition_edit("ata1",1,UINT32_MAX,2,12));
 REJECT(bk_device_partition_edit("ata1",1,1024,5000,12));
 devices[1].read_only=true;REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));devices[1].read_only=false;
 active=&devices[1];REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));active=NULL;
 busy=true;REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));busy=false;
 assert(bk_device_partition_edit("ata1",1,2048,32768,12)==0);assert(writes==1&&disk[510]==0x55&&disk[511]==0xaa&&disk[450]==12);
 assert(public_read_le32(disk+454)==2048&&public_read_le32(disk+458)==32768);
 assert(block_get("ata1p1")&&block_get("ata1p1")->base_lba==2048);
 REJECT(bk_device_partition_edit("ata1",2,30000,10000,12));
 REJECT(bk_device_partition_edit("ata1p1",1,2048,5000,12));
 assert(bk_device_partition_edit("ata1",2,40000,10000,0x83)==0);
 REJECT(bk_device_partition_edit("ata1",2,60000,1000,12));
 assert(bk_device_partition_edit("ata1",1,0,0,0)==0);assert(!block_get("ata1p1"));assert(disk[466]==0x83);
 REJECT(bk_device_partition_edit("ata1",1,0,0,0));
 reset();disk[510]=0x55;disk[511]=0xaa;disk[446+4]=0xee;command_put32(disk+446+8,1);command_put32(disk+446+12,200000);
 REJECT(bk_device_partition_edit("ata1",2,2048,5000,12));
 reset();disk[11]=0;disk[12]=2;disk[510]=0x55;disk[511]=0xaa;REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));
 reset();disk[30]=7;REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));
 reset();count=24;REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));
 reset();assert(block_register_view("ata1p1",&devices[1],2048,40000));active=block_get("ata1p1");REJECT(bk_device_partition_edit("ata1",2,80000,5000,12));
 reset();bfs_volume=true;REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));
 reset();write_fail=true;REJECT(bk_device_partition_edit("ata1",1,2048,5000,12));
 reset();bad_verify=true;assert(bk_device_partition_edit("ata1",1,2048,5000,12)==-4);for(int i=0;i<512;i++)assert(!disk[i]);assert(writes==2);
 puts("PASS: real MBR editing, exact ranges, overlap/overflow/root/partition/GPT/FAT/readonly/busy guards, views, delete, readback and rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-diskpart-') as d:
 p=Path(d);(p/'test.c').write_text(h+s+tests)
 subprocess.run(['gcc','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
