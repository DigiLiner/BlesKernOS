#include "bleskernos.h"
#include "bleskernos_thread.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define PIPE_FD_BASE 256
#define PIPE_COUNT 16
#define PIPE_CAPACITY 16384U
#define PIPE_READER 0
#define PIPE_WRITER 1

typedef struct {
    unsigned char *buffer;
    unsigned int read_position, write_position, count;
    unsigned int readers, writers;
    int read_flags, write_flags;
    bk_mutex_t lock;
    int used;
} posix_pipe_t;
static posix_pipe_t pipes[PIPE_COUNT];
static bk_mutex_t pipe_table_lock;
static volatile unsigned int pipe_table_initialized;

static void table_init(void) {
    if (__sync_bool_compare_and_swap(&pipe_table_initialized, 0U, 1U))
        bk_mutex_init(&pipe_table_lock);
    while (!pipe_table_initialized) bk_yield();
}
static int pipe_fd(int index,int end){return PIPE_FD_BASE+index*2+end;}
static posix_pipe_t *pipe_for(int fd,int *end){int n=fd-PIPE_FD_BASE,index;if(n<0)return 0;index=n/2;if(index<0||index>=PIPE_COUNT||!pipes[index].used)return 0;if(end)*end=n&1;return &pipes[index];}
int __blesk_posix_is_pipe(int fd){return pipe_for(fd,0)!=0;}

int pipe(int descriptors[2]) {
    int index=-1;
    if(!descriptors){errno=EFAULT;return-1;}
    table_init(); bk_mutex_lock(&pipe_table_lock);
    for(int i=0;i<PIPE_COUNT;i++)if(!pipes[i].used){index=i;break;}
    if(index<0){bk_mutex_unlock(&pipe_table_lock);errno=EMFILE;return-1;}
    memset(&pipes[index],0,sizeof(pipes[index]));
    pipes[index].buffer=(unsigned char*)malloc(PIPE_CAPACITY);
    if(!pipes[index].buffer){bk_mutex_unlock(&pipe_table_lock);return-1;}
    bk_mutex_init(&pipes[index].lock);pipes[index].readers=1;pipes[index].writers=1;pipes[index].used=1;
    descriptors[0]=pipe_fd(index,PIPE_READER);descriptors[1]=pipe_fd(index,PIPE_WRITER);
    bk_mutex_unlock(&pipe_table_lock);return 0;
}

ssize_t __blesk_posix_pipe_read(int fd,void *buffer,size_t length){int end;posix_pipe_t*p=pipe_for(fd,&end);size_t done=0;if(!p||end!=PIPE_READER){errno=EBADF;return-1;}if(!buffer&&length){errno=EFAULT;return-1;}while(done<length){bk_mutex_lock(&p->lock);while(p->count&&done<length){((unsigned char*)buffer)[done++]=p->buffer[p->read_position];p->read_position=(p->read_position+1U)%PIPE_CAPACITY;p->count--;}if(done||!p->writers){bk_mutex_unlock(&p->lock);return(ssize_t)done;}if(p->read_flags&O_NONBLOCK){bk_mutex_unlock(&p->lock);errno=EAGAIN;return-1;}bk_mutex_unlock(&p->lock);bk_sleep(1U);}return(ssize_t)done;}
ssize_t __blesk_posix_pipe_write(int fd,const void *buffer,size_t length){int end;posix_pipe_t*p=pipe_for(fd,&end);size_t done=0;if(!p||end!=PIPE_WRITER){errno=EBADF;return-1;}if(!buffer&&length){errno=EFAULT;return-1;}while(done<length){bk_mutex_lock(&p->lock);if(!p->readers){bk_mutex_unlock(&p->lock);errno=EPIPE;return done?(ssize_t)done:-1;}while(p->count<PIPE_CAPACITY&&done<length){p->buffer[p->write_position]=((const unsigned char*)buffer)[done++];p->write_position=(p->write_position+1U)%PIPE_CAPACITY;p->count++;}if(done==length){bk_mutex_unlock(&p->lock);return(ssize_t)done;}if(p->write_flags&O_NONBLOCK){bk_mutex_unlock(&p->lock);if(done)return(ssize_t)done;errno=EAGAIN;return-1;}bk_mutex_unlock(&p->lock);bk_sleep(1U);}return(ssize_t)done;}
int __blesk_posix_pipe_close(int fd){int end,index;posix_pipe_t*p=pipe_for(fd,&end);if(!p){errno=EBADF;return-1;}index=(fd-PIPE_FD_BASE)/2;bk_mutex_lock(&p->lock);if(end==PIPE_READER){if(!p->readers){bk_mutex_unlock(&p->lock);errno=EBADF;return-1;}p->readers=0;}else{if(!p->writers){bk_mutex_unlock(&p->lock);errno=EBADF;return-1;}p->writers=0;}if(!p->readers&&!p->writers){unsigned char*b=p->buffer;p->used=0;bk_mutex_unlock(&p->lock);free(b);memset(p,0,sizeof(*p));(void)index;}else bk_mutex_unlock(&p->lock);return 0;}
int __blesk_posix_pipe_fcntl(int fd,int command,int value){int end;posix_pipe_t*p=pipe_for(fd,&end);int*r;if(!p){errno=EBADF;return-1;}r=end==PIPE_READER?&p->read_flags:&p->write_flags;if(command==F_GETFL)return*r;if(command==F_SETFL){*r=value&O_NONBLOCK;return 0;}errno=EINVAL;return-1;}
short __blesk_posix_pipe_poll(int fd,short events){int end;posix_pipe_t*p=pipe_for(fd,&end);short result=0;if(!p)return POLLNVAL;bk_mutex_lock(&p->lock);if(end==PIPE_READER){if((events&POLLIN)&&p->count)result|=POLLIN;if(!p->writers)result|=POLLHUP;}else{if(!p->readers)result|=POLLERR;if((events&POLLOUT)&&p->readers&&p->count<PIPE_CAPACITY)result|=POLLOUT;}bk_mutex_unlock(&p->lock);return result;}

int select(int nfds,fd_set*r,fd_set*w,fd_set*x,struct timeval*t){struct pollfd*items;int count=0,timeout=-1,result,index=0;if(nfds<0||nfds>FD_SETSIZE){errno=EINVAL;return-1;}for(int fd=0;fd<nfds;fd++)if((r&&FD_ISSET(fd,r))||(w&&FD_ISSET(fd,w))||(x&&FD_ISSET(fd,x)))count++;items=count?(struct pollfd*)calloc((size_t)count,sizeof(*items)):0;if(count&&!items)return-1;for(int fd=0;fd<nfds;fd++){short events=0;if(r&&FD_ISSET(fd,r))events|=POLLIN;if(w&&FD_ISSET(fd,w))events|=POLLOUT;if(x&&FD_ISSET(fd,x))events|=POLLERR;if(events){items[index].fd=fd;items[index++].events=events;}}if(t){if(t->tv_sec<0||t->tv_usec<0||t->tv_usec>=1000000){free(items);errno=EINVAL;return-1;}timeout=t->tv_sec>2147483?2147483647:(int)(t->tv_sec*1000+t->tv_usec/1000);}result=poll(items,(nfds_t)count,timeout);if(result>=0){if(r)FD_ZERO(r);if(w)FD_ZERO(w);if(x)FD_ZERO(x);for(int i=0;i<count;i++){if(r&&(items[i].revents&(POLLIN|POLLHUP)))FD_SET(items[i].fd,r);if(w&&(items[i].revents&POLLOUT))FD_SET(items[i].fd,w);if(x&&(items[i].revents&(POLLERR|POLLNVAL)))FD_SET(items[i].fd,x);}}free(items);return result;}

int sem_init(sem_t*s,int shared,unsigned int value){if(!s||shared){errno=shared?ENOSYS:EINVAL;return-1;}bk_semaphore_init(s,value);return 0;}int sem_destroy(sem_t*s){if(!s){errno=EINVAL;return-1;}return 0;}int sem_wait(sem_t*s){if(!s){errno=EINVAL;return-1;}bk_semaphore_wait(s);return 0;}int sem_trywait(sem_t*s){if(!s){errno=EINVAL;return-1;}if(bk_semaphore_try_wait(s)<0){errno=EAGAIN;return-1;}return 0;}int sem_post(sem_t*s){if(!s){errno=EINVAL;return-1;}bk_semaphore_post(s);return 0;}int sem_getvalue(sem_t*s,int*v){if(!s||!v){errno=EINVAL;return-1;}*v=(int)bk_atomic_load(&s->value);return 0;}int sem_timedwait(sem_t*s,const struct timespec*a){if(!s||!a){errno=EINVAL;return-1;}for(;;){if(bk_semaphore_try_wait(s)==0)return 0;struct timespec now;if(clock_gettime(CLOCK_REALTIME,&now)<0)return-1;if(now.tv_sec>a->tv_sec||(now.tv_sec==a->tv_sec&&now.tv_nsec>=a->tv_nsec)){errno=ETIMEDOUT;return-1;}bk_sleep(1U);}}
