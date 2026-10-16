#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>

enum { THREADS=8, ROUNDS=1500, INCREMENTS=12 };
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static pthread_barrier_t barrier;
static uint32_t counter, completed;
static void check(int result) {
    if(result){fprintf(stderr,"[FUTEX-STRESS] error=%d\n",result);abort();}
}
static void *worker(void *unused) {
    (void)unused;
    for(unsigned i=0;i<ROUNDS;i++) {
        int result=pthread_barrier_wait(&barrier);
        if(result!=PTHREAD_BARRIER_SERIAL_THREAD)check(result);
        for(unsigned j=0;j<INCREMENTS;j++) {
            check(pthread_mutex_lock(&mutex));counter++;
            if(j==5)sched_yield();
            check(pthread_mutex_unlock(&mutex));
        }
    }
    check(pthread_mutex_lock(&mutex));
    completed++;check(pthread_cond_signal(&condition));
    check(pthread_mutex_unlock(&mutex));return NULL;
}
int main(void) {
    pthread_t threads[THREADS];
    setvbuf(stdout,NULL,_IONBF,0);
    check(pthread_barrier_init(&barrier,NULL,THREADS));
    puts("[FUTEX-STRESS] starting 8 threads, mutexes, barriers and condition waits");
    for(unsigned i=0;i<THREADS;i++)check(pthread_create(&threads[i],NULL,worker,NULL));
    check(pthread_mutex_lock(&mutex));
    while(completed<THREADS)check(pthread_cond_wait(&condition,&mutex));
    check(pthread_mutex_unlock(&mutex));
    for(unsigned i=0;i<THREADS;i++)check(pthread_join(threads[i],NULL));
    check(pthread_barrier_destroy(&barrier));
    if(counter!=THREADS*ROUNDS*INCREMENTS)return 1;
    printf("[FUTEX-STRESS] PASS counter=%u rounds=%u threads=%u\n",counter,ROUNDS,THREADS);
    return 0;
}
