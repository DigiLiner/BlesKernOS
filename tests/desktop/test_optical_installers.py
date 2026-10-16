#!/usr/bin/env python3
"""Check actual CD statfs and Wine drive/path helpers used by split installers."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
vfs=(root/'kernel/drivers/core/vfs.c').read_text()
fs=(root/'kernel/linux/i386_fs.inc').read_text()
launcher=(root/'ports/tinyx/wine-launch.c').read_text()
def function(source,signature):
 a=source.index(signature);brace=source.index('{',a);n=1;b=brace+1
 while n:
  if source[b]=='{':n+=1
  if source[b]=='}':n-=1
  b+=1
 return source[a:b]+'\n'
shim=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define VFS_MAX_PATH 260
#define PAGING_PAGE_SIZE 4096
#define PACKED __attribute__((packed))
#define BK_EINVAL 22
#define BK_EFAULT 14
#define BK_EBADF 9
#define BK_EIO 5
#define LINUX_IO_BASE 1000
#define FAT_TYPE_NONE 0
#define FAT_TYPE_BFS4 4
#define BLOCK_DEVICE_ATAPI 3
#define kstrncpy strncpy
typedef unsigned fat_type_t;
typedef struct {unsigned type;} fat_fs_t;
typedef struct {unsigned type,sector_count,sector_size;} block_device_t;
typedef struct {uintptr_t ebx,ecx,edx;} registers_t;
static bool mounted=true;
static block_device_t cd={3,282924,2048};
static bool vfs_normalize_path(const char *path,char *out){if(!path || *path!='/')return false;strcpy(out,path);return true;}
static bool vfs_iso_path(const char *path,const char **inner){if(strncmp(path,"/CDROM",6) || (path[6] && path[6]!='/'))return false;*inner=path[6]?path+6:"/";return true;}
static bool iso9660_is_mounted(void){return mounted;}
static block_device_t *block_get(const char *name){assert(!strcmp(name,"cd0"));return &cd;}
static bool vfs_select_fat(const char *path,fat_fs_t *fs,const char **inner){*inner=path;fs->type=!strncmp(path,"/FAT",4)?1:FAT_TYPE_BFS4;return true;}
static bool fat_get_space(fat_fs_t *fs,uint64_t *total,uint64_t *available){(void)fs;*total=768*1024*1024u;*available=400*1024*1024u;return true;}
static bool user_write_range_ok(registers_t *r,void *p,unsigned size){(void)r;return p && size;}
static void *resource_for(unsigned pid,bool create){(void)pid;(void)create;return NULL;}
static unsigned task_current_process_id(void){return 1;}
static int resource_fd(void *r,unsigned fd){(void)r;return fd==3?10:fd==4?11:fd==5?1001:-1;}
static const char *vfs_fd_path(int fd){return fd==10?"/CDROM/PHOTOSHP/DISK2/DATA.2":fd==11?"/SYSTEM/WINE":NULL;}
static int linux_at_path(registers_t *r,int fd,const char *path,char *out){(void)r;(void)fd;strcpy(out,path);return 0;}
#define kmemset memset
static bool copy_to_user(void *out,const void *in,unsigned size){memcpy(out,in,size);return true;}
'''
a=fs.index('typedef struct PACKED {',fs.index('/* Linux i386 statfs64 ABI.'));b=fs.index('linux_i386_statfs64_t;',a)+len('linux_i386_statfs64_t;')
code=shim+function(vfs,'bool vfs_get_optical_space(')+function(vfs,'bool vfs_get_space_path_type(')+fs[a:b]+'\n'+function(fs,'static int32_t sys_linux_statfs64(')
code+=r'''
int main(void){
 linux_i386_statfs64_t out;assert(sizeof(out)==84);
 registers_t r={(uintptr_t)"/CDROM/PHOTOSHP/DISK1",84,(uintptr_t)&out};
 assert(!sys_linux_statfs64(&r,false));assert(out.type==0x9660 && out.block_size==2048 && out.flags==1);
 assert(out.blocks==282924 && !out.blocks_available && !out.blocks_free);
 r.ebx=3;assert(!sys_linux_statfs64(&r,true));assert(out.type==0x9660);
 r.ebx=4;assert(!sys_linux_statfs64(&r,true));assert(out.type==0x34534642 && out.flags==0 && out.blocks_available);
 r.ebx=(uintptr_t)"/FAT";assert(!sys_linux_statfs64(&r,false));assert(out.type==0x4d44);
 r.ebx=2;assert(sys_linux_statfs64(&r,true)==-9);
 r.ebx=5;assert(sys_linux_statfs64(&r,true)==-22);
 r.ebx=3;r.ecx=80;assert(sys_linux_statfs64(&r,true)==-22);r.ecx=84;
 mounted=false;assert(sys_linux_statfs64(&r,true)==-5);mounted=true;
 uint64_t total,available;fat_type_t type;
 assert(vfs_get_space_path_type("/CDROM",&total,&available,&type));assert(total==282924ull*2048 && !available && type==FAT_TYPE_NONE);
 assert(!vfs_get_optical_space("/CDROM-backup",&total));
 puts("PASS optical statfs64/fstatfs64: ISO magic, geometry, read-only, disk backends and ABI errors");
}
'''
drive=r'''
#define _GNU_SOURCE
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
static int optical_stat(const char *path,struct stat *info){return stat(!strcmp(path,"/CDROM")?"cdrom":path,info);}
#define stat(path,info) optical_stat(path,info)
static char optical_drive_letter;
'''
drive+=function(launcher,'static int ensure_optical_drive(').replace('/WINEGATE/prefix/dosdevices/','drives/')
drive+=function(launcher,'static int optical_windows_path(')
drive+=r'''
int main(void){
 char out[260],target[64];assert(!mkdir("drives",0700));
 assert(!ensure_optical_drive() && !optical_drive_letter);
 assert(!mkdir("cdrom",0700));assert(!ensure_optical_drive() && optical_drive_letter=='D');
 assert(optical_windows_path("/CDROM/PHOTOSHP/DISK1/SETUP.EXE",out,sizeof(out))==1);
 assert(!strcmp(out,"D:\\PHOTOSHP\\DISK1\\SETUP.EXE"));
 assert(optical_windows_path("/CDROM",out,sizeof(out))==1 && !strcmp(out,"D:\\"));
 assert(!optical_windows_path("/CDROM-backup/setup.exe",out,sizeof(out)));
 assert(optical_windows_path("/CDROM/SETUP.EXE",out,4)==-1);
 assert(!unlink("drives/d:"));assert(!symlink("/custom/disk","drives/d:"));
 assert(!ensure_optical_drive() && optical_drive_letter=='E');
 assert(readlink("drives/d:",target,sizeof(target))==12 && !memcmp(target,"/custom/disk",12));
 assert(!ensure_optical_drive() && optical_drive_letter=='E'); /* Reuse, do not allocate F. */
 assert(!unlink("drives/e:"));
 for(char c='e';c<='y';++c){char path[32];snprintf(path,sizeof(path),"drives/%c:",c);assert(!symlink("/custom/disk",path));}
 assert(!ensure_optical_drive() && !optical_drive_letter); /* All existing letters survive. */
 puts("PASS Wine optical drive creation, reuse, preserved user mappings and CD-relative Windows paths");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-optical-') as temp:
 path=Path(temp)
 for name,text in [('statfs',code),('drives',drive)]:
  (path/(name+'.c')).write_text(text)
  subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror',str(path/(name+'.c')),'-o',str(path/(name+'-test'))],check=True)
  subprocess.run([str(path/(name+'-test'))],cwd=temp,check=True)
