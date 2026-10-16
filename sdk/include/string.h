#ifndef BLESKERNOS_POSIX_STRING_H
#define BLESKERNOS_POSIX_STRING_H
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int size_t;

void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
int memcmp(const void *a, const void *b, size_t count);
void *memchr(const void *memory, int value, size_t count);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t maximum);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t count);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t count);
char *strcat(char *destination, const char *source);
char *strncat(char *destination, const char *source, size_t count);
char *strchr(const char *text, int character);
char *strrchr(const char *text, int character);
char *strstr(const char *text, const char *needle);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t count);
char *strdup(const char *text);
char *strndup(const char *text, size_t maximum);
char *strsep(char **text, const char *delimiters);
void *memmem(const void *memory, size_t memory_length,
             const void *needle, size_t needle_length);
size_t strlcpy(char *destination, const char *source, size_t capacity);
size_t strlcat(char *destination, const char *source, size_t capacity);
size_t strspn(const char *text, const char *accept);
size_t strcspn(const char *text, const char *reject);
char *strpbrk(const char *text, const char *accept);
char *strtok(char *text, const char *delimiters);
char *strerror(int error);
char *strtok_r(char *text,const char *delimiters,char **save);
char *stpcpy(char *destination,const char *source);
char *stpncpy(char *destination,const char *source,size_t count);
void *memrchr(const void *memory,int value,size_t count);
char *strcasestr(const char *text,const char *needle);
int strcoll(const char *a,const char *b);
size_t strxfrm(char *destination,const char *source,size_t count);
/* POSIX/XSI variant: returns an error number, not a char pointer. */
int strerror_r(int error,char *buffer,size_t capacity);
#ifdef __cplusplus
}
#endif

#endif
