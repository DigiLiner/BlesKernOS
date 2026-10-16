#include <unistd.h>
#include <stdio.h>
#include <errno.h>
int main(void) {
 char *args[]={"/WINEGATE/wine/wine","--version",0};
 char *env[]={"LD_LIBRARY_PATH=/WINEGATE/lib","WINEPREFIX=/WINEGATE/prefix","HOME=/WINEGATE","PATH=/WINEGATE/wine","WINEDEBUG=warn+all",0};
 puts("[WINE:PROC] W1 exec Wine upstream --version");fflush(stdout);
 execve(args[0],args,env);printf("[WINE:PROC] exec errno=%d\n",errno);return 1;
}
