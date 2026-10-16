#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){printf("[DOS-VM] FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
static sigjmp_buf jump;
static volatile sig_atomic_t caught, fault_code;
static void signal_handler(int sig,siginfo_t *info,void *context){(void)context;caught=sig;fault_code=info->si_code;siglongjmp(jump,1);}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    const int flags=MAP_PRIVATE|MAP_ANONYMOUS;
    CHECK(mmap((void*)0x10000,0x100000,PROT_READ|PROT_WRITE,flags|MAP_FIXED_NOREPLACE,-1,0)==(void*)0x10000);
    CHECK(mmap((void*)0x1000,0xf000,PROT_NONE,flags|MAP_FIXED_NOREPLACE,-1,0)==(void*)0x1000);
    CHECK(mmap((void*)0x1000,0x10f000,PROT_NONE,flags|MAP_FIXED,-1,0)==(void*)0x1000);
    CHECK(mprotect((void*)0xf0000,0x10000,PROT_READ|PROT_WRITE)==0);
    volatile uint32_t *bios=(void*)0xf0400;*bios=0x76543210;CHECK(*bios==0x76543210);
    CHECK(mmap((void*)0,4096,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0)==MAP_FAILED && errno==EACCES);
    CHECK(*bios==0x76543210);
    CHECK(mmap((void*)0x110000,4096,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0)==MAP_FAILED);
    CHECK(munmap((void*)0x1000,0x10f000)==0);
    puts("[DOS-VM] PASS low DOS remap, private BIOS write, null/live-kernel guards");

    size_t large_size=128U*1024U*1024U;
    char *large=mmap((void*)0x40000000,large_size,PROT_NONE,
                    flags|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0);
    CHECK(large==(void*)0x40000000 && mprotect(large,large_size,PROT_READ|PROT_WRITE)==0);
    large[0]='P';large[large_size-1]='T';
    pid_t child=fork();CHECK(child>=0);
    if(!child){if(large[0]!='P' || large[large_size-1]!='T')_exit(2);large[0]='C';_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(large[0]=='P' && large[large_size-1]=='T' && munmap(large,large_size)==0);
    puts("[DOS-VM] PASS 128 MiB virtual commit and isolated fork without an eager 128 MiB allocation");

    const char *path="/WINEGATE/elf-bss-tail.dat";
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    CHECK(ftruncate(fd,4096+77)==0);
    char *view=mmap(NULL,3*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    CHECK(view!=MAP_FAILED && close(fd)==0);
    CHECK(view[4096+77]==0);
    struct sigaction action={.sa_sigaction=signal_handler,.sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);CHECK(sigaction(SIGBUS,&action,NULL)==0 && sigaction(SIGSEGV,&action,NULL)==0);
    if(!sigsetjmp(jump,1)){volatile char value=view[8192];(void)value;CHECK(0);}
    CHECK(caught==SIGBUS && fault_code==BUS_ADRERR);
    CHECK(mprotect(view+8192,4096,PROT_NONE)==0);caught=0;
    if(!sigsetjmp(jump,1)){volatile char value=view[8192];(void)value;CHECK(0);}
    CHECK(caught==SIGSEGV);
    CHECK(mmap(view+8192,4096,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0)==view+8192);
    view[8192]='B';CHECK(view[8192]=='B' && view[4096+77]==0);
    CHECK(munmap(view,3*4096)==0 && unlink(path)==0);
    puts("[DOS-VM] PASS file partial-page zero fill, EOF SIGBUS, permission SIGSEGV, anonymous BSS replacement");
    return 0;
}
