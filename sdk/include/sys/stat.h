#ifndef BLESKERNOS_POSIX_SYS_STAT_H
#define BLESKERNOS_POSIX_SYS_STAT_H

#include <sys/types.h>

#define S_IFMT  0170000U
#define S_IFDIR 0040000U
#define S_IFREG 0100000U
#define S_IRUSR 0000400U
#define S_IWUSR 0000200U
#define S_IXUSR 0000100U
#define S_IRGRP 0000040U
#define S_IWGRP 0000020U
#define S_IROTH 0000004U
#define S_IWOTH 0000002U

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    off_t st_size;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

int stat(const char *path, struct stat *info);
int fstat(int fd, struct stat *info);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif
