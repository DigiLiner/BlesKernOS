#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <spawn.h>
#include <pthread.h>
extern char **environ;
static char spawn_stack[65536] __attribute__((aligned(16)));
static volatile int shared_result;
static int shared_child(void *arg) {
    shared_result=73;
    close(*(int *)arg);
    return 9;
}
static volatile int global=11;
static __thread int tls=13;
#define CHECK(x) do {if(!(x)){printf("[FORK] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
static void *fork_from_thread(void *arg) {
    volatile int local=33;
    volatile int *main_stack=arg;
    pid_t parent=getpid();
    tls=77;
    pid_t child=fork();
    if(child<0)return (void *)1;
    if(!child) {
        if(getppid()!=parent || tls!=77 || local!=33 || *main_stack!=17)_exit(91);
        *main_stack=99;global=99;tls=88;local=44;
        pid_t grandchild=fork();
        if(grandchild<0)_exit(92);
        if(!grandchild)_exit(*main_stack==99 && tls==88 && local==44?0:93);
        int status;
        if(waitpid(grandchild,&status,0)!=grandchild || status)_exit(94);
        char *av[]={"abi",NULL};
        execve("/WINEGATE/bin/abi",av,environ);
        _exit(95);
    }
    int status;
    if(waitpid(child,&status,0)!=child || status || tls!=77 || local!=33 ||
       *main_stack!=17 || global!=11)return (void *)2;
    return NULL;
}
int main(void) {
    volatile int local=17;int *heap=malloc(sizeof(int));CHECK(heap);*heap=19;
    char *map=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED);
    int fd=open("/WINEGATE/bin/case01",O_RDONLY);char c;CHECK(fd>=0 && read(fd,&c,1)==1 && c==0x7f);
    pid_t parent=getpid(),child=fork();CHECK(child>=0);
    if(!child) {
        if (getpid()==parent || getppid()!=parent || global!=11 || tls!=13 || local!=17 || *heap!=19) _exit(80);
        global=21;tls=23;local=27;*heap=29;
        if (mprotect(map,4096,PROT_READ|PROT_WRITE) || read(fd,&c,1)!=1 || c!='E') _exit(81);
        *map='x';
        pid_t grandchild=fork();if(grandchild<0)_exit(82);
        if(!grandchild) _exit((global==21 && tls==23 && local==27 && *heap==29 && *map=='x')?7:83);
        int status;if(waitpid(grandchild,&status,0)!=grandchild || !WIFEXITED(status) || WEXITSTATUS(status)!=7)_exit(84);
        _exit(37);
    }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==37);
    CHECK(global==11 && tls==13 && local==17 && *heap==19);
    CHECK(read(fd,&c,1)==1 && c=='L');close(fd);
    CHECK(getrandom(map,1,0)==-1 && errno==EFAULT);
    CHECK(waitpid(child,&status,WNOHANG)==-1 && errno==ECHILD);
    child=fork();CHECK(child>=0);
    if(!child) {raise(SIGTERM);_exit(85);}
    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM);
    /* Reap a fork parent before its child uses the cloned VM metadata and
       inherited open-description cache. The grandparent controls ordering. */
    int go[2],done[2];CHECK(pipe(go)==0 && pipe(done)==0);
    child=fork();CHECK(child>=0);
    if(!child) {
        int inherited=open("/WINEGATE/bin/case01",O_RDONLY);
        if(inherited<0 || read(inherited,&c,1)!=1)_exit(86);
        pid_t orphan=fork();if(orphan<0)_exit(87);
        if(orphan)_exit(0);
        if(read(go[0],&c,1)!=1)_exit(88);
        int ok=mprotect(map,4096,PROT_READ|PROT_WRITE)==0;
        if(ok){*map='v';ok=read(inherited,&c,1)==1 && c=='E';}
        c=ok?'Y':'N';if(write(done[1],&c,1)!=1)_exit(90);_exit(ok?0:89);
    }
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    char *churn=malloc(65536);CHECK(churn);
    for(unsigned i=0;i<65536;i++)churn[i]=(char)i;
    CHECK(write(go[1],"x",1)==1 && read(done[0],&c,1)==1 && c=='Y');
    free(churn);close(go[0]);close(go[1]);close(done[0]);close(done[1]);
    puts("[FORK] child VM and inherited cache survive parent reap PASS");
    int kept=open("/WINEGATE/bin/case01",O_RDONLY);CHECK(kept>=0);
    child=clone(shared_child,spawn_stack+sizeof(spawn_stack),CLONE_VM|CLONE_VFORK|SIGCHLD,&kept);
    CHECK(child>0 && shared_result==73);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==9);
    CHECK(read(kept,&c,1)==1 && c==0x7f);close(kept);
    char *av[]={"abi",NULL};
    CHECK(posix_spawn(&child,"/WINEGATE/no-existe",NULL,NULL,av,environ)==ENOENT);
    CHECK(posix_spawn(&child,"/WINEGATE/bin/abi",NULL,NULL,av,environ)==0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("[VFORK] memoria compartida, FD separado, exit/exec y posix_spawn PASS");
    pthread_t worker;void *result;
    CHECK(pthread_create(&worker,NULL,fork_from_thread,(void *)&local)==0);
    CHECK(pthread_join(worker,&result)==0 && result==NULL);
    CHECK(local==17 && tls==13 && global==11);
    puts("[FORK] worker thread: TLS, both stacks, private copy, fork and exec PASS");
    munmap(map,4096);free(heap);puts("[FORK] memoria privada, TLS, permisos, FD compartido, wait/status PASS");return 0;
}
