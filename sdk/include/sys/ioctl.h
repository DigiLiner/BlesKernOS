#ifndef BLESKERNOS_POSIX_SYS_IOCTL_H
#define BLESKERNOS_POSIX_SYS_IOCTL_H
struct winsize { unsigned short ws_row,ws_col,ws_xpixel,ws_ypixel; };
#define TIOCGWINSZ 0x5413
int ioctl(int fd, unsigned long request, ...);
#endif
