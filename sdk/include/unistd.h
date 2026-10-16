#ifndef BLESKERNOS_POSIX_UNISTD_H
#define BLESKERNOS_POSIX_UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _POSIX_VERSION 200809L
#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 3
#define _SC_PAGESIZE 4
#define _SC_NPROCESSORS_ONLN 5

ssize_t read(int fd, void *buffer, unsigned int count);
ssize_t write(int fd, const void *buffer, unsigned int count);
int close(int fd);
int dup(int fd);
int dup2(int old_fd, int new_fd);
off_t lseek(int fd, off_t offset, int whence);
int fsync(int fd);
int ftruncate(int fd, off_t length);
int truncate(const char *path, off_t length);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buffer, unsigned int size);
int access(const char *path, int mode);
int isatty(int fd);
int pipe(int descriptors[2]);
long sysconf(int name);
int gethostname(char *name, unsigned int length);
int brk(void *address);
void *sbrk(int increment);
char *realpath(const char *path, char *resolved);
pid_t getpid(void);
pid_t getppid(void);
unsigned int getuid(void);
unsigned int geteuid(void);
unsigned int getgid(void);
unsigned int getegid(void);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t microseconds);
int execv(const char *path, char *const argv[]);
int execve(const char *path, char *const argv[], char *const envp[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg0, ...);
int execlp(const char *file, const char *arg0, ...);
void _exit(int status) __attribute__((noreturn));
extern char **environ;

#endif
