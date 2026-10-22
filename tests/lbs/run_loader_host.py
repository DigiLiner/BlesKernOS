#!/usr/bin/env python3
"""Execute the real native loader on i386 host, including malformed ELF input.

Only its allocation wrapper enables execution on host heap pages. Guest testing
uses the unchanged native allocator and syscall boundary.
"""
from pathlib import Path
import subprocess,struct,tempfile
ROOT=Path(__file__).resolve().parents[2]
def run(args,**kw):return subprocess.run(args,check=True,**kw)
with tempfile.TemporaryDirectory(prefix='bk-lbs-') as td:
 d=Path(td)
 def obj(name,source):
  (d/(name+'.c')).write_text(source)
  run(['gcc','-m32','-fPIC','-ffreestanding','-fno-stack-protector','-c',str(d/(name+'.c')),'-o',str(d/(name+'.o'))])
 def shared(name,*inputs):
  run(['ld','-m','elf_i386','-shared','--hash-style=sysv','-soname',name+'.LBS','-o',str(d/(name+'.LBS'))]+[str(d/x) for x in inputs])
 obj('dep','static int state;static int *counter;__attribute__((constructor)) static void init(void){state=41;} __attribute__((destructor)) static void fini(void){if(counter)(*counter)++;} int answer(void){return state;} void counter_set(int *p){counter=p;} unsigned int bk_lbs_live_resources(void){return state==99;} void fixture_busy(int v){state=v?99:41;}')
 shared('DEP','dep.o')
 obj('root','extern int answer(void);int result(void){return answer()+1;}')
 shared('ROOT','root.o','DEP.LBS')
 (d/'PLAIN.SO').write_bytes((d/'DEP.LBS').read_bytes())
 obj('provider','int global_answer(void){return 43;}');shared('PROVIDER','provider.o')
 obj('consumer','extern int global_answer(void);int consumer_answer(void){return global_answer();}');shared('CONSUMER','consumer.o')
 obj('a','int a(void){return 1;}');obj('b','int b(void){return 2;}')
 shared('A','a.o');shared('B','b.o','A.LBS');shared('A','a.o','B.LBS')
 for n in range(24):
  obj('diamond'+str(n),'int diamond_'+str(n)+'(void){return '+str(n)+';}')
  shared('D'+str(n),'diamond'+str(n)+'.o',*(['D'+str(n-1)+'.LBS'] if n else []),*(['D'+str(n-2)+'.LBS'] if n>1 else []))
 raw=bytearray((d/'DEP.LBS').read_bytes())
 mutations={'magic':(0,b'BAD!'),'class':(4,b'\x02'),'endian':(5,b'\x02'),'machine':(18,b'\x3e\0'),'phoff':(28,struct.pack('<I',0xfffffff0))}
 for name,(off,payload) in mutations.items():
  data=raw.copy();data[off:off+len(payload)]=payload;(d/(name+'.BAD')).write_bytes(data)
 phoff=struct.unpack_from('<I',raw,28)[0];phsize,phcount=struct.unpack_from('<HH',raw,42)
 # Dynamic pointers, extents, entry sizes and callback arrays.
 for j in range(phcount):
  off=phoff+j*phsize
  if struct.unpack_from('<I',raw,off)[0]!=2:continue
  doff,dsz=struct.unpack_from('<I',raw,off+4)[0],struct.unpack_from('<I',raw,off+16)[0]
  for k in range(doff,doff+dsz,8):
   tag,val=struct.unpack_from('<II',raw,k)
   if tag in [4,5,6,10,17,18,25,27]:
    data=raw.copy();struct.pack_into('<I',data,k+4,0xfffffff0);(d/(f'dyn-{tag}.BAD')).write_bytes(data)
 (d/'textrel.c').write_text('int textrel_state=17;int textrel_value(void){return textrel_state;}')
 run(['gcc','-m32','-ffreestanding','-fno-pic','-fno-pie','-fno-stack-protector','-c',str(d/'textrel.c'),'-o',str(d/'textrel.o')])
 shared('TEXTREL','textrel.o');(d/'TEXTREL.SO').write_bytes((d/'TEXTREL.LBS').read_bytes())
 phbase=struct.unpack_from('<I',raw,28)[0];phstride,phnum=struct.unpack_from('<HH',raw,42)
 load_offsets=[phbase+j*phstride for j in range(phnum) if struct.unpack_from('<I',raw,phbase+j*phstride)[0]==1]
 broken=raw.copy();struct.pack_into('<I',broken,load_offsets[0]+24,7);(d/'RWX.LBS').write_bytes(broken)
 broken=raw.copy();struct.pack_into('<I',broken,load_offsets[1]+8,0);(d/'OVERLAP.LBS').write_bytes(broken)
 harness=r'''
#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <glob.h>
#include <pthread.h>
#include <string.h>
static int vm_enabled, vm_live, vm_fail_after=-1;
int __bk_lbs_vm_available(void){return vm_enabled;}
void *__bk_lbs_vm_map(void *address,unsigned length,unsigned protection,unsigned flags,int fd,unsigned offset){
 if(vm_fail_after==0)return 0;if(vm_fail_after>0)vm_fail_after--;
 void *p=mmap(address,length,(int)protection,(int)flags,fd,(off_t)offset);
 if(p==MAP_FAILED)return 0;if(!(flags&MAP_FIXED))vm_live++;return p;
}
int __bk_lbs_vm_unmap(void *address,unsigned length){int result=munmap(address,length);assert(!result);vm_live--;return result;}
static int fail_after=-1;
static int fail_now(void){if(fail_after==0)return 1;if(fail_after>0)fail_after--;return 0;}
static void *test_malloc(size_t n){return fail_now()?0:malloc(n);}
static char *test_strdup(const char *s){size_t n=strlen(s)+1;char *p=test_malloc(n);if(p)memcpy(p,s,n);return p;}
static void *executable_calloc(size_t n,size_t z){
 void *p=fail_now()?0:calloc(n,z);if(p){uintptr_t b=(uintptr_t)p&~4095U;size_t size=((uintptr_t)p+n*z-b+4095)&~4095U;
 assert(!mprotect((void*)b,size,PROT_READ|PROT_WRITE|PROT_EXEC));}return p;
}
static int fail_keys=1;
static int test_key_create(pthread_key_t *key,void (*destructor)(void *)){
 return fail_keys?11:pthread_key_create(key,destructor);
}
#define pthread_key_create test_key_create
#define calloc executable_calloc
#define malloc test_malloc
#define strdup test_strdup
#define dlopen native_dlopen
#define dlsym native_dlsym
#define dlclose native_dlclose
#define dlerror native_dlerror
#define dl_iterate_phdr native_dl_iterate_phdr
#include <dlfcn.h>
#include <link.h>
#include "LOADER_PATH"
#undef calloc
#undef malloc
#undef strdup
static int count_cb(struct dl_phdr_info *p,unsigned int size,void *v){(void)p;(void)size;(*(int*)v)++;return 0;}
static int modules(void){int n=0;dl_iterate_phdr(count_cb,&n);return n;}
static void *stress(void *u){int i;(void)u;for(i=0;i<100;i++){void *h=dlopen("DEP.LBS",RTLD_NOW);assert(h);assert(((int(*)(void))dlsym(h,"answer"))()==41);assert(!dlsym(h,"missing_in_thread"));sched_yield();assert(dlerror());assert(!dlclose(h));}return 0;}
int main(void){void *h,*dep,*again;int (*f)(void);int counter=0,i;glob_t bad;pthread_t threads[4];
 assert(dlclose((void*)0x12345)<0);assert(dlerror());fail_keys=0;
 assert(!dlsym((void*)0x12345,"x"));
 h=dlopen("./ROOT.LBS",RTLD_NOW);if(!h){fprintf(stderr,"%s\n",dlerror());return 1;}assert(modules()==2);
 f=dlsym(h,"result");assert(f&&f()==42);assert(dlsym(h,"answer"));
 assert(!dlsym(RTLD_DEFAULT,"result"));
 dep=dlopen("./DEP.LBS",RTLD_NOW);assert(dep);
 ((void(*)(int*))dlsym(dep,"counter_set"))(&counter);
 again=dlopen("./ROOT.LBS",RTLD_NOW);assert(again==h);
 assert(!dlclose(h)&&f()==42);assert(!dlclose(again)&&counter==0);assert(!dlclose(dep)&&counter==1);assert(modules()==0);
 h=dlopen("PLAIN.SO",RTLD_NOW);assert(h);assert(!dlclose(h));
 h=dlopen("./ROOT.LBS",RTLD_NOW);assert(h);
 ((void(*)(int))dlsym(h,"fixture_busy"))(1);
 assert(dlclose(h)<0&&modules()==2);assert(strstr(dlerror(),"live resources"));
 ((void(*)(int))dlsym(h,"fixture_busy"))(0);assert(!dlclose(h)&&modules()==0);
 dep=dlopen("./PROVIDER.LBS",RTLD_NOW|RTLD_GLOBAL);assert(dep);
 h=dlopen("./CONSUMER.LBS",RTLD_NOW);assert(h);assert(!dlclose(dep));
 f=dlsym(h,"consumer_answer");assert(f&&f()==43);assert(!dlclose(h)&&modules()==0);
 assert(!dlopen("./A.LBS",RTLD_NOW));assert(strstr(dlerror(),"circular"));assert(modules()==0);
 h=dlopen("./D23.LBS",RTLD_NOW);assert(h&&modules()==24);
 assert(!dlsym(h,"missing_in_deep_diamond"));assert(dlerror());assert(!dlclose(h)&&modules()==0);
 assert(!dlopen("./MISSING.LBS",RTLD_NOW));assert(dlerror());
 glob("*.BAD",0,0,&bad);for(i=0;i<(int)bad.gl_pathc;i++){
 char path[300];snprintf(path,sizeof(path),"./%s",bad.gl_pathv[i]);h=dlopen(path,RTLD_NOW);
 if(h){fprintf(stderr,"Accepted corrupt %s\n",path);return 2;}assert(dlerror());assert(modules()==0);
 }printf("malformed ELF rejected: %u\n",(unsigned)bad.gl_pathc);globfree(&bad);
 for(i=0;i<25;i++){fail_after=i;h=dlopen("./ROOT.LBS",RTLD_NOW);if(h)assert(!dlclose(h));fail_after=-1;assert(modules()==0);}
 printf("allocation failure rollback: 25 positions PASS\n");
 /* Use explicit identical paths so reference accounting is exercised. */
 for(i=0;i<4;i++)assert(!pthread_create(&threads[i],0,stress,0));
 for(i=0;i<4;i++)assert(!pthread_join(threads[i],0));
 assert(modules()==0);
 vm_enabled=1;
 assert(!dlopen("./TEXTREL.LBS",RTLD_NOW));assert(strstr(dlerror(),"relocation"));assert(!vm_live&&modules()==0);
 assert(!dlopen("./RWX.LBS",RTLD_NOW));assert(dlerror());assert(!vm_live&&modules()==0);
 assert(!dlopen("./OVERLAP.LBS",RTLD_NOW));assert(dlerror());assert(!vm_live&&modules()==0);
 h=dlopen("./TEXTREL.SO",RTLD_NOW);assert(h);f=dlsym(h,"textrel_value");assert(f&&f()==17);assert(!dlclose(h)&&!vm_live);
 h=dlopen("./ROOT.LBS",RTLD_NOW);assert(h&&modules()==2&&vm_live==2);
 f=dlsym(h,"result");assert(f&&f()==42);
 bk_dl_mapping_statistics_t mappings={sizeof(mappings),0,0,0,0};
 assert(!bk_dl_mapping_statistics(&mappings)&&mappings.file_readonly_bytes>0&&mappings.private_bytes<mappings.virtual_bytes);
 assert(!dlclose(h)&&!vm_live&&modules()==0);
 for(i=0;i<12;i++){vm_fail_after=i;h=dlopen("./ROOT.LBS",RTLD_NOW);if(h)assert(!dlclose(h));vm_fail_after=-1;assert(!vm_live&&modules()==0);}
 for(i=0;i<25;i++){fail_after=i;h=dlopen("./ROOT.LBS",RTLD_NOW);if(h)assert(!dlclose(h));fail_after=-1;assert(!vm_live&&modules()==0);}
 for(i=0;i<4;i++)assert(!pthread_create(&threads[i],0,stress,0));
 for(i=0;i<4;i++)assert(!pthread_join(threads[i],0));assert(!vm_live&&modules()==0);
 printf("PASS: native file mappings, read-only text relocation policy, 12 VM failures, 25 allocation failures and 400 mapped concurrent cycles\n");
 assert(modules()==0);printf("PASS: dependencies, calls, init/fini, refs, isolation of local symbols, cycles, corrupt inputs, 400 concurrent open/close\n");return 0;
}
'''.replace('LOADER_PATH',str(ROOT/'sdk/lib/posix_dlfcn.c'))
 # Bare names on a real guest use the installed native directory. Host fixtures
 # use a local-only copy of the same search paths, without changing the loader.
 harness=harness.replace('dlopen("DEP.LBS",','dlopen("./DEP.LBS",')
 (d/'test.c').write_text(harness)
 run(['gcc','-m32','-O1','-g','-pthread',str(d/'test.c'),'-o',str(d/'test')])
 run([str(d/'test')],cwd=d)
