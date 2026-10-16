#!/usr/bin/env python3
"""Exercise the production inotify queue and watch transitions under ASan/UBSan."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linux/i386_inotify.inc').read_text();part=s[:s.index('static int32_t linux_inotify_init(')]
constants='\n'.join(l for l in (root/'kernel/include/vfs.h').read_text().splitlines() if l.startswith('#define VFS_NOTIFY_'))
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VFS_MAX_PATH 260
#define kmemcpy memcpy
#define kmemset memset
#define kmemcmp memcmp
#define kstrlen strlen
#define kstrcpy strcpy
typedef struct {unsigned refs,kind;struct linux_inotify *inotify;} linux_device;
static linux_device linux_devices[128];
static unsigned linux_io_lock,locked,wakes;
static unsigned kspin_lock_irqsave(unsigned *p){(void)p;assert(!locked);locked=1;return 1;}
static void kspin_unlock_irqrestore(unsigned *p,unsigned f){(void)p;assert(locked && f==1);locked=0;}
static unsigned linux_io_event_key(void){return 71;}
static void task_event_signal(unsigned key){assert(!locked && key==71);wakes++;}
'''+constants+'\n'+part+r'''
static void seed(struct linux_inotify *s,unsigned wd,const char *path,unsigned mask,bool dir){
 for(unsigned i=0;i<64;i++)if(!s->watches[i].wd){
  linux_inotify_watch *w=&s->watches[i];w->wd=wd;w->mask=mask;w->directory=dir;strcpy(w->path,path);return;
 }assert(0);
}
static unsigned scan(struct linux_inotify *s,unsigned mask,int wd,const char *name,unsigned *cookie){
 unsigned found=0;
 assert(s->used<=LINUX_INOTIFY_BYTES);
 for(unsigned at=0;at<s->used;){
  assert(s->used-at>=16);linux_inotify_event *e=(void *)(s->queue+at);
  assert(e->length%4==0 && e->length<=s->used-at-16);
  if(e->length)assert(memchr(e+1,0,e->length));
  if((e->mask&mask)==mask && e->wd==wd && (!name || (e->length && !strcmp((char *)(e+1),name)))){found++;if(cookie)*cookie=e->cookie;}
  at+=16+e->length;
 }return found;
}
int main(void){
 struct linux_inotify *s=calloc(1,sizeof *s);assert(s);
 linux_devices[0]=(linux_device){1,6,s};linux_inotify_instances=1;
 seed(s,1,"/WATCH",LINUX_IN_ALL_EVENTS,true);
 seed(s,2,"/watch/child",LINUX_IN_ALL_EVENTS,true);
 seed(s,3,"/watch/child/file",LINUX_IN_ALL_EVENTS,false);
 linux_inotify_changed("/watch/a",NULL,VFS_NOTIFY_CREATE,false);
 assert(scan(s,VFS_NOTIFY_CREATE,1,"a",NULL)==1 && wakes==1);
 linux_inotify_changed("/watch/a",NULL,VFS_NOTIFY_CREATE,false);assert(wakes==1);
 linux_inotify_changed("/watch/a/deep",NULL,VFS_NOTIFY_CREATE,false);assert(wakes==1); /* one level only */
 linux_inotify_changed("/watch/child","/watch/new",VFS_NOTIFY_MOVE_SELF,true);
 unsigned from=0,to=0;
 assert(scan(s,VFS_NOTIFY_MOVED_FROM|LINUX_IN_ISDIR,1,"child",&from)==1);
 assert(scan(s,VFS_NOTIFY_MOVED_TO|LINUX_IN_ISDIR,1,"new",&to)==1 && from && from==to);
 assert(scan(s,VFS_NOTIFY_MOVE_SELF,2,NULL,NULL)==1);
 assert(!strcmp(s->watches[1].path,"/watch/new") && !strcmp(s->watches[2].path,"/watch/new/file"));
 linux_inotify_changed("/watch/new/file",NULL,VFS_NOTIFY_MODIFY,false);
 assert(scan(s,VFS_NOTIFY_MODIFY,2,"file",NULL)==1 && scan(s,VFS_NOTIFY_MODIFY,3,NULL,NULL)==1);
 linux_inotify_changed("/watch/new/file",NULL,VFS_NOTIFY_DELETE,false);
 assert(scan(s,VFS_NOTIFY_DELETE_SELF,3,NULL,NULL)==1 && scan(s,LINUX_IN_IGNORED,3,NULL,NULL)==1 && !s->watches[2].wd);
 memset(s,0,sizeof *s);seed(s,7,"/watch",VFS_NOTIFY_CREATE|LINUX_IN_ONESHOT,true);
 linux_inotify_changed("/watch/sub",NULL,VFS_NOTIFY_CREATE,true);
 assert(scan(s,VFS_NOTIFY_CREATE|LINUX_IN_ISDIR,7,"sub",NULL)==1 && scan(s,LINUX_IN_IGNORED,7,NULL,NULL)==1);
 assert(!s->watches[0].wd);
 memset(s,0,sizeof *s);
 char name[256];memset(name,'n',255);name[255]=0;
 for(unsigned i=0;i<10000;i++)assert(linux_inotify_enqueue(s,1+i,VFS_NOTIFY_MODIFY,0,name)==(i<=30));
 assert(s->overflow && scan(s,LINUX_IN_OVERFLOW,-1,NULL,NULL)==1 && s->used<=8192);
 /* A completely full queue including its overflow record must not make the
    reserve-space subtraction wrap and permit an out-of-bounds append. */
 memset(s,0,sizeof *s);
 for(unsigned i=0;i<511;i++)assert(linux_inotify_enqueue(s,i+1,VFS_NOTIFY_MODIFY,0,NULL));
 assert(s->used==8176);assert(linux_inotify_enqueue(s,999,VFS_NOTIFY_CREATE,0,"long-enough"));
 assert(s->overflow && s->used==8192);
 for(unsigned i=0;i<10000;i++)linux_inotify_enqueue(s,1000+i,VFS_NOTIFY_CREATE,0,name);
 assert(s->used==8192 && scan(s,LINUX_IN_OVERFLOW,-1,NULL,NULL)==1);
 /* Suppressed/closed instances leave other resources untouched. */
 linux_inotify_instances=0;unsigned before=wakes;linux_inotify_changed("/watch/a",NULL,VFS_NOTIFY_CREATE,false);assert(wakes==before);
 free(s);puts("PASS production queue bounds/coalescing/overflow, rename pairing/descendants, masks, oneshot, ignored and wake-after-unlock");
}
'''
# Exercise the target's 32-bit layout as well as native host sanitizer layout.
with tempfile.TemporaryDirectory(prefix='bk-inotify-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 for flags in [[],['-m32']]:
  subprocess.run(['gcc',*flags,'-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
