#include <string.h>
#include <errno.h>

char *strtok_r(char *text,const char *delimiters,char **save) {
    char *end;
    if(!text)text=*save;
    if(!text)return 0;
    text+=strspn(text,delimiters);
    if(!*text){*save=text;return 0;}
    end=text+strcspn(text,delimiters);
    if(*end)*end++=0;
    *save=end;return text;
}
char *stpcpy(char *d,const char *s){while((*d=*s)){++d;++s;}return d;}
char *stpncpy(char *d,const char *s,size_t n) {
    size_t i=0;while(i<n&&s[i]){d[i]=s[i];++i;}
    char *end=d+i;while(i<n)d[i++]=0;return end;
}
void *memrchr(const void *p,int value,size_t n) {
    const unsigned char *s=p;while(n){--n;if(s[n]==(unsigned char)value)return (void*)(s+n);}return 0;
}
char *strcasestr(const char *s,const char *needle) {
    size_t n=strlen(needle);if(!n)return (char*)s;
    while(*s){if(!strncasecmp(s,needle,n))return (char*)s;++s;}return 0;
}
/* The SDK currently supports the C locale. */
int strcoll(const char *a,const char *b){return strcmp(a,b);}
size_t strxfrm(char *d,const char *s,size_t n){size_t len=strlen(s);if(n){size_t copy=len<n?len:n;memcpy(d,s,copy);if(len<n)d[len]=0;}return len;}
int strerror_r(int error,char *buffer,size_t capacity) {
    const char *s=strerror(error);size_t n=strlen(s);
    if(!capacity)return ERANGE;
    strlcpy(buffer,s,capacity);
    if(n>=capacity)return ERANGE;
    return !strcmp(s,"Unknown error")?EINVAL:0;
}

void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    for (size_t i = 0U; i < count; i++) d[i] = s[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t count) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    if (d < s) for (size_t i = 0U; i < count; i++) d[i] = s[i];
    else while (count) { count--; d[count] = s[count]; }
    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *d = (unsigned char *)destination;
    for (size_t i = 0U; i < count; i++) d[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *a, const void *b, size_t count) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0U; i < count; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

void *memchr(const void *memory, int value, size_t count) {
    const unsigned char *bytes = (const unsigned char *)memory;
    for (size_t i = 0U; i < count; i++)
        if (bytes[i] == (unsigned char)value) return (void *)(bytes + i);
    return (void *)0;
}

size_t strlen(const char *text) {
    size_t length = 0U;
    while (text && text[length]) length++;
    return length;
}
size_t strnlen(const char *text,size_t maximum){size_t length=0U;while(text&&length<maximum&&text[length])length++;return length;}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t count) {
    while (count && *a && *a == *b) { a++; b++; count--; }
    return count ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strcpy(char *destination, const char *source) {
    char *result = destination;
    while ((*destination++ = *source++) != '\0') {}
    return result;
}

char *strncpy(char *destination, const char *source, size_t count) {
    size_t i = 0U;
    while (i < count && source[i]) { destination[i] = source[i]; i++; }
    while (i < count) destination[i++] = '\0';
    return destination;
}

char *strcat(char *destination, const char *source) {
    strcpy(destination + strlen(destination), source);
    return destination;
}
char *strncat(char *destination, const char *source, size_t count) {
    char *end = destination + strlen(destination); size_t i=0;
    while(i<count && source[i]){end[i]=source[i];i++;}end[i]=0;return destination;
}

char *strchr(const char *text, int character) {
    do { if (*text == (char)character) return (char *)text; } while (*text++);
    return (char *)0;
}

char *strrchr(const char *text, int character) {
    const char *found = (char *)0;
    do { if (*text == (char)character) found = text; } while (*text++);
    return (char *)found;
}

char *strstr(const char *text, const char *needle) {
    size_t length = strlen(needle);
    if (!length) return (char *)text;
    while (*text) {
        if (*text == *needle && strncmp(text, needle, length) == 0)
            return (char *)text;
        text++;
    }
    return (char *)0;
}

static unsigned char ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + 32U) : value;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b)) {
        a++; b++;
    }
    return (int)ascii_lower((unsigned char)*a) -
           (int)ascii_lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t count) {
    while (count && *a &&
           ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b)) {
        a++; b++; count--;
    }
    return count ? (int)ascii_lower((unsigned char)*a) -
                   (int)ascii_lower((unsigned char)*b) : 0;
}

char *strdup(const char *text) {
    size_t length = strlen(text) + 1U;
    extern void *malloc(size_t size);
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}
char *strndup(const char *text,size_t maximum){size_t length=strnlen(text,maximum);extern void*malloc(size_t);char*copy=(char*)malloc(length+1U);if(copy){memcpy(copy,text,length);copy[length]=0;}return copy;}
char *strsep(char **text,const char *delimiters){char*start,*cursor;if(!text||!*text)return 0;start=*text;cursor=start;while(*cursor&&!strchr(delimiters,*cursor))cursor++;if(*cursor){*cursor=0;*text=cursor+1;}else *text=0;return start;}
void *memmem(const void *memory,size_t length,const void *needle,size_t needle_length){const unsigned char*source=(const unsigned char*)memory;if(!needle_length)return(void*)memory;if(needle_length>length)return 0;for(size_t i=0;i<=length-needle_length;i++)if(!memcmp(source+i,needle,needle_length))return(void*)(source+i);return 0;}
size_t strlcpy(char *destination,const char *source,size_t capacity){size_t length=strlen(source);if(capacity){size_t copy=length<capacity-1U?length:capacity-1U;memcpy(destination,source,copy);destination[copy]=0;}return length;}
size_t strlcat(char *destination,const char *source,size_t capacity){size_t used=strnlen(destination,capacity),length=strlen(source);if(used<capacity){size_t room=capacity-used-1U,copy=length<room?length:room;memcpy(destination+used,source,copy);destination[used+copy]=0;}return used+length;}

size_t strspn(const char*t,const char*a){size_t n=0;while(t[n]&&strchr(a,t[n]))n++;return n;}
size_t strcspn(const char*t,const char*r){size_t n=0;while(t[n]&&!strchr(r,t[n]))n++;return n;}
char *strpbrk(const char*t,const char*a){while(*t){if(strchr(a,*t))return(char*)t;t++;}return 0;}
char *strtok(char*t,const char*d){static char*next;if(t)next=t;if(!next)return 0;next+=strspn(next,d);if(!*next){next=0;return 0;}t=next;next+=strcspn(next,d);if(*next)*next++=0;else next=0;return t;}
char *strerror(int error){switch(error){case 0:return"Success";case 1:return"Operation not permitted";case 3:return"No such process";case 4:return"Interrupted system call";case 8:return"Exec format error";case 10:return"No child processes";case 16:return"Device or resource busy";case 19:return"No such device";case 20:return"Not a directory";case 21:return"Is a directory";case 29:return"Illegal seek";case 30:return"Read-only file system";case 33:return"Numerical argument out of domain";case 34:return"Numerical result out of range";case 35:return"Resource deadlock avoided";case 36:return"File name too long";case 39:return"Directory not empty";case 2:return"No such file or directory";case 5:return"Input/output error";case 9:return"Bad file descriptor";case 11:return"Resource temporarily unavailable";case 12:return"Out of memory";case 13:return"Permission denied";case 17:return"File exists";case 22:return"Invalid argument";case 24:return"Too many open files";case 28:return"No space left on device";case 32:return"Broken pipe";case 38:return"Function not implemented";case 110:return"Operation timed out";case 111:return"Connection refused";default:return"Unknown error";}}
