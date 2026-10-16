#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <errno.h>
#include <stdio.h>
#define CHECK(x) do {if(!(x)){printf("[ROBUST] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static pthread_mutex_t mutex;
static void *owner(void *arg) { return (void *)(long)pthread_mutex_lock(&mutex); }
int main(void) {
    struct robust_list_head *head;size_t length;
    CHECK(syscall(SYS_get_robust_list,0,&head,&length)==0 && head && length==sizeof(*head));
    pthread_mutexattr_t attr;CHECK(!pthread_mutexattr_init(&attr));
    CHECK(!pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST));
    CHECK(!pthread_mutex_init(&mutex,&attr));CHECK(!pthread_mutexattr_destroy(&attr));
    for(unsigned i=0;i<20;i++) {
        pthread_t t;void *result;
        CHECK(!pthread_create(&t,NULL,owner,NULL) && !pthread_join(t,&result) && result==NULL);
        CHECK(pthread_mutex_lock(&mutex)==EOWNERDEAD);
        CHECK(!pthread_mutex_consistent(&mutex) && !pthread_mutex_unlock(&mutex));
    }
    CHECK(!pthread_mutex_destroy(&mutex));
    puts("[ROBUST] muerte del propietario, EOWNERDEAD y recuperacion PASS");return 0;
}
