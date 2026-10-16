#!/usr/bin/env python3
"""Check the actual VFS transfer guard, including aliases and case folding."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/drivers/core/vfs.c').read_text()
s=s[s.index('int vfs_transfer_check('):s.index('bool vfs_rename(')]
shim=r'''
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define VFS_MAX_PATH 260
#define kstrlen strlen
typedef struct { void *device;unsigned volume_lba;} fat_fs_t;
static bool vfs_normalize_path(const char *p,char *out){if(!p || p[0]!='/')return false;strcpy(out,p);return true;}
static bool vfs_iso_path(const char *p,const char **inner){if(strncmp(p,"/CDROM",6) || (p[6] && p[6]!='/'))return false;*inner=p[6]?p+6:"/";return true;}
static bool vfs_select_fat(const char *p,fat_fs_t *fs,const char **inner){
 fs->device=(void *)1;fs->volume_lba=0;*inner=p;
 if(!strncmp(p,"/ata0/",6))*inner=p+5;
 if(!strncmp(p,"/usb0/",6)){*inner=p+5;fs->device=(void *)2;}
 return true;
}
'''
test=r'''
int main(void){
 assert(vfs_transfer_check("/SYSTEM/DESKTOP/folder","/ata0/system/desktop/folder/sub")==-1);
 assert(vfs_transfer_check("/ata0/SYSTEM/DESKTOP/folder","/system/desktop/FOLDER")==-1);
 assert(vfs_transfer_check("/SYSTEM/DESKTOP/folder","/ata0/SYSTEM/DESKTOP/folder2")==1);
 assert(vfs_transfer_check("/SYSTEM/DESKTOP/folder","/usb0/SYSTEM/DESKTOP/folder")==0);
 assert(vfs_transfer_check("/CDROM/folder","/SYSTEM/DESKTOP")==0);
 assert(vfs_transfer_check("/","/ata0/SYSTEM")==-1);
 assert(vfs_transfer_check("/CDROM/folder","/CDROM/folder/sub")==-1);
 puts("PASS VFS transfer aliases, case folding, root/descendant rejection and separate volumes");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-transfer-guard-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+s+test)
 subprocess.run(['gcc','-Wall','-Wextra',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
