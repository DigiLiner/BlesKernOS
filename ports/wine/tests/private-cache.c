#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"

#define CHECK(x) do { if (!(x)) { \
    printf("[PRIVATE-CACHE] line=%d errno=%d FAIL\n", __LINE__, errno); \
    return 1; } } while (0)

static int put(const char *path, unsigned char value) {
    unsigned char page[4096];
    memset(page,value,sizeof(page));
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);
    if(fd<0)return -1;
    int ok=write(fd,page,sizeof(page))==sizeof(page);
    return close(fd)==0 && ok?0:-1;
}

static unsigned char *view(const char *path) {
    int fd=open(path,O_RDONLY);
    if(fd<0)return MAP_FAILED;
    void *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    if(close(fd))return MAP_FAILED;
    return p;
}

int main(int argc,char **argv) {
    (void)argv;
    setvbuf(stdout,NULL,_IONBF,0);
    const int count=argc>1?16:100;
    char paths[100][64];
    unsigned char *maps[100];
    int control=open("/dev/blesgfx",O_RDWR);
    CHECK(control>=0);
    bles_gfx_wine_cache_perf_t before,after;
    CHECK(ioctl(control,BLES_GFX_WINE_CACHE_PERF,&before)==0);
    for(int i=0;i<count;i++) {
        snprintf(paths[i],sizeof(paths[i]),"/WINEGATE/pc-%d.dat",i);
        CHECK(put(paths[i],i)==0);
        maps[i]=view(paths[i]);
        CHECK(maps[i]!=MAP_FAILED && maps[i][4095]==i);
    }
    CHECK(ioctl(control,BLES_GFX_WINE_CACHE_PERF,&after)==0);
    CHECK(after.vfs_handles==before.vfs_handles);
    CHECK(after.private_fds==before.private_fds);
    CHECK(after.cache_entries>=(uint32_t)count);
    CHECK(after.vfs_handles_peak<=before.vfs_handles_peak+4);
    maps[0][0]=231;
    int fd=open(paths[0],O_RDONLY);unsigned char byte;
    CHECK(fd>=0 && read(fd,&byte,1)==1 && byte==0 && close(fd)==0);
    for(int i=0;i<count;i++)CHECK(munmap(maps[i],4096)==0);
    printf("[PRIVATE-CACHE] %d live images without retained FDs; COW isolation PASS\n",count);

    CHECK(put(paths[1],42)==0);
    unsigned char *p=view(paths[1]);
    CHECK(p!=MAP_FAILED && p[0]==42 && munmap(p,4096)==0);
    fd=open(paths[1],O_RDWR);CHECK(fd>=0 && write(fd,"N",1)==1);
    p=view(paths[1]);
    CHECK(p!=MAP_FAILED && p[0]=='N' && close(fd)==0);
    CHECK(munmap(p,4096)==0);

    /* Promote a hot private image to a genuinely writable shared object. */
    p=view(paths[2]);CHECK(p!=MAP_FAILED);
    fd=open(paths[2],O_RDWR);CHECK(fd>=0);
    unsigned char *shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(shared!=MAP_FAILED);shared[0]=54;CHECK(p[0]==54);
    p[0]=12;CHECK(shared[0]==54);
    CHECK(msync(shared,4096,MS_SYNC)==0 && close(fd)==0);
    CHECK(munmap(shared,4096)==0 && munmap(p,4096)==0);
    p=view(paths[2]);CHECK(p!=MAP_FAILED && p[0]==54 && munmap(p,4096)==0);

    CHECK(rename(paths[3],"/WINEGATE/pc-renamed.dat")==0);
    strcpy(paths[3],"/WINEGATE/pc-renamed.dat");
    p=view(paths[3]);CHECK(p!=MAP_FAILED && p[0]==3);
    CHECK(unlink(paths[3])==0 && put(paths[3],91)==0);
    unsigned char *replacement=view(paths[3]);
    CHECK(replacement!=MAP_FAILED && replacement[0]==91 && p[0]==3);
    CHECK(munmap(replacement,4096)==0 && munmap(p,4096)==0);
    for(int i=0;i<count;i++)CHECK(unlink(paths[i])==0);
    puts("[PRIVATE-CACHE] truncate/pending-write/shared-promotion/rename/unlink coherency PASS");

    /* Force reuse of idle slots while keeping physical memory bounded. */
    CHECK(ioctl(control,BLES_GFX_WINE_CACHE_PERF,&before)==0);
    int rounds=argc>1?48:300;
    for(int i=0;i<rounds;i++) {
        char path[64];snprintf(path,sizeof(path),"/WINEGATE/pc-lru-%d.dat",i);
        CHECK(put(path,i)==0);
        p=view(path);CHECK(p!=MAP_FAILED && p[0]==(unsigned char)i);
        CHECK(munmap(p,4096)==0);
    }
    CHECK(ioctl(control,BLES_GFX_WINE_CACHE_PERF,&after)==0);
    CHECK(after.cache_evictions>before.cache_evictions);
    CHECK(after.cache_bytes<=before.cache_bytes);
    CHECK(after.vfs_handles==before.vfs_handles && after.private_fds==before.private_fds);
    for(int i=0;i<rounds;i++) {
        char path[64];snprintf(path,sizeof(path),"/WINEGATE/pc-lru-%d.dat",i);
        CHECK(unlink(path)==0);
    }
    puts("[PRIVATE-CACHE] cache pages released on final unmap and zero FD growth PASS");
    /* The Wine shell32 Unix image exceeds 8 MiB. Exercise a large private
       image with two views after closing its descriptor, including COW. */
    if(argc<=1) {
    const uint32_t large_size=9U*1024U*1024U;
    CHECK(ioctl(control,BLES_GFX_WINE_CACHE_PERF,&before)==0);
    fd=open("/WINEGATE/pc-large.dat",O_RDONLY);CHECK(fd>=0);
    p=mmap(NULL,large_size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    replacement=mmap(NULL,large_size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    CHECK(p!=MAP_FAILED && replacement!=MAP_FAILED && close(fd)==0);
    CHECK(p[0]==37 && p[large_size-1]==37 && replacement[large_size-1]==37);
    p[large_size-1]=93;CHECK(replacement[large_size-1]==37);
    CHECK(ioctl(control,BLES_GFX_WINE_CACHE_PERF,&after)==0);
    CHECK(after.cache_bytes==before.cache_bytes+2U*4096U);
    CHECK(after.cache_entries>=before.cache_entries+1U);
    CHECK(after.vfs_handles==before.vfs_handles && after.private_fds==before.private_fds);
    CHECK(munmap(p,large_size)==0 && munmap(replacement,large_size)==0);
    puts("[PRIVATE-CACHE] 9 MiB image uses two clean physical pages; closed FD and per-page COW PASS");
    }
    CHECK(close(control)==0);
    return 0;
}
