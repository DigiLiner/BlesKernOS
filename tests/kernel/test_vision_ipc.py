#!/usr/bin/env python3
"""Production fixed-message transport: concurrency, cancellation and stale replies."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/api/vision_ipc.c').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code=r'''
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t cookie,operation,process_id,thread_id,window;
 gui_rect_t bounds;uint32_t values[6];char title[64];char path[260],extension[16];struct {char name[48],version[24],description[128],copyright[64],icon_path[64];} about;} bk_vision_request_t;
typedef pthread_mutex_t kspinlock_t;
#define KSPINLOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER
typedef struct {uint16_t depth[9];} kernel_domain_snapshot_t;
static __thread uint32_t pid,tid;
static __thread bool dropped;
static uint32_t graphical_pid=900;
static volatile bool stop_server,pause_server;
static uint32_t task_current_pid(void){return tid;}
static uint32_t task_current_process_id(void){return pid;}
static bool task_current_is_user(void){return true;}
static bool task_exit_requested(void){return false;}
static uint32_t gui_vision_owner_pid(void){return graphical_pid;}
static uint32_t kspin_lock_irqsave(kspinlock_t *mutex){assert(!pthread_mutex_lock(mutex));return 0;}
static void kspin_unlock_irqrestore(kspinlock_t *mutex,uint32_t f){(void)f;assert(!pthread_mutex_unlock(mutex));}
static void kernel_domains_drop_current(kernel_domain_snapshot_t *s){assert(!dropped);s->depth[0]=7;dropped=true;}
static void kernel_domains_restore(const kernel_domain_snapshot_t *s){assert(dropped && s->depth[0]==7);dropped=false;}
static uint32_t pit_get_ticks(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000U+t.tv_nsec/1000000U);}
static uint32_t pit_get_frequency_hz(void){return 1000;}
static void task_sleep(uint32_t n){(void)n;assert(dropped);usleep(100);}
'''+source+r'''
static void *server(void *argument) {
 (void)argument;pid=900;tid=900;
 while(!stop_server) {
  if(pause_server){usleep(100);continue;}
  bk_vision_request_t r;
  if(!bk_vision_ipc_take(&r)){usleep(100);continue;}
  assert(r.title[0]=='T' && r.values[0]==r.process_id);
  assert(bk_vision_ipc_reply(r.cookie,r.process_id^r.operation));
  assert(!bk_vision_ipc_reply(r.cookie,123));
 }
 return NULL;
}
static void *client(void *argument) {
 pid=(uint32_t)(uintptr_t)argument;tid=pid+1000;
 assert(vision_ipc_routes_user());
 for(uint32_t i=1;i<=100;i++) {
  bk_vision_request_t r={0};uint32_t result=0;
  r.operation=i;r.title[0]='T';r.values[0]=pid;
  assert(vision_ipc_call(&r,&result));assert(result==(pid^i));assert(!dropped);
 }
 return NULL;
}
static void *cancel_client(void *argument) {
 pid=(uint32_t)(uintptr_t)argument;tid=pid+1000;
 bk_vision_request_t r={0};uint32_t result=42;r.operation=10;r.title[0]='T';r.values[0]=pid;
 assert(vision_ipc_call(&r,&result) && !result && !dropped);return NULL;
}
int main(void) {
 pthread_t service,clients[8];pid=1;tid=1;
 assert(!vision_ipc_routes_user());vision_ipc_server_online(900);
 assert(!pthread_create(&service,NULL,server,NULL));
 for(uint32_t i=0;i<8;i++)assert(!pthread_create(&clients[i],NULL,client,(void *)(uintptr_t)(100+i)));
 for(uint32_t i=0;i<8;i++)assert(!pthread_join(clients[i],NULL));
 stop_server=true;assert(!pthread_join(service,NULL));
 for(uint32_t i=0;i<8;i++)assert(!pthread_create(&clients[i],NULL,cancel_client,(void *)(uintptr_t)(200+i)));
 uint32_t stale=0;
 for(uint32_t wait=0;wait<10000;wait++) {
  uint32_t count=0;pthread_mutex_lock(&lock);
  for(uint32_t i=0;i<VISION_IPC_SLOTS;i++)if(slots[i].state==1){count++;stale=slots[i].request.cookie;}
  pthread_mutex_unlock(&lock);if(count==8)break;usleep(100);
 }
 assert(stale);vision_ipc_server_offline(900);
 for(uint32_t i=0;i<8;i++)assert(!pthread_join(clients[i],NULL));
 graphical_pid=901;pid=901;tid=901;vision_ipc_server_online(901);
 assert(!bk_vision_ipc_reply(stale,42));
 bk_vision_request_t r;assert(!bk_vision_ipc_take(&r));
 for(uint32_t i=0;i<VISION_IPC_SLOTS;i++)assert(!slots[i].state);
 puts("PASS 800 concurrent requests, dropped caller locks, cancelled waits and stale reply rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-vision-ipc-') as d:
 d=Path(d);c=d/'test.c';c.write_text(code);binary=d/'test'
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror','-pthread',str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
