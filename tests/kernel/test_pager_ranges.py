#!/usr/bin/env python3
"""Execute production pager range operations with observable PTE/pin mocks."""
from pathlib import Path
import subprocess
import argparse
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'kernel/mm/pager.c')
parser.add_argument('cases', nargs='*', choices=['pin', 'unmap', 'gap'])
args = parser.parse_args()
source = args.source.read_text()
pin = source[source.index('static bool pager_pin_resident('):source.index('bool pager_pin_word(')]
unpin = source[source.index('void pager_unpin('):source.index('void pager_destroy(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef struct {uint32_t frame,slot,pins,source_frame;bool alive;} pager_page_t;
typedef struct pager_region {struct pager_region *next;uint32_t count;pager_page_t *pages;} pager_region_t;
static pager_region_t *regions;
static pager_page_t pages[3];
static uint32_t base, fail_at, mutations, loads, guards;
static bool expect_high;
static int lock;
static uint32_t resident_flags[3];
static uint32_t paging_pager_entry(uint32_t pid,uint32_t a){(void)pid;return a>=base && (a-base)/4096U<3U?resident_flags[(a-base)/4096U]:0;}
#define kmemset memset
static uint32_t paging_process_owner(uint32_t pid) {return pid;}
static bool file_guard(uint32_t pid,uint32_t a,uint32_t n) {(void)pid;(void)a;(void)n;guards++;return false;}
static void file_unguard(bool g) {(void)g;}
static void kmutex_lock(int *l) {(void)l;}
static void kmutex_unlock(int *l) {(void)l;}
static pager_page_t *find(uint32_t pid,uint32_t a) {
    (void)pid;
    if(expect_high)assert(a==0xfffff000U); /* no wrap into unrelated low VAs */
    return a>=base && (a-base)/4096U<3U?&pages[(a-base)/4096U]:NULL;
}
static bool load(uint32_t pid,uint32_t a,pager_page_t *p,bool w) {
    (void)pid;(void)p;(void)w;loads++;return a!=fail_at;
}
static bool paging_pager_replace(uint32_t p,uint32_t a,uint32_t f,bool present) {
    (void)p;(void)a;(void)f;(void)present;mutations++;return true;
}
static bool paging_pager_replace_cow(uint32_t p,uint32_t a,uint32_t f,bool present,bool cow) {
    (void)p;(void)a;(void)f;(void)present;(void)cow;mutations++;return true;
}
static bool paging_process_unmap(uint32_t p,uint32_t a,uint32_t n) {
    (void)p;
    return n && !(a&4095U) && a+n>=a &&
        n<=0xffffffffU-4095U && a+n<=0xffffffffU-4095U;
}
static void mm_vm_frame_free(uint32_t f,uint32_t n) {(void)f;(void)n;}
static void swap_slot_free(uint32_t s) {(void)s;}
static void region_forget(pager_region_t *r) {(void)r;}
static void region_free(pager_region_t *r) {(void)r;}
''' + pin + unpin + r'''
int main(int argc,char **argv) {
    assert(argc==2);
    fail_at=0xdeadbeefU;
    if(!strcmp(argv[1],"pin")) {
        base=0x10000U;
        for(unsigned i=0;i<3;i++){pages[i].source_frame=0x80000U+i*4096;resident_flags[i]=pages[i].source_frame|5U|0x800U;}
        assert(pager_pin(1,base,12288,false) && !loads && !guards);
        for(unsigned i=0;i<3;i++)assert(pages[i].pins==1);
        pager_unpin(1,base,12288);
        assert(pager_pin(1,base,4096,true) && loads==1 && guards==1);
        pager_unpin(1,base,4096);
        loads=guards=0;resident_flags[0]=7U;
        assert(pager_pin(1,base,4096,true) && loads==1 && guards==1); /* borrowed source stays private on write */
        pager_unpin(1,base,4096);
        loads=guards=0;resident_flags[1]=4U;
        assert(pager_pin(1,base,8192,false) && loads==2 && guards==1);
        assert(pages[0].pins==1 && pages[1].pins==1); /* no preflight pin leak */
        pager_unpin(1,base,8192);
        resident_flags[1]=pages[1].source_frame|7U;pages[1].frame=pages[1].source_frame;pages[1].source_frame=0;
        loads=guards=0;assert(pager_pin(1,base+4096,4096,true) && !loads && !guards);
        pager_unpin(1,base+4096,4096);
        loads=guards=0;resident_flags[1]=0x12345007U;
        assert(pager_pin(1,base+4096,4096,false) && loads==1 && guards==1); /* a different installed frame needs the slow path */
        pager_unpin(1,base+4096,4096);loads=guards=0;
        resident_flags[1]=0x201U; /* PROT_NONE must use checked load, never fast-pin. */
        assert(pager_pin(1,base+4096,4096,false) && loads==1 && guards==1);
        pager_unpin(1,base+4096,4096);
        memset(pages,0,sizeof pages);memset(resident_flags,0,sizeof resident_flags);loads=guards=0;

        base=0xffffd000U;
        assert(pager_pin(1,0xfffff000U,1,false));
        assert(pages[2].pins==1);
        pager_unpin(1,0xfffff000U,1);
        assert(!pages[2].pins);
        assert(pager_pin(1,0xfffffffeU,1,true));
        pager_unpin(1,0xfffffffeU,1);
        assert(!pages[2].pins);
        assert(!pager_pin(1,0xffffffffU,2,false));
        assert(!pages[2].pins);
        assert(pager_pin(1,0,0,false));
        base=0x10000U;fail_at=base+4096U;
        assert(!pager_pin(1,base,8192,false));
        assert(!pages[0].pins && !pages[1].pins);
        fail_at=0xdeadbeefU;
        assert(pager_pin(1,base+4095U,4098U,true));
        assert(pages[0].pins==1 && pages[1].pins==1 && pages[2].pins==1);
        pager_unpin(1,base+4095U,4098U);
        assert(!pages[0].pins && !pages[1].pins && !pages[2].pins);
    } else if(!strcmp(argv[1],"gap")) {
        base=0x10000U;expect_high=true;
        assert(pager_pin(1,0xfffff000U,1,false));
        assert(!loads); /* user_access_ok subsequently rejects this gap */
    } else {
        base=0x10000U;pages[0].frame=0x80000U;pages[0].alive=true;
        assert(!pager_unmap(1,base+1,4096));
        assert(!mutations && !guards && pages[0].frame==0x80000U);
        assert(!pager_unmap(1,base,0));
        assert(!pager_unmap(1,0xfffff000U,8192));
        assert(!pager_unmap(1,0xfffff000U,1));
        assert(!pager_unmap(1,base,0xffffffffU));
        assert(!mutations && !guards);
        pages[0].pins=1;
        assert(!pager_unmap(1,base,4096));
        assert(!mutations && pages[0].frame==0x80000U);
        pages[0].pins=0;
        assert(pager_unmap(1,base,4096));
        assert(mutations==1 && !pages[0].alive && !pages[0].frame);
        mutations=0;pages[0].alive=pages[1].alive=true;
        pages[0].frame=0x80000U;pages[1].frame=0x81000U;
        assert(pager_unmap(1,base,4097));
        assert(mutations==2 && !pages[0].alive && !pages[1].alive);
    }
    puts("PASS production pager range, pin balance and mutation checks");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-pager-ranges-') as directory:
    c = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    c.write_text(code)
    subprocess.run(['gcc', '-O2', '-Wall', '-Werror', str(c), '-o', str(binary)], check=True)
    for case in args.cases or ['pin', 'unmap', 'gap']:
        subprocess.run([str(binary), case], check=True)
