/* Native X11 functional gate for BlesKernOS TinyX, GPL-3.0-or-later. */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    mkdir("/tmp", 01777);
    mkdir("/tmp/.X11-unix", 01777);
    setenv("LD_LIBRARY_PATH", "/WINEGATE/lib", 1);
    setenv("DISPLAY", ":0", 1);
    setenv("HOME", "/WINEGATE", 1);
    pid_t server = fork();
    if (server < 0) { perror("fork"); return 1; }
    if (server == 0) {
        char *const args[] = {
            "/WINEGATE/bin/XFBDEV", ":0", "-ac", "-nolock", "-fp",
            "/WINEGATE/fonts/misc", NULL
        };
        puts("[TINYX] starting Xfbdev"); fflush(stdout);
        execv(args[0], args);
        perror("Xfbdev exec"); _exit(127);
    }
    Display *display = NULL;
    for (int attempt = 0; attempt < 100 && !display; ++attempt) {
        display = XOpenDisplay(":0");
        if (!display) usleep(100000);
    }
    if (!display) {
        int status = 0;
        if (waitpid(server, &status, WNOHANG) == server)
            printf("[TINYX] server exited status=%d\n", status);
        else kill(server, SIGTERM);
        puts("[TINYX] XOpenDisplay failed"); fflush(stdout);
        return 2;
    }
    printf("[TINYX] XOpenDisplay OK, screen=%d\n", DefaultScreen(display));
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                         70, 60, 300, 200, 1,
                                         BlackPixel(display, screen),
                                         WhitePixel(display, screen));
    XSelectInput(display, window, ExposureMask | KeyPressMask |
                                 ButtonPressMask | PointerMotionMask);
    XMapWindow(display, window);
    XSync(display, False);
    XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
    XSetForeground(display, DefaultGC(display, screen), 0x00cc3333UL);
    XFillRectangle(display, window, DefaultGC(display, screen),
                   30, 30, 180, 100);
    XFlush(display);
    int expose = 0;
    for (int i = 0; i < 40; ++i) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == Expose) expose = 1;
        }
        if (expose) break;
        usleep(50000);
    }
    printf("[TINYX] rectangle mapped, expose=%d\n", expose);
    fflush(stdout);
    int keys = 0, moves = 0, clicks = 0;
    for (int i = 0; i < 600; ++i) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == KeyPress) {
                keys++;
                printf("[TINYX] key keysym=0x%lx state=0x%x\n",
                       XLookupKeysym(&event.xkey, 0), event.xkey.state);
                fflush(stdout);
            }
            if (event.type == MotionNotify) moves++;
            if (event.type == ButtonPress) clicks++;
        }
        usleep(50000);
    }
    printf("[TINYX] input keys=%d moves=%d clicks=%d\n", keys, moves, clicks);
    fflush(stdout);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    return expose ? 0 : 3;
}
