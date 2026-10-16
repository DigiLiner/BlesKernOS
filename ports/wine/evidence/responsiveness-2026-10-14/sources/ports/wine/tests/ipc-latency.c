/* SPDX-License-Identifier: MIT */
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

static void fail(void) { perror("[IPC-LATENCY] FAIL"); exit(1); }
static unsigned long long micros(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail();
    return (unsigned long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void transfer(int fd, unsigned char *byte, int writing, int polling) {
    if (!writing && polling) {
        struct pollfd p = {fd, POLLIN, 0};
        int n;
        do n = poll(&p, 1, 5000); while (n < 0 && errno == EINTR);
        if (n != 1 || !(p.revents & POLLIN)) fail();
    }
    ssize_t n;
    do n = writing ? write(fd, byte, 1) : read(fd, byte, 1);
    while (n < 0 && errno == EINTR);
    if (n != 1) fail();
}
int main(void) {
    setbuf(stdout, NULL);
    alarm(90);
    for (int polling = 0; polling <= 1; polling++) {
        for (int sample = 0; sample < 3; sample++) {
            int fds[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) fail();
            pid_t child = fork();
            if (child < 0) fail();
            if (!child) {
                close(fds[0]);
                for (int i = 0; i < 520; i++) {
                    unsigned char byte;
                    transfer(fds[1], &byte, 0, polling);
                    byte ^= 0x5a;
                    transfer(fds[1], &byte, 1, 0);
                }
                close(fds[1]);
                _exit(0);
            }
            close(fds[1]);
            unsigned long long begin = 0;
            for (int i = 0; i < 520; i++) {
                if (i == 20) begin = micros();
                unsigned char byte = (unsigned char)i;
                transfer(fds[0], &byte, 1, 0);
                transfer(fds[0], &byte, 0, polling);
                if (byte != ((unsigned char)i ^ 0x5a)) fail();
            }
            unsigned long long elapsed = micros() - begin;
            close(fds[0]);
            int status;
            if (waitpid(child, &status, 0) != child || status) fail();
            printf("[IPC-LATENCY] poll=%d sample=%d rounds=500 us=%llu\n",
                   polling, sample, elapsed);
        }
    }
    puts("[IPC-LATENCY] PASS");
    return 0;
}
