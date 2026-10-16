#!/usr/bin/env python3
"""Production PIO completion is retained when swap skips durable cache flush."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/storage/ata.c').read_text().split('static bool ata_write_block_once(',1)[1].split('static bool ata_write_block_mode(',1)[0]
code=r'''#include <assert.h>
#include <stdio.h>
#undef NULL
#include "kernel/include/block.h"
#define ATA_REG_SECCOUNT0 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_COMMAND 7
#define ATA_REG_DATA 0
#define ATA_CMD_WRITE_MULTIPLE 0xc5
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_CACHE_FLUSH 0xe7
typedef struct {uint16_t io,ctrl;uint8_t multiple_sectors;} ata_device_t;
static unsigned flushes,words,completions;static bool fail_flush,fail_data;
static bool ata_select_lba28(ata_device_t *d,uint32_t l){(void)d;(void)l;return true;}
static void ata_log_failure(ata_device_t *d,const char *s,uint32_t l,uint8_t c,uint32_t a){(void)d;(void)s;(void)l;(void)c;(void)a;}
static void outb(uint16_t p,uint8_t v){if(p==0x1f7 && v==ATA_CMD_CACHE_FLUSH)flushes++;}
static void ata_delay_400ns(uint16_t p){(void)p;}
static bool ata_wait_drq(uint16_t p){(void)p;return !fail_data;}
static void outsw(uint16_t p,const void *b,uint32_t n){(void)p;(void)b;words+=n;}
static bool ata_wait_command_complete(ata_device_t *d){(void)d;completions++;return !(flushes && fail_flush);}
static bool ata_wait_not_busy(uint16_t p){(void)p;return true;}
static bool ata_write_block_once('''+source+r'''
int main(void){uint16_t page[2048]={0};ata_device_t d={0x1f0,0x3f6,4};block_device_t b={0};b.driver_data=&d;
 assert(ata_write_block_once(&b,16,8,page,1,true));assert(flushes==1 && words==2048 && completions==3);
 flushes=words=completions=0;assert(ata_write_block_once(&b,16,8,page,1,false));assert(!flushes && words==2048 && completions==2);
 fail_data=true;assert(!ata_write_block_once(&b,16,8,page,1,false));assert(!flushes && words==2048);fail_data=false;fail_flush=true;
 assert(!ata_write_block_once(&b,16,8,page,1,true));assert(flushes==1);
 assert(!ata_write_block_once(&b,0x0fffffff,8,page,1,false));assert(!ata_write_block_once(&b,0,0,page,1,true));
 puts("PASS completed sector transfers, filesystem cache flush, swap without flush, data/flush errors and LBA28 bounds");}
'''
with tempfile.TemporaryDirectory(prefix='bk-ata-') as directory:
 d=Path(directory);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror','-I'+str(root),str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
