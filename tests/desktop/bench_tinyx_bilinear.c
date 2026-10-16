/* Diagnostic: time the real scanline kernels, not game FPS. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
typedef uint32_t CARD32;typedef int32_t xFixed;
#include "fb_bles_bilinear.h"
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
int main(void){
 CARD32 top[2048],bottom[2048],out[800];
 for(unsigned i=0;i<2048;i++){top[i]=i*2137291U;bottom[i]=i*3355211U;}
 for(unsigned run=0;run<3;run++)for(unsigned mode=0;mode<2;mode++){
  void (*fn)(const CARD32 *,const CARD32 *,CARD32 *,int,xFixed,xFixed,int,unsigned,int)=mode?fbBlesBilinear32:fbBlesBilinear32Scalar;
  uint64_t start=now();uint32_t checksum=0;
  for(unsigned n=0;n<20000;n++){fn(top,bottom,out,798,0,52429,0,n&255,0);checksum+=out[n%798];}
  printf("run=%u mode=%s ns=%llu checksum=%u\n",run,mode?"dispatch":"scalar",(unsigned long long)(now()-start),checksum);
 }
 return 0;
}
