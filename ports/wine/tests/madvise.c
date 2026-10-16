#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
#define CHECK(x) do {if(!(x)){printf("[MADVISE] line=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    int control=open("/dev/blesgfx",O_RDWR);CHECK(control>=0);
    unsigned char *p=mmap(NULL,16*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(p!=MAP_FAILED);memset(p,73,16*4096);
    bles_gfx_wine_pager_perf_t before={.pid=getpid()},after=before;
    CHECK(ioctl(control,BLES_GFX_WINE_PAGER_PERF,&before)==0);
    CHECK(madvise(p,16*4096,MADV_DONTNEED)==0);
    CHECK(ioctl(control,BLES_GFX_WINE_PAGER_PERF,&after)==0);
    CHECK(after.anonymous_pages+14<=before.anonymous_pages);
    CHECK(p[0]==0 && p[16*4096-1]==0);p[0]=19;
    CHECK(mprotect(p,4096,PROT_READ)==0 && madvise(p,4096,MADV_DONTNEED)==0 && p[0]==0);
    CHECK(mprotect(p,4096,PROT_NONE)==0 && madvise(p,4096,MADV_DONTNEED)==0);
    CHECK(mprotect(p,4096,PROT_READ|PROT_WRITE)==0 && p[0]==0);
    errno=0;CHECK(madvise(p+1,4096,MADV_DONTNEED)==-1 && errno==EINVAL);
    CHECK(madvise(p,0,MADV_DONTNEED)==0);
    p[0]=83;CHECK(munmap(p+4096,4096)==0);
    errno=0;CHECK(madvise(p,8192,MADV_DONTNEED)==-1 && errno==ENOMEM && p[0]==83);
    CHECK(munmap(p,16*4096)==0);
    const char *path="/WINEGATE/madvise-file.dat";
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    unsigned char page[4096];memset(page,42,sizeof(page));CHECK(write(fd,page,sizeof(page))==sizeof(page));
    p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    unsigned char *other=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    CHECK(p!=MAP_FAILED && other!=MAP_FAILED && p[0]==42 && other[0]==42);
    p[0]=98;CHECK(madvise(p,4096,MADV_DONTNEED)==0 && p[0]==42 && other[0]==42);
    CHECK(munmap(p,4096)==0 && munmap(other,4096)==0);
    p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    other=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(p!=MAP_FAILED && other!=MAP_FAILED);p[0]=65;
    CHECK(madvise(p,4096,MADV_DONTNEED)==0 && p[0]==65 && other[0]==65);
    CHECK(munmap(p,4096)==0 && munmap(other,4096)==0 && close(fd)==0 && unlink(path)==0);
    CHECK(close(control)==0);
    puts("[MADVISE] actual private RAM release, zero/refault, permissions, holes and shared data PASS");
    return 0;
}
