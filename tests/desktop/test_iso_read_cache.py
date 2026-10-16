#!/usr/bin/env python3
"""Exercise real ISO reader against counted, fault-injected CD block I/O."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/storage/iso9660.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
source = source[:source.index('bool iso9660_builtin_init(')]
shim = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define BLOCK_DEVICE_ATAPI 1
#define ISO9660_MAX_NAME 256
#define KSPINLOCK_INITIALIZER 0
#define kmemset memset
#define kmemcpy memcpy
#define kmemcmp memcmp
#define kstrcpy strcpy
#define kmalloc malloc
#define kfree free
#define kprintf(...) ((void)0)
typedef unsigned kspinlock_t;
typedef struct {unsigned type,sector_size,sector_count;} block_device_t;
typedef struct {char name[ISO9660_MAX_NAME];uint32_t extent,size;bool is_directory;} iso9660_entry_t;
static block_device_t device={1,2048,1024};
static unsigned calls,bulk_calls,failures,media_refreshes;
static bool reject_bulk,fail_all,fail_alloc,media_ready=true;
static unsigned char disc[1024][2048];
static bool mm_set_allocation_owner(void *p,unsigned owner){assert(p && owner==0);return true;}
static void *kzalloc(size_t n){return fail_alloc?NULL:calloc(1,n);}
static uint32_t kspin_lock_irqsave(kspinlock_t *l){assert(!*l);*l=1;return 0;}
static void kspin_unlock_irqrestore(kspinlock_t *l,uint32_t flags){(void)flags;assert(*l);*l=0;}
static block_device_t *block_get(const char *name){assert(!strcmp(name,"cd0"));return &device;}
static bool ata_refresh_media(const char *name){assert(!strcmp(name,"cd0"));media_refreshes++;return media_ready;}
static bool block_read(block_device_t *d,uint32_t lba,uint8_t n,void *out){
 assert(d==&device && n && lba+n<=1024);calls++;if(n>1)bulk_calls++;
 if(fail_all || (reject_bulk && n>1) || failures){if(failures)failures--;memset(out,0xcc,n*2048);return false;}
 memcpy(out,disc[lba],n*2048);return true;
}
'''
tests = r'''
static void reset(void){iso_cache_reset();g_iso_device=&device;g_iso_mounted=true;calls=bulk_calls=0;reject_bulk=fail_all=false;failures=0;}
static void le32(unsigned char *p,unsigned v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
int main(void){
 for(unsigned l=0;l<1024;l++)for(unsigned b=0;b<2048;b++)disc[l][b]=(l*13+b*17)&255;
 unsigned char out[65536];uint32_t got,epoch;iso9660_entry_t f={.extent=32,.size=65536};
 reset();assert(iso_impl_read_at(&f,0,out,sizeof(out),&got));assert(got==sizeof(out));
 assert(!memcmp(out,disc[32],sizeof(out)));assert(calls==1 && bulk_calls==1);
 assert(iso_impl_read_at(&f,0,out,sizeof(out),&got));assert(calls==1);
 puts("PASS sequential 64 KiB: 1 CD request; repeated read: 0 additional requests");
 reset();for(unsigned i=0;i<128;i++){assert(iso_impl_read_at(&f,i*8,out,8,&got));assert(got==8 && !memcmp(out,disc[32]+i*8,8));}
 assert(calls==1);puts("PASS 128 small reads within a sector: 1 CD request");
 reset();assert(iso_impl_read_at(&f,2040,out,32,&got));assert(got==32);
 assert(!memcmp(out,disc[32]+2040,8) && !memcmp(out+8,disc[33],24));assert(calls==2);
 assert(iso_impl_read_at(&f,65530,out,32,&got));assert(got==6 && !memcmp(out,disc[63]+2042,6));
 assert(iso_impl_read_at(&f,65536,out,32,&got) && got==0);
 reset();reject_bulk=true;assert(iso_impl_read_at(&f,0,out,sizeof(out),&got));
 assert(got==65536 && !memcmp(out,disc[32],65536) && bulk_calls==1 && !g_iso_bulk_reads);
 assert(iso_impl_read_at(&f,0,out,sizeof(out),&got));assert(calls==33);
 assert(iso_impl_read_at(&f,0,out,sizeof(out),&got));assert(bulk_calls==1);
 puts("PASS rejected bulk command falls back, caches correct bytes and stays in safe mode");
 reset();failures=1;assert(iso_read_sector(8,out));assert(calls==2 && !memcmp(out,disc[8],2048));
 reset();fail_all=true;assert(!iso_read_sector(8,out));assert(calls==3);
 fail_all=false;assert(iso_read_sector(8,out));assert(calls==4 && !memcmp(out,disc[8],2048));
 assert(iso_read_sector(8,out));assert(calls==4);
 reset();assert(iso_read_sector(8,out));iso_cache_reset();disc[8][0]^=255;
 assert(iso_read_sector(8,out));assert(calls==2 && out[0]==disc[8][0]);
 iso_cache_lookup(9,out,&epoch);iso_cache_reset();memset(out,0xcc,2048);iso_cache_publish(9,out,epoch);
 assert(iso_read_sector(9,out) && !memcmp(out,disc[9],2048));
 reset();for(unsigned i=0;i<5;i++)assert(iso_read_sector(10+i*128,out));
 assert(iso_read_sector(10,out));assert(calls==6);assert(!memcmp(out,disc[10],2048));
 puts("PASS bounded eviction, retry, failed-read exclusion and remount invalidation");
 memset(disc[20],0,2048);disc[20][0]=40;le32(disc[20]+2,32);le32(disc[20]+10,65536);
 disc[20][32]=6;memcpy(disc[20]+33,"DATA.1",6);
 reset();memset(&g_iso_root,0,sizeof(g_iso_root));g_iso_root.extent=20;g_iso_root.size=2048;g_iso_root.is_directory=true;
 for(unsigned i=0;i<100;i++){assert(iso_impl_resolve("/data.1",&f));assert(f.extent==32 && f.size==65536);}
 assert(calls==1);puts("PASS 100 repeated directory lookups: 1 CD request");
 kfree(g_iso_cache);g_iso_cache=NULL;fail_alloc=true;reset();assert(!g_iso_cache);
 assert(iso_read_sector(8,out) && !memcmp(out,disc[8],2048));assert(iso_read_sector(8,out) && calls==2);
 fail_alloc=false;reset();assert(g_iso_cache);
 memset(disc[16],0,2048);disc[16][0]=1;memcpy(disc[16]+1,"CD001",5);disc[16][6]=1;
 le32(disc[16]+158,20);le32(disc[16]+166,2048);
 assert(iso_impl_mount_default() && iso_impl_is_mounted());
 le32(disc[16]+166,0);assert(!iso_impl_mount_default() && !iso_impl_is_mounted() && !g_iso_device);
 le32(disc[16]+166,2048);media_ready=false;assert(!iso_impl_mount_default() && !iso_impl_is_mounted());
 media_ready=true;device.sector_count=0;assert(!iso_impl_mount_default() && !iso_impl_is_mounted());
 device.sector_count=1024;assert(iso_impl_mount_default() && iso_impl_is_mounted());
 puts("PASS mount publishes only validated ready ISO media and recovers on subsequent attempts");
 /* A supplementary tree has its own root and UCS-2BE records. */
 memset(disc[17],0,2048);disc[17][0]=2;memcpy(disc[17]+1,"CD001",5);disc[17][6]=1;
 memcpy(disc[17]+88,"%/E",3);disc[17][156]=34;
 le32(disc[17]+158,21);le32(disc[17]+166,2048);
 memset(disc[18],0,2048);disc[18][0]=255;memcpy(disc[18]+1,"CD001",5);disc[18][6]=1;
 memset(disc[21],0,2048);
 const char *longname="softdrawz.dll;1";unsigned len=strlen(longname);
 disc[21][0]=34+len*2;disc[21][32]=len*2;
 le32(disc[21]+2,32);le32(disc[21]+10,176128);
 for(unsigned i=0;i<len;i++)disc[21][34+i*2]=longname[i];
 assert(iso_impl_mount_default() && g_iso_joliet && g_iso_root.extent==21);
 assert(iso_impl_resolve("/SOFTDRAWZ.DLL",&f) && !strcmp(f.name,"softdrawz.dll") && f.size==176128);
 iso9660_entry_t listed[2];unsigned count;
 assert(iso_impl_list(&g_iso_root,listed,2,&count) && count==1 && !strcmp(listed[0].name,"softdrawz.dll"));
 /* Accents, long names, malformed byte counts and surrogate rejection. */
 uint8_t record[255]={0};record[0]=40;record[32]=6;
 record[34]='x';record[36]=0xe9;record[38]='z';
 assert(iso_parse_record(record,&f) && !strcmp(f.name,"x\xc3\xa9z"));
 record[32]=7;assert(!iso_parse_record(record,&f));
 record[32]=8;assert(!iso_parse_record(record,&f));
 record[32]=6;record[33]=0xd8;record[34]=0;assert(!iso_parse_record(record,&f));
 memset(record,0,sizeof(record));record[0]=194;record[32]=160;
 for(unsigned i=0;i<80;i++)record[34+i*2]='a';
 assert(iso_parse_record(record,&f) && strlen(f.name)==80);
 /* Unknown SVDs must not replace the primary tree. */
 disc[17][90]='X';assert(iso_impl_mount_default() && !g_iso_joliet && g_iso_root.extent==20);
 assert(iso_impl_resolve("/DATA.1",&f));
 puts("PASS Joliet root, long names, UTF-8, malformed records and primary fallback");
 kfree(g_iso_cache);
 puts("PASS memory allocation failure preserves uncached reads");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bk-iso-cache-') as d:
    p=Path(d)/'test.c';p.write_text(shim+source+tests)
    binary=Path(d)/'test'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined',str(p),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
