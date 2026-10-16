#!/usr/bin/env python3
"""Run both production ET_REL loaders on valid and malformed in-memory files."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root/'kernel/exec/elf_loader.c')
parser.add_argument('--case', default='all')
args = parser.parse_args()
source = args.source.read_text()
def function(name):
    start=source.index(name)
    return source[start:source.index('\n}\n',start)+3]
constants='\n'.join(s for s in source.splitlines() if re.match(r'#define (EI_|ET_|EM_|EV_|SHT_|SHF_|SHN_|STB_|R_386_|ELF32_)',s))+'\n'
types=source[source.index('typedef struct {\n    uint8_t ident['):source.index('} PACKED elf32_rel_t;')+len('} PACKED elf32_rel_t;')]
helpers=''
for name in ['static bool elf_object_layout(', 'static bool elf_object_string_ok(']:
    if name in source:helpers+=function(name)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define PACKED __attribute__((packed))
#define VFS_O_RDONLY 1U
#define MM_SUBSYSTEM_PROCESS 1U
#define SHT_STRTAB 3U
#define kprintf(...) ((void)0)
#define kstrcmp strcmp
#define kmemcpy memcpy
static const char *g_elf_error;
static unsigned char file[1024];static uint32_t file_size,position;
static unsigned nodes,calls,fail_call;
static struct {unsigned char *p;uint32_t size;} blocks[32];
static void *kmalloc(uint32_t size) {
    if(++calls==fail_call)return NULL;
    assert(size<1024*1024); /* huge inputs must be rejected before allocation */
    for(unsigned i=0;i<32;i++)if(!blocks[i].p) {
        unsigned char *p=calloc(1,(size_t)size+64);assert(p);
        memset(p+size,0xa5,64);blocks[i].p=p;blocks[i].size=size;nodes++;return p;
    }
    abort();
}
static void *kzalloc(uint32_t size){return kmalloc(size);}
static void kfree(void *p) {
    if(!p)return;
    for(unsigned i=0;i<32;i++)if(blocks[i].p==p) {
        for(unsigned j=0;j<64;j++)assert(blocks[i].p[blocks[i].size+j]==0xa5);
        free(p);blocks[i].p=NULL;assert(nodes);nodes--;return;
    }
    abort();
}
static bool mm_set_allocation_subsystem(void *p,unsigned s){(void)p;(void)s;return true;}
static bool perfmon_enabled(void){return false;}
static __attribute__((unused)) size_t mm_heap_size(void){return 1024U*1024U;}
static int vfs_open(const char *p,unsigned flags){(void)p;(void)flags;position=0;return 1;}
static int32_t vfs_size(int fd){(void)fd;return file_size;}
static int32_t vfs_seek(int fd,int32_t offset,unsigned mode){(void)fd;(void)mode;position=offset;return offset;}
static int32_t vfs_read(int fd,void *out,uint32_t size){(void)fd;if(position>file_size || size>file_size-position)return -1;memcpy(out,file+position,size);position+=size;return size;}
static void vfs_close(int fd){(void)fd;}
''' + constants + types + function('static uint32_t elf_align(') + function('static bool elf_range_ok(') + helpers + r'''
static uint32_t elf_symbol_value(const elf32_symbol_t *s,const char *names,const uint32_t *addresses,uint16_t count,bool user) {
    (void)names;(void)user;
    return s->shndx<count && s->shndx?addresses[s->shndx]+s->value:0;
}
''' + function('static bool elf_validate(') + function('static bool elf_load(') + function('static bool elf_read_exact(') + function('static bool elf_load_streamed(') + r'''
static void seed(void) {
    memset(file,0,sizeof(file));calls=0;fail_call=0;
    elf32_header_t *h=(void *)file;
    h->ident[0]=0x7f;h->ident[1]='E';h->ident[2]='L';h->ident[3]='F';h->ident[4]=h->ident[5]=1;
    h->type=ET_REL;h->machine=EM_386;h->version=EV_CURRENT;
    h->shoff=sizeof(*h);h->shnum=5;h->shentsize=sizeof(elf32_section_t);
    elf32_section_t *s=(void *)(file+h->shoff);
    s[1]=(elf32_section_t){.type=SHT_PROGBITS,.flags=SHF_ALLOC,.offset=256,.size=4,.addralign=4};
    s[2]=(elf32_section_t){.type=SHT_SYMTAB,.offset=264,.size=2*sizeof(elf32_symbol_t),.link=3,.entsize=sizeof(elf32_symbol_t)};
    s[3]=(elf32_section_t){.type=SHT_STRTAB,.offset=320,.size=6};
    memcpy(file+320,"\0main\0",6);
    elf32_symbol_t *symbols=(void *)(file+264);symbols[1].name=1;symbols[1].shndx=1;
    file_size=360;
}
static void run(bool streamed,bool expected) {
    void *image=NULL,*entry=NULL;
    bool ok=streamed?elf_load_streamed("fixture","main",&image,&entry,true):elf_load(file,file_size,"main",&image,&entry,true);
    if(ok){assert(image && entry);kfree(((void **)image)[-1]);}
    assert(ok==expected && !nodes);
}
int main(int argc,char **argv) {
    assert(argc==2);
    for(unsigned streamed=0;streamed<2;streamed++) {
        seed();run(streamed,true);
        elf32_section_t *s=(void *)(file+sizeof(elf32_header_t));
        seed();s[1].size=1;s[4]=(elf32_section_t){.type=SHT_REL,.offset=344,.size=8,.link=2,.info=1,.entsize=8};
        ((elf32_rel_t *)(file+344))->offset=4;((elf32_rel_t *)(file+344))->info=(1U<<8)|R_386_32;
        run(streamed,false);
        if(!strcmp(argv[1],"reloc"))continue;
        seed();s[3].size=5;run(streamed,false); /* missing NUL inside table */
        seed();((elf32_symbol_t *)(file+264))[1].name=99;run(streamed,false);
        seed();s[1].addralign=3;run(streamed,false);
        seed();s[1].addralign=0x80000000U;s[1].type=SHT_NOBITS;s[1].size=0x7fffffffU;run(streamed,false);
        seed();s[1].size=0xfffffff8U;s[1].type=SHT_NOBITS;run(streamed,false);
        seed();s[2].size--;run(streamed,false);
        seed();file_size=sizeof(elf32_header_t)-1;run(streamed,false);
        for(unsigned failure=1;failure<=(streamed?5U:2U);failure++) {
            seed();fail_call=failure;run(streamed,false);
        }
    }
    puts("PASS both ET_REL loaders: valid input, tiny relocation, bounded strings, malformed alignment/tables, overflow and OOM cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-elf-bounds-') as directory:
    c=Path(directory)/'test.c';binary=Path(directory)/'test';c.write_text(code)
    subprocess.run(['gcc','-m32','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary),args.case],check=True)
