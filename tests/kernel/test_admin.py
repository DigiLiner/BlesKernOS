#!/usr/bin/env python3
"""Exercise actual admin snapshot/config functions with simulated durable storage."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/core/admin.c').read_text()
production=s[s.index('static kspinlock_t lock'):s.index('bool bk_memory_detail(')]
production+=s[s.index('static void load_config('):s.index('static void save_events(')]
header=(root/'sdk/include/bleskernos_admin.h').read_text()
harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define ADMIN_CONFIG "/STARTUP.INI"
#define SMP_MAX_CPUS 4
#define KSPINLOCK_INITIALIZER 0
#define KMUTEX_INITIALIZER 0
typedef int kspinlock_t;
typedef int kmutex_t;
#define kstrncpy strncpy
#define kstrlen strlen
#define kstrncmp strncmp
#define kstrcmp strcmp
#define kmemset memset
#define kfree free
static char durable[8192], pending[8192];
static uint32_t durable_size, pending_size, caller=99, process=99;
static bool disk_failed;
static uint32_t pit_get_frequency_hz(void){return 100;}
static uint32_t pit_get_ticks(void){return 200;}
static uint32_t smp_cpu_index(void){return 0;}
static uint32_t task_current_pid(void){return caller;}
static uint32_t task_current_process_id(void){return process;}
static uint32_t kspin_lock_irqsave(kspinlock_t *l){(void)l;return 0;}
static void kspin_unlock_irqrestore(kspinlock_t *l,uint32_t f){(void)l;(void)f;}
static bool kspin_try_lock_irqsave(kspinlock_t *l,uint32_t *f){*f=0;(void)l;return true;}
static void kmutex_lock(kmutex_t *l){(void)l;}
static void kmutex_unlock(kmutex_t *l){(void)l;}
static bool copy_from_user(void *d,const void *s,size_t n){if(!s)return false;memcpy(d,s,n);return true;}
static bool copy_to_user(void *d,const void *s,size_t n){if(!d)return false;memcpy(d,s,n);return true;}
static bool vfs_write_all(const char *p,const void *d,uint32_t n){(void)p;if(disk_failed)return false;assert(n<=sizeof(pending));memcpy(pending,d,n);pending_size=n;return true;}
static bool vfs_rename(const char *a,const char *b){(void)a;(void)b;if(disk_failed)return false;memcpy(durable,pending,pending_size);durable_size=pending_size;return true;}
static bool vfs_read_all(const char *p,void **d,uint32_t *n){(void)p;*d=malloc(durable_size);memcpy(*d,durable,durable_size);*n=durable_size;return true;}
'''+header+production+r'''
static bk_startup_info_t custom(const char *name){bk_startup_info_t e={0};strcpy(e.name,name);strcpy(e.path,"/APP.BEX");e.timeout_ms=1000;e.service=1;return e;}
int main(void){
 admin_init();storage=true;
 for(unsigned i=0;i<200;i++)admin_emit(BK_EVENT_DRIVER,0,"TEST","bounded event",i);
 assert(bk_event_count()==128);
 bk_event_info_t event;assert(bk_event_get(0,&event)&&event.sequence==201&&event.pid==199);
 assert(bk_event_get(127,&event)&&event.sequence==74);
 assert(!bk_event_get(128,&event)&&!bk_event_get(0,NULL));
 caller=777;process=42;
 const char *line="[APP] ERROR worker failure\n";
 for(const char *p=line;*p;p++)admin_capture_char(*p);
 assert(bk_event_get(0,&event)&&event.pid==42&&event.severity==BK_EVENT_ERROR);
 caller=process=99;
 assert(!bk_startup_save(NULL));
 bk_startup_info_t a=custom("A");strcpy(a.path,"relative");assert(!bk_startup_save(&a));strcpy(a.path,"/A.BEX");
 assert(bk_startup_save(&a));a.id=4;
 bk_startup_info_t b=custom("B");b.dependencies=1U<<3;assert(bk_startup_save(&b));
 a.dependencies=1U<<4;assert(!bk_startup_save(&a));
 assert(!bk_startup_remove(4));assert(bk_startup_remove(5));
 bk_startup_info_t c=custom("C");c.id=6;assert(bk_startup_save(&c));assert(entry_count==6);
 bk_startup_info_t d=custom("D");d.id=7;assert(bk_startup_save(&d));a.dependencies=1U<<6;assert(bk_startup_save(&a));
 disk_failed=true;bk_startup_info_t bad=custom("disk failure");assert(!bk_startup_save(&bad)&&entry_count==7);disk_failed=false;
 entries[5].last_result=77;strcpy(entries[5].last_error,"BOOT_FAILURE");assert(persist());
 memset(entries,0,sizeof(entries));entry_count=0;admin_init();load_config();
 assert(entry_count==7 && !entries[4].name[0] && !strcmp(entries[5].name,"C"));
 assert(entries[3].dependencies==(1U<<6));assert(entries[5].last_result==77&&!strcmp(entries[5].last_error,"BOOT_FAILURE"));
 bk_startup_info_t reused=custom("reused");assert(bk_startup_save(&reused)&&entry_count==7&&!strcmp(entries[4].name,"reused"));
 admin_adopt(4,99);assert(bk_service_ready(4));assert(entries[3].state==BK_SERVICE_RUNNING);
 assert(!bk_service_ready(1));assert(!bk_service_control(1,BK_SERVICE_STOP));
 assert(bk_service_control(4,BK_SERVICE_STOP));assert(!bk_service_control(4,BK_SERVICE_STOP));
 a=entries[3];strcpy(a.path,"/replacement.BEX");assert(!bk_startup_save(&a));
 assert(!bk_startup_remove(4));
 a=entries[0];a.enabled=0;assert(bk_startup_save(&a)&&entries[0].enabled);
 puts("PASS bounded events, user snapshots, paths, dependency cycles, tombstones, forward dependencies, disk rollback, readiness ownership and critical controls");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-admin-') as folder:
 p=Path(folder);(p/'test.c').write_text(harness)
 subprocess.run(['gcc','-O2','-Wall','-Wno-unused-variable','-Wno-unused-function','-Wno-stringop-truncation',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
