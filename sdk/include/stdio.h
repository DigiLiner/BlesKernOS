#ifndef BLESKERNOS_POSIX_STDIO_H
#define BLESKERNOS_POSIX_STDIO_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef struct __blesk_posix_FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fileno(FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *tmpfile(void);
char *tmpnam(char *buffer);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
char *fgets(char *buffer, int capacity, FILE *stream);
int fputs(const char *text, FILE *stream);
void clearerr(FILE *stream);
void rewind(FILE *stream);
void perror(const char *prefix);
#define _IONBF 2
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
int remove(const char *path);
int rename(const char *old_path, const char *new_path);
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list arguments);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);
int sprintf(char *buffer, const char *format, ...);
int vsprintf(char *buffer, const char *format, va_list arguments);
int putchar(int character);
int puts(const char *text);
int sscanf(const char *text, const char *format, ...);
int vsscanf(const char *text, const char *format, va_list arguments);
int scanf(const char *format, ...);
int vscanf(const char *format, va_list arguments);
int fscanf(FILE *stream, const char *format, ...);
int vfscanf(FILE *stream, const char *format, va_list arguments);
int asprintf(char **text, const char *format, ...);
int vasprintf(char **text, const char *format, va_list arguments);
int getdelim(char **line, size_t *capacity, int delimiter, FILE *stream);
int getline(char **line, size_t *capacity, FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
