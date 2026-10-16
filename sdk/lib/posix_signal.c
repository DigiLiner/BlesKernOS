#include "bleskernos.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>

static struct sigaction handlers[NSIG];
static sigset_t blocked_signals;
static volatile sigset_t pending_signals;
static int dispatcher_registered;
static volatile unsigned int alarm_generation;
static volatile unsigned int alarm_deadline;
static int valid_signal(int number) { return number > 0 && number < NSIG; }
static sigset_t signal_bit(int number) { return 1U << (unsigned int)number; }
static void posix_signal_dispatch(int number);
static int ensure_dispatcher(void) {
    int result;
    if (dispatcher_registered) return 0;
    result = bk_signal_register(posix_signal_dispatch);
    if (result < 0) { errno = -result; return -1; }
    dispatcher_registered = 1;
    return 0;
}

int sigemptyset(sigset_t *set){if(!set){errno=EINVAL;return-1;}*set=0U;return 0;}
int sigfillset(sigset_t *set){if(!set){errno=EINVAL;return-1;}*set=0xFFFFFFFFU;return 0;}
int sigaddset(sigset_t *set,int n){if(!set||!valid_signal(n)){errno=EINVAL;return-1;}*set|=signal_bit(n);return 0;}
int sigdelset(sigset_t *set,int n){if(!set||!valid_signal(n)){errno=EINVAL;return-1;}*set&=~signal_bit(n);return 0;}
int sigismember(const sigset_t *set,int n){if(!set||!valid_signal(n)){errno=EINVAL;return-1;}return(*set&signal_bit(n))?1:0;}

int sigprocmask(int how,const sigset_t *set,sigset_t *old){
    if(old)*old=blocked_signals;
    if(!set)return 0;
    if(how==SIG_BLOCK)blocked_signals|=*set;
    else if(how==SIG_UNBLOCK)blocked_signals&=~*set;
    else if(how==SIG_SETMASK)blocked_signals=*set;
    else{errno=EINVAL;return-1;}
    blocked_signals&=~(signal_bit(SIGKILL)|signal_bit(SIGSTOP));
    for(int number=1;number<NSIG;number++)if((pending_signals&signal_bit(number))&&!(blocked_signals&signal_bit(number))){pending_signals&=~signal_bit(number);posix_signal_dispatch(number);}
    return 0;
}
int sigaction(int n,const struct sigaction *action,struct sigaction *old){
    if(!valid_signal(n)||n==SIGKILL||n==SIGSTOP){errno=EINVAL;return-1;}
    if(ensure_dispatcher()<0)return-1;
    if(old)*old=handlers[n];
    if(action)handlers[n]=*action;
    return 0;
}
sighandler_t signal(int n,sighandler_t handler){struct sigaction a,o;a.sa_handler=handler;a.sa_mask=0U;a.sa_flags=0;if(sigaction(n,&a,&o)<0)return SIG_ERR;return o.sa_handler;}
static void posix_signal_dispatch(int n){sighandler_t handler;sigset_t previous;if(!valid_signal(n))return;if(blocked_signals&signal_bit(n)){pending_signals|=signal_bit(n);return;}handler=handlers[n].sa_handler;if(handler==SIG_IGN||(handler==SIG_DFL&&n==SIGCHLD))return;if(handler&&handler!=SIG_ERR){previous=blocked_signals;blocked_signals|=handlers[n].sa_mask|signal_bit(n);handler(n);blocked_signals=previous;return;}bk_exit(128+n);}
int raise(int n){if(!valid_signal(n)){errno=EINVAL;return-1;}if(ensure_dispatcher()<0)return-1;posix_signal_dispatch(n);return 0;}

typedef struct{unsigned int generation;unsigned int seconds;}alarm_job_t;
static void *alarm_worker(void *opaque){alarm_job_t*job=(alarm_job_t*)opaque;unsigned int generation=job->generation;unsigned int seconds=job->seconds;free(job);while(seconds--){unsigned int start=bk_uptime_ms();while((unsigned int)(bk_uptime_ms()-start)<1000U)bk_sleep(1U);if(alarm_generation!=generation)return(void*)0;}if(alarm_generation==generation){alarm_deadline=0U;(void)raise(SIGALRM);}return(void*)0;}
unsigned int alarm(unsigned int seconds){unsigned int now=bk_uptime_ms();unsigned int remaining=alarm_deadline&&alarm_deadline>now?(alarm_deadline-now+999U)/1000U:0U;pthread_t thread;alarm_job_t*job;alarm_generation++;alarm_deadline=seconds?now+seconds*1000U:0U;if(!seconds)return remaining;job=(alarm_job_t*)malloc(sizeof(*job));if(!job){alarm_deadline=0U;return remaining;}job->generation=alarm_generation;job->seconds=seconds;if(pthread_create(&thread,(const pthread_attr_t*)0,alarm_worker,job)){free(job);alarm_deadline=0U;}else(void)pthread_detach(thread);return remaining;}
