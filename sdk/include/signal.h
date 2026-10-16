#ifndef BLESKERNOS_POSIX_SIGNAL_H
#define BLESKERNOS_POSIX_SIGNAL_H

#include <sys/types.h>

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define NSIG 32

typedef unsigned int sigset_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigaction { sighandler_t sa_handler; sigset_t sa_mask; int sa_flags; };
sighandler_t signal(int signal_number, sighandler_t handler);
int sigaction(int signal_number, const struct sigaction *action, struct sigaction *old_action);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signal_number);
int sigdelset(sigset_t *set, int signal_number);
int sigismember(const sigset_t *set, int signal_number);
int sigprocmask(int how, const sigset_t *set, sigset_t *old_set);
int raise(int signal_number);
int kill(pid_t pid, int signal_number);
unsigned int alarm(unsigned int seconds);

#endif
