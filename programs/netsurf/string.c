/* Private libc primitives: parser/CSS operations on application memory must
 * stay in Ring 3 instead of invoking int 0x80 for every byte/string helper. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *memcpy(void *destination, const void *source, size_t count) {
    uint8_t *d=destination;
    const uint8_t *s=source;
#if defined(__i386__)
    size_t words=count/4U,tail=count%4U;
    __asm__ volatile("cld; rep movsl" : "+D"(d),"+S"(s),"+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d),"+S"(s),"+c"(tail) : : "memory");
#else
    for (size_t i=0;i<count;i++) d[i]=s[i];
#endif
    return destination;
}
void *memmove(void *destination,const void *source,size_t count) {
    uint8_t *d=destination;const uint8_t *s=source;
    if ((uintptr_t)d<=(uintptr_t)s) return memcpy(destination,source,count);
    while (count) {count--;d[count]=s[count];}
    return destination;
}
void *memset(void *destination,int value,size_t count) {
    uint8_t *d=destination;
    for (size_t i=0;i<count;i++)d[i]=(uint8_t)value;
    return destination;
}
int memcmp(const void *left,const void *right,size_t count) {
    const uint8_t *a=left,*b=right;
    for (size_t i=0;i<count;i++)if(a[i]!=b[i])return (int)a[i]-(int)b[i];
    return 0;
}
size_t strlen(const char *text) {size_t n=0;while(text[n])n++;return n;}
int strcmp(const char *left,const char *right) {
    while (*left && *left==*right) {left++;right++;}
    return (int)(uint8_t)*left-(int)(uint8_t)*right;
}
int strncmp(const char *left,const char *right,size_t count) {
    for(size_t i=0;i<count;i++) {
        int a=(uint8_t)left[i],b=(uint8_t)right[i];
        if(a!=b || !a)return a-b;
    }
    return 0;
}
char *strncpy(char *destination,const char *source,size_t count) {
    size_t i=0;
    for(;i<count && source[i];i++)destination[i]=source[i];
    for(;i<count;i++)destination[i]=0;
    return destination;
}
static int ascii_lower(unsigned char c) {return c>='A'&&c<='Z'?c+('a'-'A'):c;}
int strcasecmp(const char *left,const char *right) {
    for(;;left++,right++) {
        int a=ascii_lower((uint8_t)*left),b=ascii_lower((uint8_t)*right);
        if(a!=b || !a)return a-b;
    }
}
int strncasecmp(const char *left,const char *right,size_t count) {
    for(size_t i=0;i<count;i++) {
        int a=ascii_lower((uint8_t)left[i]),b=ascii_lower((uint8_t)right[i]);
        if(a!=b || !a)return a-b;
    }
    return 0;
}
char *strchr(const char *text,int character) {
    uint8_t c=(uint8_t)character;
    do {if((uint8_t)*text==c)return (char *)text;}while(*text++);
    return NULL;
}
char *strrchr(const char *text,int character) {
    const char *last=NULL;uint8_t c=(uint8_t)character;
    do {if((uint8_t)*text==c)last=text;}while(*text++);
    return (char *)last;
}
char *strstr(const char *text,const char *needle) {
    if(!*needle)return (char *)text;
    for(;*text;text++) {
        size_t i=0;while(needle[i] && text[i] && needle[i]==text[i])i++;
        if(!needle[i])return (char *)text;
    }
    return NULL;
}
