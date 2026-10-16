#include "bleskernos.h"
#include "bleskernos_thread.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct { void *(*start)(void *); void *argument; } posix_thread_start_t;
/* Slot 31 belongs to libc's per-thread errno. */
static volatile bk_u32 key_mask = 0x80000000U;
static void release_posix_thread_state(void){void*p=bk_tls_get(31U);if(p){(void)bk_tls_set(31U,0);free(p);}}

static bk_i32 thread_trampoline(void *opaque) {
    posix_thread_start_t *start = (posix_thread_start_t *)opaque;
    void *(*function)(void *) = start->start;
    void *argument = start->argument;
    void *result;
    free(start);
    result = function(argument);
    release_posix_thread_state();
    return (bk_i32)(bk_u32)result;
}

static int thread_error(bk_i32 result) { return result < 0 ? -result : result; }

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                   void *(*start)(void *), void *argument) {
    posix_thread_start_t *context;
    bk_i32 result;
    if (!thread || !start) return EINVAL;
    context = (posix_thread_start_t *)malloc(sizeof(*context));
    if (!context) return ENOMEM;
    context->start = start; context->argument = argument;
    result = bk_thread_create(thread, "pthread", thread_trampoline, context,
        attributes ? attributes->stack_size : BK_THREAD_DEFAULT_STACK);
    if (result < 0) { free(context); return thread_error(result); }
    if (attributes && attributes->detached) (void)bk_thread_detach(*thread);
    return 0;
}

int pthread_join(pthread_t thread, void **result) {
    bk_i32 raw = 0;
    bk_i32 status = bk_thread_join(thread, &raw);
    if (status < 0) return thread_error(status);
    if (result) *result = (void *)(bk_u32)raw;
    return 0;
}
int pthread_detach(pthread_t thread) { return thread_error(bk_thread_detach(thread)); }
pthread_t pthread_self(void) { return bk_thread_self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
void pthread_exit(void *result) { release_posix_thread_state(); bk_thread_exit((bk_i32)(bk_u32)result); }
int pthread_attr_init(pthread_attr_t *a) { if (!a) return EINVAL; a->stack_size=BK_THREAD_DEFAULT_STACK; a->detached=0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { return a ? 0 : EINVAL; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) { if (!a || size<BK_THREAD_MIN_STACK || size>BK_THREAD_MAX_STACK) return EINVAL; a->stack_size=size; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int state) { if (!a || (state!=0 && state!=1)) return EINVAL; a->detached=state; return 0; }
int pthread_mutex_init(pthread_mutex_t *m,const void*a){(void)a;if(!m)return EINVAL;bk_mutex_init(m);return 0;}
int pthread_mutex_destroy(pthread_mutex_t*m){return m?0:EINVAL;}
int pthread_mutex_lock(pthread_mutex_t*m){return thread_error(bk_mutex_lock(m));}
int pthread_mutex_trylock(pthread_mutex_t*m){return thread_error(bk_mutex_try_lock(m));}
int pthread_mutex_unlock(pthread_mutex_t*m){return thread_error(bk_mutex_unlock(m));}
int pthread_cond_init(pthread_cond_t*c,const void*a){(void)a;if(!c)return EINVAL;bk_cond_init(c);return 0;}
int pthread_cond_destroy(pthread_cond_t*c){return c?0:EINVAL;}
int pthread_cond_wait(pthread_cond_t*c,pthread_mutex_t*m){if(!c||!m)return EINVAL;bk_cond_wait(c,m);return 0;}
int pthread_cond_timedwait(pthread_cond_t*c,pthread_mutex_t*m,const struct timespec*a){bk_u32 sequence;if(!c||!m||!a)return EINVAL;sequence=bk_atomic_load(&c->sequence);if(bk_mutex_unlock(m)<0)return EINVAL;while(bk_atomic_load(&c->sequence)==sequence){struct timespec now;if(clock_gettime(CLOCK_REALTIME,&now)<0){(void)bk_mutex_lock(m);return EINVAL;}if(now.tv_sec>a->tv_sec||(now.tv_sec==a->tv_sec&&now.tv_nsec>=a->tv_nsec)){(void)bk_mutex_lock(m);return ETIMEDOUT;}bk_sleep(1U);}(void)bk_mutex_lock(m);return 0;}
int pthread_cond_signal(pthread_cond_t*c){if(!c)return EINVAL;bk_cond_signal(c);return 0;}
int pthread_cond_broadcast(pthread_cond_t*c){if(!c)return EINVAL;bk_cond_broadcast(c);return 0;}
int pthread_rwlock_init(pthread_rwlock_t*l,const void*a){(void)a;if(!l)return EINVAL;bk_rwlock_init(l);return 0;}
int pthread_rwlock_rdlock(pthread_rwlock_t*l){if(!l)return EINVAL;bk_rwlock_read_lock(l);return 0;}
int pthread_rwlock_wrlock(pthread_rwlock_t*l){if(!l)return EINVAL;bk_rwlock_write_lock(l);return 0;}
int pthread_rwlock_unlock(pthread_rwlock_t*l){if(!l)return EINVAL;if(bk_atomic_load(&l->writer))bk_rwlock_write_unlock(l);else bk_rwlock_read_unlock(l);return 0;}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor;
    if (!key) return EINVAL;
    for (bk_u32 i=0U;i<BK_TLS_SLOTS;i++) {
        bk_u32 bit=1U<<i, old=key_mask;
        while (!(old&bit)) { bk_u32 expected=old; if (__sync_bool_compare_and_swap(&key_mask,expected,expected|bit)){*key=i;return 0;} old=key_mask; }
    }
    return EAGAIN;
}
int pthread_key_delete(pthread_key_t key){if(key>=BK_TLS_SLOTS)return EINVAL;__sync_fetch_and_and(&key_mask,~(1U<<key));return 0;}
int pthread_setspecific(pthread_key_t key,const void*value){return thread_error(bk_tls_set(key,(void*)value));}
void *pthread_getspecific(pthread_key_t key){return bk_tls_get(key);}
int pthread_once(pthread_once_t*control,void(*initialize)(void)){if(!control||!initialize)return EINVAL;if(__sync_bool_compare_and_swap(control,0U,1U)){initialize();__sync_synchronize();*control=2U;}else while(*control!=2U)bk_yield();return 0;}
int pthread_barrier_init(pthread_barrier_t*b,const void*a,unsigned int n){(void)a;if(!b||!n)return EINVAL;bk_barrier_init(b,n);return 0;}
int pthread_barrier_destroy(pthread_barrier_t*b){return b?0:EINVAL;}
int pthread_barrier_wait(pthread_barrier_t*b){if(!b)return EINVAL;bk_barrier_wait(b);return 0;}
