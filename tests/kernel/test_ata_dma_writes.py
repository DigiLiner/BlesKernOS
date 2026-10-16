#!/usr/bin/env python3
"""Actual DMA chunking, completion, flush policy and bounce-buffer integrity."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/storage/ata.c').read_text().split('static bool ata_dma_transfer(',1)[1].split('static void ata_try_dma(',1)[0]
code=r'''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "kernel/include/block.h"
#define ATA_SECONDARY_IO 0x170
#define ATA_REG_SECCOUNT0 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_COMMAND 7
#define ATA_CMD_WRITE_DMA 0xca
#define ATA_CMD_READ_DMA 0xc8
#define ATA_CMD_CACHE_FLUSH 0xe7
#define kmemcpy memcpy
typedef struct {uint16_t io,ctrl;bool dma;uint32_t sectors;} ata_device_t;
typedef struct {uint32_t allocation,buffer,prd[2];uint16_t base;} ata_dma_channel_t;
static ata_dma_channel_t g_ata_dma[2];
static unsigned flushes,commands;static bool error,fail_flush;static uint8_t engine;
static bool ata_select_lba28(ata_device_t *d,uint32_t l){(void)d;(void)l;return true;}
static void outb(uint16_t p,uint8_t v){if(p==0xc000)engine=v;if(p==0x1f7){if(v==ATA_CMD_CACHE_FLUSH)flushes++;if(v==ATA_CMD_WRITE_DMA || v==ATA_CMD_READ_DMA)commands++;}}
static uint8_t inb(uint16_t p){(void)p;return error?2:4;}
static void outl(uint16_t p,uint32_t v){assert(p==0xc004 && v==(uint32_t)g_ata_dma[0].prd);}
static void ata_delay_400ns(uint16_t p){(void)p;}
static bool ata_wait_command_complete(ata_device_t *d){(void)d;return !(flushes && fail_flush);}
static uint32_t pit_get_ticks(void){return 0;}
static uint32_t pit_get_frequency_hz(void){return 0;}
static bool smp_scheduler_started(void){return true;}
static void task_yield(void){assert(!"unexpected polling");}
static bool ata_dma_transfer('''+source+r'''
int main(void){void *bounce;assert(!posix_memalign(&bounce,65536,65536));g_ata_dma[0].buffer=(uint32_t)bounce;g_ata_dma[0].base=0xc000;
 ata_device_t d={0x1f0,0x3f6,true,10000};unsigned char data[129*512];memset(data,0x5a,sizeof(data));
 assert(ata_dma_transfer(&d,16,129,data,true,true));assert(commands==2 && flushes==1 && engine==0 && !memcmp(bounce,data,512));
 flushes=commands=0;assert(ata_dma_transfer(&d,16,8,data,true,false));assert(commands==1 && !flushes && engine==0 && !memcmp(bounce,data,4096));
 memset(bounce,0xa5,4096);assert(ata_dma_transfer(&d,16,8,data,false,true));for(unsigned i=0;i<4096;i++)assert(data[i]==0xa5);assert(!flushes && engine==8);
 error=true;assert(!ata_dma_transfer(&d,16,8,data,true,false));assert(!flushes && engine==0);error=false;fail_flush=true;
 assert(!ata_dma_transfer(&d,16,8,data,true,true));assert(flushes==1 && engine==0);
 assert(!ata_dma_transfer(&d,9999,8,data,true,false));free(bounce);
 puts("PASS DMA chunking/bounce integrity, completed stop, durable/volatile flush policy, controller errors and bounds");}
'''
with tempfile.TemporaryDirectory(prefix='bk-dma-') as directory:
 d=Path(directory);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror','-I'+str(root),str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
