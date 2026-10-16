#!/usr/bin/env python3
"""Exercise the actual shell file helpers against an isolated host filesystem."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
header=(ROOT/'system/desktop/file_ops.h').read_text()
helpers=header[header.index('typedef void (*bk_shell_progress_fn)'):header.rindex('#endif')]
shim=r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>
#include <dirent.h>
#define VFS_MAX_PATH 260
#define VFS_NODE_DIR 2
#define VFS_O_RDONLY O_RDONLY
#define VFS_O_WRONLY O_WRONLY
#define VFS_O_CREATE O_CREAT
#define VFS_O_TRUNC O_TRUNC
#define bk_sys_alloc_zero(n) calloc(1,n)
#define bk_sys_free free
#define bk_sys_getpid getpid
#define bk_file_write write
#define BK_TRASH_FILES "trash"
#define bk_runtime_strlen strlen
#define bk_runtime_strcmp strcmp
#define bk_runtime_strncmp strncmp
#define bk_file_open open
#define bk_file_read read
static bool bk_file_close(int fd){return close(fd)==0;}
static int bk_file_transfer_check(const char *a,const char *b){
 if(strstr(a,"/external/") && !strstr(b,"/external/"))return 0;
 size_t n=strlen(a);if(!strncmp(a,b,n) && (!b[n] || b[n]=='/'))return -1;
 return 1;
}
static bool bk_file_rename(const char *a,const char *b){if(strstr(a,"/external/") && !strstr(b,"/external/"))return false;return rename(a,b)==0;}
typedef struct {int type;char name[256];} vfs_dir_entry_t;
static bool bk_file_stat(const char *p,vfs_dir_entry_t *entry) {
 struct stat s; if(stat(p,&s)) return false;
 entry->type=S_ISDIR(s.st_mode)?2:1;return true;
}
static bool bk_file_list_dir(const char *p,vfs_dir_entry_t *entries,uint32_t capacity,uint32_t *count) {
 DIR *dir=opendir(p);if(!dir)return false;*count=0;struct dirent *e;
 while((e=readdir(dir)) && *count<capacity) {
  char child[512];snprintf(child,sizeof(child),"%s/%s",p,e->d_name);
  strcpy(entries[*count].name,e->d_name);bk_file_stat(child,&entries[*count]);++*count;
 } closedir(dir);return true;
}
static bool bk_file_mkdir(const char *p) {return mkdir(p,0700)==0;}
static bool bk_file_remove(const char *p) {struct stat s;if(stat(p,&s))return false;return (S_ISDIR(s.st_mode)?rmdir(p):unlink(p))==0;}
static bool bk_file_write_all(const char *p,const void *data,uint32_t bytes) {
 int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600); if(fd<0)return false;
 bool ok=write(fd,data,bytes)==bytes;close(fd);return ok;
}
'''
test=r'''
static unsigned notifications,completed,bytes_written;
static void progress(void *context,const char *path,uint32_t bytes,bool done) {
 assert(context==&notifications && path && path[0]);++notifications;bytes_written+=bytes;if(done)++completed;
}
int main(void) {
 char cwd[200],a[260],b[260],target[260];assert(getcwd(cwd,sizeof(cwd)));
 assert(bk_file_mkdir("first"));assert(bk_file_mkdir("second"));
 snprintf(a,sizeof(a),"%s/first/document",cwd);snprintf(b,sizeof(b),"%s/second/document",cwd);
 assert(bk_file_write_all(a,"one",3));assert(bk_file_write_all(b,"two",3));
 assert(bk_shell_trash(a));assert(access(a,F_OK));assert(bk_shell_trash(b));
 assert(!access("trash/document",F_OK));assert(!access("trash/document.1",F_OK));
 assert(bk_file_write_all(a,"keep",4));
 assert(!bk_shell_restore("trash/document")); /* No overwrite on restore. */
 assert(bk_file_remove(a));assert(bk_shell_restore("trash/document"));
 assert(bk_shell_restore("trash/document.1"));
 char text[5]={0};int fd=open(a,O_RDONLY);assert(read(fd,text,4)==3);close(fd);assert(!strcmp(text,"one"));
 snprintf(target,sizeof(target),"%s/first",cwd);
 assert(!bk_shell_move(b,target)); /* Same name exists. */
 snprintf(target,sizeof(target),"%s/second",cwd);
 assert(!bk_shell_move(target,target));
 assert(bk_shell_trash(a));assert(bk_file_remove("trash/document"));bk_shell_remove_trash_metadata("trash/document");
 assert(access("trash/.bk-trash-document",F_OK));
 assert(bk_file_mkdir("external"));assert(bk_file_mkdir("external/folder"));
 assert(bk_file_write_all("external/folder/data","copied",6));
 snprintf(a,sizeof(a),"%s/external/folder",cwd);snprintf(target,sizeof(target),"%s/second",cwd);
 assert(bk_shell_move(a,target));assert(!access("external/folder/data",F_OK));
 assert(!access("second/folder/data",F_OK));assert(!bk_shell_move(a,target));
 assert(!bk_shell_trash("/SYSTEM"));assert(!bk_shell_trash("/"));
 puts("PASS trash duplicates, restore contents, collision refusal, permanent cleanup, protected roots, transactional cross-volume copy");
 assert(bk_file_mkdir("observed"));assert(bk_file_mkdir("observed/sub"));
 assert(bk_file_write_all("observed/sub/file","observed bytes",14));
 bk_shell_progress_t observer={progress,&notifications};
 assert(bk_shell_copy_tree_progress("observed","observed-copy",0,&observer));
 assert(completed==3 && bytes_written==14 && notifications>completed);
 completed=0;assert(bk_shell_remove_tree_progress("observed-copy",0,&observer));assert(completed==3);

}
'''
with tempfile.TemporaryDirectory(prefix='bk-file-ops-') as directory:
 d=Path(directory);(d/'test.c').write_text(shim+helpers+test)
 subprocess.run(['gcc','-Wall','-Wextra',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],cwd=d,check=True)
