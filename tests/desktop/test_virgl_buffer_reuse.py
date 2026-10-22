#!/usr/bin/env python3
"""A host buffer upload may be unsynchronized: fence previous GPU users first."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
s = (ROOT/'ports/gallium/virgl-winsys.c').read_text()
functions = s[s.index('static int bk_ioctl('):s.index('static void bk_destroy(')]
functions += s[s.index('static int bk_transfer('):s.index('static int bk_put(')]
functions += s[s.index('static int bk_submit('):s.index('static int bk_caps(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"
enum {PIPE_BUFFER,PIPE_TEXTURE_2D};
struct virgl_winsys {int unused;};
struct bk_winsys {struct virgl_winsys base;int fd;pthread_mutex_t lock;bool async,failed;uint64_t submitted,completed,readbacks;};
struct virgl_hw_res {uint32_t id,size,target;void *map;uint64_t last_submit,last_use;};
struct pipe_box {int x,y,z,width,height,depth;};
struct virgl_cmd_buf {uint32_t *buf,cdw;};
struct bk_command {struct virgl_cmd_buf base;struct virgl_winsys *ws;struct virgl_hw_res **refs;unsigned count,capacity,words;bool failed;};
struct pipe_fence_handle;
static struct pipe_fence_handle *bk_fence(struct virgl_winsys *ws,int fd){(void)ws;(void)fd;abort();}
static void bk_release_commands(struct bk_command *c){c->count=0;}
static uint8_t host_buffer[64],drawn[64];
static unsigned barriers,uploads,corruption,calls;
static bool gpu_read,fail_wait;
static int fake_ioctl(int fd,unsigned request,bles_virgl_t *a){
 (void)fd;assert(request==BLES_GFX_VIRGL);calls++;
 switch(a->op){
 case BLES_VIRGL_SUBMIT_ASYNC:gpu_read=true;break;
 case BLES_VIRGL_WAIT:
   barriers++;if(fail_wait)return -1;
   if(gpu_read)memcpy(drawn,host_buffer,sizeof(drawn));
   gpu_read=false;break;
 case BLES_VIRGL_TRANSFER_WRITE:
   uploads++;
   if(a->p[0]==1){
     if(gpu_read)corruption++;
     memcpy(host_buffer+a->p[11],(void *)(uintptr_t)a->buffer,a->bytes);
   }
   if(gpu_read)memcpy(drawn,host_buffer,sizeof(drawn));
   gpu_read=false; /* The WRITE fence completes uploads and earlier work. */
   break;
 default:abort();
 }
 return 0;
}
#define ioctl fake_ioctl
''' + functions + r'''
int main(void){
 (void)bk_sync_locked;(void)bk_call;
 struct bk_winsys ws={.lock=PTHREAD_MUTEX_INITIALIZER,.async=true};
 uint8_t cpu[64];memset(cpu,0x11,sizeof(cpu));
 struct virgl_hw_res r={.id=1,.size=sizeof(cpu),.target=PIPE_BUFFER,.map=cpu};
 struct pipe_box box={.width=64,.height=1,.depth=1};
 assert(!bk_transfer(&ws.base,&r,&box,0,0,0,0,true));
 memset(cpu,0x22,sizeof(cpu));assert(!bk_transfer(&ws.base,&r,&box,0,0,0,0,true));
 assert(!barriers && uploads==2);
 struct virgl_hw_res *refs[]={&r};uint32_t word=0;
 struct bk_command c={.base={&word,1},.refs=refs,.count=1,.ws=&ws.base};
 assert(!bk_submit(&ws.base,&c.base,NULL) && gpu_read);
 memset(cpu,0x33,sizeof(cpu));assert(!bk_transfer(&ws.base,&r,&box,0,0,0,0,true));
 /* A fence AFTER an upload is insufficient; the old draw must finish first. */
 assert(!corruption && barriers==1 && !gpu_read);
 for(unsigned i=0;i<64;i++)assert(drawn[i]==0x22 && host_buffer[i]==0x33);
 c.base.cdw=1;c.count=1;assert(!bk_submit(&ws.base,&c.base,NULL));
 struct virgl_hw_res texture={.id=2,.size=64,.target=PIPE_TEXTURE_2D,.map=cpu};
 assert(!bk_transfer(&ws.base,&texture,&box,0,0,0,0,true));
 assert(barriers==1 && !gpu_read && ws.completed==ws.submitted);
 struct virgl_hw_res fresh={.id=3,.size=64,.target=PIPE_BUFFER,.map=cpu};
 assert(!bk_transfer(&ws.base,&fresh,&box,0,0,0,0,true));assert(barriers==1);
 /* A completed buffer needs no extra WAIT before its next fenced upload. */
 assert(!bk_transfer(&ws.base,&r,&box,0,0,0,0,true));assert(barriers==1);
 c.base.cdw=1;c.count=1;assert(!bk_submit(&ws.base,&c.base,NULL) && gpu_read);
 unsigned before=uploads;fail_wait=true;
 assert(bk_transfer(&ws.base,&r,&box,0,0,0,0,true)<0 && ws.failed);
 assert(uploads==before && !corruption);
 before=calls;assert(bk_transfer(&ws.base,&texture,&box,0,0,0,0,true)<0 && calls==before);
 puts("PASS: fenced uploads, buffer dependency waits before writing, no redundant waits, failed waits prevent overwrites");
}

'''
with tempfile.TemporaryDirectory(prefix='bk-buffer-reuse-') as directory:
    c = Path(directory)/'test.c';c.write_text(code)
    exe = Path(directory)/'test'
    subprocess.run(['gcc','-m32','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-pthread','-I'+str(ROOT),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
