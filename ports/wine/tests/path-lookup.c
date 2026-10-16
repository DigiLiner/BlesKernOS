/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
static unsigned long long micros(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (unsigned long long)t.tv_sec*1000000+t.tv_nsec/1000;}
int main(void){
 setbuf(stdout,NULL);
 const char *path="/SYSTEM/WINE/WINDOWS/SYSTEM32/NOTEPAD.EXE";struct stat st;
 if(stat(path,&st) || !st.st_size)return 1;
 for(unsigned sample=0;sample<3;sample++){
  unsigned long long start=micros();
  for(unsigned i=0;i<3000;i++)if(stat(path,&st))return 1;
  printf("[PATH-LOOKUP] sample=%u calls=3000 us=%llu size=%llu\n",sample,micros()-start,(unsigned long long)st.st_size);
 }
 puts("[PATH-LOOKUP] PASS");return 0;
}
