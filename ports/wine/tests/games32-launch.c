#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void){
 setbuf(stdout,NULL);setenv("WINEDEBUG","-all,err+all",1);
 execl("/WINEGATE/bin/wine-launch","wine-launch","--diagnostics","/WINEGATE/bin/games32.exe",NULL);
 perror("wine-launch");return 127;
}
