#!/usr/bin/env python3
"""Regression-test production FILE operations with bounded VFS descriptors."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/libc/stdio.c').read_text()
layout=s[s.index('typedef enum {'):s.index('/* Native read-only FILE')].replace('struct FILE {','typedef struct FILE {').replace('\n};','\n} FILE;')
capacity=s[s.index('static bool file_ensure_capacity('):s.index('\nstatic void format_putc(')]
write=s[s.index('static int file_write_text('):s.index('\nFILE *fopen(')]
ops=s[s.index('FILE *fopen('):s.index('\nint vfprintf(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#define VFS_MAX_PATH 260
#define VFS_O_RDONLY 1
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EINVAL 22
#define EIO 5
#define ENOENT 2
#define ENOMEM 12
#define EACCES 13
static int test_errno;
#define errno test_errno
static int read_all_calls,open_count,offset;
static bool read_failure,close_failure;
static unsigned char value(unsigned i) {return (unsigned char)(i*37U);}
static int vfs_open(const char *path,unsigned flags) {
 assert(flags==1);if(!strcmp(path,"missing"))return -1;
 assert(!open_count);open_count++;offset=0;return 42;
}
static int32_t vfs_size(int fd) {assert(fd==42);return 18689236;}
static bool vfs_close(int fd) {
 assert(fd==42);if(close_failure){close_failure=false;return false;}
 assert(open_count==1);open_count--;return true;
}
static int32_t vfs_seek(int fd,int32_t where,unsigned origin) {
 assert(fd==42 && origin==SEEK_SET);offset=where;return where;
}
static int vfs_read(int fd,void *buf,unsigned n) {
 assert(fd==42);if(read_failure){read_failure=false;return -1;}
 for(unsigned i=0;i<n;i++)((unsigned char*)buf)[i]=value(offset+i);
 offset+=n;return n;
}
static bool vfs_read_all(const char *path,void **buf,uint32_t *len) {
 (void)path;read_all_calls++;*len=3;*buf=malloc(4);memcpy(*buf,"abc",4);return true;
}
static bool vfs_write_all(const char *path,void *buf,uint32_t len) {
 (void)path;return len==3 && !memcmp(buf,"abc",3);
}
static void vga_putchar(char c) {(void)c;}
'''+layout+r'''
typedef int kspinlock_t;
#define KSPINLOCK_INITIALIZER 0
static uint32_t current_pid=7;
static uint32_t task_current_process_id(void){return current_pid;}
static uint32_t kspin_lock_irqsave(int *p){(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;}
'''+s[s.index('static FILE *native_streams;'):s.index('typedef struct {\n    char *buffer;')]+capacity+write+'\nint fflush(FILE *);\n'+ops+r'''
int main(void) {
 FILE *abandoned=fopen("pak","rb");assert(abandoned&&open_count==1);
 libc_process_stdio_cleanup(99);assert(open_count==1);
 libc_process_stdio_cleanup(7);assert(!open_count&&!native_streams);

 FILE *f=fopen("pak","rb");assert(f && f->kind==FILE_KIND_VFS_STREAM);
 assert(f->data==NULL && !f->capacity && !read_all_calls && open_count==1);
 assert(fseek(f,0,SEEK_END)==0 && ftell(f)==18689236);
 assert(fseek(f,-13,SEEK_CUR)==0);unsigned char buf[32];
 assert(fread(buf,2,8,f)==6 && feof(f));
 for(unsigned i=0;i<13;i++)assert(buf[i]==value(18689236-13+i));
 assert(fseek(f,0,SEEK_SET)==0 && !feof(f));
 assert(fread(buf,4,4,f)==4 && ftell(f)==16);
 assert(fseek(f,-17,SEEK_CUR)==-1 && ftell(f)==16);
 assert(fseek(f,LONG_MAX,SEEK_CUR)==-1 && ftell(f)==16);
 read_failure=true;assert(fread(buf,1,4,f)==0 && ferror(f) && ftell(f)==16);
 assert(fread(buf,2,UINT_MAX,f)==0 && ftell(f)==16);
 close_failure=true;assert(fclose(f)==EOF && open_count==1);assert(fclose(f)==0 && !open_count);
 for(int i=0;i<1000;i++){f=fopen("pak","rb");assert(f && fclose(f)==0);}
 assert(!read_all_calls && !open_count && !fopen("missing","rb"));
 /* Keep update-mode behavior: it has a mutable buffered copy and flushes. */
 f=fopen("small","r+");assert(f && read_all_calls==1 && f->data);
 assert(fread(buf,1,3,f)==3 && !memcmp(buf,"abc",3));
 assert(fseek(f,0,SEEK_SET)==0 && fwrite("abc",1,3,f)==3);
 assert(fflush(f)==0 && fclose(f)==0 && !open_count);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bk-stdio-') as d:
    d=Path(d);c=d/'test.c';c.write_text(code);binary=d/'test'
    subprocess.run(['gcc','-m32','-O2','-fno-builtin','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS large readonly streaming, bounded memory, seek/EOF, read/close errors, overflow, 1000 closes, update mode')
