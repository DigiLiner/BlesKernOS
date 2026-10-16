#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#define CHECK(x) do { if(!(x)) {printf("[FS] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;} } while(0)
int main(void) {
    struct stat a,b,c;char byte;
    int dir=open("/WINEGATE",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(dir>=0);
    CHECK(fstat(dir,&a)==0 && S_ISDIR(a.st_mode));
    CHECK(read(dir,&byte,1)==-1 && errno==EISDIR);
    CHECK(openat(-1,"/WINEGATE/lib",O_DIRECTORY)>=0);
    CHECK(openat(-1,"lib",O_DIRECTORY)==-1 && errno==EBADF);
    int fd=openat(dir,"identity empty A.tmp",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    int other=openat(dir,"identity empty B.tmp",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(other>=0);
    CHECK(fstat(fd,&a)==0 && fstat(other,&b)==0 && a.st_dev==b.st_dev && a.st_ino!=b.st_ino);
    CHECK(fstatat(dir,"identity empty A.tmp",&c,0)==0 && c.st_dev==a.st_dev && c.st_ino==a.st_ino);
    CHECK(fstatat(fd,"",&c,AT_EMPTY_PATH)==0 && c.st_ino==a.st_ino);
    CHECK(openat(fd,"x",O_RDONLY)==-1 && errno==ENOTDIR);
    CHECK(openat(dir,"identity empty A.tmp",O_DIRECTORY)==-1 && errno==ENOTDIR);
    CHECK(openat(dir,"identity empty A.tmp",O_CREAT|O_EXCL,0600)==-1 && errno==EEXIST);
    CHECK(write(fd,"abc",3)==3 && fstat(fd,&c)==0 && c.st_ino==a.st_ino && c.st_size==3);
    CHECK(rename("/WINEGATE/identity empty A.tmp","/WINEGATE/identity renamed.tmp")==0);
    CHECK(stat("/WINEGATE/identity renamed.tmp",&c)==0 && c.st_ino==a.st_ino);
    CHECK(fstat(fd,&c)==0 && c.st_ino==a.st_ino && lseek(fd,0,SEEK_SET)==0 && read(fd,&byte,1)==1 && byte=='a');
    CHECK(close(fd)==0 && close(other)==0);
    CHECK(unlink("/WINEGATE/identity renamed.tmp")==0 && unlink("/WINEGATE/identity empty B.tmp")==0);
    /* wineserver saves registry hives by renaming a temporary file over an
       existing one. The old open description must not overwrite the result
       when it closes, and the new file must retain the source identity. */
    const char *replace_source="/WINEGATE/registry replacement source.tmp";
    const char *replace_target="/WINEGATE/registry replacement target.reg";
    char replace_buffer[8];
    fd=open(replace_target,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    CHECK(write(fd,"old-hive",8)==8 && fsync(fd)==0);
    other=open(replace_source,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(other>=0);
    CHECK(write(other,"new-hive",8)==8 && fsync(other)==0);
    CHECK(fstat(other,&a)==0);
    CHECK(rename(replace_source,replace_target)==0);
    CHECK(stat(replace_source,&c)==-1 && errno==ENOENT);
    CHECK(stat(replace_target,&c)==0 && c.st_ino==a.st_ino && c.st_size==8);
    CHECK(lseek(fd,0,SEEK_SET)==0 && read(fd,replace_buffer,8)==8 && !memcmp(replace_buffer,"old-hive",8));
    CHECK(close(fd)==0 && close(other)==0);
    fd=open(replace_target,O_RDONLY);CHECK(fd>=0);
    CHECK(read(fd,replace_buffer,8)==8 && !memcmp(replace_buffer,"new-hive",8));
    CHECK(close(fd)==0 && unlink(replace_target)==0);
    puts("[FS] replace existing hive with rename, preserve open descriptor PASS");
    fd=openat(dir,"lib",O_DIRECTORY);CHECK(fd>=0);
    CHECK(syscall(SYS_getdents64,fd,&byte,1)==-1 && errno==EINVAL);
    char buffer[48];int n,found=0;uint64_t last=0;
    while((n=syscall(SYS_getdents64,fd,buffer,sizeof buffer))>0) {
        for(int off=0;off<n;) {
            uint64_t ino,next;uint16_t len;
            memcpy(&ino,buffer+off,8);memcpy(&next,buffer+off+8,8);memcpy(&len,buffer+off+16,2);
            CHECK(len>=24 && !(len&7) && off+len<=n && next>last);last=next;
            CHECK(fstatat(fd,buffer+off+19,&c,0)==0 && c.st_ino==ino);
            if(!strcasecmp(buffer+off+19,"libgate.so"))found=1;
            off+=len;
        }
    }
    CHECK(n==0 && found && lseek(fd,0,SEEK_SET)==0);
    DIR *stream=fdopendir(fd);CHECK(stream && readdir(stream));rewinddir(stream);CHECK(readdir(stream));
    CHECK(closedir(stream)==0 && close(dir)==0);
    CHECK(symlink("lib/libgate.so","/WINEGATE/enumerated-link")==0);
    CHECK(lstat("/WINEGATE/enumerated-link",&c)==0 && S_ISLNK(c.st_mode));
    stream=opendir("/WINEGATE");CHECK(stream);
    struct dirent *de;found=0;
    while((de=readdir(stream))) if(!strcasecmp(de->d_name,"enumerated-link")) {
        CHECK(de->d_ino==c.st_ino && de->d_type==DT_LNK);found=1;
    }
    CHECK(found && closedir(stream)==0 && unlink("/WINEGATE/enumerated-link")==0);
    puts("[FS] getdents reports symlink identity and type PASS");
    /* Repeated statfs must stay coherent across allocation and truncation.
       This also exercises the FAT empty-file fast path before allocating. */
    struct statfs before,allocated,after;
    CHECK(statfs("/WINEGATE",&before)==0);
    fd=open("/WINEGATE/space-check.tmp",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    char payload[16384];memset(payload,0x5a,sizeof payload);
    CHECK(write(fd,payload,sizeof payload)==sizeof payload && fsync(fd)==0);
    CHECK(fstatfs(fd,&allocated)==0 && allocated.f_type==before.f_type);
    CHECK(allocated.f_bfree<before.f_bfree);
    CHECK(ftruncate(fd,0)==0 && fsync(fd)==0);
    CHECK(fstatfs(fd,&after)==0 && after.f_bfree==before.f_bfree);
    CHECK(close(fd)==0 && unlink("/WINEGATE/space-check.tmp")==0);
    CHECK(statfs("/WINEGATE",&after)==0 && after.f_bfree==before.f_bfree);
    puts("[FS] statfs allocation/truncate/unlink coherency PASS");
    /* Office 97 Spanish creates paths such as "Diseños de presentaciones"
       and "Ángulos.pot".  Exact UTF-8 byte names must survive the Linux/VFS
       path, and a filesystem that cannot resolve Unicode case variants must
       not advertise itself as MSDOS/VFAT to Wine (that would disable Wine's
       own case-insensitive directory scan). */
    CHECK(mkdir("/WINEGATE/Diseños de presentaciones",0700)==0);
    fd=open("/WINEGATE/Diseños de presentaciones/Ángulos.pot",
            O_CREAT|O_EXCL|O_RDWR,0600);
    CHECK(fd>=0 && write(fd,"ok",2)==2 && close(fd)==0);
    CHECK(stat("/WINEGATE/Diseños de presentaciones/Ángulos.pot",&c)==0 && c.st_size==2);
    int unicode_case_native =
        stat("/WINEGATE/DISEÑOS DE PRESENTACIONES/ÁNGULOS.POT",&c)==0;
    CHECK(statfs("/WINEGATE",&after)==0);
    CHECK(unicode_case_native || (unsigned long)after.f_type!=0x4d44UL);
    CHECK(unlink("/WINEGATE/Diseños de presentaciones/Ángulos.pot")==0);
    CHECK(rmdir("/WINEGATE/Diseños de presentaciones")==0);
    puts("[FS] UTF-8 names + statfs case-semantics contract PASS");
    /* Exercise cached free counts when a replacement frees an old chain and
       allocates another, then returns all data clusters. Precreate directory
       entries so their allocation cannot affect these exact data deltas. */
    fd=open("/WINEGATE/count-check.tmp",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    CHECK(fstatfs(fd,&before)==0);
    CHECK(statfs("/WINEGATE",&before)==0);
    CHECK(write(fd,payload,sizeof payload)==sizeof payload && fsync(fd)==0);
    CHECK(statfs("/WINEGATE",&allocated)==0);
    printf("[FS:COUNTS] before=%llu allocated=%llu block=%ld expected=%lu\n",
           (unsigned long long)before.f_bfree,(unsigned long long)allocated.f_bfree,
           (long)before.f_bsize,(unsigned long)(sizeof payload/before.f_bsize));
    CHECK(before.f_bfree-allocated.f_bfree==sizeof payload/before.f_bsize);
    CHECK(lseek(fd,0,SEEK_SET)==0 && write(fd,payload,sizeof payload)==sizeof payload && fsync(fd)==0);
    CHECK(statfs("/WINEGATE",&after)==0 && after.f_bfree==allocated.f_bfree);
    CHECK(ftruncate(fd,8192)==0 && fsync(fd)==0);
    CHECK(statfs("/WINEGATE",&after)==0 && before.f_bfree-after.f_bfree==8192U/before.f_bsize);
    CHECK(ftruncate(fd,0)==0 && fsync(fd)==0);
    CHECK(statfs("/WINEGATE",&after)==0 && after.f_bfree==before.f_bfree);
    CHECK(close(fd)==0 && unlink("/WINEGATE/count-check.tmp")==0);
    puts("[FS] exact free counts across rewrite and shrink PASS");
    /* Long names force a newly created directory beyond its first cluster
       on both the 512-byte and 4-KiB fixtures. Verify every entry after growth. */
    CHECK(mkdir("/WINEGATE/directory-growth",0700)==0);
    errno=0;CHECK(rmdir("/WINEGATE/directory-growth/.")==-1 && errno==EINVAL);
    char growing[180];
    for (unsigned i=0;i<48;i++) {
        snprintf(growing,sizeof growing,
            "/WINEGATE/directory-growth/long-name-for-directory-chain-growth-regression-%03u.tmp",i);
        fd=open(growing,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0 && close(fd)==0);
    }
    stream=opendir("/WINEGATE/directory-growth");CHECK(stream);
    found=0;
    while ((de=readdir(stream))) if(de->d_name[0]!='.')found++;
    CHECK(found==48 && closedir(stream)==0);
    errno=0;CHECK(rmdir("/WINEGATE/directory-growth")==-1 && errno==ENOTEMPTY);
    errno=0;CHECK(rmdir(growing)==-1 && errno==ENOTDIR);
    for (unsigned i=0;i<48;i++) {
        snprintf(growing,sizeof growing,
            "/WINEGATE/directory-growth/long-name-for-directory-chain-growth-regression-%03u.tmp",i);
        CHECK(stat(growing,&c)==0 && c.st_size==0 && unlink(growing)==0);
    }
    CHECK(rmdir("/WINEGATE/directory-growth")==0);
    errno=0;CHECK(rmdir("/WINEGATE/directory-growth")==-1 && errno==ENOENT);
    puts("[FS] grow directory across clusters, enumerate and remove PASS");
    /* Tiny reads of a large file use the uncached FAT range path. Include
       a cluster boundary, a distant seek and a short read at EOF. */
    static unsigned char large[131072];
    for(unsigned i=0;i<sizeof large;i++)large[i]=(unsigned char)(i%251);
    fd=open("/WINEGATE/range-check.tmp",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    CHECK(write(fd,large,sizeof large)==(ssize_t)sizeof large);
    CHECK(close(fd)==0);
    fd=open("/WINEGATE/range-check.tmp",O_RDONLY);CHECK(fd>=0);
    static unsigned char large_copy[131072];
    CHECK(read(fd,large_copy,sizeof large_copy)==(ssize_t)sizeof large_copy);
    CHECK(memcmp(large,large_copy,sizeof large)==0);
    const unsigned offsets[]={0,4090,70000,sizeof large-7};
    for(unsigned i=0;i<sizeof offsets/sizeof offsets[0];i++) {
        unsigned char small[17];unsigned want=sizeof large-offsets[i];
        if(want>sizeof small)want=sizeof small;
        CHECK(pread(fd,small,sizeof small,offsets[i])==(int)want);
        CHECK(memcmp(small,large+offsets[i],want)==0);
    }
    CHECK(close(fd)==0 && unlink("/WINEGATE/range-check.tmp")==0);
    puts("[FS] tiny reads, distant offsets, cluster boundary and EOF PASS");
    puts("[FS] directorios, *at, cookies, identidades y rename PASS");return 0;
}
