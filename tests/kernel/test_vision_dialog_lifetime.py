#!/usr/bin/env python3
"""Exercise the production completion broker independently of a live UI."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/ui/file_dialog.c').read_text()
broker=source[source.index('/* Pending completions'):source.rindex('#endif')]
code='''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#undef NULL
#include "kernel/include/file_dialog.h"
#include "kernel/include/vision_ipc.h"
typedef pthread_mutex_t kmutex_t;
static uint32_t process=7,thread=7,owner=5,queued,allocated;
static bool full,ipc_ok=true;
static char delivered[260];
static void kmutex_lock(kmutex_t *m){assert(!pthread_mutex_lock(m));}
static void kmutex_unlock(kmutex_t *m){assert(!pthread_mutex_unlock(m));}
static void *kmalloc(uint32_t n){void *p=malloc(n);if(p)allocated++;return p;}
static void kfree(void *p){if(p){assert(allocated);allocated--;free(p);}}
static bool mm_set_allocation_owner(void *p,uint32_t pid){assert(p && !pid);return true;}
static uint32_t task_current_process_id(void){return process;}
static uint32_t task_current_pid(void){return thread;}
uint32_t gui_vision_owner_pid(void){return owner;}
static bool task_current_is_user(void){return true;}
static bool user_access_ok(void *p,uint32_t n,bool w){(void)n;(void)w;return p!=NULL;}
static void kstrncpy(char *d,const char *s,uint32_t n){uint32_t i=0;while(i<n && s[i]){d[i]=s[i];i++;}while(i<n)d[i++]=0;}
static bool copy_string_from_user(char *d,uint32_t n,const char *s){if(strlen(s)>=n)return false;strcpy(d,s);return true;}
bool vision_ipc_call(bk_vision_request_t *r,uint32_t *v){assert(r->values[2]);*v=ipc_ok;return ipc_ok;}
static bool task_queue_user_upcall(uint32_t tid,uint32_t fn,const uint32_t *a,uint8_t c,const void *p,uint8_t n,int8_t i){
 assert(tid==7 && fn && c==2 && a[0]==0 && a[1]==0x1234 && !p && !n && i==-6);
 if(full)return false;queued++;return true;
}
static bool task_queue_user_upcall_owned(uint32_t tid,uint32_t fn,const uint32_t *a,uint8_t c,void *release){
 assert(tid==7 && fn && c==2 && a[1]==0x1234 && (void *)(uintptr_t)a[0]==release);
 if(full)return false;strcpy(delivered,release);queued++;kfree(release);return true;
}
'''+broker+'''
static void callback(const char *p,void *context){(void)p;(void)context;}
static bool open_dialog(void){process=thread=7;return bk_file_dialog_open(NULL,"test","/SYSTEM",NULL,0,callback,(void *)0x1234);}
int main(void){
 assert(open_dialog() && allocated==1);uint32_t token=completion_sequence;
 process=6;assert(!bk_vision_file_dialog_pending(token));assert(!bk_vision_file_dialog_complete(token,"/SYSTEM/test"));
 process=5;assert(bk_vision_file_dialog_pending(token));
 process=5;thread=8;assert(bk_vision_file_dialog_complete(token,"/SYSTEM/test"));
 assert(!allocated && queued==1 && !strcmp(delivered,"/SYSTEM/test"));
 assert(!bk_vision_file_dialog_complete(token,NULL));assert(!bk_vision_file_dialog_pending(token));
 assert(open_dialog());full=true;vision_file_dialog_offline(5);
 assert(allocated==1 && queued==1);vision_file_dialog_poll();assert(queued==1);
 full=false;vision_file_dialog_poll();assert(!allocated && queued==2);
 vision_file_dialog_offline(5);vision_file_dialog_poll();assert(queued==2);
 assert(open_dialog());token=completion_sequence;vision_file_dialog_process_cleanup(7);assert(!allocated);
 process=5;assert(!bk_vision_file_dialog_pending(token));
 ipc_ok=false;assert(!open_dialog() && !allocated);
 puts("PASS copied completion, server authorization, stale token, exactly-once cancellation, full queue retry and client cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-dialog-lifetime-') as directory:
 directory=Path(directory);c=directory/'test.c';c.write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror','-Wno-misleading-indentation','-I'+str(root),str(c),'-pthread','-o',str(directory/'test')],check=True)
 subprocess.run([str(directory/'test')],check=True)
