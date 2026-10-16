#!/usr/bin/env python3
"""Exercise production VFS buffering and streaming fallback under allocation pressure."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/drivers/core/vfs.c').read_text()
part=s[s.index('static bool vfs_stream_file(const vfs_file_t *file, uint32_t size) {'):s.index('\nint vfs_write(int fd,')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define VFS_MAX_OPEN_FILES 2
#define VFS_FILE_FAT 0
#define FAT_TYPE_BFS4 4
#define VFS_STREAM_THRESHOLD (1024U*1024U)
#define VFS_WRITE_CACHE_LIMIT (64U*1024U*1024U)
#define VFS_FILE_POSITION_MAX 0x7fffffffU
#define W3P_SCOPE(a,b) ((void)0)
#define kmemset memset
#define kmemcpy memcpy
typedef struct {unsigned type;} fat_fs_t;
typedef struct {
 bool used,unlinked,cache_loaded,cache_dirty,cache_append_only;
 unsigned source,flags,offset,cache_size,cache_capacity,cache_persisted_size;
 unsigned char *cache;char path[32];
 struct {struct {fat_fs_t fs;struct {unsigned size;} entry;} fat;} entry;
} vfs_file_t;
typedef struct {uintptr_t backing;unsigned length,size;bool writable;} vfs_shared_mapping_t;
static vfs_file_t g_files[2];static unsigned g_vfs_dirty_files,ram=512U*1024U*1024U;
static unsigned char disk[9U*1024U*1024U];static unsigned disk_size,stream_calls,flush_calls;
static bool fail_load,fail_reserve,fail_sync,fail_io;
static unsigned mm_total_bytes(void){return ram;}
static bool vfs_flags_writable(unsigned f){return f==2;}
static vfs_shared_mapping_t *vfs_shared_for_file(vfs_file_t *f){(void)f;return NULL;}
static bool vfs_mapping_attach(vfs_shared_mapping_t *m,int fd,bool w){(void)m;(void)fd;(void)w;return false;}
static bool vfs_sync_path(const char *p,vfs_file_t *f){(void)p;(void)f;return !fail_sync;}
static bool vfs_flush_file(vfs_file_t *f){
 if(fail_io)return false;
 if(f->cache_dirty){memcpy(disk,f->cache,f->cache_size);disk_size=f->cache_size;f->cache_dirty=false;g_vfs_dirty_files--;flush_calls++;}
 return true;
}
static bool vfs_select_fat(const char *p,fat_fs_t *fs,const char **inner){*inner=p;fs->type=4;return true;}
static void vfs_drop_cache(vfs_file_t *f){assert(!f->cache_dirty);free(f->cache);f->cache=NULL;f->cache_capacity=f->cache_size=0;f->cache_loaded=false;}
static void vfs_private_invalidate(const char *p){(void)p;}
static bool bfs4_write_at(fat_fs_t *fs,const char *p,unsigned off,const void *data,unsigned size){
 (void)fs;(void)p;stream_calls++;if(fail_io)return false;assert(off+size<=sizeof disk);memcpy(disk+off,data,size);if(off+size>disk_size)disk_size=off+size;return true;
}
static void vfs_refresh_path_handles(const char *p,vfs_file_t *keep){(void)p;(void)keep;g_files[0].entry.fat.entry.size=disk_size;}
static bool vfs_cache_reserve(vfs_file_t *f,unsigned n,unsigned limit){
 if(n>limit || fail_reserve)return false;
 if(n<=f->cache_capacity)return true;
 unsigned cap=4096;while(cap<n)cap*=2;
 void *ptr=realloc(f->cache,cap);assert(ptr);f->cache=ptr;f->cache_capacity=cap;return true;
}
static bool vfs_cache_load(vfs_file_t *f,bool w){
 (void)w;if(f->cache_loaded)return true;if(fail_load)return false;
 unsigned n=f->entry.fat.entry.size;if(!vfs_cache_reserve(f,n,VFS_WRITE_CACHE_LIMIT))return false;
 if(n)memcpy(f->cache,disk,n);f->cache_size=f->cache_persisted_size=n;f->cache_loaded=true;return true;
}
'''+part+r'''
static void reset(void){free(g_files[0].cache);memset(g_files,0,sizeof g_files);memset(disk,0,sizeof disk);disk_size=stream_calls=flush_calls=g_vfs_dirty_files=0;fail_load=fail_reserve=fail_sync=fail_io=false;
 g_files[0].used=true;g_files[0].flags=2;g_files[0].entry.fat.fs.type=4;
}
int main(void){
 unsigned char data[4096];memset(data,0x63,sizeof data);
 reset();for(unsigned off=0;off<4U*1024U*1024U;off+=4096)assert(vfs_write_internal(0,data,4096)==4096);
 assert(!stream_calls && !flush_calls && g_files[0].cache_size==4U*1024U*1024U && g_vfs_dirty_files==1);
 assert(vfs_flush_file(&g_files[0]) && flush_calls==1 && disk_size==4U*1024U*1024U);
 for(unsigned i=0;i<disk_size;i++)assert(disk[i]==0x63);
 /* Failure growing a dirty resident cache must flush its prefix first. */
 fail_reserve=true;assert(vfs_write_internal(0,data,4096)==4096 && stream_calls==1 && disk_size==4U*1024U*1024U+4096);
 assert(!g_files[0].cache_loaded && !g_vfs_dirty_files);
 reset();disk_size=g_files[0].entry.fat.entry.size=2U*1024U*1024U;fail_load=true;
 assert(vfs_write_internal(0,data,4096)==4096 && stream_calls==1);
 reset();ram=64U*1024U*1024U;g_files[0].offset=1024U*1024U;
 assert(vfs_write_internal(0,data,4096)==4096 && stream_calls==1);
 reset();ram=512U*1024U*1024U;g_files[0].offset=8U*1024U*1024U;
 assert(vfs_write_internal(0,data,4096)==4096 && stream_calls==1);
 reset();fail_sync=true;assert(vfs_write_internal(0,data,4096)==-1 && !g_files[0].offset && !stream_calls);
 reset();fail_load=fail_io=true;assert(vfs_write_internal(0,data,4096)==-1 && !g_files[0].offset);
 assert(vfs_write_internal(-1,data,1)==-1 && vfs_write_internal(2,data,1)==-1);
 reset();free(g_files[0].cache);puts("PASS production VFS: 4 MiB/4 KiB writes stay buffered; dirty-prefix OOM fallback, uncached OOM fallback, RAM/size bounds and I/O errors preserve progress");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-vfs-policy-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
