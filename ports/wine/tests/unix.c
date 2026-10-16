#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define CHECK(x) do{if(!(x)){printf("[UNIX] linea=%d errno=%d FAIL\n",__LINE__,errno);return 1;}}while(0)
int main(void) {
    const char *path="/WINEGATE/test.sock";unlink(path);
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    int server=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);CHECK(server>=0);
    CHECK(bind(server,(struct sockaddr *)&address,sizeof address)==0 && listen(server,4)==0);
    struct stat st;CHECK(stat(path,&st)==0 && S_ISSOCK(st.st_mode));
    CHECK(accept(server,NULL,NULL)==-1 && errno==EAGAIN);
    CHECK(fcntl(server,F_SETFL,0)==0);pid_t self=getpid(),child=fork();CHECK(child>=0);
    if(!child) {
        close(server);int client=socket(AF_UNIX,SOCK_STREAM,0);if(client<0)_exit(81);
        if(connect(client,(struct sockaddr *)&address,sizeof address))_exit(82);
        struct ucred cred;socklen_t size=sizeof cred;
        if(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&cred,&size) || cred.pid!=self)_exit(83);
        char byte;if(write(client,"C",1)!=1 || read(client,&byte,1)!=1 || byte!='S')_exit(84);
        close(client);_exit(0);
    }
    int client=accept4(server,NULL,NULL,SOCK_CLOEXEC);CHECK(client>=0 && fcntl(client,F_GETFD)==FD_CLOEXEC);
    struct ucred cred;socklen_t size=sizeof cred;
    CHECK(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&cred,&size)==0 && cred.pid==child);
    struct sockaddr_un own;size=sizeof own;CHECK(getsockname(client,(struct sockaddr *)&own,&size)==0 && !strcmp(own.sun_path,path));
    char byte;CHECK(read(client,&byte,1)==1 && byte=='C' && write(client,"S",1)==1);close(client);close(server);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(stat(path,&st)==0 && S_ISSOCK(st.st_mode));CHECK(unlink(path)==0 && stat(path,&st)==-1 && errno==ENOENT);
    memset(&address,0,sizeof address);address.sun_family=AF_UNIX;memcpy(address.sun_path,"\0B\0K",4);
    for(unsigned i=0;i<40;i++) {
        server=socket(AF_UNIX,SOCK_STREAM,0);CHECK(server>=0 && bind(server,(struct sockaddr *)&address,6)==0);
        size=sizeof own;CHECK(getsockname(server,(struct sockaddr *)&own,&size)==0 && size==6 && !memcmp(own.sun_path,address.sun_path,4));close(server);
    }
    puts("[UNIX] bind/listen/accept, credenciales, pathname persistente y abstracto PASS");return 0;
}
