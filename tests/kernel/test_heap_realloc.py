#!/usr/bin/env python3
"""Run production allocator functions on a bounded, low-address host arena."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
src = (root / 'kernel/mm/memory.c').read_text()
def section(start, end):
    return src[src.index(start):src.index(end, src.index(start))]
code = r'''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#define MM_SUBSYSTEM_COUNT 8
#define HEAP_MAGIC 0xB1E5C0DE
#define U32_MAX_VALUE UINT32_MAX
#define ALIGN4(x) (((x)+3)&~3)
typedef struct heap_block {
 uint32_t magic; size_t size; bool free; uint8_t subsystem; uint32_t owner_process_id;
 struct heap_block *next,*prev;
} heap_block_t;
static heap_block_t *heap_head,*heap_exec_reservation;
static uint32_t heap_start_address,heap_end_address,current_pid=7;
static void *fake_cache;
static uint32_t mm_lock(void) {return 0;}
static void mm_unlock(uint32_t flags) {(void)flags;}
static uint32_t task_current_process_id(void) {return current_pid;}
static size_t mm_heap_size(void) {return heap_end_address-heap_start_address;}
#define kprintf(...) ((void)0)
static void mm_dump_locked(void) {}
#define kmemset memset
#define kmemcpy memcpy
extern uint32_t bk_graphics_cache_reclaim(void) __attribute__((weak));
extern uint32_t gui_content_staging_reclaim(void) __attribute__((weak));
void kfree(void *);
void *krealloc(void *,size_t);
bool mm_set_allocation_owner(void *,uint32_t);
bool mm_set_allocation_subsystem(void *,uint8_t);
''' + section('static bool mm_block_valid_locked(', '\nstatic void mm_dump_locked(void);') + section('void *kmalloc(', '\nvoid *kzalloc(') + section('static struct { void *ptr;', '\nvoid mm_get_info(') + section('bool mm_set_allocation_owner(', '\nvoid mm_swap_process_owners(') + section('size_t mm_release_process(', '\nvoid mm_resident_pages(') + r'''
uint32_t bk_graphics_cache_reclaim(void) {
 if(!fake_cache)return 0;
 uint32_t size=mm_allocation_size(fake_cache);
 void *p=fake_cache;fake_cache=NULL;kfree(p);return size;
}
static void reset(void) {
 memset(address_pins,0,sizeof(address_pins));
 heap_head=(void *)(uintptr_t)heap_start_address;
 *heap_head=(heap_block_t){.magic=HEAP_MAGIC,.size=mm_heap_size()-sizeof(*heap_head),.free=true};
 current_pid=7;
}
static void valid(void) {
 for(heap_block_t *b=heap_head;b;b=b->next) assert(mm_block_valid_locked(b));
}
int main(void) {
 void *arena=mmap(NULL,1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(arena!=MAP_FAILED && (uintptr_t)arena<UINT32_MAX-1024*1024);
 heap_start_address=(uintptr_t)arena;heap_end_address=heap_start_address+1024*1024;
 reset();
 fake_cache=kmalloc(800*1024);assert(fake_cache);
 void *live=kmalloc(100*1024);assert(live);memset(live,0x7b,100*1024);
 void *pressure=kmalloc(256*1024);assert(pressure&&!fake_cache);
 for(unsigned i=0;i<100*1024;i++)assert(((unsigned char*)live)[i]==0x7b);
 kfree(live);kfree(pressure);valid();reset();
 unsigned char *a=kmalloc(700*1024);assert(a);memset(a,0x5a,700*1024);
 assert(krealloc(a,900*1024)==a); /* old + new cannot fit */
 for(unsigned i=0;i<700*1024;i++)assert(a[i]==0x5a);
 assert(krealloc(a,64*1024)==a);assert(mm_allocation_size(a)==64*1024);
 void *b=kmalloc(900*1024);assert(b);valid();kfree(b);kfree(a);
 assert(heap_head->free && !heap_head->next);valid();
 reset();a=kmalloc(100);b=kmalloc(100);assert(a && b);memset(a,19,100);
 assert(mm_set_allocation_subsystem(a,3));current_pid=99;unsigned char *moved=krealloc(a,400);assert(moved && moved!=a);assert(((heap_block_t*)moved-1)->subsystem==3);
 assert(mm_find_block_locked(moved)->owner_process_id==7);
 for(unsigned i=0;i<100;i++)assert(moved[i]==19);
 assert(mm_release_process(7)>=500);assert(heap_head->free && !heap_head->next);valid();
 reset();a=kmalloc(128);b=kmalloc(1000*1024);assert(a && b);memset(a,23,128);
 assert(!krealloc(a,2000*1024));assert(mm_allocation_size(a)==128 && a[0]==23);valid();
 assert(!krealloc(a,UINT32_MAX));assert(mm_allocation_size(a)==128);valid();
 reset();a=kmalloc(1024);assert(mm_pin_address_allocation(a));
 assert(krealloc(a,64)==a && mm_allocation_size(a)==1024);
 assert(!krealloc(a,2048));kfree(a);assert(mm_allocation_size(a)==1024);
 mm_unpin_address_allocation(a);assert(heap_head->free && !heap_head->next);valid();
 reset();a=kmalloc(128);assert(a);memset(a,41,128);
 assert(mm_user_publish(a,7));assert(mm_find_block_locked(a)->owner_process_id==0);
 assert(!mm_user_publish(a,99));assert(mm_user_subsystem(a,3,7));assert(!mm_user_subsystem(a,3,99));
 assert(mm_set_allocation_owner(a,99));assert(!mm_user_free(a,7));assert(mm_user_free(a,99));
 current_pid=99;a=kmalloc(128);assert(a);
 assert(!mm_user_free(a,7));kfree(a);current_pid=7;a=kmalloc(128);assert(a);memset(a,41,128);
 assert(!mm_user_free(a,99) && mm_allocation_size(a)==128);
 assert(!mm_user_free(a+1,7));
 assert(!mm_user_realloc(a,2048,99) && a[0]==41);
 assert(!mm_user_heap_payload((uint32_t)(uintptr_t)a-1,1,7));
 assert(!mm_user_heap_payload((uint32_t)(uintptr_t)a,129,7));
 assert(!mm_user_heap_payload((uint32_t)(uintptr_t)a,128,99));
 assert(mm_user_heap_payload((uint32_t)(uintptr_t)a,128,7));
 assert(mm_user_heap_available((uint32_t)(uintptr_t)a+4,7)==124);
 assert(!mm_user_heap_available((uint32_t)(uintptr_t)a-1,7));
 void *payload_token=NULL;
 assert(mm_user_heap_pin((uint32_t)(uintptr_t)a+4,124,7,&payload_token) && payload_token==a);
 assert(!mm_user_heap_pin((uint32_t)(uintptr_t)a-1,1,7,&payload_token));
 mm_unpin_address_allocation(a);
 assert(!mm_user_heap_payload(UINT32_MAX-1,3,7));
 assert(mm_user_realloc(a,256,7)==a && a[0]==41);
 b=kmalloc(128);unsigned char *replacement=mm_user_realloc(a,1024,7);
 assert(replacement && replacement!=a && replacement[0]==41);
 assert(mm_user_free(replacement,7));assert(mm_user_free(NULL,99));kfree(b);valid();
 reset();void *published[MM_USER_PUBLICATIONS+1];
 for(unsigned i=0;i<MM_USER_PUBLICATIONS+1;i++){published[i]=kmalloc(4);assert(published[i]);}
 for(unsigned i=0;i<MM_USER_PUBLICATIONS;i++)assert(mm_user_publish(published[i],7));
 assert(!mm_user_publish(published[MM_USER_PUBLICATIONS],7));
 assert(mm_find_block_locked(published[MM_USER_PUBLICATIONS])->owner_process_id==7);
 for(unsigned i=0;i<MM_USER_PUBLICATIONS+1;i++){assert(mm_user_free(published[i],7));}
 valid();
 /* Repeated shrink/grow/free cycles must leave one free extent. */
 for(unsigned i=0;i<1000;i++) {
  a=kmalloc(4096);b=kmalloc(4096);kfree(b);
  assert(krealloc(a,16384)==a);assert(krealloc(a,17)==a);
  kfree(a);valid();assert(heap_head->free && !heap_head->next);
 }
 munmap(arena,1024*1024);
 puts("PASS in-place growth under pressure, shrink/coalescing, ownership, OOM, overflow, fork pins, 1000 cycles");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-realloc-') as d:
    c = Path(d)/'test.c'; c.write_text(code)
    binary = Path(d)/'test'
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
