#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void){
 setbuf(stdout,NULL);setenv("WINEDEBUG","-all",1);
 pid_t p=fork();if(p<0)return 1;
 if(!p){execl("/WINEGATE/bin/wine-tool.ELF","wine-tool","/WINEGATE/bin/directory-notify.exe",NULL);perror("wine-tool");_exit(127);}
 int status;if(waitpid(p,&status,0)!=p || !WIFEXITED(status))return 2;
 printf("[WIN-NOTIFY-LAUNCH] status=%d\n",WEXITSTATUS(status));return WEXITSTATUS(status);
}
