#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define BYTES (4U*1024U*1024U+17U)
#define CHECK(x) do {if(!(x)){printf("[WRITEBACK] line=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static unsigned char pattern(unsigned i){return (unsigned char)(i*37U+(i>>9));}
static unsigned long long ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000ULL+t.tv_nsec/1000000;}
int main(void){
 setbuf(stdout,NULL);
 unsigned char *data=malloc(BYTES),*copy=malloc(BYTES);CHECK(data && copy);
 for(unsigned i=0;i<BYTES;i++)data[i]=pattern(i);
 const char *path="/WINEGATE/writeback-check.dat";
 int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
 unsigned long long begin=ms();
 for(unsigned off=0;off<BYTES;){unsigned n=BYTES-off;if(n>4096)n=4096;CHECK(write(fd,data+off,n)==(int)n);off+=n;}
 struct stat st;CHECK(fstat(fd,&st)==0 && st.st_size==BYTES);
 /* A second description must see dirty cached bytes, before explicit fsync. */
 int alias=open(path,O_RDONLY);CHECK(alias>=0);
 CHECK(read(alias,copy,BYTES)==BYTES && !memcmp(data,copy,BYTES) && close(alias)==0);
 CHECK(fsync(fd)==0 && close(fd)==0);
 printf("[WRITEBACK] small writes bytes=%u ms=%llu\n",BYTES,ms()-begin);
 fd=open(path,O_RDWR);CHECK(fd>=0);
 memset(data+65530,0xc7,3000);
 CHECK(pwrite(fd,data+65530,3000,65530)==3000 && fsync(fd)==0 && close(fd)==0);
 fd=open(path,O_RDONLY);CHECK(fd>=0);
 CHECK(read(fd,copy,BYTES)==BYTES && !memcmp(data,copy,BYTES) && close(fd)==0);
 CHECK(rename(path,"/WINEGATE/writeback-persisted.dat")==0);
 free(data);free(copy);
 puts("[WRITEBACK] 4 MiB/4 KiB writes, alias coherence, EOF, fsync/reopen, unaligned overwrite and rename PASS");
 return 0;
}
