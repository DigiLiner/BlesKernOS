/* Caller-memory helpers execute in Ring 3; they need no kernel service. */
#include "kernel/include/memory.h"
void *kmemcpy(void *dst,const void *src,size_t n) {
    void *result=dst;uint8_t *d=dst;const uint8_t *s=src;
    uint32_t count=n/4;
    __asm__ volatile("cld; rep movsl":"+D"(d),"+S"(s),"+c"(count)::"memory");
    n&=3U;while(n--)*d++=*s++;
    return result;
}
void *kmemset(void *dst,int c,size_t n) {
    void *result=dst;uint8_t *d=dst;uint32_t count=n/4;
    uint32_t value=(uint8_t)c*0x01010101U;
    __asm__ volatile("cld; rep stosl":"+D"(d),"+c"(count):"a"(value):"memory");
    n&=3U;while(n--)*d++=(uint8_t)c;
    return result;
}
int kmemcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return (int)(*pa - *pb);
        pa++;
        pb++;
    }
    return 0;
}

size_t kstrlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

int kstrcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int kstrncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && (*a == *b)) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *kstrcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *kstrncpy(char *dst, const char *src, size_t n) {
    size_t i=0;
    while(i<n && src[i]){dst[i]=src[i];i++;}
    while(i<n)dst[i++]='\0';
    return dst;
}

char *kstrcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}
