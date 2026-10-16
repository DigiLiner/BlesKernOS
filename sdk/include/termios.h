#ifndef BLESKERNOS_POSIX_TERMIOS_H
#define BLESKERNOS_POSIX_TERMIOS_H
typedef unsigned int tcflag_t; typedef unsigned char cc_t; typedef unsigned int speed_t;
#define NCCS 20
struct termios { tcflag_t c_iflag,c_oflag,c_cflag,c_lflag; cc_t c_cc[NCCS]; speed_t c_ispeed,c_ospeed; };
#define TCSANOW 0
#define ICANON 0x2U
#define ECHO 0x8U
#define ISIG 0x1U
#define OPOST 0x1U
#define VMIN 6
#define VTIME 5
#define B9600 9600U
#define B38400 38400U
int tcgetattr(int fd, struct termios *settings);
int tcsetattr(int fd, int actions, const struct termios *settings);
speed_t cfgetispeed(const struct termios *settings);
speed_t cfgetospeed(const struct termios *settings);
int cfsetispeed(struct termios *settings, speed_t speed);
int cfsetospeed(struct termios *settings, speed_t speed);
#endif
