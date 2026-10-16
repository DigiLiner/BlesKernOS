#define _GNU_SOURCE
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){printf("[INOTIFY] line=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static unsigned char events[16384];static int bytes;
static int collect(int fd){
 bytes=read(fd,events,sizeof events);
 if(bytes<0)return 0;
 for(unsigned at=0;at<(unsigned)bytes;){
  if((unsigned)bytes-at<sizeof(struct inotify_event))return 0;
  struct inotify_event *e=(void *)(events+at);
  if(e->len%4 || e->len>(unsigned)bytes-at-sizeof *e || (e->len && !memchr(e->name,0,e->len)))return 0;
  at+=sizeof(*e)+e->len;
 }
 return 1;
}
static unsigned matches(int wd,unsigned mask,const char *name,unsigned *cookie){
 unsigned count=0;
 for(unsigned at=0;at<(unsigned)bytes;){
  struct inotify_event *e=(void *)(events+at);
  if((wd<0 || e->wd==wd) && (e->mask&mask)==mask &&
     (!name || (e->len && !strcmp(e->name,name)))){count++;if(cookie)*cookie=e->cookie;}
  at+=sizeof(*e)+e->len;
 }
 return count;
}
static int make(const char *path){int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;return close(fd);}
static volatile sig_atomic_t signalled;
static void signal_handler(int sig){(void)sig;signalled=1;}
int main(void){
 setbuf(stdout,NULL);
 const char *dir="/WINEGATE/notify-abi",*moved="/WINEGATE/notify-moved";
 char a[128],b[128],link[128],resolved[260];
 snprintf(a,sizeof a,"%s/a",dir);snprintf(b,sizeof b,"%s/b",dir);
 CHECK(mkdir(dir,0700)==0);
 CHECK(inotify_init1(1)==-1 && errno==EINVAL);
 int fd=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);CHECK(fd>=0);
 CHECK(fcntl(fd,F_GETFD)==FD_CLOEXEC && (fcntl(fd,F_GETFL)&O_NONBLOCK));
 CHECK(inotify_add_watch(-1,dir,IN_CREATE)==-1 && errno==EBADF);
 int plain=open("/dev/null",O_RDONLY);CHECK(plain>=0);
 CHECK(inotify_add_watch(plain,dir,IN_CREATE)==-1 && errno==EINVAL);CHECK(close(plain)==0);
 CHECK(inotify_add_watch(fd,dir,0)==-1 && errno==EINVAL);
 CHECK(inotify_add_watch(fd,"/WINEGATE/missing-notify",IN_CREATE)==-1 && errno==ENOENT);
 CHECK(read(fd,events,sizeof events)==-1 && errno==EAGAIN);
 struct pollfd p={fd,POLLIN|POLLOUT,0};CHECK(poll(&p,1,0)==0);
 int available=-1;CHECK(ioctl(fd,FIONREAD,&available)==0 && available==0);
 int wd=inotify_add_watch(fd,dir,IN_ALL_EVENTS|IN_ONLYDIR);CHECK(wd>=0);
 CHECK(make(a)==0);p.revents=0;CHECK(poll(&p,1,0)==1 && p.revents==POLLIN);
 CHECK(ioctl(fd,FIONREAD,&available)==0 && available>16);
 CHECK(read(fd,events,15)==-1 && errno==EINVAL);
 CHECK(read(fd,events,16)==-1 && errno==EINVAL);
 CHECK(syscall(SYS_read,fd,(void *)1,4096)==-1 && errno==EFAULT);
 int after=0;CHECK(ioctl(fd,FIONREAD,&after)==0 && after==available);
 CHECK(collect(fd) && matches(wd,IN_CREATE,"a",NULL)==1 && matches(wd,IN_OPEN,"a",NULL)==1 && matches(wd,IN_CLOSE_WRITE,"a",NULL)==1);
 puts("[INOTIFY] flags/errors, poll, FIONREAD, whole padded events and nondestructive invalid reads PASS");
 int file=open(a,O_WRONLY);CHECK(file>=0 && collect(fd));
 CHECK(write(file,"A",1)==1 && write(file,"B",1)==1);
 CHECK(collect(fd) && matches(wd,IN_MODIFY,"a",NULL)==1); /* consecutive unread events coalesce */
 CHECK(close(file)==0 && collect(fd));
 int fw=inotify_add_watch(fd,a,IN_MOVE_SELF|IN_MODIFY|IN_DELETE_SELF);CHECK(fw>=0);
 CHECK(inotify_add_watch(fd,a,IN_CREATE|IN_ONLYDIR)==-1 && errno==ENOTDIR);
 CHECK(rename(a,b)==0 && collect(fd));
 unsigned from=0,to=0;
 CHECK(matches(wd,IN_MOVED_FROM,"a",&from)==1 && matches(wd,IN_MOVED_TO,"b",&to)==1 && from && from==to);
 CHECK(matches(fw,IN_MOVE_SELF,NULL,NULL)==1);
 file=open(b,O_WRONLY);CHECK(file>=0 && write(file,"C",1)==1 && close(file)==0 && collect(fd));
 CHECK(matches(fw,IN_MODIFY,NULL,NULL)==1 && matches(wd,IN_MODIFY,"b",NULL)==1);
 CHECK(unlink(b)==0 && collect(fd) && matches(wd,IN_DELETE,"b",NULL)==1);
 CHECK(matches(fw,IN_DELETE_SELF,NULL,NULL)==1 && matches(fw,IN_IGNORED,NULL,NULL)==1);
 CHECK(inotify_rm_watch(fd,fw)==-1 && errno==EINVAL);
 puts("[INOTIFY] write coalescing, rename cookies, moved file identity and deletion/ignored PASS");
 CHECK(inotify_add_watch(fd,dir,IN_CREATE)==wd);
 CHECK(inotify_add_watch(fd,dir,IN_DELETE|IN_MASK_ADD)==wd);
 CHECK(inotify_add_watch(fd,dir,IN_CREATE|IN_MASK_CREATE)==-1 && errno==EEXIST);
 CHECK(inotify_add_watch(fd,dir,IN_CREATE|IN_MASK_CREATE|IN_MASK_ADD)==-1 && errno==EINVAL);
 CHECK(make(a)==0 && unlink(a)==0 && collect(fd));
 CHECK(matches(wd,IN_CREATE,"a",NULL)==1 && matches(wd,IN_DELETE,"a",NULL)==1 && !matches(wd,IN_OPEN,"a",NULL));
 CHECK(inotify_add_watch(fd,dir,IN_CREATE|IN_ONESHOT)==wd);
 CHECK(make(a)==0 && collect(fd) && matches(wd,IN_CREATE,"a",NULL)==1 && matches(wd,IN_IGNORED,NULL,NULL)==1);
 CHECK(inotify_rm_watch(fd,wd)==-1 && errno==EINVAL);
 CHECK(unlink(a)==0 && read(fd,events,sizeof events)==-1 && errno==EAGAIN);
 wd=inotify_add_watch(fd,dir,IN_OPEN|IN_CLOSE_NOWRITE);CHECK(wd>0);
 CHECK(make(a)==0); /* write-close does not match this mask; discard its OPEN */
 CHECK(collect(fd));
 for(unsigned i=0;i<400;i++){file=open(a,O_RDONLY);CHECK(file>=0 && close(file)==0);}
 CHECK(collect(fd) && matches(-1,IN_Q_OVERFLOW,NULL,NULL)==1);
 CHECK(read(fd,events,sizeof events)==-1 && errno==EAGAIN);
 file=open(a,O_RDONLY);CHECK(file>=0 && close(file)==0 && collect(fd));
 CHECK(matches(wd,IN_OPEN,"a",NULL)==1 && matches(wd,IN_CLOSE_NOWRITE,"a",NULL)==1);
 CHECK(unlink(a)==0);
 puts("[INOTIFY] mask replacement/add/create, oneshot, bounded queue overflow and recovery PASS");
 int directory=open(dir,O_RDONLY|O_DIRECTORY);CHECK(directory>=0);
 snprintf(link,sizeof link,"/proc/self/fd/%d",directory);
 CHECK(inotify_add_watch(fd,link,IN_CREATE|IN_DELETE|IN_MOVE_SELF)==wd);
 struct stat st,alias;CHECK(fstat(directory,&st)==0 && stat(link,&alias)==0 && st.st_ino==alias.st_ino && st.st_dev==alias.st_dev);
 int n=readlink(link,resolved,sizeof resolved-1);CHECK(n>0);resolved[n]=0;CHECK(!strcmp(resolved,dir));
 CHECK(rename(dir,moved)==0 && collect(fd) && matches(wd,IN_MOVE_SELF,NULL,NULL)==1);
 CHECK(inotify_add_watch(fd,link,IN_CREATE|IN_DELETE|IN_MOVE_SELF)==wd);
 n=readlink(link,resolved,sizeof resolved-1);CHECK(n>0);resolved[n]=0;CHECK(!strcmp(resolved,moved));
 snprintf(a,sizeof a,"%s/child",moved);CHECK(make(a)==0 && collect(fd) && matches(wd,IN_CREATE,"child",NULL)==1);
 CHECK(unlink(a)==0 && collect(fd));
 char parent[160];snprintf(parent,sizeof parent,"%s/..",link);CHECK(stat(parent,&alias)==0 && stat("/WINEGATE",&st)==0 && alias.st_ino==st.st_ino);
 CHECK(close(directory)==0);
 CHECK(stat(link,&alias)==-1 && errno==ENOENT);
 CHECK(inotify_rm_watch(fd,wd)==0 && collect(fd) && matches(wd,IN_IGNORED,NULL,NULL)==1);
 CHECK(rmdir(moved)==0 && close(fd)==0);
 puts("[INOTIFY] live /proc/self/fd paths, stat/readlink/.. and renamed directory watches PASS");
 CHECK(mkdir(dir,0700)==0);fd=inotify_init();CHECK(fd>=0 && !(fcntl(fd,F_GETFL)&O_NONBLOCK));
 wd=inotify_add_watch(fd,dir,IN_CREATE);CHECK(wd>=0);
 int duplicate=fcntl(fd,F_DUPFD_CLOEXEC,300);CHECK(duplicate>=300 && fcntl(duplicate,F_GETFD)==FD_CLOEXEC);
 snprintf(a,sizeof a,"%s/wakeup",dir);
 pid_t child=fork();CHECK(child>=0);
 if(!child){close(fd);close(duplicate);usleep(30000);_exit(make(a)?1:0);}
 CHECK(collect(duplicate) && matches(wd,IN_CREATE,"wakeup",NULL)==1);
 int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
 struct sigaction sa={0};sa.sa_handler=signal_handler;sigemptyset(&sa.sa_mask);CHECK(sigaction(SIGUSR1,&sa,NULL)==0);
 child=fork();CHECK(child>=0);
 if(!child){close(fd);close(duplicate);usleep(30000);_exit(kill(getppid(),SIGUSR1)?1:0);}
 CHECK(read(duplicate,events,sizeof events)==-1 && errno==EINTR && signalled);
 CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
 CHECK(close(fd)==0 && close(duplicate)==0 && unlink(a)==0 && rmdir(dir)==0);
 for(unsigned i=0;i<256;i++){fd=inotify_init1(IN_NONBLOCK);CHECK(fd>=0 && close(fd)==0);}
 puts("[INOTIFY] blocking wake, dup/fork shared queue, EINTR and repeated instance cleanup PASS");
 puts("[INOTIFY] ALL PASS");return 0;
}
