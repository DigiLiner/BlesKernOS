#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "bles_present.h"
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void){
 size_t count=760*536,bytes=count*4;uint32_t *p=malloc(bytes);if(!p)return 1;
 for(size_t i=0;i<count;i++)p[i]=(uint32_t)(i*1234567U);
 if(!BlesFrameChanged(p,bytes))return 2;BlesFramePublished(BlesFramePrepare(p,bytes),bytes);
 volatile uint32_t sink=0;uint64_t start=ns();
 for(unsigned k=0;k<200;k++){
  __asm__ volatile("" ::: "memory");
  uint32_t h=2166136261U,s=0;
  for(size_t i=0;i<count;i++){h=(h^p[i])*16777619U;s+=p[i]+i;}
  sink^=h^s;
 }
 uint64_t hash=ns()-start;start=ns();
 for(unsigned k=0;k<200;k++){__asm__ volatile("" ::: "memory");sink^=BlesFrameChanged(p,bytes);}
 uint64_t exact=ns()-start;
 printf("pixels=%zu rounds=200 fnv_ms=%.3f compare_ms=%.3f ratio=%.2f sink=%u\n",count,hash/1e6,exact/1e6,(double)hash/exact,sink);
 BlesFrameFini();free(p);return 0;
}
