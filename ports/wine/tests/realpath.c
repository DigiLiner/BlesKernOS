#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define CHECK(x) do{if(!(x)){printf("[REALPATH] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
int main(void) {
 char b[512];const char *path="/WINEGATE/bin/case30";
 CHECK(readlink(path,b,sizeof b)==-1 && errno==EINVAL);
 CHECK(readlink("/WINEGATE/does-not-exist",b,sizeof b)==-1 && errno==ENOENT);
 CHECK(readlink(path,b,0)==-1 && errno==EINVAL);
 int fd=open("/WINEGATE/bin",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);
 CHECK(readlinkat(fd,"case30",b,sizeof b)==-1 && errno==EINVAL);
 CHECK(readlinkat(-1,"relative",b,sizeof b)==-1 && errno==EBADF);close(fd);
 char *s=realpath(path,0);CHECK(s && !strcmp(s,path));free(s);
 const char *link="/WINEGATE/link30";unlink(link);
 CHECK(symlink("bin",link)==0);CHECK(readlink(link,b,2)==2 && !memcmp(b,"bi",2));
 struct stat st;CHECK(lstat(link,&st)==0 && S_ISLNK(st.st_mode) && st.st_size==3);
 CHECK(stat(link,&st)==0 && S_ISDIR(st.st_mode));
 CHECK(open(link,O_RDONLY|O_NOFOLLOW)==-1 && errno==ELOOP);
 s=realpath("/WINEGATE/link30/case30",NULL);CHECK(s && !strcmp(s,path));free(s);
 fd=open("/WINEGATE/link30/case30",O_RDONLY);CHECK(fd>=0 && read(fd,b,1)==1 && b[0]==0x7f);close(fd);
 CHECK(unlink(link)==0 && stat("/WINEGATE/bin",&st)==0);
 CHECK(symlink("missing-target",link)==0 && stat(link,&st)==-1 && errno==ENOENT);
 CHECK(lstat(link,&st)==0 && S_ISLNK(st.st_mode));CHECK(unlink(link)==0);
 puts("[SYMLINK] relativo, stat/lstat, NOFOLLOW, dangling, unlink PASS");
 puts("[REALPATH] readlink de archivo/directorio y realpath glibc PASS");return 0;
}
