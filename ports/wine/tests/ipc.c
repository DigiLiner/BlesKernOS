#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
#define CHECK(x) do{if(!(x)){printf("[IPC] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static volatile sig_atomic_t interrupted;
static void interrupt_read(int signal) { if(signal==SIGUSR1)interrupted=1; }
static int sendfd(int sock,int fd) {
    char byte='F',control[CMSG_SPACE(sizeof(int))];struct iovec iov={&byte,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
    memset(control,0,sizeof control);struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof fd);memcpy(CMSG_DATA(c),&fd,sizeof fd);
    return sendmsg(sock,&msg,0);
}
int main(void) {
    int fd[2];char data[4096],readback[4096];memset(data,'P',sizeof data);
    CHECK(pipe2(fd,O_NONBLOCK|O_CLOEXEC)==0 && fcntl(fd[0],F_GETFD)==FD_CLOEXEC);
    CHECK(read(fd[0],readback,1)==-1 && errno==EAGAIN);
    CHECK(write(fd[1],data,4000)==4000 && write(fd[1],data,100)==-1 && errno==EAGAIN);
    CHECK(read(fd[0],readback,4096)==4000 && !memcmp(data,readback,4000));
    int duplicate=dup(fd[0]);CHECK(duplicate>=0 && fcntl(duplicate,F_GETFD)==0);
    CHECK(fcntl(duplicate,F_SETFL,0)==0 && !(fcntl(fd[0],F_GETFL)&O_NONBLOCK));
    close(duplicate);close(fd[0]);close(fd[1]);
    CHECK(pipe(fd)==0);pid_t child=fork();CHECK(child>=0);
    if(!child){close(fd[1]);unsigned n=0;int got;while((got=read(fd[0],readback,sizeof readback))>0)n+=got;_exit(n==16384?0:81);}
    close(fd[0]);unsigned n=0;while(n<16384){int written=write(fd[1],data,sizeof data);CHECK(written>0);n+=written;}close(fd[1]);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    /* Delayed peers force the empty-wait path beyond immediate handoffs.
       Verify readiness, blocking reads, timeout, EOF and signal interruption. */
    for(unsigned mode=0;mode<3;mode++) {
        CHECK(pipe(fd)==0);child=fork();CHECK(child>=0);
        if(!child) {
            close(fd[0]);struct timespec delay={0,100000000};
            if(nanosleep(&delay,NULL) || write(fd[1],"D",1)!=1)_exit(82);
            close(fd[1]);_exit(0);
        }
        close(fd[1]);
        if(mode==0) {
            struct pollfd pending={fd[0],POLLIN,0};
            CHECK(poll(&pending,1,1000)==1 && (pending.revents&POLLIN));
        } else if(mode==1) {
            fd_set ready;FD_ZERO(&ready);FD_SET(fd[0],&ready);
            struct timeval timeout={1,0};
            CHECK(select(fd[0]+1,&ready,NULL,NULL,&timeout)==1 && FD_ISSET(fd[0],&ready));
        }
        CHECK(read(fd[0],readback,1)==1 && readback[0]=='D');
        CHECK(read(fd[0],readback,1)==0 && close(fd[0])==0);
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    CHECK(pipe(fd)==0);
    struct pollfd pending={fd[0],POLLIN,0};
    CHECK(poll(&pending,1,30)==0);
    struct sigaction action={.sa_handler=interrupt_read},old_action;
    CHECK(sigaction(SIGUSR1,&action,&old_action)==0);
    int ready_pipe[2];CHECK(pipe(ready_pipe)==0);
    pid_t parent=getpid();child=fork();CHECK(child>=0);
    if(!child) {
        close(ready_pipe[1]);char ready;
        if(read(ready_pipe[0],&ready,1)!=1)_exit(83);
        close(ready_pipe[0]);
        struct timespec delay={0,100000000};
        if(nanosleep(&delay,NULL) || kill(parent,SIGUSR1))_exit(83);
        /* Bound a failed interruption test instead of hanging the gate. */
        if(nanosleep(&delay,NULL) || write(fd[1],"!",1)!=1)_exit(83);
        _exit(0);
    }
    CHECK(close(ready_pipe[0])==0 && write(ready_pipe[1],"R",1)==1 && close(ready_pipe[1])==0);
    errno=0;CHECK(read(fd[0],readback,1)==-1 && errno==EINTR && interrupted);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(close(fd[0])==0 && close(fd[1])==0 && sigaction(SIGUSR1,&old_action,NULL)==0);
    puts("[IPC] delayed read/poll/select, timeout, EOF and EINTR PASS");
    unsigned args[4]={AF_UNIX,SOCK_STREAM,0,(unsigned)fd};CHECK(syscall(SYS_socketcall,8,args)==0);
    int source=open("/WINEGATE/bin/case01",O_RDONLY);CHECK(source>=0 && sendfd(fd[0],source)==1);close(source);
    char control[CMSG_SPACE(sizeof(int))],byte;struct iovec iov={&byte,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
    CHECK(recvmsg(fd[1],&msg,MSG_CMSG_CLOEXEC)==1 && !(msg.msg_flags&MSG_CTRUNC));
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);int received;CHECK(c && c->cmsg_type==SCM_RIGHTS);
    memcpy(&received,CMSG_DATA(c),sizeof received);CHECK(fcntl(received,F_GETFD)==FD_CLOEXEC && read(received,&byte,1)==1 && byte==0x7f);close(received);
    source=dup(2);CHECK(source>=0 && sendfd(fd[0],source)==1);close(source);
    msg.msg_controllen=sizeof control;msg.msg_flags=0;
    CHECK(recvmsg(fd[1],&msg,MSG_CMSG_CLOEXEC)==1);
    c=CMSG_FIRSTHDR(&msg);CHECK(c && c->cmsg_type==SCM_RIGHTS);
    memcpy(&received,CMSG_DATA(c),sizeof received);
    CHECK(write(received,"[IPC] stderr transferido PASS\n",29)==29);close(received);
    source=open("/dev/null",O_RDWR);CHECK(source>=0 && sendfd(fd[0],source)==1);close(source);
    msg.msg_controllen=sizeof control;msg.msg_flags=0;
    CHECK(recvmsg(fd[1],&msg,0)==1);c=CMSG_FIRSTHDR(&msg);CHECK(c);
    memcpy(&received,CMSG_DATA(c),sizeof received);
    CHECK(read(received,&byte,1)==0 && write(received,"N",1)==1);close(received);
    close(fd[0]);close(fd[1]);
    for(unsigned i=0;i<40;i++) {
        CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);
        CHECK(sendfd(fd[0],fd[1])==1); /* ciclo hacia la propia cola receptora */
        close(fd[0]);close(fd[1]);
    }
    for(unsigned i=0;i<40;i++) {
        int other[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0 && socketpair(AF_UNIX,SOCK_STREAM,0,other)==0);
        CHECK(sendfd(fd[0],other[1])==1 && sendfd(other[0],fd[1])==1);
        close(fd[0]);close(fd[1]);close(other[0]);close(other[1]);
    }
    /* Wine keeps request/reply/wait pipes for several simultaneous clients.
       Exercise endpoints beyond the former 32-pair ceiling, including the
       old /dev/null raw-handle range and SCM_RIGHTS cycle collection there. */
    int held[56][2];
    for(unsigned i=0;i<56;i++) CHECK(pipe(held[i])==0);
    int nullfd=open("/dev/null",O_RDWR);CHECK(nullfd>=0);
    CHECK(read(nullfd,&byte,1)==0 && write(nullfd,"N",1)==1);
    for(unsigned i=0;i<56;i++) {
        CHECK(write(held[i][1],"P",1)==1);
        CHECK(read(held[i][0],&byte,1)==1 && byte=='P');
    }
    for(unsigned i=0;i<50;i++) {
        int other[2];
        CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0 && socketpair(AF_UNIX,SOCK_STREAM,0,other)==0);
        CHECK(sendfd(fd[0],other[1])==1 && sendfd(other[0],fd[1])==1);
        close(fd[0]);close(fd[1]);close(other[0]);close(other[1]);
    }
    CHECK(close(nullfd)==0);
    for(unsigned i=0;i<56;i++) CHECK(close(held[i][0])==0 && close(held[i][1])==0);
    puts("[IPC] 56 live pipes, device separation and high-endpoint cycles PASS");
    /* Wine's server combines disk files with many client pipes. Check the
       descriptor table independently of the number of raw IPC objects. */
    CHECK(pipe(fd)==0);
    int high=fcntl(fd[0],F_DUPFD_CLOEXEC,300);CHECK(high>=300);
    CHECK(fcntl(high,F_GETFD)==FD_CLOEXEC);
    CHECK(write(fd[1],"H",1)==1);
    struct pollfd high_poll={high,POLLIN,0};
    CHECK(poll(&high_poll,1,0)==1 && (high_poll.revents&POLLIN));
    fd_set high_set;FD_ZERO(&high_set);FD_SET(high,&high_set);
    struct timeval immediate={0,0};
    CHECK(select(high+1,&high_set,NULL,NULL,&immediate)==1 && FD_ISSET(high,&high_set));
    CHECK(read(high,&byte,1)==1 && byte=='H');
    int duplicates[200];
    for(unsigned i=0;i<200;i++)CHECK((duplicates[i]=dup(fd[1]))>=0);
    CHECK(write(duplicates[199],"D",1)==1 && read(high,&byte,1)==1 && byte=='D');
    for(unsigned i=0;i<200;i++)CHECK(close(duplicates[i])==0);
    CHECK(close(high)==0 && close(fd[0])==0 && close(fd[1])==0);
    puts("[IPC] 200 descriptors plus fd>=300 poll/select PASS");
    /* A wait spanning unrelated pairs must see either producer. Keep one
       writer idle so that readiness cannot be confused with premature EOF. */
    for(unsigned mode=0;mode<2;mode++) {
        int other[2];CHECK(pipe(fd)==0 && pipe(other)==0);
        child=fork();CHECK(child>=0);
        if(!child) {
            close(fd[0]);close(other[0]);
            struct timespec delay={0,120000000};nanosleep(&delay,NULL);
            _exit(write(other[1],"M",1)==1?0:83);
        }
        if(mode==0) {
            struct pollfd set[2]={{fd[0],POLLIN,0},{other[0],POLLIN,0}};
            CHECK(poll(set,2,30)==0);
            CHECK(poll(set,2,1000)==1 && (set[1].revents&POLLIN));
        } else {
            fd_set set;FD_ZERO(&set);FD_SET(fd[0],&set);FD_SET(other[0],&set);
            struct timeval timeout={1,0};
            int highest=fd[0]>other[0]?fd[0]:other[0];
            CHECK(select(highest+1,&set,NULL,NULL,&timeout)==1 && FD_ISSET(other[0],&set));
        }
        CHECK(read(other[0],&byte,1)==1 && byte=='M');
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
        close(fd[0]);close(fd[1]);close(other[0]);close(other[1]);
    }
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);
    child=fork();CHECK(child>=0);
    if(!child) {
        close(fd[0]);struct timespec delay={0,120000000};nanosleep(&delay,NULL);
        _exit(shutdown(fd[1],SHUT_WR)==0?0:84);
    }
    close(fd[1]);CHECK(read(fd[0],&byte,1)==0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(fd[0]);
    puts("[IPC] multi-pair poll/select, deadlines and shutdown EOF PASS");
    /* A frame-sized stream wraps the larger socket ring several times. Pipes
       above still require atomic EAGAIN when only 96 bytes remain. */
    CHECK(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,fd)==0);
    static unsigned char frame[300000],frame_read[300000];
    for(unsigned i=0;i<sizeof frame;i++)frame[i]=(unsigned char)(i*13U+i/251U);
    unsigned sent=0,frame_received=0;
    while(frame_received<sizeof frame){
        if(sent<sizeof frame){
            int n=write(fd[0],frame+sent,sizeof frame-sent);
            CHECK(n>0 || (n<0 && errno==EAGAIN));if(n>0)sent+=n;
        }
        int n=read(fd[1],frame_read+frame_received,sizeof frame-frame_received<12345?sizeof frame-frame_received:12345);
        CHECK(n>0 || (n<0 && errno==EAGAIN));if(n>0)frame_received+=n;
    }
    CHECK(sent==sizeof frame && !memcmp(frame,frame_read,sizeof frame));
    CHECK(close(fd[0])==0 && close(fd[1])==0);
    puts("[IPC] 300000-byte socket frame, ring wrap and backpressure PASS");
    puts("[IPC] backpressure, flags compartidos, fork, socketcall, SCM_RIGHTS y ciclos PASS");return 0;
}
