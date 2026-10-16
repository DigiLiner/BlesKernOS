#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
static int global=31;
static char alternate[65536];
static void handler(int sig){(void)sig;}
#define CHECK(x) do {if(!(x)){printf("[EXEC] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
int main(int argc,char **argv) {
    if(argc>1) {
        CHECK(argc==5 && !strcmp(argv[0],"nombre personalizado") && !strcmp(argv[1],"argumento con espacios"));
        CHECK(getpid()==atoi(argv[2]) && global==31);
        CHECK(getenv("BLESK_EXEC_TEST") && !strcmp(getenv("BLESK_EXEC_TEST"),"valor con espacios"));
        int closed=atoi(argv[3]),kept=atoi(argv[4]);char c;
        CHECK(fcntl(closed,F_GETFD)==-1 && errno==EBADF);
        CHECK(read(kept,&c,1)==1 && c==0x7f);close(kept);
        struct sigaction sa;sigset_t mask;stack_t stack;
        CHECK(sigaction(SIGUSR1,NULL,&sa)==0 && sa.sa_handler==SIG_DFL);
        CHECK(sigaction(SIGUSR2,NULL,&sa)==0 && sa.sa_handler==SIG_IGN);
        CHECK(sigprocmask(SIG_SETMASK,NULL,&mask)==0 && sigismember(&mask,SIGUSR1)==1);
        CHECK(sigaltstack(NULL,&stack)==0 && stack.ss_flags==SS_DISABLE);
        puts("[EXEC] PID estable, argv/envp, CLOEXEC, acciones/mascara PASS");return 0;
    }
    pid_t self=getpid();char *bad[]={(char *)1,NULL};
    CHECK(syscall(SYS_execve,"/WINEGATE/bin/case22",bad,NULL)==-1 && errno==EFAULT && getpid()==self);
    CHECK(execl("/WINEGATE/no-existe","no-existe",NULL)==-1 && errno==ENOENT && getpid()==self);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        int closed=open("/WINEGATE/bin/case01",O_RDONLY|O_CLOEXEC);
        int kept=open("/WINEGATE/bin/case01",O_RDONLY);CHECK(closed>=0 && kept>=0);
        char pid_text[32],closed_text[32],kept_text[32];
        snprintf(pid_text,sizeof pid_text,"%d",getpid());snprintf(closed_text,sizeof closed_text,"%d",closed);snprintf(kept_text,sizeof kept_text,"%d",kept);
        struct sigaction sa={.sa_handler=handler};CHECK(sigaction(SIGUSR1,&sa,NULL)==0);
        sa.sa_handler=SIG_IGN;CHECK(sigaction(SIGUSR2,&sa,NULL)==0);
        sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);CHECK(sigprocmask(SIG_BLOCK,&mask,NULL)==0);
        stack_t stack={.ss_sp=alternate,.ss_size=sizeof alternate};CHECK(sigaltstack(&stack,NULL)==0);global=55;
        char *av[]={"nombre personalizado","argumento con espacios",pid_text,closed_text,kept_text,NULL};
        char *env[]={"BLESK_EXEC_TEST=valor con espacios",NULL};
        execve("/WINEGATE/bin/case22",av,env);_exit(90);
    }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
