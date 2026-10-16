#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#define CHECK(x) do{if(!(x)){printf("[LOCK] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static int lock(int fd,int cmd,int type,off_t start,off_t len) {
 struct flock f={.l_type=type,.l_whence=SEEK_SET,.l_start=start,.l_len=len};return fcntl(fd,cmd,&f);
}
int main(void) {
 const char *path="/WINEGATE/lock.dat";unlink(path);
 int fd=open(path,O_CREAT|O_RDWR,0600);CHECK(fd>=0);
 CHECK(lock(fd,F_SETLK,F_WRLCK,0,100)==0);
 CHECK(lock(fd,F_SETLK,F_UNLCK,20,20)==0);
 int p[2],q[2];CHECK(pipe(p)==0 && pipe(q)==0);pid_t parent=getpid(),child=fork();CHECK(child>=0);
 if(!child) {
  struct flock f={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0,.l_len=1};
  if(fcntl(fd,F_GETLK,&f) || f.l_type!=F_WRLCK || f.l_pid!=parent || f.l_start || f.l_len!=20)_exit(81);
  if(lock(fd,F_SETLK,F_WRLCK,0,1)!=-1 || (errno!=EACCES && errno!=EAGAIN))_exit(82);
  if(lock(fd,F_SETLK,F_WRLCK,20,20))_exit(83);
  if(write(p[1],"R",1)!=1)_exit(84);
  if(lock(fd,F_SETLKW,F_WRLCK,0,1))_exit(85);
  if(write(p[1],"L",1)!=1)_exit(86);
  char b;if(read(q[0],&b,1)!=1)_exit(87);
  _exit(0);
 }
 char b;CHECK(read(p[0],&b,1)==1 && b=='R');
 /* Cerrar cualquier descriptor de la identidad libera TODOS los locks POSIX del proceso. */
 int another=open(path,O_RDONLY);CHECK(another>=0 && close(another)==0);
 CHECK(read(p[0],&b,1)==1 && b=='L');
 CHECK(lock(fd,F_SETLK,F_WRLCK,0,1)==-1 && (errno==EAGAIN || errno==EACCES));
 CHECK(write(q[1],"X",1)==1);int status;CHECK(waitpid(child,&status,0)==child && status==0);
 CHECK(lock(fd,F_SETLK,F_WRLCK,0,0)==0);
 CHECK(lock(fd,F_SETLK,F_UNLCK,0,0)==0);
 CHECK(lock(fd,F_SETLK,F_WRLCK,0x100000000LL,4096)==0);
 CHECK(close(fd)==0 && unlink(path)==0);
 close(p[0]);close(p[1]);close(q[0]);close(q[1]);
 puts("[LOCK] identidad, conflicto, split, bloqueo, close/exit y rangos 64-bit PASS");return 0;
}
