#ifndef BLESKERNOS_POSIX_SEMAPHORE_H
#define BLESKERNOS_POSIX_SEMAPHORE_H

#include "bleskernos_thread.h"
#include <time.h>
typedef bk_semaphore_t sem_t;
int sem_init(sem_t *semaphore, int process_shared, unsigned int value);
int sem_destroy(sem_t *semaphore);
int sem_wait(sem_t *semaphore);
int sem_trywait(sem_t *semaphore);
int sem_timedwait(sem_t *semaphore, const struct timespec *absolute_timeout);
int sem_post(sem_t *semaphore);
int sem_getvalue(sem_t *semaphore, int *value);

#endif
