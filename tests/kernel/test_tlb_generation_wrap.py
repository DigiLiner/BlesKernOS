#!/usr/bin/env python3
"""Production shootdown wait must not accept stale ACKs at uint32 wrap."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'kernel/mm/paging.c')
args = parser.parse_args()
source = args.source.read_text()
body = source[source.index('static bool paging_wait_generation('):source.index('static uint32_t *paging_split_pde_locked(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
static volatile uint32_t g_tlb_seen[32];
static bool started=true,online=true;
static unsigned samples,ack_after;
static uint32_t next_ack;
static bool smp_scheduler_started(void){return started;}
static bool smp_cpu_online(uint32_t cpu) {
    assert(cpu==1);
    if(ack_after && ++samples==ack_after)g_tlb_seen[cpu]=next_ack;
    return online;
}
#define kprintf(...) ((void)0)
''' + body + r'''
int main(void) {
    g_tlb_seen[1]=0xfffffffeU;
    bool ok=paging_wait_generation(0U,2U,7U);
    if(ok)fputs("FAIL stale pre-wrap TLB acknowledged as current after wrap\n",stderr);
    assert(!ok);
    g_tlb_seen[1]=0xffffffffU;
    assert(!paging_wait_generation(1U,2U,7U));
    /* A remote CPU may already have invalidated a newer generation. */
    g_tlb_seen[1]=1;
    assert(paging_wait_generation(0xffffffffU,2U,7U));
    g_tlb_seen[1]=8;
    assert(paging_wait_generation(7U,2U,7U));
    assert(!paging_wait_generation(9U,2U,7U));
    g_tlb_seen[1]=0xfffffffeU;samples=0;ack_after=10;next_ack=0;
    assert(paging_wait_generation(0U,2U,7U) && samples==10);
    ack_after=0;started=false;
    assert(paging_wait_generation(9U,2U,7U));
    started=true;assert(paging_wait_generation(9U,0U,7U));
    online=false;assert(paging_wait_generation(9U,2U,7U));
    puts("PASS TLB generation wrap: stale ACK refusal, current/newer ACK and delayed remote invalidation");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-tlb-wrap-') as directory:
    c = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    c.write_text(code)
    subprocess.run(['gcc', '-O2', '-Wall', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
