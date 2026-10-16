/* Diagnostic harness, not the normal desktop launch policy. */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
    if(argc!=2)return 2;
    char *args[]={"/WINEGATE/bin/wine-launch","--diagnostics",argv[1],NULL};
    char *env[]={"WINEDEBUG=-all,err+all,warn+seh",NULL};
    execve(args[0],args,env);perror("compat-launch");return 127;
}
