#ifndef BLESKERNOS_POSIX_PTHREAD_H
#define BLESKERNOS_POSIX_PTHREAD_H

#include "bleskernos_thread.h"
#include <stddef.h>
#include <time.h>

typedef bk_thread_t pthread_t;
typedef bk_mutex_t pthread_mutex_t;
typedef bk_cond_t pthread_cond_t;
typedef bk_rwlock_t pthread_rwlock_t;
typedef bk_barrier_t pthread_barrier_t;
typedef bk_u32 pthread_key_t;
typedef volatile bk_u32 pthread_once_t;
typedef struct { size_t stack_size; int detached; } pthread_attr_t;

#define PTHREAD_MUTEX_INITIALIZER {{0U}, 0U}
#define PTHREAD_COND_INITIALIZER {{0U}}
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_ONCE_INIT 0U
#define PTHREAD_BARRIER_SERIAL_THREAD 1

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                   void *(*start)(void *), void *argument);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
void pthread_exit(void *result) __attribute__((noreturn));
int pthread_attr_init(pthread_attr_t *attributes);
int pthread_attr_destroy(pthread_attr_t *attributes);
int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size);
int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state);
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_cond_init(pthread_cond_t *condition, const void *attributes);
int pthread_cond_destroy(pthread_cond_t *condition);
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                           const struct timespec *absolute_timeout);
int pthread_cond_signal(pthread_cond_t *condition);
int pthread_cond_broadcast(pthread_cond_t *condition);
int pthread_rwlock_init(pthread_rwlock_t *lock, const void *attributes);
int pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int pthread_rwlock_wrlock(pthread_rwlock_t *lock);
int pthread_rwlock_unlock(pthread_rwlock_t *lock);
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);
int pthread_once(pthread_once_t *control, void (*initialize)(void));
int pthread_barrier_init(pthread_barrier_t *barrier, const void *attributes,
                         unsigned int participants);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

#endif
