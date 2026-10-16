/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/futex.h>
static void check(int ok,const char *name){if(!ok){printf("[REALTIME] FAIL %s errno=%d\n",name,errno);exit(1);}}
static int64_t nanos(struct timespec t){return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void){
 setbuf(stdout,NULL);alarm(30);
 struct timespec previous={0},now,coarse,mono0,mono1;
 check(!clock_gettime(CLOCK_MONOTONIC,&mono0),"monotonic");
 for(int i=0;i<900;i++){
  struct {int64_t sec,nsec;} t64;
  struct {int32_t sec,nsec;} t32;
  struct timeval tv;
  check(!clock_gettime(CLOCK_REALTIME,&now),"realtime");
  check(nanos(now)>=nanos(previous),"realtime must not regress at fraction wrap");previous=now;
  check(now.tv_nsec>=0 && now.tv_nsec<1000000000,"nanosecond range");
  check(!syscall(403,0,&t64),"clock_gettime64");
  check(!syscall(265,0,&t32),"clock_gettime32");
  check(!gettimeofday(&tv,NULL),"gettimeofday");
  check(!clock_gettime(CLOCK_REALTIME_COARSE,&coarse),"coarse");
  check(t64.sec>=now.tv_sec && t64.sec<=now.tv_sec+1,"64-bit ABI agreement");
  check(t32.sec>=t64.sec && t32.sec<=t64.sec+1,"32-bit ABI agreement");
  check(tv.tv_sec>=t32.sec && tv.tv_sec<=t32.sec+1 && tv.tv_usec<1000000,"timeval agreement");
  check(coarse.tv_sec>=tv.tv_sec && coarse.tv_sec<=tv.tv_sec+1,"coarse agreement");
  struct timespec pause={0,4000000};nanosleep(&pause,NULL);
 }
 check(!clock_gettime(CLOCK_MONOTONIC,&mono1),"monotonic end");
 int word=0;check(!clock_gettime(CLOCK_REALTIME,&now),"absolute wait clock");
 int64_t deadline=nanos(now)+100000000;
 struct timespec timeout={deadline/1000000000,deadline%1000000000};
 struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);
 errno=0;int ret=syscall(SYS_futex,&word,FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG|FUTEX_CLOCK_REALTIME,0,&timeout,NULL,FUTEX_BITSET_MATCH_ANY);
 check(ret==-1 && errno==ETIMEDOUT,"absolute realtime futex timeout");
 clock_gettime(CLOCK_MONOTONIC,&end);
 check(nanos(end)-nanos(begin)>=80000000 && nanos(end)-nanos(begin)<1000000000,"absolute wait duration");
 printf("[REALTIME] PASS 900 ABI/boundary samples over %lld ms; absolute futex timeout %lld ms\n",(long long)((nanos(mono1)-nanos(mono0))/1000000),(long long)((nanos(end)-nanos(begin))/1000000));
 return 0;
}
