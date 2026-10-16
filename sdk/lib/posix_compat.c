#include "bleskernos_api.h"
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

char *optarg;
int optind = 1, opterr = 1, optopt;
static const char *option_cursor;

double strtod(const char*s,char**end){double value=0.0,place=0.1;int negative=0,exponent=0,exp_negative=0;if(!s){if(end)*end=(char*)s;return 0;}while(*s==' '||*s=='\t'||*s=='\n')s++;if(*s=='+'||*s=='-')negative=(*s++=='-');while(*s>='0'&&*s<='9')value=value*10.0+(*s++-'0');if(*s=='.'){s++;while(*s>='0'&&*s<='9'){value+=(*s++-'0')*place;place*=0.1;}}if(*s=='e'||*s=='E'){const char*mark=s++;if(*s=='+'||*s=='-')exp_negative=(*s++=='-');if(*s<'0'||*s>'9')s=mark;else{while(*s>='0'&&*s<='9')exponent=exponent*10+(*s++-'0');while(exponent-->0)value=exp_negative?value/10.0:value*10.0;}}if(end)*end=(char*)s;return negative?-value:value;}
char*mktemp(char*name){int fd;if(!name){errno=EINVAL;return 0;}fd=mkstemp(name);if(fd<0){name[0]=0;return name;}close(fd);unlink(name);return name;}

int getopt(int argc, char *const argv[], const char *options) {
    const char *definition;
    if (!options) return -1;
    optarg = 0;
    if (!option_cursor || !*option_cursor) {
        if (optind >= argc || !argv[optind] || argv[optind][0] != '-' ||
            !argv[optind][1]) return -1;
        if (!strcmp(argv[optind], "--")) { optind++; return -1; }
        option_cursor = argv[optind++] + 1;
    }
    optopt = (unsigned char)*option_cursor++;
    definition = strchr(options, optopt);
    if (!definition || optopt == ':') return '?';
    if (definition[1] == ':') {
        if (*option_cursor) { optarg=(char *)option_cursor; option_cursor=0; }
        else if (definition[2] == ':') option_cursor=0;
        else if (optind < argc) { optarg=argv[optind++]; option_cursor=0; }
        else return options[0] == ':' ? ':' : '?';
    }
    return optopt;
}

static int getopt_long_impl(int argc, char *const argv[], const char *shorts,
                            const struct option *longs, int *index,
                            int long_only) {
    const char *arg, *name, *equals;
    size_t length;
    if (optind >= argc) return -1;
    arg=argv[optind];
    if (!arg || arg[0]!='-' || !arg[1]) return -1;
    if (arg[1]!='-' && !long_only) return getopt(argc,argv,shorts);
    name=arg+(arg[1]=='-'?2:1); equals=strchr(name,'=');
    length=equals?(size_t)(equals-name):strlen(name);
    for (int i=0; longs && longs[i].name; i++) {
        if (strlen(longs[i].name)!=length || strncmp(name,longs[i].name,length))
            continue;
        optind++; if(index)*index=i;
        if(longs[i].has_arg==required_argument) {
            if(equals)optarg=(char*)equals+1;
            else if(optind<argc)optarg=argv[optind++];
            else return shorts&&shorts[0]==':'?':':'?';
        } else if(longs[i].has_arg==optional_argument)
            optarg=equals?(char*)equals+1:0;
        else if(equals) return '?';
        if(longs[i].flag){*longs[i].flag=longs[i].val;return 0;}
        return longs[i].val;
    }
    return arg[1]=='-'?'?':getopt(argc,argv,shorts);
}
int getopt_long(int a,char*const v[],const char*s,const struct option*l,int*i){return getopt_long_impl(a,v,s,l,i,0);}
int getopt_long_only(int a,char*const v[],const char*s,const struct option*l,int*i){return getopt_long_impl(a,v,s,l,i,1);}

static char locale_name[]="C.UTF-8";
static struct lconv c_locale={".","","","","","","","","","",127,127,127,127,127,127,127,127};
char *setlocale(int category,const char*name){if(category<LC_ALL||category>LC_TIME)return 0;if(!name||!*name||!strcmp(name,"C")||!strcmp(name,"POSIX")||!strcmp(name,"C.UTF-8")||!strcmp(name,"en_US.UTF-8"))return locale_name;return 0;}
struct lconv *localeconv(void){return &c_locale;}

static int leap(int y){return(y%4==0&&y%100!=0)||y%400==0;}
static const int month_days[12]={31,28,31,30,31,30,31,31,30,31,30,31};
static time_t datetime_epoch(int y,int m,int d,int h,int min,int s){long days=0;for(int n=1970;n<y;n++)days+=365+leap(n);for(int n=1;n<m;n++)days+=month_days[n-1]+(n==2&&leap(y));days+=d-1;return(time_t)(days*86400L+h*3600L+min*60L+s);}
time_t time(time_t*out){bk_datetime_t d;time_t v;if(!bk_time_datetime(&d)){errno=EIO;return(time_t)-1;}v=datetime_epoch(d.date.year,d.date.month,d.date.day,d.time.hour,d.time.minute,d.time.second);if(out)*out=v;return v;}
static struct tm tm_store;
static struct tm *break_time(const time_t*source){long v=*source,days;if(v<0)return 0;memset(&tm_store,0,sizeof(tm_store));tm_store.tm_sec=v%60;v/=60;tm_store.tm_min=v%60;v/=60;tm_store.tm_hour=v%24;days=v/24;tm_store.tm_wday=(days+4)%7;int y=1970;while(days>=365+leap(y)){days-=365+leap(y);y++;}tm_store.tm_year=y-1900;tm_store.tm_yday=days;int m=0;while(m<11&&days>=month_days[m]+(m==1&&leap(y))){days-=month_days[m]+(m==1&&leap(y));m++;}tm_store.tm_mon=m;tm_store.tm_mday=days+1;return&tm_store;}
struct tm*gmtime(const time_t*v){return v?break_time(v):0;}
struct tm*localtime(const time_t*v){return gmtime(v);}
time_t mktime(struct tm*t){return t?datetime_epoch(t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec):(time_t)-1;}
clock_t clock(void){struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)<0)return(clock_t)-1;return(clock_t)(ts.tv_sec*CLOCKS_PER_SEC+ts.tv_nsec/(1000000000L/CLOCKS_PER_SEC));}
double difftime(time_t end,time_t beginning){return(double)(end-beginning);}
char*asctime(const struct tm*t){static char text[32];static const char*days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};static const char*months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};if(!t)return 0;snprintf(text,sizeof(text),"%s %s %2d %02d:%02d:%02d %d\n",days[(unsigned)t->tm_wday<7?t->tm_wday:0],months[(unsigned)t->tm_mon<12?t->tm_mon:0],t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec,t->tm_year+1900);return text;}
static void append_text(char**out,size_t*left,const char*text){while(*text&&*left>1){*(*out)++=*text++;(*left)--;}}
static void append_number(char**out,size_t*left,int value,int width){char b[12];int n=0;do{b[n++]=(char)('0'+value%10);value/=10;}while(value&&n<10);while(n<width)b[n++]='0';while(n&&*left>1){*(*out)++=b[--n];(*left)--;}}
size_t strftime(char*d,size_t capacity,const char*f,const struct tm*t){static const char*months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};static const char*days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};char*out=d;size_t left=capacity;if(!d||!capacity||!f||!t)return 0;while(*f&&left>1){if(*f++!='%'){*out++=f[-1];left--;continue;}char c=*f++;if(c=='Y')append_number(&out,&left,t->tm_year+1900,4);else if(c=='m')append_number(&out,&left,t->tm_mon+1,2);else if(c=='d')append_number(&out,&left,t->tm_mday,2);else if(c=='H')append_number(&out,&left,t->tm_hour,2);else if(c=='M')append_number(&out,&left,t->tm_min,2);else if(c=='S')append_number(&out,&left,t->tm_sec,2);else if(c=='b')append_text(&out,&left,months[t->tm_mon]);else if(c=='a')append_text(&out,&left,days[t->tm_wday]);else if(c=='%'){*out++='%';left--;}else{*out++='%';left--;if(left>1){*out++=c;left--;}}}*out=0;return *f?0:(size_t)(out-d);}
int gettimeofday(struct timeval*v,void*z){struct timespec t;(void)z;if(!v){errno=EFAULT;return-1;}if(clock_gettime(CLOCK_REALTIME,&t)<0)return-1;v->tv_sec=t.tv_sec;v->tv_usec=t.tv_nsec/1000;return 0;}

int uname(struct utsname*u){if(!u){errno=EFAULT;return-1;}memset(u,0,sizeof(*u));strcpy(u->sysname,"BlesKernOS");strcpy(u->nodename,"bleskernos");strcpy(u->release,"0.9");strcpy(u->version,"POSIX ABI 15 Ring 3");strcpy(u->machine,"i686");return 0;}
int gethostname(char*n,unsigned int length){const char*h="bleskernos";if(!n||length<=strlen(h)){errno=EINVAL;return-1;}strcpy(n,h);return 0;}
long sysconf(int name){if(name==_SC_ARG_MAX)return 2048;if(name==_SC_CHILD_MAX)return 31;if(name==_SC_CLK_TCK)return 100;if(name==_SC_OPEN_MAX)return 64;if(name==_SC_PAGESIZE)return 4096;if(name==_SC_NPROCESSORS_ONLN)return(long)bk_proc_cpu_count();errno=EINVAL;return-1;}

char *basename(char*p){char*end;if(!p||!*p)return".";end=p+strlen(p)-1;while(end>p&&*end=='/')*end--=0;char*slash=strrchr(p,'/');return slash?slash+1:p;}
char *dirname(char*p){static char dot[]=".";if(!p||!*p)return dot;char*end=p+strlen(p)-1;while(end>p&&*end=='/')*end--=0;char*slash=strrchr(p,'/');if(!slash)return dot;while(slash>p&&*slash=='/')slash--;if(slash==p){p[1]=0;return p;}slash[1]=0;return p;}
char *realpath(const char*path,char*resolved){char input[PATH_MAX],*output;size_t used=0;if(!path){errno=EINVAL;return 0;}output=resolved?resolved:(char*)malloc(PATH_MAX);if(!output)return 0;if(path[0]=='/')strncpy(input,path,sizeof(input)-1);else{if(!getcwd(input,sizeof(input))){if(!resolved)free(output);return 0;}size_t n=strlen(input);if(n+1+strlen(path)>=sizeof(input)){errno=ENAMETOOLONG;if(!resolved)free(output);return 0;}if(n>1)input[n++]='/';strcpy(input+n,path);}input[sizeof(input)-1]=0;output[used++]='/';const char*p=input;while(*p){while(*p=='/')p++;const char*start=p;while(*p&&*p!='/')p++;size_t n=p-start;if(!n||(n==1&&start[0]=='.'))continue;if(n==2&&start[0]=='.'&&start[1]=='.'){if(used>1){used--;while(used>1&&output[used-1]!='/')used--;}continue;}if(used>1&&output[used-1]!='/')output[used++]='/';if(used+n>=PATH_MAX){errno=ENAMETOOLONG;if(!resolved)free(output);return 0;}memcpy(output+used,start,n);used+=n;}output[used]=0;return output;}

int tcgetattr(int fd,struct termios*t){if(!isatty(fd)||!t){if(!t)errno=EFAULT;return-1;}memset(t,0,sizeof(*t));t->c_lflag=ICANON|ECHO|ISIG;t->c_oflag=OPOST;t->c_ispeed=t->c_ospeed=B38400;t->c_cc[VMIN]=1;return 0;}
int tcsetattr(int fd,int action,const struct termios*t){if(!isatty(fd)||!t||action!=TCSANOW){if(!t)errno=EFAULT;else if(action!=TCSANOW)errno=EINVAL;return-1;}return 0;}
speed_t cfgetispeed(const struct termios*t){return t?t->c_ispeed:0;} speed_t cfgetospeed(const struct termios*t){return t?t->c_ospeed:0;}
int cfsetispeed(struct termios*t,speed_t s){if(!t)return-1;t->c_ispeed=s;return 0;} int cfsetospeed(struct termios*t,speed_t s){if(!t)return-1;t->c_ospeed=s;return 0;}
int ioctl(int fd,unsigned long request,...){va_list a;if(request==TIOCGWINSZ){struct winsize*w;va_start(a,request);w=va_arg(a,struct winsize*);va_end(a);if(!isatty(fd)||!w)return-1;w->ws_row=25;w->ws_col=80;w->ws_xpixel=640;w->ws_ypixel=400;return 0;}errno=ENOTTY;return-1;}
