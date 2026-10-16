#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
int main(void) {
 mkdir("/WINEGATE/prefix",0700);mkdir("/tmp",01777);
 char *args[]={"/WINEGATE/wine/wineserver","-f","-d2",0};
 char *env[]={"LD_LIBRARY_PATH=/WINEGATE/lib","WINEPREFIX=/WINEGATE/prefix","HOME=/WINEGATE","PATH=/WINEGATE/wine","USER=root",0};
 puts("[WINE:PROC] W2 probe wineserver upstream foreground");fflush(stdout);
 execve(args[0],args,env);printf("[WINE:PROC] exec errno=%d\n",errno);return 1;
}
