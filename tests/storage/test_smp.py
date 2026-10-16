#!/usr/bin/env python3
"""Concurrent production BFS4 operations outside the VFS domain lock."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
base=(root/'tests/storage/bfs4_host.c').read_text().split('int main(')[0]
base=base.replace('"../../kernel/','"'+str(root/'kernel')+'/')
base='#include <pthread.h>\n'+base
base=base.replace('void kmutex_lock(kmutex_t *lock){(void)lock;}\nvoid kmutex_unlock(kmutex_t *lock){(void)lock;}',r'''
static pthread_mutex_t mutex;static pthread_once_t once=PTHREAD_ONCE_INIT;
static void init_mutex(void) {
 pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);
 pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
 pthread_mutex_init(&mutex,&attr);pthread_mutexattr_destroy(&attr);
}
void kmutex_lock(kmutex_t *lock){(void)lock;pthread_once(&once,init_mutex);pthread_mutex_lock(&mutex);}
void kmutex_unlock(kmutex_t *lock){(void)lock;pthread_mutex_unlock(&mutex);}
''')
code=base+'\n#include "'+str(root/'kernel/drivers/storage/bfs4.c')+'"\n'+r'''
static fat_fs_t fs;
static void *worker(void *arg) {
 unsigned id=(uintptr_t)arg;char path[40],new[40];unsigned char a[12289],b[12289];
 sprintf(path,"/thread%u",id);sprintf(new,"/renamed%u",id);
 for(unsigned round=0;round<40;round++) {
  memset(a,id+round,sizeof(a));
  require(bfs4_write(&fs,path,a,sizeof(a)),"concurrent file write");
  require(bfs4_rename(&fs,path,new),"concurrent rename");
  fat_dir_entry_t e;uint32_t n;
  require(bfs4_resolve(&fs,new,&e) && bfs4_read(&fs,&e,0,b,sizeof(b),&n) &&
    n==sizeof(b) && !memcmp(a,b,n),"concurrent byte integrity");
  fat_check_report_t r;require(bfs4_check(&fs,&r) && !r.errors,"consistent concurrent snapshot");
  require(bfs4_remove(&fs,new),"concurrent delete");
 }
 return NULL;
}
int main(int argc,char **argv) {
 if(argc!=2 || !(disk=fopen(argv[1],"r+b")))return 2;
 block_device_t dev={0};dev.sector_count=32*1024*1024/512;dev.sector_size=512;
 dev.read=host_read;dev.write=host_write;
 require(bfs4_format(&dev,"SMP") && bfs4_mount(&fs,&dev,0),"format/mount");
 uint64_t total,before,after;require(bfs4_space(&fs,&total,&before),"baseline");
 pthread_t threads[8];
 for(uintptr_t i=0;i<8;i++)require(!pthread_create(&threads[i],NULL,worker,(void *)i),"spawn concurrent worker");
 for(unsigned i=0;i<8;i++)require(!pthread_join(threads[i],NULL),"join worker");
 require(bfs4_space(&fs,&total,&after) && before==after,"all blocks reclaimed");
 require(bfs4_unmount(&fs) && bfs4_mount(&fs,&dev,0),"remount after SMP work");
 fclose(disk);puts("PASS 8 concurrent callers, 320 create/rename/read/delete cycles, snapshots, exact reclamation and remount");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-bfs4-smp-') as folder:
 d=Path(folder);(d/'test.c').write_text(code)
 with (d/'disk.img').open('wb') as f:f.truncate(32*1024*1024)
 subprocess.run(['gcc','-m32','-O2','-pthread','-Wall','-Wno-stringop-truncation',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test'),str(d/'disk.img')],check=True)
