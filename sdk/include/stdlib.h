#ifndef BLESKERNOS_POSIX_STDLIB_H
#define BLESKERNOS_POSIX_STDLIB_H
#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

typedef unsigned int size_t;
typedef struct {int quot, rem;} div_t;
typedef struct {long quot, rem;} ldiv_t;
typedef struct {long long quot, rem;} lldiv_t;

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void *reallocarray(void *pointer, size_t count, size_t size);
void free(void *pointer);
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int abs(int value);
long labs(long value);
long long llabs(long long value);
div_t div(int numerator,int denominator);
ldiv_t ldiv(long numerator,long denominator);
lldiv_t lldiv(long long numerator,long long denominator);
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));
int atoi(const char *text);
long atol(const char *text);
long long atoll(const char *text);
long long strtoll(const char *text,char **end,int base);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
double strtod(const char *text, char **end);
int rand(void);
int rand_r(unsigned int *state);
void srand(unsigned int seed);
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int system(const char *command);
int mkstemp(char *template_name);
char *mkdtemp(char *template_name);
char *mktemp(char *template_name);

#ifdef __cplusplus
}
#endif

#endif
