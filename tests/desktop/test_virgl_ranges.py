#!/usr/bin/env python3
"""Partial buffer transfers must preserve bytes outside the mapped range."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'ports/gallium/virgl-winsys.c').read_text()
transfer = source[source.index('static int bk_transfer('):source.index('static int bk_put(')]
wait = source[source.index('static void bk_wait_resource('):source.index('static bool bk_busy(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"
enum { PIPE_BUFFER, PIPE_TEXTURE_2D };
struct virgl_winsys {int unused;};
struct bk_winsys {struct virgl_winsys base;pthread_mutex_t lock;uint64_t submitted,completed;bool failed;};
struct virgl_hw_res {uint32_t id,size,target;void *map;uint64_t last_submit,last_use;};
static unsigned syncs;
static bool bk_sync_locked(struct virgl_winsys *ws){struct bk_winsys *w=(void *)ws;syncs++;w->completed=w->submitted;return true;}
struct pipe_box {int x,y,z,width,height,depth;};
static unsigned calls,bytes;
static uint8_t backing[1024];
static int bk_call_locked(struct virgl_winsys *ws,bles_virgl_t *a,void *p,uint32_t n) {
 struct bk_winsys *w=(void *)ws;calls++;bytes=n;assert(a->p[11]+n<=sizeof(backing));
 if(a->op==BLES_VIRGL_TRANSFER_WRITE || a->op==BLES_VIRGL_TRANSFER_WRITE_ASYNC)memcpy(backing+a->p[11],p,n);
 else memcpy(p,backing+a->p[11],n);
 if(a->op==BLES_VIRGL_TRANSFER_WRITE_ASYNC)w->submitted++;else w->completed=w->submitted;
 assert(a->p[10]==(a->p[11]?a->p[11]:a->p[10]));
 return 0;
}
''' + wait + transfer + r'''
int main(void) {
 struct bk_winsys ws={.lock=PTHREAD_MUTEX_INITIALIZER};
 uint8_t map[1024];memset(map,0x11,sizeof(map));memset(backing,0x22,sizeof(backing));
 struct virgl_hw_res r={.id=7,.size=sizeof(map),.target=PIPE_BUFFER,.map=map};
 struct pipe_box box={.x=200,.width=84,.height=1,.depth=1};
 assert(!bk_transfer(&ws.base,&r,&box,0,0,200,0,true));assert(bytes==84 && calls==1);
 for(unsigned i=0;i<sizeof(map);i++)assert(backing[i]==(i>=200 && i<284?0x11:0x22));
 memset(map,0x33,sizeof(map));memset(backing+200,0x44,84);
 assert(!bk_transfer(&ws.base,&r,&box,0,0,200,0,false));assert(bytes==84 && calls==2);
 for(unsigned i=0;i<sizeof(map);i++)assert(map[i]==(i>=200 && i<284?0x44:0x33));
 box.x=1023;box.width=1;assert(!bk_transfer(&ws.base,&r,&box,0,0,1023,0,true));
 unsigned valid=calls;box.width=2;assert(bk_transfer(&ws.base,&r,&box,0,0,1023,0,true)<0);
 box.width=-1;assert(bk_transfer(&ws.base,&r,&box,0,0,1023,0,true)<0);
 box.width=1;assert(bk_transfer(&ws.base,&r,&box,0,0,0,0,true)<0);
 assert(bk_transfer(&ws.base,&r,&box,0,0,1023,1,true)<0);assert(calls==valid);
 ws.submitted=2;ws.completed=1;
 bk_wait_resource(&ws.base,&r);assert(calls==valid && !syncs);
 r.last_submit=2;bk_wait_resource(&ws.base,&r);assert(syncs==1 && ws.completed==2);
 bk_wait_resource(&ws.base,&r);assert(syncs==1);
 r.target=PIPE_TEXTURE_2D;box=(struct pipe_box){.x=4,.y=4,.width=2,.height=2,.depth=1};
 assert(!bk_transfer(&ws.base,&r,&box,64,1024,272,0,true));assert(bytes==sizeof(map));
 memset(map,0x5a,sizeof(map));
 assert(!bk_transfer(&ws.base,&r,&box,64,1024,272,0,true));
 assert(r.last_submit==ws.submitted && ws.submitted==ws.completed);
 memset(map,0x6b,sizeof(map));for(unsigned i=0;i<sizeof(map);i++)assert(backing[i]==0x5a);
 assert(!bk_transfer(&ws.base,&r,&box,64,1024,272,0,true));
 for(unsigned i=0;i<sizeof(map);i++)assert(backing[i]==0x6b);
 assert(!bk_transfer(&ws.base,&r,&box,64,1024,272,0,false));
 assert(ws.submitted==ws.completed);
 puts("PASS: buffer subranges preserve neighbours, reject overflow, retain texture layout, avoid redundant waits");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-virgl-ranges-') as directory:
    c = Path(directory)/'test.c'; c.write_text(code)
    exe = Path(directory)/'test'
    subprocess.run(['gcc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I'+str(ROOT),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
