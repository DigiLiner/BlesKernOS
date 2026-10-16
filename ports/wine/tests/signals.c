#define _GNU_SOURCE
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

#define CHECK(x) do { if (!(x)) { printf("[SIGNAL] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1; } } while(0)
static volatile sig_atomic_t count,depth,nested,error;
static unsigned char alternate[65536];
static void simple(int sig) { if(sig==SIGUSR1) ++count; }
static void handler(int sig,siginfo_t *info,void *ctx) {
    char local;stack_t old;
    if ((uintptr_t)&local<(uintptr_t)alternate ||
        (uintptr_t)&local>=(uintptr_t)alternate+sizeof(alternate)) error=1;
    if (sigaltstack(NULL,&old) || !(old.ss_flags&SS_ONSTACK)) error=2;
    if (sigaltstack(&old,NULL)!=-1 || errno!=EPERM) error=3;
    if (!info || info->si_signo!=sig || !ctx) error=4;
    ++count;
    if (nested && !depth) {++depth;raise(SIGUSR2);--depth;}
    __asm__ volatile("fninit; fldz; xorps %%xmm0,%%xmm0":::"memory");
}
int main(void) {
    uint64_t mask=~0ULL,old=0;
    CHECK(syscall(SYS_rt_sigprocmask,SIG_SETMASK,&mask,&old,4)==-1 && errno==EINVAL);
    CHECK(syscall(SYS_rt_sigprocmask,SIG_SETMASK,&mask,&old,8)==0);
    CHECK(syscall(SYS_rt_sigprocmask,SIG_SETMASK,NULL,&mask,8)==0);
    CHECK(!(mask&((1ULL<<(SIGKILL-1))|(1ULL<<(SIGSTOP-1)))));
    CHECK(syscall(SYS_rt_sigprocmask,SIG_SETMASK,&old,NULL,8)==0);
    struct sigaction sa={.sa_handler=simple},readback;
    CHECK(sigaction(SIGKILL,&sa,NULL)==-1 && errno==EINVAL);
    CHECK(sigaction(SIGUSR1,&sa,&readback)==0);
    sigset_t blocked,pending;
    sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK,&blocked,NULL)==0);
    CHECK(raise(SIGUSR1)==0 && raise(SIGUSR1)==0 && count==0);
    CHECK(sigpending(&pending)==0 && sigismember(&pending,SIGUSR1)==1);
    CHECK(sigprocmask(SIG_UNBLOCK,&blocked,NULL)==0 && count==1);
    CHECK(raise(SIGUSR1)==0 && count==2); /* sigreturn legacy, no SA_SIGINFO */
    CHECK(syscall(SYS_tgkill,getpid()+10000,syscall(SYS_gettid),SIGUSR1)==-1 && errno==ESRCH);
    CHECK(syscall(SYS_rt_sigaction,SIGUSR1,(void *)1,NULL,8)==-1 && errno==EFAULT);
    stack_t stack={.ss_sp=alternate,.ss_size=sizeof alternate};
    CHECK(sigaltstack(&stack,NULL)==0);
    sa.sa_sigaction=handler;sa.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_NODEFER;
    CHECK(sigaction(SIGUSR2,&sa,NULL)==0);
    nested=1;count=0;CHECK(raise(SIGUSR2)==0 && count==2 && !error);nested=0;
    uint32_t pattern[4]={0x12345678,0x87654321,0xaabbccdd,0xfedcba98},seen[4];
    double one=0;
    int pid=getpid(),tid=syscall(SYS_gettid);
    __asm__ volatile("fld1; movups (%4),%%xmm0; int $0x80; movups %%xmm0,(%5); fstpl %6"
        : "+a"((int){SYS_tgkill}) : "b"(pid),"c"(tid),"d"(SIGUSR2),"S"(pattern),"D"(seen),"m"(one)
        : "memory");
    CHECK(!memcmp(pattern,seen,sizeof seen) && one==1.0 && !error);
    sa.sa_handler=simple;sa.sa_flags=SA_RESETHAND;
    CHECK(sigaction(SIGUSR1,&sa,NULL)==0 && raise(SIGUSR1)==0);
    CHECK(sigaction(SIGUSR1,NULL,&readback)==0 && readback.sa_handler==SIG_DFL);
    stack.ss_flags=SS_DISABLE;CHECK(sigaltstack(&stack,NULL)==0);
    puts("[SIGNAL] mascaras, pendientes, contexto, pila anidada, x87/XMM PASS");
    return 0;
}
