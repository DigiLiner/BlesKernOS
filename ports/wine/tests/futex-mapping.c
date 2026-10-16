#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){printf("[FUTEX-MAPPING] line=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static volatile int ready;
static int outcome,err;
static void *waiter(void *arg) {
    struct timespec limit={5,0};
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);
    outcome=syscall(SYS_futex,arg,FUTEX_WAIT,0,&limit,NULL,0);err=errno;
    return NULL;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    const char *path="/WINEGATE/futex-private.dat";
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    CHECK(ftruncate(fd,4096)==0);
    int *a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    int *b=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    CHECK(a!=MAP_FAILED && b!=MAP_FAILED && close(fd)==0 && *a==0 && *b==0);
    pthread_t thread;CHECK(pthread_create(&thread,NULL,waiter,a)==0);
    while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))usleep(1000);
    usleep(200000);
    /* Same inode/source frame does not make distinct private views aliases. */
    CHECK(syscall(SYS_futex,b,FUTEX_WAKE,1,NULL,NULL,0)==0);
    /* This first write detaches the file page while the waiter is queued. */
    __atomic_store_n(a,1,__ATOMIC_RELEASE);
    CHECK(*b==0 && syscall(SYS_futex,a,FUTEX_WAKE,1,NULL,NULL,0)==1);
    CHECK(pthread_join(thread,NULL)==0 && outcome==0);
    CHECK(munmap(a,4096)==0 && munmap(b,4096)==0 && unlink(path)==0);
    puts("[FUTEX-MAPPING] private aliases stay separate; wake survives file COW without PRIVATE flag PASS");
    return 0;
}
