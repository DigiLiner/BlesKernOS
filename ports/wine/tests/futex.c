#define _GNU_SOURCE
#include <pthread.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#define CHECK(x) do { if(!(x)) {printf("[FUTEX] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;} } while(0)
static _Atomic int word,ready,done;
static int outcome,error;
static volatile sig_atomic_t signals;
static void handler(int sig) {(void)sig;++signals;}
static void *worker(void *unused) {
    (void)unused;atomic_store(&ready,1);
    outcome=syscall(SYS_futex,&word,FUTEX_WAIT_PRIVATE,0,NULL,NULL,0);
    error=errno;atomic_store(&done,1);return NULL;
}
int main(void) {
    struct timespec delay={0,30000000},short_wait={0,20000000};
    CHECK(syscall(SYS_futex,&word,FUTEX_WAIT_PRIVATE,1,NULL,NULL,0)==-1 && errno==EAGAIN);
    CHECK(syscall(SYS_futex,(char *)&word+1,FUTEX_WAKE_PRIVATE,1,NULL,NULL,0)==-1 && errno==EINVAL);
    CHECK(syscall(SYS_futex,&word,FUTEX_WAIT_PRIVATE,0,&short_wait,NULL,0)==-1 && errno==ETIMEDOUT);
    CHECK(syscall(SYS_futex,&word,FUTEX_WAKE_PRIVATE,1,NULL,NULL,0)==0);
    pthread_t thread;CHECK(pthread_create(&thread,NULL,worker,NULL)==0);
    while(!atomic_load(&ready)) sched_yield();
    nanosleep(&delay,NULL);CHECK(!atomic_load(&done));
    CHECK(syscall(SYS_futex,&word,FUTEX_WAKE_PRIVATE,1,NULL,NULL,0)==1);
    CHECK(pthread_join(thread,NULL)==0 && done && outcome==0);
    struct sigaction action={.sa_handler=handler};sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1,&action,NULL)==0);
    ready=done=0;CHECK(pthread_create(&thread,NULL,worker,NULL)==0);
    while(!atomic_load(&ready)) sched_yield();nanosleep(&delay,NULL);
    CHECK(pthread_kill(thread,SIGUSR1)==0);
    CHECK(pthread_join(thread,NULL)==0 && done && outcome==-1 && error==EINTR && signals==1);
    puts("[FUTEX] espera real, timeout, WAKE, clear_child_tid, EINTR PASS");return 0;
}
