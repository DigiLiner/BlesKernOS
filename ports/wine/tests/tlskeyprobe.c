#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <errno.h>

extern char **environ;

static pthread_key_t key;
static int main_value = 0x11111111;
static int worker_value = 0x22222222;
static int child_value = 0x33333333;

static inline uint32_t read_gs0(void)
{
    uint32_t value;
    __asm__ volatile ("movl %%gs:0,%0" : "=r"(value));
    return value;
}

static inline uint32_t read_gs8(void)
{
    uint32_t value;
    __asm__ volatile ("movl %%gs:8,%0" : "=r"(value));
    return value;
}

static int snapshot(const char *phase, void *expected,
                    uint32_t expected_gs0, uint32_t expected_gs8)
{
    void *value = pthread_getspecific(key);
    uint32_t gs0 = read_gs0();
    uint32_t gs8 = read_gs8();

    if (value != expected || gs0 != expected_gs0 || gs8 != expected_gs8) {
        printf("[TLSKEY] FAIL phase=%s value=%p expected=%p gs0=%08x/%08x gs8=%08x/%08x\n",
               phase, value, expected, gs0, expected_gs0, gs8, expected_gs8);
        return 1;
    }
    return 0;
}

static void *worker(void *unused)
{
    uint32_t gs0, gs8;
    (void)unused;

    if (pthread_setspecific(key, &worker_value)) return (void *)10;
    gs0 = read_gs0();
    gs8 = read_gs8();
    printf("[TLSKEY] worker-start tid=%ld gs0=%08x gs8=%08x value=%p\n",
           (long)syscall(SYS_gettid), gs0, gs8, pthread_getspecific(key));

    for (int i = 0; i < 4000; ++i) {
        if (snapshot("worker-yield", &worker_value, gs0, gs8))
            return (void *)11;
        sched_yield();
    }
    return NULL;
}

int main(void)
{
    uint32_t main_gs0, main_gs8;
    pthread_t thread;
    void *thread_result = NULL;
    pid_t pid;
    int status;
    char *argv[] = { "/WINEGATE/bin/abi", NULL };

    setvbuf(stdout, NULL, _IONBF, 0);

    if (pthread_key_create(&key, NULL)) {
        printf("[TLSKEY] FAIL key_create errno=%d\n", errno);
        return 1;
    }
    if (pthread_setspecific(key, &main_value)) {
        printf("[TLSKEY] FAIL main setspecific errno=%d key=%u\n",
               errno, (unsigned)key);
        return 2;
    }

    main_gs0 = read_gs0();
    main_gs8 = read_gs8();
    printf("[TLSKEY] main-start pid=%ld tid=%ld key=%u gs0=%08x gs8=%08x value=%p\n",
           (long)getpid(), (long)syscall(SYS_gettid), (unsigned)key,
           main_gs0, main_gs8, pthread_getspecific(key));

    for (int i = 0; i < 4000; ++i) {
        if (snapshot("main-yield", &main_value, main_gs0, main_gs8)) return 3;
        sched_yield();
    }

    if (pthread_create(&thread, NULL, worker, NULL)) {
        puts("[TLSKEY] FAIL pthread_create");
        return 4;
    }
    for (int i = 0; i < 4000; ++i) {
        if (snapshot("main-with-worker", &main_value, main_gs0, main_gs8)) return 5;
        sched_yield();
    }
    if (pthread_join(thread, &thread_result) || thread_result) {
        printf("[TLSKEY] FAIL pthread_join result=%p\n", thread_result);
        return 6;
    }
    if (snapshot("main-after-worker", &main_value, main_gs0, main_gs8)) return 7;
    puts("[TLSKEY] pthread switching PASS");

    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ)) {
        puts("[TLSKEY] FAIL posix_spawn");
        return 8;
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("[TLSKEY] FAIL spawn wait status=%x\n", status);
        return 9;
    }
    if (snapshot("main-after-posix_spawn", &main_value, main_gs0, main_gs8)) return 10;
    puts("[TLSKEY] posix_spawn preservation PASS");

    pid = fork();
    if (pid < 0) {
        puts("[TLSKEY] FAIL fork");
        return 11;
    }
    if (!pid) {
        uint32_t child_gs0 = read_gs0();
        uint32_t child_gs8 = read_gs8();
        if (pthread_getspecific(key) != &main_value) _exit(41);
        if (pthread_setspecific(key, &child_value)) _exit(42);
        for (int i = 0; i < 1000; ++i) {
            if (snapshot("fork-child", &child_value, child_gs0, child_gs8)) _exit(43);
            sched_yield();
        }
        _exit(0);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("[TLSKEY] FAIL fork wait status=%x\n", status);
        return 12;
    }
    if (snapshot("main-after-fork", &main_value, main_gs0, main_gs8)) return 13;

    puts("[TLSKEY] pthread key, GS, fork y posix_spawn PASS");
    return 0;
}
