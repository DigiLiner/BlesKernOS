#!/usr/bin/env python3
"""Verify shared-CR3 shootdowns and generation acknowledgement on switching."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
src=(root/'kernel/mm/paging.c').read_text()
mask=src[src.index('static uint32_t paging_shootdown_mask('):src.index('\nstatic uint32_t paging_kick_generation(')]
switch=src[src.index('void paging_switch_process('):src.index('\nuint32_t paging_user_physical(')]
asm='''__asm__ volatile ("movl %0,%%cr3" : :
            "r"((uint32_t)(uintptr_t)directory) : "memory")'''
assert switch.count(asm)==1
switch=switch.replace(asm,'mock_load(directory)')
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
typedef struct {uint32_t *directory;} paging_process_space_t;
static uint32_t kernel_page_directory[1024],private_directory[1024],other_directory[1024];
static paging_process_space_t shared={private_directory},other={other_directory};
static uint32_t g_tlb_generation=1,g_tlb_seen[4],g_active_process[4],*current=kernel_page_directory;
static int g_paging_lock;
static unsigned reloads;
static bool concurrent_edit;
static uint32_t paging_cpu(void) {return 0;}
static bool smp_scheduler_started(void) {return true;}
static uint32_t smp_cpu_count(void) {return 4;}
static bool smp_cpu_online(uint32_t cpu) {(void)cpu;return true;}
static uint32_t kspin_lock_irqsave(int *p) {(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f) {(void)p;(void)f;}
static paging_process_space_t *paging_find_space(uint32_t id) {
    return id==1 || id==2?&shared:id==3?&other:NULL;
}
static uint32_t *paging_current_directory(void) {return current;}
static void mock_load(uint32_t *directory) {
    current=directory;++reloads;
    if(concurrent_edit)++g_tlb_generation;
}
''' + mask + switch + r'''
int main(void) {
    g_active_process[1]=2; /* borrowing pid 1's address space */
    g_active_process[2]=3; /* unrelated private CR3 */
    g_active_process[3]=1;
    assert(paging_shootdown_mask(1)==((1U<<1)|(1U<<3)));
    assert(paging_shootdown_mask(2)==((1U<<1)|(1U<<3)));
    assert(paging_shootdown_mask(0)==14);
    paging_switch_process(1);
    assert(reloads==1 && g_tlb_seen[0]==1);
    paging_switch_process(2); /* same CR3, clean generation */
    assert(reloads==1 && g_active_process[0]==2);
    ++g_tlb_generation;
    paging_switch_process(1); /* same CR3, stale generation */
    assert(reloads==2 && g_tlb_seen[0]==2);
    concurrent_edit=true;++g_tlb_generation;
    paging_switch_process(1);
    assert(reloads==3 && g_tlb_seen[0]==3 && g_tlb_generation==4);
    concurrent_edit=false;
    paging_switch_process(1);
    assert(reloads==4 && g_tlb_seen[0]==4);
    puts("PASS borrowed spaces receive shootdowns; stale CR3 reloads; concurrent edits remain unacknowledged");
}
'''
replace=src[src.index('bool paging_pager_replace('):src.index('\nuint32_t paging_process_owner(')]
strict=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define PAGE_PAGED 0x100U
#define PAGE_PRIVATE_PTE 0x400U
#define PAGE_PRESENT 1U
#define PAGE_WRITABLE 2U
#define PAGE_COPY_ON_WRITE 0x800U
bool paging_pager_replace_cow(uint32_t,uint32_t,uint32_t,bool,bool);
typedef struct {uint32_t table_count,pde_index[1],*table[1];} paging_process_space_t;
static uint32_t table[1024],generation;
static paging_process_space_t space={1,{0},{table}};
static int g_paging_lock;
static bool acknowledge;
static uint32_t kspin_lock_irqsave(int *p){(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;}
static paging_process_space_t *paging_find_space(uint32_t pid){return pid==1?&space:NULL;}
static uint32_t paging_publish_change(void){return ++generation;}
static uint32_t paging_kick_generation(uint32_t pid){(void)pid;return 2;}
static bool paging_wait_generation(uint32_t g,uint32_t m,uint32_t p){(void)g;(void)m;(void)p;return acknowledge;}
''' + replace + r'''
int main(void){
 table[1]=0x12345000U|0x507U;
 acknowledge=false;
 assert(!paging_pager_replace(1,4096,0,false));
 assert(table[1]==(0x12345000U|0x507U)); /* no frame handoff on stalled AP */
 acknowledge=true;
 assert(paging_pager_replace(1,4096,0,false)&&table[1]==0x506U);
 acknowledge=false;
 assert(paging_pager_replace(1,4096,0x98765000U,true));
 assert(table[1]==(0x98765000U|0x507U)); /* published frame must stay owned */
 table[2]=0x700U; /* PROT_NONE */
 assert(paging_pager_replace(1,8192,0x45678000U,true));
 assert(table[2]==(0x45678000U|0x701U));
 assert(!paging_pager_replace(2,4096,0,false));
 table[3]=0x12345000U|0x507U;
 assert(paging_pager_replace_cow(1,12288,0x12345000U,true,true));
 assert(table[3]==(0x12345000U|0xd05U));
 assert(paging_pager_replace_cow(1,12288,0x23456000U,true,false));
 assert(table[3]==(0x23456000U|0x507U));
 table[4]=0x12345000U|0x505U;
 assert(paging_pager_replace_cow(1,16384,0x12345000U,true,true));
 assert(table[4]==(0x12345000U|0x505U)); /* readonly never gains write access */
 /* Repeated pins retain hardware A/D without a CR3 reload or IPI. */
 table[5]=0x12345000U|0xd65U;
 uint32_t before=generation;
 assert(paging_pager_replace_cow(1,20480,0x12345000U,true,true));
 assert(table[5]==(0x12345000U|0xd65U) && generation==before);
 assert(paging_pager_replace_cow(1,20480,0x12345000U,true,false));
 assert(table[5]==(0x12345000U|0x507U) && generation==before+1);
 before=generation;acknowledge=true;
 assert(paging_pager_replace(1,20480,0,false) && generation==before+1);
 before=generation;
 assert(paging_pager_replace(1,20480,0,false) && generation==before+1);
 puts("PASS actual pager PTE rollback, acknowledged revocation, late-AP publication and PROT_NONE");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-tlb-') as d:
    c=Path(d)/'test.c';c.write_text(code)
    binary=Path(d)/'test'
    subprocess.run(['gcc','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    c.write_text(strict)
    subprocess.run(['gcc','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
