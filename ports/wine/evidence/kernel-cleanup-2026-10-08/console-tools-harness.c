#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
static int run(char *const args[]) {
 pid_t child=fork();int status=0;
 if(child==0){execv(args[0],args);perror(args[0]);_exit(127);}
 if(child<0)return 127;
 for(int i=0;i<1200;i++){
  pid_t got=waitpid(child,&status,WNOHANG);
  if(got==child)return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
  if(got<0)return 127;
  usleep(50000);
 }
 kill(child,SIGKILL);waitpid(child,&status,0);return 124;
}
int main(void){
 char *warm[]={"/WINEGATE/bin/wine-launch-real","--warmup",NULL};
 int status=run(warm);printf("[TOOLS-SMOKE] warmup status=%d\n",status);fflush(stdout);
 const char *lines[]={
  "/SYSTEM/DEVEL/NASM/NASM.EXE --version",
  "/SYSTEM/DEVEL/TCC/I386-TCC.EXE -c -I/SYSTEM/SDK/INCLUDE /SYSTEM/SDK/EXAMPLES/HELLO/HELLO.C -o /tmp/hello.o",
  "/SYSTEM/DEVEL/MAKE/MAKE.EXE --version",
  "/SYSTEM/DEVEL/BINUTILS/LD.EXE --version",
  "/SYSTEM/DEVEL/TCC/I386-TCC.EXE -c --version"
 };
 for(int i=0;i<5;i++){
  char *args[]={"/WINEGATE/bin/wine-tool.ELF",(char *)lines[i],NULL};
  status=run(args);printf("[TOOLS-SMOKE] case=%d status=%d\n",i,status);fflush(stdout);
 }
 return 0;
}
