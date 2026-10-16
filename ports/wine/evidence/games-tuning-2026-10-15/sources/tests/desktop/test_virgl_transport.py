#!/usr/bin/env python3
"""Exercise production raw VirGL transport with a recording host queue."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/drivers/graphics/virtio_gpu_dvr.c').read_text()
names=['ctrl_hdr','get_capset','ctx_create','resource_create_3d','box','transfer_host_3d','cmd_submit']
types=[]
for name in names:
    pattern=r'typedef struct PACKED \{[^}]*\} virtio_gpu_'+name+r'_t;'
    types.append(re.search(pattern,source).group())
constants='\n'.join(line for line in source.splitlines() if line.startswith('#define VIRTIO_GPU_CMD_') or line.startswith('#define VIRTIO_GPU_RESP_'))
head=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PACKED __attribute__((packed))
#define kmalloc malloc
#define kfree free
#define kmemset memset
#define kmemcpy memcpy
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"
'''
stubs=r'''
static struct {uint32_t virgl_capset_size,virgl_capset_id,virgl_capset_version,virgl_context_id;bool context_init_available,virgl_commands_ready;} g_virtio={128,2,2,1,true,true};
static unsigned submitted,transfers,created,destroyed,unrefs,attached,last_context,async_submits,barriers;
static bool host_pending[128];
static bool fail_create;
static uint32_t virtio_gpu_next_resource(void){static uint32_t id=100;return ++id;}
static bool virtio_gpu_resource_attach(uint32_t id,void *p,uint32_t size){assert(id && p && size);return true;}
static bool virtio_gpu_resource_unref(uint32_t id){assert(id);assert(!host_pending[g_virtio.virgl_context_id]);unrefs++;return true;}
static bool virtio_gpu_context_resource(uint32_t op,uint32_t id){assert(id);assert(g_virtio.virgl_context_id!=1);if(op==VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE)attached++;return true;}
static bool virtio_gpu_control_command(void *request,uint32_t reqsize,void *response,uint32_t ressize,uint32_t expected,bool fenced,uint32_t *out){
    (void)response;(void)ressize;(void)expected;(void)out;
    virtio_gpu_ctrl_hdr_t *h=request;assert(reqsize>=sizeof(*h));
    last_context=h->ctx_id;
    if(h->type==VIRTIO_GPU_CMD_CTX_CREATE){assert(fenced && h->ctx_id>1);created++;}
    if(h->type==VIRTIO_GPU_CMD_CTX_DESTROY){assert(fenced && !host_pending[h->ctx_id]);destroyed++;}
    if(h->type==VIRTIO_GPU_CMD_RESOURCE_CREATE_3D && fail_create)return false;
    if(h->type==VIRTIO_GPU_CMD_SUBMIT_3D){submitted++;assert(((virtio_gpu_cmd_submit_t *)request)->size==reqsize-sizeof(virtio_gpu_cmd_submit_t));
     if(!fenced){async_submits++;host_pending[h->ctx_id]=true;}
     else {host_pending[h->ctx_id]=false;if(!((virtio_gpu_cmd_submit_t *)request)->size)barriers++;}}
    if(h->type==VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D || h->type==VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D){if(h->type==VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D)assert(fenced);host_pending[h->ctx_id]=!fenced;transfers++;}
    return true;
}
#include "kernel/drivers/graphics/virtio_gpu_gallium.inc"
int main(void){
 bles_virgl_t a={.abi=1,.op=BLES_VIRGL_CONTEXT_CREATE};assert(vg_raw_call(&a,NULL));unsigned c1=a.context;
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CONTEXT_CREATE};assert(vg_raw_call(&a,NULL));unsigned c2=a.context;assert(c1!=c2);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_RESOURCE_CREATE,.context=c1,.p={2,1,2,16,16,1,1,0,0,0,1024}};
 assert(vg_raw_call(&a,NULL));unsigned res=a.p[0];assert(attached==1 && g_virtio.virgl_context_id==1);
 unsigned char data[1024]={0};
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_TRANSFER_WRITE,.context=c2,.bytes=1024,.p={res,0,64,1024,0,0,0,16,16,1,0,0}};
 assert(!vg_raw_call(&a,data));assert(!transfers);a.context=c1;assert(vg_raw_call(&a,data));assert(last_context==c1);
 a.p[7]=17;assert(!vg_raw_call(&a,data));a.p[7]=16;a.p[1]=1;assert(!vg_raw_call(&a,data));a.p[1]=0;a.p[11]=1;assert(!vg_raw_call(&a,data));
 uint32_t cmd[]={1U<<16,0};a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_SUBMIT,.context=c1,.bytes=sizeof(cmd)};
 assert(vg_raw_call(&a,cmd));assert(submitted==1);cmd[0]=2U<<16;assert(!vg_raw_call(&a,cmd));assert(submitted==1);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CAPS};assert(vg_raw_call(&a,NULL));assert((a.p[4]&(BLES_VIRGL_FEATURE_ASYNC_SUBMIT|BLES_VIRGL_FEATURE_ASYNC_UPLOAD))==3);
 cmd[0]=1U<<16;a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_SUBMIT_ASYNC,.context=c1,.bytes=sizeof(cmd)};
 assert(vg_raw_call(&a,cmd));assert(async_submits==1 && host_pending[c1]);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_WAIT,.context=c2};assert(vg_raw_call(&a,NULL));assert(host_pending[c1] && barriers==0);
 a.context=c1;assert(vg_raw_call(&a,NULL));assert(!host_pending[c1] && barriers==1);assert(vg_raw_call(&a,NULL));assert(barriers==1);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_SUBMIT_ASYNC,.context=c1,.bytes=sizeof(cmd)};
 assert(vg_raw_call(&a,cmd));
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_TRANSFER_READ,.context=c1,.bytes=128,.p={res,0,64,1024,0,4,0,16,2,1,256,256}};
 assert(vg_raw_call(&a,data));assert(!host_pending[c1] && !vg_raw_context(c1)->pending);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_TRANSFER_WRITE_ASYNC,.context=c1,.bytes=128,.p={res,0,64,1024,0,4,0,16,2,1,256,256}};
 memset(data,0x51,128);assert(vg_raw_call(&a,data));assert(host_pending[c1] && vg_raw_context(c1)->pending);
 memset(data,0x62,128);assert(((unsigned char *)vg_raw_resource(vg_raw_context(c1),res)->backing)[256]==0x51);
 assert(vg_raw_call(&a,data));assert(((unsigned char *)vg_raw_resource(vg_raw_context(c1),res)->backing)[256]==0x62);
 a.op=BLES_VIRGL_TRANSFER_READ;assert(vg_raw_call(&a,data));assert(!host_pending[c1] && !vg_raw_context(c1)->pending);
 a.op=BLES_VIRGL_TRANSFER_WRITE_ASYNC;assert(vg_raw_call(&a,data));assert(host_pending[c1]);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_RESOURCE_DESTROY,.context=c2,.p={res}};assert(!vg_raw_call(&a,NULL));assert(!unrefs);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CONTEXT_DESTROY,.context=c1};assert(vg_raw_call(&a,NULL));assert(unrefs==1 && destroyed==1);
 a.context=c2;assert(vg_raw_call(&a,NULL));assert(destroyed==2);assert(!vg_raw_context(c1) && !vg_raw_context(c2));assert(g_virtio.virgl_context_id==1);
 puts("PASS: context isolation, transfer bounds, packet validation, legacy fenced submission, asynchronous barriers and safe cleanup");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bk-virgl-test-') as d:
    c=Path(d)/'test.c';c.write_text(head+'\n'+constants+'\n'+'\n'.join(types)+stubs)
    exe=Path(d)/'test'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
