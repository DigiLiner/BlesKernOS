#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <sys/uio.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr,"[GATE:A] caso=%d linea=%d errno=%d\n", CASE, __LINE__, errno); \
    return 1; } } while (0)

#if CASE == 4 || CASE == 5 || CASE == 6
static __thread int tls = 17;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int counter;
static void *worker(void *unused) {
    (void)unused;
    if (tls != 17) return (void *)1;
    tls = 31;
    for (int i=0; i<1000; ++i) {
        if (pthread_mutex_lock(&mutex)) return (void *)2;
        ++counter;
        if (pthread_mutex_unlock(&mutex)) return (void *)3;
    }
    return 0;
}
#endif
#if CASE == 6
static int exit_cleanup_ran;
static void exit_cleanup(void *arg) { *(int *)arg=73; }
static void *exit_worker(void *arg) {
    pthread_cleanup_push(exit_cleanup,arg);
    pthread_exit((void *)91);
    pthread_cleanup_pop(0);
    return NULL;
}
#endif
#if CASE == 13 || CASE == 14
static volatile sig_atomic_t received;
static unsigned char alternate[65536];
static void handler(int sig, siginfo_t *info, void *context) {
    uintptr_t sp = (uintptr_t)&sig;
    if (sp < (uintptr_t)alternate || sp >= (uintptr_t)(alternate+sizeof alternate)) _exit(91);
    received = sig;
#if CASE == 14
    ucontext_t *uc = context;
    if (info->si_addr != 0) _exit(92);
    /* movb $1,(%eax) con eax=0: tres bytes. Recuperar EIP via ucontext. */
    uc->uc_mcontext.gregs[REG_EIP] += 3;
#else
    (void)info; (void)context;
#endif
}
#endif

int main(int argc, char **argv) {
    CHECK(argc >= 1 && argv && argv[0]);
#if CASE == 1 || CASE == 2
    CHECK(getauxval(AT_PHENT) == 32 && getauxval(AT_PAGESZ) == 4096);
    CHECK(getauxval(AT_RANDOM)!=0 && getauxval(AT_EXECFN)!=0);
    CHECK(puts("[GATE:A] glibc real: main alcanzado") >= 0);
#elif CASE == 3
    unsigned char *p = malloc(131072);
    CHECK(p); memset(p,0xa5,131072); CHECK(p[131071]==0xa5); free(p);
#elif CASE == 4 || CASE == 5 || CASE == 6
    pthread_t a,b; void *ret;
    tls = 23;
    CHECK(pthread_create(&a,0,worker,0)==0);
    CHECK(pthread_create(&b,0,worker,0)==0);
    CHECK(pthread_join(a,&ret)==0 && ret==0);
    CHECK(pthread_join(b,&ret)==0 && ret==0);
    CHECK(counter==2000 && tls==23);
#if CASE == 6
    CHECK(pthread_create(&a,0,exit_worker,&exit_cleanup_ran)==0);
    CHECK(pthread_join(a,&ret)==0 && ret==(void *)91 && exit_cleanup_ran==73);
    puts("[THREAD] pthread_exit unwinds cleanup handlers PASS");
#endif
#elif CASE == 7
    char *p=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(p!=MAP_FAILED); p[0]=42;p[4096]=19;p[8192]=24;
    CHECK(mmap(p+4096,4096,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==p+4096);
    CHECK(p[0]==42 && p[4096]==0 && p[8192]==24);
    CHECK(mmap(p+1,4096,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED && errno==EINVAL);
    CHECK(mprotect(p,4096,PROT_READ)==0 && p[0]==42);
    CHECK(syscall(SYS_getrandom,p,4,0)==-1 && errno==EFAULT);
    p[8192]=25;CHECK(mprotect(p,4096,PROT_READ|PROT_WRITE)==0); p[0]=43;
    CHECK(munmap(p,12288)==0);
    /* Incremental Win32 commits can cover already-backed pages and the next
       PROT_NONE reservation in one mprotect call. Keep the old bytes. */
    char *commit=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(commit!=MAP_FAILED);
    CHECK(mmap(commit,12288,PROT_NONE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|
        MAP_NORESERVE,-1,0)==commit);
    CHECK(mprotect(commit,4096,PROT_READ|PROT_WRITE)==0);
    commit[0]=57;commit[4095]=81;
    CHECK(mprotect(commit,8192,PROT_READ|PROT_WRITE)==0);
    CHECK(commit[0]==57 && commit[4095]==81 && commit[4096]==0);
    commit[4096]=93;
    CHECK(syscall(SYS_getrandom,commit+8192,4,0)==-1 && errno==EFAULT);
    CHECK(mprotect(commit,12288,PROT_READ|PROT_WRITE)==0);
    CHECK(commit[0]==57 && commit[4096]==93 && commit[8192]==0);
    CHECK(mprotect(commit,12288,PROT_READ)==0);
    CHECK(syscall(SYS_getrandom,commit+4096,4,0)==-1 && errno==EFAULT);
    CHECK(mprotect(commit,12288,PROT_READ|PROT_WRITE)==0);
    CHECK(commit[0]==57 && commit[4096]==93 && munmap(commit,12288)==0);
    puts("[VM] incremental commits across adjacent VMAs preserve old data PASS");
    /* Texture uploads grow Wine heaps one page at a time and recommit the
       entire prefix. Exercise a fragmented, growing VMA table. */
    const unsigned heap_pages=128;
    char *heap=mmap(0,heap_pages*4096,PROT_NONE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    CHECK(heap!=MAP_FAILED);
    for (unsigned n=1;n<=heap_pages;n++) {
        CHECK(mprotect(heap,n*4096,PROT_READ|PROT_WRITE)==0);
        CHECK(heap[(n-1)*4096]==0);
        heap[(n-1)*4096]=(char)n;
        for (unsigned i=0;i<n;i++) CHECK((unsigned char)heap[i*4096]==i+1);
    }
    CHECK(mprotect(heap,heap_pages*4096,PROT_READ)==0);
    CHECK(syscall(SYS_getrandom,heap+64*4096,4,0)==-1 && errno==EFAULT);
    CHECK(mprotect(heap,heap_pages*4096,PROT_READ|PROT_WRITE)==0);
    CHECK(munmap(heap,heap_pages*4096)==0);
    puts("[VM] growing texture heap preserves 128 incremental commits PASS");
    /* The resident kernel occupies this low identity window on BlesKernOS.
       Wine's reserve-then-mprotect sequence must never replace its PTEs. */
    void *kernel_window=(void *)0x00110000;
    CHECK(mmap(kernel_window,0x100000,PROT_NONE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED && errno==EEXIST);
    CHECK(mmap(kernel_window,0x100000,PROT_NONE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_NORESERVE,-1,0)==MAP_FAILED && errno==ENOMEM);
    CHECK(mprotect(kernel_window,4096,PROT_READ|PROT_WRITE)==-1 && errno==ENOMEM);
    CHECK(mprotect(kernel_window,4096,PROT_NONE)==-1 && errno==ENOMEM);
    puts("[VM] kernel window rejects reservation and mprotect PASS");
    /* The kernel still dereferences its heap through the process CR3.
       Replacing these PTEs must not hide its BSS or allocator header.
       Test both a direct hit and a range starting below that boundary. */
    const uintptr_t heap_probes[] = {0x02000000U, 0x01fff000U, 0x02400000U};
    for (unsigned i=0; i<sizeof(heap_probes)/sizeof(heap_probes[0]); ++i) {
        void *base=(void *)heap_probes[i];
        CHECK(mmap(base,8192,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|
            MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED && errno==EEXIST);
        CHECK(mmap(base,8192,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|
            MAP_FIXED|MAP_NORESERVE,-1,0)==MAP_FAILED && errno==ENOMEM);
        CHECK(mmap(base,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|
            MAP_FIXED,-1,0)==MAP_FAILED && errno==ENOMEM);
        CHECK(mprotect(base,8192,PROT_READ|PROT_WRITE)==-1 && errno==ENOMEM);
    }
    /* Still allow the DOS window and an ordinary hint in the heap: a hint
       must be relocated, not treated as MAP_FIXED. */
    char *hint=mmap((void *)0x02400000U,4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(hint!=MAP_FAILED && hint!=(void *)0x02400000U);
    hint[0]=71; CHECK(hint[0]==71 && munmap(hint,4096)==0);
    char *dos=mmap((void *)0x00010000U,0x100000U,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    CHECK(dos==(void *)0x00010000U);
    dos[0]=19;dos[0xfffff]=73;
    CHECK(dos[0]==19 && dos[0xfffff]==73 && munmap(dos,0x100000U)==0);
    puts("[VM] kernel heap aliases protected; DOS window and hints PASS");
#elif CASE == 8
    pid_t child=fork(); int status;
    CHECK(child>=0);
    if (!child) { char *args[]={"/WINEGATE/bin/case01",0}; char *env[]={0}; execve(args[0],args,env); _exit(90); }
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
#elif CASE == 9
    int fd[2]; char c=0; CHECK(pipe(fd)==0);
    CHECK(write(fd[1],"P",1)==1 && close(fd[1])==0);
    CHECK(read(fd[0],&c,1)==1 && c=='P' && read(fd[0],&c,1)==0); CHECK(close(fd[0])==0);
#elif CASE == 10
    int fd[2]; CHECK(pipe(fd)==0);
    struct pollfd p={fd[0],POLLIN,0}; CHECK(poll(&p,1,0)==0);
    CHECK(write(fd[1],"x",1)==1 && poll(&p,1,0)==1 && (p.revents&POLLIN));
    fd_set set; FD_ZERO(&set); FD_SET(fd[0],&set); struct timeval tv={0,0};
    CHECK(select(fd[0]+1,&set,0,0,&tv)==1 && FD_ISSET(fd[0],&set)); close(fd[0]);close(fd[1]);
#elif CASE == 11
    int fd[2]; char c; CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);
    CHECK(write(fd[0],"U",1)==1 && read(fd[1],&c,1)==1 && c=='U');
    CHECK(shutdown(fd[0],SHUT_WR)==0 && read(fd[1],&c,1)==0);close(fd[0]);close(fd[1]);
#elif CASE == 12
    int fd[2],sent=open("/WINEGATE/bin/case01",O_RDONLY),got=-1; char c='F';
    CHECK(sent>=0 && socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);
    union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control,0,sizeof control);
    struct iovec iov={&c,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof control};
    struct cmsghdr *cm=CMSG_FIRSTHDR(&msg); cm->cmsg_level=SOL_SOCKET;cm->cmsg_type=SCM_RIGHTS;cm->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm),&sent,sizeof sent);CHECK(sendmsg(fd[0],&msg,0)==1);close(sent);
    memset(&control,0,sizeof control);CHECK(recvmsg(fd[1],&msg,0)==1 && !(msg.msg_flags&MSG_CTRUNC));
    cm=CMSG_FIRSTHDR(&msg);CHECK(cm && cm->cmsg_level==SOL_SOCKET && cm->cmsg_type==SCM_RIGHTS);
    memcpy(&got,CMSG_DATA(cm),sizeof got);CHECK(read(got,&c,1)==1 && c==0x7f);close(got);close(fd[0]);close(fd[1]);
#elif CASE == 13 || CASE == 14
    stack_t ss={.ss_sp=alternate,.ss_size=sizeof alternate};
    struct sigaction sa={.sa_sigaction=handler,.sa_flags=SA_SIGINFO|SA_ONSTACK};
    CHECK(sigemptyset(&sa.sa_mask)==0 && sigaltstack(&ss,0)==0);
#if CASE == 13
    const int signals[]={SIGILL,SIGFPE,SIGTRAP,SIGINT,SIGTERM};
    for (unsigned i=0;i<sizeof signals/sizeof signals[0];++i) {
        received=0;CHECK(sigaction(signals[i],&sa,0)==0);
        CHECK(raise(signals[i])==0 && received==signals[i]);
    }
#else
    CHECK(sigaction(SIGSEGV,&sa,0)==0);
    __asm__ volatile("xorl %%eax,%%eax; movb $1,(%%eax)" ::: "eax","memory");
    CHECK(received==SIGSEGV);
#endif
#elif CASE == 15
    DIR *dir=opendir("/WINEGATE/bin"); int found=0;struct dirent *ent; CHECK(dir);
    errno=0;while ((ent=readdir(dir))) if (!strcasecmp(ent->d_name,"case01")) found=1;
    CHECK(errno==0 && found && closedir(dir)==0);
#elif CASE == 16
    void *so=dlopen("/WINEGATE/lib/libgate.so",RTLD_NOW);
    if (!so) fprintf(stderr,"[GATE:A] dlopen: %s\n",dlerror());CHECK(so);
    int (*value)(void)=dlsym(so,"gate_value");
    if (!value) fprintf(stderr,"[GATE:A] dlsym: %s\n",dlerror());
    CHECK(value && value()==73);CHECK(dlclose(so)==0);
#elif CASE == 17
    const char *a="/WINEGATE/long file name.tmp",*b="/WINEGATE/renamed file.tmp";struct stat st;
    int fd=open(a,O_CREAT|O_TRUNC|O_RDWR|O_CLOEXEC,0600);CHECK(fd>=0);
    CHECK(fcntl(fd,F_GETFD)==FD_CLOEXEC);
    int duplicate=fcntl(fd,F_DUPFD_CLOEXEC,10);CHECK(duplicate>=10 && fcntl(duplicate,F_GETFD)==FD_CLOEXEC);
    CHECK(close(duplicate)==0);
    duplicate=dup(fd);CHECK(duplicate>=0 && fcntl(duplicate,F_GETFD)==0 && fcntl(fd,F_GETFD)==FD_CLOEXEC);
    CHECK(close(duplicate)==0);
    struct iovec vec[2]={{"fi",2},{"le",2}};
    CHECK(writev(fd,vec,2)==4 && fstat(fd,&st)==0 && st.st_size==4);CHECK(close(fd)==0);
    CHECK(rename(a,b)==0 && stat(b,&st)==0 && st.st_size==4 && unlink(b)==0);
    CHECK(stat(b,&st)==-1 && errno==ENOENT);
#elif CASE == 18
    unsigned char random_ids[128][16], again[16];
    CHECK(syscall(SYS_getrandom,random_ids,sizeof random_ids,0)==sizeof random_ids);
    CHECK(syscall(SYS_getrandom,again,sizeof again,0)==sizeof again);
    CHECK(memcmp(random_ids[0],again,sizeof again)!=0);
    for(unsigned i=0;i<128;i++)for(unsigned j=0;j<i;j++)
        CHECK(memcmp(random_ids[i],random_ids[j],16)!=0);
    puts("[RANDOM] distinct UUID-sized blocks within one getrandom call PASS");
    struct timespec a,b,delay={0,20000000};CHECK(clock_gettime(CLOCK_MONOTONIC,&a)==0);
    CHECK(nanosleep(&delay,0)==0 && clock_gettime(CLOCK_MONOTONIC,&b)==0);
    CHECK((b.tv_sec-a.tv_sec)*1000000000LL+b.tv_nsec-a.tv_nsec>=20000000);
#endif
    printf("[GATE:A] caso=%d PASS\n", CASE);
    return 0;
}
