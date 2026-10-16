/* Reproduce el bloqueo W2 real; se espera que falle hasta implementar modos persistentes. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
int main(void) {
 const char *path="/WINEGATE/private-mode";rmdir(path);umask(0);
 if(mkdir(path,0700)){printf("[MODE] mkdir errno=%d\n",errno);return 1;}
 struct stat st;if(stat(path,&st))return 2;
 printf("[MODE] solicitado=0700 observado=%04o\n",st.st_mode&07777);
 return (st.st_mode&0777)==0700?0:3;
}
