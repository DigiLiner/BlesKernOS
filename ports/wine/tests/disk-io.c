#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)) {printf("[DISK-IO] line=%d errno=%d FAIL\n",__LINE__,errno);return 1;} } while(0)
#define SIZE (1024U*1024U+3073U)

static int roundtrip(unsigned seed,int keep) {
    char path[64];snprintf(path,sizeof(path),"/WINEGATE/disk-io-%u.dat",seed);
    unsigned char *expected=malloc(SIZE),*actual=malloc(SIZE);
    CHECK(expected && actual);
    for(unsigned i=0;i<SIZE;i++)expected[i]=(unsigned char)(i*37U+(i>>8)+seed);
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    CHECK(write(fd,expected,SIZE)==SIZE && fsync(fd)==0 && close(fd)==0);
    fd=open(path,O_RDONLY);CHECK(fd>=0);
    CHECK(read(fd,actual,SIZE)==SIZE && !memcmp(actual,expected,SIZE) && close(fd)==0);
    /* An unaligned update straddles the PRD window's 64 KiB boundary. */
    memset(expected+65530U,211U,3000U);
    fd=open(path,O_RDWR);CHECK(fd>=0);
    CHECK(pwrite(fd,expected+65530U,3000U,65530)==3000 && fsync(fd)==0 && close(fd)==0);
    fd=open(path,O_RDONLY);CHECK(fd>=0);
    CHECK(read(fd,actual,SIZE)==SIZE && !memcmp(actual,expected,SIZE) && close(fd)==0);
    if(!keep)CHECK(unlink(path)==0);
    free(expected);free(actual);return 0;
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    unsigned char *data=malloc(SIZE);CHECK(data);
    if(argc>1 && !strcmp(argv[1],"--verify")) {
        int fd=open("/WINEGATE/disk-io-99.dat",O_RDONLY);CHECK(fd>=0);
        CHECK(read(fd,data,SIZE)==SIZE && close(fd)==0);
        for(unsigned i=0;i<SIZE;i++) {
            unsigned char expected=i>=65530U && i<68530U?211U:
                (unsigned char)(i*37U+(i>>8)+99U);
            CHECK(data[i]==expected);
        }
        free(data);puts("[DISK-IO] cold reboot, persisted bytes PASS");return 0;
    }
    int fd=open("/WINEGATE/pc-large.dat",O_RDONLY);CHECK(fd>=0);
    CHECK(read(fd,data,SIZE)==SIZE && close(fd)==0);
    for(unsigned i=0;i<SIZE;i++)CHECK(data[i]==37U);
    free(data);CHECK(roundtrip(1U,0)==0);
    pid_t children[2];
    for(unsigned i=0;i<2;i++) {
        children[i]=fork();CHECK(children[i]>=0);
        if(!children[i])_exit(roundtrip(i+2U,0));
    }
    for(unsigned i=0;i<2;i++) {
        int status;CHECK(waitpid(children[i],&status,0)==children[i]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    puts("[DISK-IO] multi-window transfers, unaligned update, fsync/reopen and concurrent writers PASS");
    CHECK(roundtrip(99U,1)==0);
    puts("[DISK-IO] persistence fixture synced PASS");
    return 0;
}
