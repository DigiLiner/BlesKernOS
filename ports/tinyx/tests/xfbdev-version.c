/* Smoke test for the genuine 32-bit TinyX server under BlesK's Linux ABI. */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char *const args[] = {"/WINEGATE/bin/XFBDEV", "-version", NULL};
    char *const env[] = {
        "LD_LIBRARY_PATH=/WINEGATE/lib",
        "HOME=/WINEGATE",
        "PATH=/WINEGATE/bin",
        NULL
    };
    puts("[TINYX] exec Xfbdev -version");
    fflush(stdout);
    execve(args[0], args, env);
    printf("[TINYX] exec failed errno=%d\n", errno);
    return 1;
}
