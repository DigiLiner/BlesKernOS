#!/usr/bin/env python3
"""Exercise actual winsys submission/barrier policy against a delayed host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'ports/wine/graphics/virgl-winsys.c').read_text()
functions = source[source.index('static int bk_ioctl('):source.index('static void bk_destroy(')]
functions += source[source.index('static void bk_wait_resource('):source.index('static uint32_t bk_size(')]
functions += source[source.index('static bool bk_fence_wait('):source.index('static int bk_caps(')]
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
struct virgl_winsys {int unused;};
struct bk_winsys {struct virgl_winsys base;int fd;pthread_mutex_t lock;bool async,failed;uint64_t submitted,completed;};
struct virgl_hw_res {uint64_t last_submit,last_use;};
struct virgl_cmd_buf {uint32_t *buf,cdw;};
struct bk_command {struct virgl_cmd_buf base;struct virgl_winsys *ws;struct virgl_hw_res **refs;unsigned count,capacity,words;bool failed;};
struct pipe_reference {int count;};
struct pipe_fence_handle;
struct bk_fence {struct pipe_reference reference;};
static void pipe_reference_init(struct pipe_reference *r,int n){r->count=n;}
static void bk_release_commands(struct bk_command *c){c->count=0;}
static unsigned last_op,calls,barriers;
static bool host_pending,fail_wait;
static int fake_ioctl(int fd,unsigned request,bles_virgl_t *a) {
 (void)fd;assert(request==BLES_GFX_VIRGL);calls++;last_op=a->op;
 if(a->op==BLES_VIRGL_SUBMIT_ASYNC || a->op==BLES_VIRGL_TRANSFER_WRITE_ASYNC)host_pending=true;
 else if(a->op==BLES_VIRGL_WAIT){barriers++;if(fail_wait)return -1;host_pending=false;}
 else if(a->op==BLES_VIRGL_SUBMIT || a->op==BLES_VIRGL_TRANSFER_READ)host_pending=false;
 else assert(0);
 return 0;
}
#define ioctl fake_ioctl
''' + functions + r'''
int main(void) {
 struct bk_winsys ws={.lock=PTHREAD_MUTEX_INITIALIZER,.async=true};
 struct virgl_hw_res r={0};struct virgl_hw_res *refs[]={&r};uint32_t word=0;
 struct bk_command c={.base={&word,1},.refs=refs,.count=1,.ws=&ws.base};
 assert(!bk_submit(&ws.base,&c.base,NULL));assert(last_op==BLES_VIRGL_SUBMIT_ASYNC && host_pending);
 assert(r.last_submit==1 && bk_busy(&ws.base,&r));
 bk_wait_resource(&ws.base,&r);assert(!host_pending && barriers==1 && !bk_busy(&ws.base,&r));
 bk_wait_resource(&ws.base,&r);assert(barriers==1);
 c.base.cdw=1;c.count=1;assert(!bk_submit(&ws.base,&c.base,NULL));
 bles_virgl_t read={.op=BLES_VIRGL_TRANSFER_READ};assert(!bk_call(&ws.base,&read,NULL,0));
 assert(!bk_busy(&ws.base,&r));bk_wait_resource(&ws.base,&r);assert(barriers==1);
 struct pipe_fence_handle *f=NULL;c.base.cdw=1;c.count=1;
 assert(!bk_submit(&ws.base,&c.base,&f));assert(last_op==BLES_VIRGL_SUBMIT && !host_pending && f);
 assert(bk_fence_wait(&ws.base,f,0));free(f);
 c.base.cdw=1;c.count=1;assert(!bk_submit(&ws.base,&c.base,NULL));
 c.base.cdw=0;c.count=0;assert(!bk_submit(&ws.base,&c.base,&f));assert(barriers==2 && !host_pending);
 assert(bk_fence_wait(&ws.base,f,0));free(f);
 ws.async=false;c.base.cdw=1;c.count=1;assert(!bk_submit(&ws.base,&c.base,NULL));
 assert(last_op==BLES_VIRGL_SUBMIT && !host_pending && !bk_busy(&ws.base,&r));
 bles_virgl_t upload={.op=BLES_VIRGL_TRANSFER_WRITE_ASYNC};uint64_t previous=ws.submitted;
 assert(!bk_call(&ws.base,&upload,NULL,0) && ws.submitted==previous+1 && ws.completed==previous && host_pending);
 f=bk_fence(&ws.base,-1);assert(f && !host_pending && ws.completed==ws.submitted);free(f);
 ws.async=true;c.base.cdw=1;c.count=1;assert(!bk_submit(&ws.base,&c.base,NULL));
 fail_wait=true;assert(!bk_fence(&ws.base,-1));assert(ws.failed);
 unsigned before=calls;c.base.cdw=1;c.count=1;assert(bk_submit(&ws.base,&c.base,NULL)<0);assert(calls==before);
 assert(!bk_fence(&ws.base,4));assert(bk_fence_fd(&ws.base,NULL)<0);
 puts("PASS: async map/readback barriers, completed public fences with zero-timeout polling, legacy fallback and failure handling");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-virgl-async-') as directory:
    c = Path(directory)/'test.c'; c.write_text(code)
    exe = Path(directory)/'test'
    subprocess.run(['gcc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-pthread','-I'+str(ROOT),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
