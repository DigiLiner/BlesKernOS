/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <pthread.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include <sched.h>
#define ROUNDS 1000
#define WARMUP 20
static _Atomic int turn;
static unsigned waits[2],wakes[2];
static void fail(void){perror("[FUTEX-LATENCY] FAIL");exit(1);}
static unsigned long long micros(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))fail();return (unsigned long long)t.tv_sec*1000000+t.tv_nsec/1000;}
static void exchange(int mine){
 while(atomic_load_explicit(&turn,memory_order_acquire)!=mine){
  long rc=syscall(SYS_futex,&turn,FUTEX_WAIT_PRIVATE,1-mine,NULL,NULL,0);
  if(rc<0 && errno!=EAGAIN && errno!=EINTR)fail();
  if(!rc)waits[mine]++;
 }
 atomic_store_explicit(&turn,1-mine,memory_order_release);
 long rc=syscall(SYS_futex,&turn,FUTEX_WAKE_PRIVATE,1,NULL,NULL,0);
 if(rc<0)fail();
 wakes[mine]+=(unsigned)rc;
}
static void *worker(void *unused){(void)unused;for(unsigned i=0;i<ROUNDS+WARMUP;i++)exchange(1);return NULL;}
int main(void){
 setbuf(stdout,NULL);
 for(unsigned run=0;run<3;run++){
  pthread_t thread;turn=0;waits[0]=waits[1]=wakes[0]=wakes[1]=0;
  if(pthread_create(&thread,NULL,worker,NULL))fail();
  unsigned long long begin=0;
  for(unsigned i=0;i<ROUNDS+WARMUP;i++){if(i==WARMUP)begin=micros();exchange(0);}
  // Wait for the final response, outside the producer's last exchange.
  while(atomic_load_explicit(&turn,memory_order_acquire)!=0)sched_yield();
  unsigned long long elapsed=micros()-begin;
  if(pthread_join(thread,NULL))fail();
  printf("[FUTEX-LATENCY] run=%u rounds=%u us=%llu waits=%u,%u wakes=%u,%u\n",run,ROUNDS,elapsed,waits[0],waits[1],wakes[0],wakes[1]);
 }
 puts("[FUTEX-LATENCY] PASS");return 0;
}
