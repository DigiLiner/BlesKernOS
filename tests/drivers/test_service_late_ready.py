#!/usr/bin/env python3
"""Late live readiness after start timeout unblocks USB-root dependents safely."""
from pathlib import Path
import tempfile,subprocess
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/core/admin.c').read_text();s=s[s.index('void admin_ready('):s.index('void admin_fail(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define BK_SERVICE_STARTING 1
#define BK_SERVICE_RUNNING 2
#define BK_SERVICE_FAILED 4
#define BK_EVENT_SERVICE 1
#define kstrcmp strcmp
static unsigned entry_count=1,lock,emitted;static bool runtime_dirty,live,race;
static struct{unsigned pid,state,last_result;char last_error[96],name[32];}entries[1];
static unsigned kspin_lock_irqsave(unsigned *p){assert(!*p);*p=1;return 0;}
static void kspin_unlock_irqrestore(unsigned *p,unsigned f){(void)f;assert(*p);*p=0;}
static bool task_query_process(unsigned pid,bool *alive,void *a,void *b){(void)a;(void)b;assert(!lock&&pid==42);*alive=live;if(race)entries[0].pid=43;return true;}
static void admin_emit(unsigned a,unsigned b,const char *c,const char *d,unsigned e){(void)a;(void)b;(void)c;(void)d;(void)e;emitted++;}
'''+s+r'''
static void setup(unsigned state,const char *error){memset(entries,0,sizeof(entries));entries[0].pid=42;entries[0].state=state;strcpy(entries[0].last_error,error);emitted=0;runtime_dirty=false;race=false;}
int main(void){
 setup(BK_SERVICE_STARTING,"");admin_ready(1);assert(entries[0].state==BK_SERVICE_RUNNING&&emitted==1);
 setup(BK_SERVICE_FAILED,"SERVICE_START_TIMEOUT");live=true;admin_ready(1);assert(entries[0].state==BK_SERVICE_RUNNING&&runtime_dirty&&!entries[0].last_error[0]);
 setup(BK_SERVICE_FAILED,"SERVICE_SPAWN_FAILED");admin_ready(1);assert(entries[0].state==BK_SERVICE_FAILED&&!emitted);
 setup(BK_SERVICE_FAILED,"SERVICE_START_TIMEOUT");live=false;admin_ready(1);assert(entries[0].state==BK_SERVICE_FAILED&&!emitted);
 setup(BK_SERVICE_FAILED,"SERVICE_START_TIMEOUT");live=true;race=true;admin_ready(1);assert(entries[0].pid==43&&entries[0].state==BK_SERVICE_FAILED&&!emitted);
 puts("PASS: late service readiness, unchanged failure/dead PID, replaced PID race, lock ordering");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';e=Path(tmp)/'test';c.write_text(code)
 subprocess.run(['gcc','-O1','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
