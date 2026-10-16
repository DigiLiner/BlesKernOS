
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "common.h"
static char output[300000];static size_t used;static int failing_fd=-1,fail_after=-1;
size_t bk_string_length(const char *s){return strlen(s);}
int bk_string_compare(const char *a,const char *b){return strcmp(a,b);}
char *bk_string_copy_n(char *d,const char *s,size_t n){return strncpy(d,s,n);}
char *bk_string_concat(char *d,const char *s){return strcat(d,s);}
int bk_memory_compare(const void *a,const void *b,size_t n){return memcmp(a,b,n);}
void *bk_sys_alloc(size_t n){return malloc(n);}
void bk_sys_free(void *p){free(p);}
void bk_console_putchar(char c){assert(used+1<sizeof(output));output[used++]=c;output[used]=0;}
void bk_console_write(const char *s){while(*s)bk_console_putchar(*s++);}
void bk_console_printf(const char *fmt,...){char s[16384];va_list a;va_start(a,fmt);vsnprintf(s,sizeof(s),fmt,a);va_end(a);bk_console_write(s);}
int32_t bk_console_read(char *p,uint32_t n){static unsigned calls;*p=(calls++%2)?'\n':'q';return 1;}
bool bk_proc_exit_requested(void){return false;}
uint32_t bk_sys_getpid(void){return 123;}
bool bk_file_stat(const char *p,bk_file_entry_t *e){struct stat st;if(stat(p,&st))return false;memset(e,0,sizeof(*e));const char *n=strrchr(p,'/');strncpy(e->name,n?n+1:p,sizeof(e->name)-1);e->size=st.st_size;e->type=S_ISDIR(st.st_mode)?BK_FILE_NODE_DIRECTORY:BK_FILE_NODE_FILE;return true;}
int bk_file_open(const char *p,uint32_t flags){int f=(flags&3)==1?O_RDONLY:(flags&3)==2?O_WRONLY:O_RDWR;if(flags&0x10)f|=O_CREAT;if(flags&0x20)f|=O_TRUNC;int fd=open(p,f,0644);if(fd>=0 && fail_after>=0 && !strcmp(p,"source"))failing_fd=fd;return fd;}
int bk_file_read(int fd,void *p,uint32_t n){if(fd==failing_fd){if(fail_after<=0)return -1;if(n>(uint32_t)fail_after)n=fail_after;int r=read(fd,p,n);if(r>0)fail_after-=r;return r;}return read(fd,p,n);}
int bk_file_write(int fd,const void *p,uint32_t n){if(n>333)n=333;return write(fd,p,n);}
bool bk_file_close(int fd){if(fd==failing_fd)failing_fd=-1;return close(fd)==0;}
int32_t bk_file_size(int fd){struct stat s;return fstat(fd,&s)?-1:(int32_t)s.st_size;}
int32_t bk_file_seek(int fd,int32_t n,uint32_t origin){return lseek(fd,n,origin);}
bool bk_file_remove(const char *p){struct stat s;return !stat(p,&s) && (S_ISDIR(s.st_mode)?rmdir(p):unlink(p))==0;}
bool bk_file_rename(const char *a,const char *b){return rename(a,b)==0;}
int bk_file_transfer_check(const char *a,const char *b){struct stat x,y;return !stat(a,&x)&&!stat(b,&y)&&x.st_ino==y.st_ino?-1:1;}
bool bk_file_touch(const char *p,bool create){if(access(p,F_OK)){if(!create)return true;int f=open(p,O_CREAT|O_WRONLY,0644);return f>=0 && !close(f);}return true;}
bool bk_file_get_mode(const char *p,uint16_t *m){struct stat s;if(stat(p,&s))return false;*m=s.st_mode&07777;return true;}
bool bk_file_set_mode(const char *p,uint16_t m){return chmod(p,m)==0;}
bool bk_file_space_path(const char *p,uint64_t *t,uint64_t *f){*t=1024*100;*f=1024*40;return true;}
bool bk_file_sync(void){return true;}
bool bk_device_attach_volume(const char *p){return true;}
bool bk_device_unmount_volume(const char *p){return false;}
uint32_t bk_proc_priority(uint32_t pid,uint32_t n){return pid==5?(n?n:2):0;}
bool bk_file_list_dir(const char *p,bk_file_entry_t *entries,uint32_t cap,uint32_t *count){DIR *d=opendir(p);if(!d)return false;*count=0;struct dirent *e;while((e=readdir(d))&&*count<cap){char path[1024];snprintf(path,sizeof(path),"%s/%s",p,e->d_name);if(bk_file_stat(path,&entries[*count]))(*count)++;}closedir(d);return true;}
static void store(const char *p,const void *d,size_t n){FILE *f=fopen(p,"wb");assert(f);assert(fwrite(d,1,n,f)==n);assert(!fclose(f));}
static void contents(const char *p,const char *expected){char b[40000];FILE *f=fopen(p,"rb");assert(f);size_t n=fread(b,1,sizeof(b),f);fclose(f);assert(n==strlen(expected)&&!memcmp(b,expected,n));}
static int invoke(int n,char **args){used=0;output[0]=0;return command_filetool(n,args);}
#define RUN(expected,...) do {char *a[]={__VA_ARGS__};int code=invoke(sizeof(a)/sizeof(a[0]),a);if(code!=(expected)){fprintf(stderr,"%s status=%d expected=%d: %s",a[0],code,(expected),output);abort();}}while(0)
