/* Integer conversion and arithmetic helpers for the public C library. */
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>

static int number_digit(unsigned char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='z')return c-'a'+10;
    if(c>='A'&&c<='Z')return c-'A'+10;
    return -1;
}
static unsigned long long number_parse(const char *text,char **end,int base,
    unsigned long long positive_limit,unsigned long long negative_limit,
    int *negative,int *overflow) {
    const char *p=text,*first;
    unsigned long long value=0,limit,cutoff;unsigned int remainder;
    int digit;
    *negative=0;*overflow=0;if(end)*end=(char*)text;
    if(base && (base<2||base>36)) {errno=EINVAL;return 0;}
    while(*p==' '||(*p>='\t'&&*p<='\r'))++p;
    if(*p=='+'||*p=='-')*negative=*p++=='-';
    if((base==0||base==16)&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')&&
       number_digit((unsigned char)p[2])>=0&&number_digit((unsigned char)p[2])<16) {
        base=16;p+=2;
    } else if(!base)base=*p=='0'?8:10;
    limit=*negative?negative_limit:positive_limit;
    /* Divide by the small radix without adding a compiler-runtime dependency. */
    remainder=0;
    { /* The public SDK targets little-endian ILP32. Use 32-bit shifts so
       * TinyCC does not require non-exported __ashldi3/__lshrdi3 helpers. */
        union {unsigned long long value;struct {unsigned int low,high;} word;} input,quotient;
        int bit;input.value=limit;quotient.word.low=quotient.word.high=0;
        for(bit=63;bit>=0;--bit){
            unsigned int digit_bit=bit>=32?(input.word.high>>(bit-32))&1U:(input.word.low>>bit)&1U;
            remainder=remainder*2U+digit_bit;
            if(remainder>=(unsigned int)base){remainder-=(unsigned int)base;
                if(bit>=32)quotient.word.high|=1U<<(bit-32);else quotient.word.low|=1U<<bit;}
        }
        cutoff=quotient.value;
    }
    first=p;
    while((digit=number_digit((unsigned char)*p))>=0&&digit<base) {
        if(value>cutoff||(value==cutoff&&(unsigned int)digit>remainder))*overflow=1;
        else if(!*overflow)value=value*(unsigned int)base+(unsigned int)digit;
        ++p;
    }
    if(end&&p!=first)*end=(char*)p;
    if(*overflow){errno=ERANGE;return limit;}
    return value;
}
unsigned long strtoul(const char *s,char **end,int base) {
    int neg,over;unsigned long v=(unsigned long)number_parse(s,end,base,~0UL,~0UL,&neg,&over);
    return neg&&!over?0UL-v:v;
}
long strtol(const char *s,char **end,int base) {
    int neg,over;unsigned long max=~0UL>>1;
    unsigned long v=(unsigned long)number_parse(s,end,base,max,max+1UL,&neg,&over);
    if(neg)return v==max+1UL?-(long)max-1L:-(long)v;
    return (long)v;
}
unsigned long long strtoull(const char *s,char **end,int base) {
    int neg,over;unsigned long long v=number_parse(s,end,base,~0ULL,~0ULL,&neg,&over);
    return neg&&!over?0ULL-v:v;
}
long long strtoll(const char *s,char **end,int base) {
    int neg,over;unsigned long long max=~0ULL>>1;
    unsigned long long v=number_parse(s,end,base,max,max+1ULL,&neg,&over);
    if(neg)return v==max+1ULL?-(long long)max-1LL:-(long long)v;
    return (long long)v;
}
int atoi(const char *s){return (int)strtol(s,0,10);}
long atol(const char *s){return strtol(s,0,10);}
long long atoll(const char *s){return strtoll(s,0,10);}
long labs(long x){return x<0?-x:x;}
long long llabs(long long x){return x<0?-x:x;}
div_t div(int n,int d){div_t r;r.quot=n/d;r.rem=n%d;return r;}
ldiv_t ldiv(long n,long d){ldiv_t r;r.quot=n/d;r.rem=n%d;return r;}
lldiv_t lldiv(long long n,long long d){lldiv_t r;volatile long long q=n/d;r.quot=q;r.rem=n-q*d;return r;}
intmax_t imaxabs(intmax_t n){return llabs(n);}
imaxdiv_t imaxdiv(intmax_t n,intmax_t d){lldiv_t value=lldiv(n,d);imaxdiv_t r;r.quot=value.quot;r.rem=value.rem;return r;}
intmax_t strtoimax(const char *s,char **end,int base){return strtoll(s,end,base);}
uintmax_t strtoumax(const char *s,char **end,int base){return strtoull(s,end,base);}
int rand_r(unsigned int *state){*state=*state*1103515245U+12345U;return (int)((*state>>16)&0x7fffU);}
