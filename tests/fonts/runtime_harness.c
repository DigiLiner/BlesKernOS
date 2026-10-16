/* Host adapters only; test_real_fonts.py appends the actual kernel renderer. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include "bleskernos_font.h"
#define PACKED __attribute__((packed))
#define VFS_MAX_PATH 260
#define VFS_NODE_FILE 1
#define VFS_O_RDONLY 1
#define KMUTEX_INITIALIZER 0
#define MM_SUBSYSTEM_GUI 1
static bool mm_set_allocation_subsystem(void *p,unsigned subsystem) { (void)p;(void)subsystem;return true; }

typedef struct { int x,y,w,h; } gui_rect_t;
typedef struct {
    uint32_t *pixels; uint16_t width,height,pitch; gui_rect_t clip;
    int16_t origin_x,origin_y; uint16_t storage_width,storage_height;
} gui_surface_t;
typedef struct { char name[256]; uint32_t size; int type; uint8_t attributes; } vfs_dir_entry_t;
typedef int kmutex_t;
static void kmutex_lock(kmutex_t *lock) { (void)lock; }
static void kmutex_unlock(kmutex_t *lock) { (void)lock; }
static void *kmalloc(size_t n) { return malloc(n); }
static void kfree(void *p) { free(p); }
static FILE *fds[32];
static const char *font_dir;
static unsigned reads;
static int vfs_open(const char *path, unsigned flags) {
    (void)flags; char host[1024];
    snprintf(host,sizeof(host),"%s/%s",font_dir,strrchr(path,'/')+1);
    for (int i=0;i<32;i++) if (!fds[i]) { fds[i]=fopen(host,"rb"); return fds[i] ? i : -1; }
    return -1;
}
static int vfs_read(int fd,void *data,uint32_t n) { reads++; return (int)fread(data,1,n,fds[fd]); }
static int32_t vfs_seek(int fd,int32_t n,uint32_t origin) { if (fseek(fds[fd],n,(int)origin)) return -1; return (int32_t)ftell(fds[fd]); }
static int32_t vfs_size(int fd) { long n=ftell(fds[fd]); fseek(fds[fd],0,SEEK_END); long size=ftell(fds[fd]); fseek(fds[fd],n,SEEK_SET); return (int32_t)size; }
static void vfs_close(int fd) { fclose(fds[fd]);fds[fd]=NULL; }
static bool vfs_listdir(const char *path,vfs_dir_entry_t *entries,uint32_t max,uint32_t *count) {
    (void)path; DIR *d=opendir(font_dir);*count=0;if (!d) return false;
    struct dirent *e;
    while ((e=readdir(d)) && *count<max) {
        if (e->d_name[0]=='.') continue;
        memset(&entries[*count],0,sizeof(*entries));
        snprintf(entries[*count].name,sizeof(entries[*count].name),"%s",e->d_name);entries[*count].type=VFS_NODE_FILE;(*count)++;
    }
    closedir(d);return true;
}
