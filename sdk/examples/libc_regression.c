/* Runs both as a native SDK test and as a freestanding Linux i386 test. */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#define CHECK(x) do{if(!(x))return __LINE__;}while(0)
static int libc_test(void) {
    char *end,*a=0,*b=0;char x[]="one,two",y[]="A:B";char buf[16];
    const char *bad=" \t-",*hex="0xQ";
    errno=0;CHECK(strtoul("4294967296!",&end,10)==ULONG_MAX&&errno==ERANGE&&*end=='!');
    errno=0;CHECK(strtoul("8589934591",&end,10)==ULONG_MAX&&errno==ERANGE);
    errno=0;CHECK(strtoul("-1",&end,10)==ULONG_MAX&&errno==0&&!*end);
    CHECK(strtol("\r\v\f-2147483648",&end,0)==LONG_MIN&&!*end);
    errno=0;CHECK(strtol("2147483648",&end,10)==LONG_MAX&&errno==ERANGE);
    CHECK(strtol(bad,&end,10)==0&&end==bad);
    CHECK(strtoul(hex,&end,0)==0&&end==hex+1);
    CHECK(strtol("0xff",&end,0)==255&&!*end);
    CHECK(strtol("077",0,0)==63);CHECK(strtol("z",0,36)==35);
    errno=0;CHECK(strtol("1",&end,1)==0&&errno==EINVAL);
    CHECK(strtoll("-9223372036854775808",&end,10)==LLONG_MIN&&!*end);
    errno=0;CHECK(strtoull("18446744073709551616tail",&end,10)==ULLONG_MAX&&errno==ERANGE&&*end=='t');
    CHECK(strtoimax("-99",0,10)==-99);CHECK(strtoumax("ff",0,16)==255);
    CHECK(atol("-12")==-12&&atoll("9999999999")==9999999999LL);
    CHECK(labs(-12)==12&&llabs(-9999999999LL)==9999999999LL);
    CHECK(div(-7,3).quot==-2&&div(-7,3).rem==-1);
    CHECK(ldiv(7,-3).quot==-2&&lldiv(-7,-3).rem==-1);
    CHECK(imaxdiv(100,9).quot==11&&imaxabs(-9)==9);
    CHECK(!strcmp(strtok_r(x,",",&a),"one"));CHECK(!strcmp(strtok_r(y,":",&b),"A"));
    CHECK(!strcmp(strtok_r(0,",",&a),"two"));CHECK(!strcmp(strtok_r(0,":",&b),"B"));
    CHECK(strtok_r(0,",",&a)==0);
    CHECK(stpcpy(buf,"abc")==buf+3);CHECK(stpncpy(buf,"x",4)==buf+1&&buf[3]==0);
    CHECK(stpncpy(buf,"12345",3)==buf+3&&buf[2]=='3');
    CHECK(memrchr("abca",'a',4)==(void*)("abca"+3));
    CHECK(memrchr(buf,0,0)==0);CHECK(strcasestr("Hello WORLD","world")!=0);
    CHECK(strcoll("a","b")<0);CHECK(strxfrm(buf,"abc",sizeof(buf))==3&&!strcmp(buf,"abc"));
    CHECK(strerror_r(ERANGE,buf,1)==ERANGE&&buf[0]==0);
    CHECK(strerror_r(0,buf,sizeof(buf))==0&&!strcmp(buf,"Success"));
    {unsigned int p=42,q=42;CHECK(rand_r(&p)==rand_r(&q)&&p==q);}
    return 0;
}
#ifdef LIBC_HOST_TEST
static int test_errno;
int *__errno_location(void){return &test_errno;}
void _start(void){int r=libc_test();__asm__ volatile("int $0x80"::"a"(1),"b"(r):"memory");for(;;){}}
#else
extern void bk_console_write(const char *);
void bleskernos_program_main(void *unused){(void)unused;bk_console_write(libc_test()?"FAIL libc regression\n":"PASS libc regression\n");}
#endif
