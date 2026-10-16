#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>
#include <signal.h>
#define CHECK(x) do{if(!(x)){printf("[SHARED] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static void expected_segv(int sig) { _exit(sig==SIGSEGV?0:92); }
int main(void) {
 const char *path="/WINEGATE/shared.dat";unlink(path);
 int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0 && ftruncate(fd,8192)==0);
 char *a=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(a!=MAP_FAILED);
 int second=open(path,O_RDWR);CHECK(second>=0);
 char *b=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,second,4096);CHECK(b!=MAP_FAILED);
 char *fixed=mmap(0,12288,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(fixed!=MAP_FAILED);
 CHECK(mmap(fixed+4096,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,second,4096)==fixed+4096);
 fixed[4096]='F';CHECK(b[0]=='F' && a[4096]=='F');
 CHECK(munmap(fixed,12288)==0);
 a[4096]='A';CHECK(b[0]=='A');b[1]='B';CHECK(a[4097]=='B');
 char byte;CHECK(lseek(second,4096,SEEK_SET)==4096 && read(second,&byte,1)==1 && byte=='A');
 CHECK(write(second,"C",1)==1 && a[4097]=='C');
 int ro=open(path,O_RDONLY);CHECK(ro>=0);
 char *c=mmap(0,4096,PROT_READ,MAP_SHARED,ro,0);CHECK(c!=MAP_FAILED);
 CHECK(mprotect(c,4096,PROT_READ|PROT_WRITE)==-1 && errno==EACCES);
 CHECK(munmap(c,4096)==0 && close(ro)==0);
 ro=open(path,O_RDONLY);CHECK(ro>=0);
 char *priv=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE,ro,0);CHECK(priv!=MAP_FAILED);
 CHECK(mprotect(priv,8192,PROT_READ)==0);
 a[0]='L';CHECK(priv[0]=='L');
 CHECK(pread(ro,priv,1,4096)==-1 && errno==EFAULT);
 CHECK(mprotect(priv,4096,PROT_READ|PROT_WRITE)==0);
 priv[0]='P';CHECK(a[0]=='L');a[0]='S';CHECK(priv[0]=='P');
 pid_t protected_child=fork();CHECK(protected_child>=0);
 if(!protected_child){signal(SIGSEGV,expected_segv);priv[4096]='!';_exit(91);}
 int protected_status;
 CHECK(waitpid(protected_child,&protected_status,0)==protected_child &&
       WIFEXITED(protected_status) && WEXITSTATUS(protected_status)==0);
 CHECK(munmap(priv,8192)==0);
 priv=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE,ro,0);CHECK(priv!=MAP_FAILED);
 CHECK(pread(ro,priv,1,4096)==1 && priv[0]=='A' && a[0]=='S');
 CHECK(munmap(priv,8192)==0 && close(ro)==0);
 puts("[SHARED] private views stay coherent until write; COW and page permissions PASS");
 CHECK(msync(a,8192,MS_SYNC)==0 && close(fd)==0 && close(second)==0);
 CHECK(munmap(a,8192)==0 && b[0]=='A' && munmap(b,4096)==0);
 fd=open(path,O_RDWR);CHECK(fd>=0 && lseek(fd,4096,SEEK_SET)==4096 && read(fd,&byte,1)==1 && byte=='A');
 a=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(a!=MAP_FAILED);
 CHECK(unlink(path)==0);a[0]='U';CHECK(lseek(fd,0,SEEK_SET)==0 && read(fd,&byte,1)==1 && byte=='U');
 CHECK(msync(a,8192,MS_SYNC)==0 && close(fd)==0 && munmap(a,8192)==0);
 fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0 && ftruncate(fd,8192)==0);
 CHECK(unlink(path)==0);
 a=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(a!=MAP_FAILED);
 c=mmap(0,8192,PROT_READ,MAP_PRIVATE,fd,0);CHECK(c!=MAP_FAILED);
 a[0]='T';CHECK(c[0]=='T');
 CHECK(munmap(c,8192)==0 && munmap(a,8192)==0 && close(fd)==0);
 CHECK(access(path,F_OK)==-1 && errno==ENOENT);
 puts("[SHARED] unlink before mapping preserves contents and removes name PASS");
 int *s=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(s!=MAP_FAILED && s[0]==0);
 int ready[2],release[2];CHECK(pipe(ready)==0 && pipe(release)==0);
 pid_t child=fork();CHECK(child>=0);
 if(!child) {
   struct timespec timeout={3,0};write(ready[1],"R",1);
   while(!__atomic_load_n(s,__ATOMIC_SEQ_CST)) {
     if(syscall(SYS_futex,s,FUTEX_WAIT,0,&timeout,0,0)<0 && errno!=EAGAIN && errno!=EINTR)_exit(71);
   }
   if(s[1]!=73)_exit(72);write(ready[1],"V",1);
   if(read(release[0],&byte,1)!=1 || s[1]!=73)_exit(73);
   if(munmap(s,4096))_exit(74);_exit(0);
 }
 CHECK(read(ready[0],&byte,1)==1);s[1]=73;__atomic_store_n(s,1,__ATOMIC_SEQ_CST);
 CHECK(syscall(SYS_futex,s,FUTEX_WAKE,1,0,0,0)>=0);
 CHECK(read(ready[0],&byte,1)==1 && byte=='V');CHECK(munmap(s,4096)==0);
 CHECK(write(release[1],"X",1)==1);int status;CHECK(waitpid(child,&status,0)==child && status==0);
 close(ready[0]);close(ready[1]);close(release[0]);close(release[1]);
 puts("[SHARED] aliases, offsets, read/write, msync, unlink, fork y futex PASS");return 0;
}
