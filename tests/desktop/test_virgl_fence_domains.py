#!/usr/bin/env python3
"""Raw-context completion must not retire desktop DMA storage."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/graphics/virtio_gpu_dvr.c').read_text()
header=re.search(r'typedef struct PACKED \{[^}]*\} virtio_gpu_ctrl_hdr_t;',source).group()
start=source.index('static bool virtio_gpu_control_command(')
end=source.index('\nstatic bool virtio_gpu_resource_create(',start)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PACKED __attribute__((packed))
#define kmemset memset
#define kprintf(...) ((void)0)
#define VIRTIO_GPU_FLAG_FENCE 1U
#define VIRTQ_ERROR_TIMEOUT 1
#define VIRTQ_ERROR_BAD_USED_ID 2
#define VIRTQ_ERROR_BUSY 3
'''+header+r'''
struct queue {unsigned last_error,avail_shadow,used_shadow,last_used_id;};
static struct {bool device_ready;struct queue controlq;uint32_t completed_fence;} g_virtio={true,{0},7};
static uint32_t next_fence=7;
static uint32_t virtio_gpu_next_fence(void){return ++next_fence;}
static bool virtio_queue_submit_control(struct queue *q,const void *request,uint32_t size,void *response,uint32_t capacity) {
    (void)q;assert(size>=sizeof(virtio_gpu_ctrl_hdr_t) && capacity>=sizeof(virtio_gpu_ctrl_hdr_t));
    *(virtio_gpu_ctrl_hdr_t *)response=*(const virtio_gpu_ctrl_hdr_t *)request;
    ((virtio_gpu_ctrl_hdr_t *)response)->type=0x1100;return true;
}
'''+source[start:end]+r'''
int main(void) {
    virtio_gpu_ctrl_hdr_t request={.type=0x207,.ctx_id=2},response;
    uint32_t token=0;
    assert(virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),0x1100,true,&token));
    assert(token==8 && g_virtio.completed_fence==7);
    request=(virtio_gpu_ctrl_hdr_t){.type=0x207,.ctx_id=1};
    assert(virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),0x1100,true,&token));
    assert(token==9 && g_virtio.completed_fence==9);
    request=(virtio_gpu_ctrl_hdr_t){.type=0x207,.ctx_id=3};
    assert(virtio_gpu_control_command(&request,sizeof(request),&response,sizeof(response),0x1100,true,&token));
    assert(token==10 && g_virtio.completed_fence==9);
    puts("PASS: private Gallium completion cannot retire desktop fences or VBO storage");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-fence-domains-') as directory:
    c=Path(directory)/'test.c';c.write_text(code);exe=Path(directory)/'test'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
