#!/usr/bin/env python3
"""Exercise interleaved GPU fences and synchronous virtqueue ACKs."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/graphics/virtio_gpu_dvr.c').read_text()
header=re.search(r'typedef struct PACKED \{[^}]*\} virtio_gpu_ctrl_hdr_t;',source).group()
start=source.index('static bool virtio_queue_complete_deferred(virtio_queue_t *queue,uint32_t head);')
end=source.index('static uint32_t virtio_queue_deferred_limit(',start)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PACKED __attribute__((packed))
#define VIRTIO_GPU_FLAG_FENCE 1U
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100U
#define VIRTIO_GPU_DEFERRED_SLOTS 4U
#define VIRTIO_GPU_QUEUE_TIMEOUT_SECONDS 1U
#define VIRTIO_GPU_QUEUE_IOWAIT_INTERVAL 4U
#define VIRTIO_GPU_QUEUE_SPIN_LIMIT 32U
#define VIRTQ_ERROR_NOT_READY 1
#define VIRTQ_ERROR_TIMEOUT 2
#define VIRTQ_ERROR_BAD_USED_ID 3
#define VIRTQ_ERROR_BUSY 4
#define VIRTQ_ERROR_NONE 0
#define kprintf(...) ((void)0)
'''+header+r'''
typedef struct {uint32_t id,len;} virtq_used_elem_t;
struct used_ring {volatile uint16_t idx;virtq_used_elem_t ring[16];};
typedef struct {
    bool ready;uint32_t owner,last_error,last_used_id,used_shadow,size,deferred_count;
    struct used_ring *used;
    struct {void *storage,*response;uint32_t fence;} deferred[VIRTIO_GPU_DEFERRED_SLOTS];
} virtio_queue_t;
static struct {uint32_t completed_fence;} g_virtio={7};
static uint32_t raw_context,raw_fence;
static void vg_raw_fence_complete(uint32_t context,uint32_t fence){raw_context=context;raw_fence=fence;}
static unsigned freed;
static void release(void *p){freed++;free(p);}
#define kfree release
static uint32_t pit_get_ticks(void){return 0;}
static uint32_t pit_get_frequency_hz(void){return 100;}
static void io_wait(void){}
static void task_yield(void){}
static void virtio_barrier(void){__sync_synchronize();}
'''+source[start:end]+r'''
static void pending(virtio_queue_t *q,unsigned slot,unsigned fence) {
    virtio_gpu_ctrl_hdr_t *p=calloc(1,sizeof(*p));
    p->type=0x1100;p->flags=1;p->fence_id=fence;
    q->deferred[slot].storage=q->deferred[slot].response=p;
    q->deferred[slot].fence=fence;q->deferred_count++;
}
int main(void) {
    struct used_ring used={0};virtio_queue_t q={.ready=true,.size=16,.used=&used};
    pending(&q,0,8);pending(&q,1,9);
    /* In-flight fences must not stall submission of a new control packet. */
    assert(virtio_queue_acquire(&q));assert(q.deferred_count==2 && !q.used_shadow);
    virtio_queue_unlock(&q);
    used.ring[0].id=6;used.ring[1].id=0;used.ring[2].id=4;used.idx=3;
    assert(virtio_queue_wait_used(&q,0));
    assert(q.used_shadow==2 && q.last_used_id==0 && q.deferred_count==1 && freed==1);
    assert(g_virtio.completed_fence==9);
    assert(virtio_queue_acquire(&q));assert(!q.deferred_count && freed==2);
    virtio_queue_unlock(&q);assert(g_virtio.completed_fence==9);
    /* A stale/forged fence response must not retire a newer GPU dependency. */
    pending(&q,0,10);((virtio_gpu_ctrl_hdr_t *)q.deferred[0].response)->fence_id=11;
    used.ring[3].id=4;used.idx=4;
    assert(!virtio_queue_wait_used(&q,0));assert(!q.ready && g_virtio.completed_fence==9);
    assert(freed==3);
    q.ready=true;pending(&q,1,12);used.ring[4].id=15;used.idx=5;
    assert(!virtio_queue_wait_used(&q,0));assert(!q.ready && q.deferred_count==1 && freed==3);
    free(q.deferred[1].storage);
    memset(&q,0,sizeof(q));memset(&used,0,sizeof(used));q.ready=true;q.size=16;q.used=&used;
    pending(&q,0,0xf0000000U);((virtio_gpu_ctrl_hdr_t *)q.deferred[0].storage)->ctx_id=2;
    used.ring[0].id=4;used.ring[1].id=0;used.idx=2;
    assert(virtio_queue_wait_used(&q,0));assert(raw_context==2 && raw_fence==0xf0000000U);
    assert(g_virtio.completed_fence==9 && !q.deferred_count);
    puts("PASS: out-of-order fence/ACK completion, nonblocking submission, monotonic retirement and DMA quarantine");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-virtqueue-') as directory:
    c=Path(directory)/'test.c';c.write_text(code);exe=Path(directory)/'test'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
