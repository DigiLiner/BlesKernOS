#include "bleskernos_api.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOCKET_FD_BASE 512
#define SOCKET_SLOTS 16
#define SOCKET_TIMEOUT 15000U
typedef struct { int used, native, type, flags; uint32_t recv_timeout, send_timeout; } socket_slot_t;
static socket_slot_t sockets[SOCKET_SLOTS];
extern int __blesk_posix_is_pipe(int fd);
extern short __blesk_posix_pipe_poll(int fd, short events);

static socket_slot_t *slot_for(int fd){int i=fd-SOCKET_FD_BASE;return i>=0&&i<SOCKET_SLOTS&&sockets[i].used?&sockets[i]:0;}
static int allocate_socket(int native,int type){for(int i=0;i<SOCKET_SLOTS;i++)if(!sockets[i].used){sockets[i].used=1;sockets[i].native=native;sockets[i].type=type;sockets[i].recv_timeout=SOCKET_TIMEOUT;sockets[i].send_timeout=SOCKET_TIMEOUT;return SOCKET_FD_BASE+i;}bk_net_socket_close(native);errno=EMFILE;return -1;}
static int socket_address(const struct sockaddr *a,socklen_t n,const struct sockaddr_in **out){if(!a||n<sizeof(struct sockaddr_in)||a->sa_family!=AF_INET){errno=EINVAL;return -1;}*out=(const struct sockaddr_in*)a;return 0;}
int __blesk_posix_is_socket(int fd){return slot_for(fd)!=0;}
int __blesk_posix_socket_close(int fd){socket_slot_t*s=slot_for(fd);if(!s){errno=EBADF;return-1;}bk_net_socket_close(s->native);memset(s,0,sizeof(*s));return 0;}
ssize_t __blesk_posix_socket_read(int fd,void*b,size_t n){return recv(fd,b,n,0);}
ssize_t __blesk_posix_socket_write(int fd,const void*b,size_t n){return send(fd,b,n,0);}

uint16_t htons(uint16_t v){return(uint16_t)((v<<8)|(v>>8));} uint16_t ntohs(uint16_t v){return htons(v);}
uint32_t htonl(uint32_t v){return(v<<24)|((v<<8)&0x00FF0000U)|((v>>8)&0x0000FF00U)|(v>>24);} uint32_t ntohl(uint32_t v){return htonl(v);}
int socket(int domain,int type,int protocol){int native;if(domain!=AF_INET||(type!=SOCK_STREAM&&type!=SOCK_DGRAM)||(protocol&&protocol!=6&&protocol!=17)){errno=EAFNOSUPPORT;return-1;}native=bk_net_socket_open(type==SOCK_STREAM?BK_NET_SOCKET_TCP:BK_NET_SOCKET_UDP);if(native<0){errno=ENFILE;return-1;}return allocate_socket(native,type);}
int connect(int fd,const struct sockaddr*a,socklen_t n){socket_slot_t*s=slot_for(fd);const struct sockaddr_in*in;if(!s){errno=EBADF;return-1;}if(socket_address(a,n,&in)<0)return-1;if(!bk_net_socket_connect(s->native,(const uint8_t*)&in->sin_addr.s_addr,ntohs(in->sin_port),s->send_timeout)){errno=ECONNREFUSED;return-1;}return 0;}
int bind(int fd,const struct sockaddr*a,socklen_t n){socket_slot_t*s=slot_for(fd);const struct sockaddr_in*in;if(!s){errno=EBADF;return-1;}if(socket_address(a,n,&in)<0)return-1;if(!bk_net_socket_bind(s->native,(const uint8_t*)&in->sin_addr.s_addr,ntohs(in->sin_port))){errno=EADDRINUSE;return-1;}return 0;}
int listen(int fd,int backlog){socket_slot_t*s=slot_for(fd);if(!s){errno=EBADF;return-1;}if(!bk_net_socket_listen(s->native,(uint8_t)(backlog<1?1:backlog>255?255:backlog))){errno=EIO;return-1;}return 0;}
int accept(int fd,struct sockaddr*a,socklen_t*n){socket_slot_t*s=slot_for(fd);uint8_t ip[4];uint16_t port=0;int native,out;if(!s){errno=EBADF;return-1;}native=bk_net_socket_accept(s->native,ip,&port,(s->flags&O_NONBLOCK)?0:s->recv_timeout);if(native<0){errno=(s->flags&O_NONBLOCK)?EAGAIN:EIO;return-1;}out=allocate_socket(native,SOCK_STREAM);if(out<0)return-1;if(a&&n&&*n>=sizeof(struct sockaddr_in)){struct sockaddr_in*in=(struct sockaddr_in*)a;memset(in,0,sizeof(*in));in->sin_family=AF_INET;in->sin_port=htons(port);memcpy(&in->sin_addr.s_addr,ip,4);*n=sizeof(*in);}return out;}
ssize_t send(int fd,const void*b,size_t n,int flags){socket_slot_t*s=slot_for(fd);int32_t r;if(!s){errno=EBADF;return-1;}r=bk_net_socket_send(s->native,b,n,(flags&MSG_DONTWAIT)||(s->flags&O_NONBLOCK)?0:s->send_timeout);if(r<0){errno=EIO;return-1;}return r;}
ssize_t recv(int fd,void*b,size_t n,int flags){socket_slot_t*s=slot_for(fd);int32_t r;if(!s){errno=EBADF;return-1;}r=bk_net_socket_receive(s->native,b,n,(flags&MSG_DONTWAIT)||(s->flags&O_NONBLOCK)?0:s->recv_timeout);if(r<0){errno=((flags&MSG_DONTWAIT)||(s->flags&O_NONBLOCK))?EAGAIN:EIO;return-1;}return r;}
ssize_t sendto(int fd,const void*b,size_t n,int flags,const struct sockaddr*a,socklen_t al){socket_slot_t*s=slot_for(fd);const struct sockaddr_in*in;int32_t r;if(!s){errno=EBADF;return-1;}if(socket_address(a,al,&in)<0)return-1;r=bk_net_socket_sendto(s->native,(const uint8_t*)&in->sin_addr.s_addr,ntohs(in->sin_port),b,n,(flags&MSG_DONTWAIT)?0:s->send_timeout);if(r<0){errno=EIO;return-1;}return r;}
ssize_t recvfrom(int fd,void*b,size_t n,int flags,struct sockaddr*a,socklen_t*al){socket_slot_t*s=slot_for(fd);uint8_t ip[4];uint16_t port;int32_t r;if(!s){errno=EBADF;return-1;}r=bk_net_socket_receivefrom(s->native,ip,&port,b,n,(flags&MSG_DONTWAIT)?0:s->recv_timeout);if(r<0){errno=(flags&MSG_DONTWAIT)?EAGAIN:EIO;return-1;}if(a&&al&&*al>=sizeof(struct sockaddr_in)){struct sockaddr_in*in=(struct sockaddr_in*)a;memset(in,0,sizeof(*in));in->sin_family=AF_INET;in->sin_port=htons(port);memcpy(&in->sin_addr.s_addr,ip,4);*al=sizeof(*in);}return r;}
int shutdown(int fd,int how){(void)how;return __blesk_posix_socket_close(fd);}
int getsockopt(int fd,int level,int option,void*v,socklen_t*n){socket_slot_t*s=slot_for(fd);if(!s||!v||!n||*n<sizeof(int)||level!=SOL_SOCKET){errno=EINVAL;return-1;}*(int*)v=option==SO_ERROR?0:(option==SO_RCVTIMEO?(int)s->recv_timeout:(int)s->send_timeout);*n=sizeof(int);return 0;}
int setsockopt(int fd,int level,int option,const void*v,socklen_t n){socket_slot_t*s=slot_for(fd);if(!s||!v||n<sizeof(int)||level!=SOL_SOCKET||(option!=SO_RCVTIMEO&&option!=SO_SNDTIMEO)){errno=EINVAL;return-1;}if(option==SO_RCVTIMEO)s->recv_timeout=*(const uint32_t*)v;else s->send_timeout=*(const uint32_t*)v;return 0;}
int __blesk_posix_socket_fcntl(int fd,int command,int value){socket_slot_t*s=slot_for(fd);if(!s){errno=EBADF;return-1;}if(command==F_GETFL)return s->flags;if(command==F_SETFL){s->flags=value&O_NONBLOCK;return 0;}errno=EINVAL;return-1;}
int poll(struct pollfd*f,nfds_t count,int timeout){uint32_t start=bk_sys_uptime_ms();if(!f&&count){errno=EFAULT;return-1;}for(;;){int ready=0;for(nfds_t i=0;i<count;i++){socket_slot_t*s=slot_for(f[i].fd);f[i].revents=0;if(s){if((f[i].events&POLLIN)&&bk_net_socket_readable(s->native))f[i].revents|=POLLIN;if(f[i].events&POLLOUT)f[i].revents|=POLLOUT;}else if(__blesk_posix_is_pipe(f[i].fd))f[i].revents=__blesk_posix_pipe_poll(f[i].fd,f[i].events);else if(f[i].fd>=0){if(f[i].events&(POLLIN|POLLOUT))f[i].revents=f[i].events&(POLLIN|POLLOUT);}if(f[i].revents)ready++;}if(ready||timeout==0)return ready;if(timeout>0&&(uint32_t)(bk_sys_uptime_ms()-start)>=(uint32_t)timeout)return 0;bk_sys_sleep_ms(1);}}

int inet_aton(const char*t,struct in_addr*a){unsigned char*o=(unsigned char*)&a->s_addr;for(int i=0;i<4;i++){unsigned v=0,d=0;while(*t>='0'&&*t<='9'){v=v*10U+(unsigned)(*t++-'0');d++;}if(!d||v>255U||(i<3&&*t++!='.')||(i==3&&*t))return 0;o[i]=(unsigned char)v;}return 1;}
in_addr_t inet_addr(const char*t){struct in_addr a;return inet_aton(t,&a)?a.s_addr:0xFFFFFFFFU;}
const char*inet_ntop(int family,const void*a,char*t,socklen_t n){const unsigned char*p=(const unsigned char*)a;char temp[16];unsigned k=0;if(family!=AF_INET||!a||!t||n<16){errno=EINVAL;return 0;}for(int i=0;i<4;i++){unsigned v=p[i];char rev[3];int r=0;do{rev[r++]=(char)('0'+v%10);v/=10;}while(v);while(r)temp[k++]=rev[--r];if(i<3)temp[k++]='.';}temp[k]=0;memcpy(t,temp,k+1);return t;}
static int parse_port(const char*s){unsigned v=0;if(!s||!*s)return 0;while(*s>='0'&&*s<='9'){v=v*10U+(unsigned)(*s++-'0');if(v>65535U)return-1;}return *s?-1:(int)v;}
int getaddrinfo(const char*node,const char*service,const struct addrinfo*h,struct addrinfo**out){unsigned char ip[4]={0,0,0,0};int port=parse_port(service);struct addrinfo*ai;struct sockaddr_in*in;if(!out||port<0)return EAI_SERVICE;if(h&&h->ai_family!=AF_UNSPEC&&h->ai_family!=AF_INET)return EAI_FAMILY;if(node&&*node){struct in_addr numeric;if(inet_aton(node,&numeric))memcpy(ip,&numeric.s_addr,4);else if(!bk_net_resolve(node,ip,SOCKET_TIMEOUT))return EAI_NONAME;}ai=(struct addrinfo*)calloc(1,sizeof(*ai)+sizeof(*in));if(!ai)return EAI_MEMORY;in=(struct sockaddr_in*)(ai+1);in->sin_family=AF_INET;in->sin_port=htons((uint16_t)port);memcpy(&in->sin_addr.s_addr,ip,4);ai->ai_family=AF_INET;ai->ai_socktype=h?h->ai_socktype:SOCK_STREAM;ai->ai_addrlen=sizeof(*in);ai->ai_addr=(struct sockaddr*)in;*out=ai;return 0;}
void freeaddrinfo(struct addrinfo*a){free(a);} const char*gai_strerror(int e){return e==0?"success":e==EAI_NONAME?"name not found":e==EAI_MEMORY?"out of memory":"address error";}
