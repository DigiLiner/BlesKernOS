/* End-to-end TinyX + Wine 11.0 gate for BlesKernOS, GPL-3.0-or-later. */
#include <X11/Xlib.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char *const wine_env[] = {
    "LD_LIBRARY_PATH=/WINEGATE/lib",
    "FONTCONFIG_FILE=/WINEGATE/etc/fonts/fonts.conf",
    "FONTCONFIG_PATH=/WINEGATE/etc/fonts",
    "WINEDLLPATH=/OPT/WINE/lib/wine",
    "XLOCALEDIR=/WINEGATE/share/X11/locale",
    "WINEPREFIX=/WINEGATE/prefix-w3",
    "DISPLAY=:0", "HOME=/WINEGATE",
    "PATH=/OPT/WINE/bin:/WINEGATE/bin",
    "USER=root", "LOGNAME=root", "TZ=ART3",
    "TMPDIR=/tmp", "TEMP=/tmp", "TMP=/tmp",
    "WINEDEBUG=+timestamp,+pid,err+all,warn+module,warn+winstation",
    "WINEDLLOVERRIDES=mscoree=",
    NULL
};

static pid_t launch(char *const args[], char *const env[])
{
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        execve(args[0], args, env);
        perror(args[0]);
        _exit(127);
    }
    return child;
}

static int wait_child(pid_t child)
{
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); return -1; }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int probe_x_error(Display *display, XErrorEvent *event)
{
    (void)display;
    /* Wine creates/destroys X windows while the probe walks the tree. */
    if (event->error_code != BadWindow)
        fprintf(stderr, "[WINE:X11] probe X error=%u request=%u\n",
                event->error_code, event->request_code);
    return 0;
}

static int find_dialog(Display *display, Window window, int depth)
{
    char *name = NULL;
    int found = 0;
    if (XFetchName(display, window, &name) && name) {
        if (strstr(name, "Wine 11.0 sobre BlesKernOS")) {
            XWindowAttributes attrs;
            found = XGetWindowAttributes(display, window, &attrs) &&
                    attrs.map_state == IsViewable;
        }
        XFree(name);
    }
    Atom utf8_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom actual;
    int format;
    unsigned long property_count, remaining;
    unsigned char *value = NULL;
    if (!found && XGetWindowProperty(display, window, utf8_name, 0, 256,
                                      False, AnyPropertyType, &actual, &format,
                                      &property_count, &remaining, &value) == Success && value) {
        if (strstr((char *)value, "Wine 11.0 sobre BlesKernOS")) {
            XWindowAttributes attrs;
            found = XGetWindowAttributes(display, window, &attrs) &&
                    attrs.map_state == IsViewable;
        }
        XFree(value);
    }
    if (found || depth == 0) return found;
    Window root, parent, *children = NULL;
    unsigned count = 0;
    if (XQueryTree(display, window, &root, &parent, &children, &count)) {
        for (unsigned i = 0; i < count && !found; ++i)
            found = find_dialog(display, children[i], depth - 1);
        if (children) XFree(children);
    }
    return found;
}

int main(void)
{
    mkdir("/tmp", 01777);
    chmod("/tmp", 01777);
    mkdir("/tmp/.X11-unix", 01777);
    setenv("LD_LIBRARY_PATH", "/WINEGATE/lib", 1);
    setenv("DISPLAY", ":0", 1);
    char *const xargs[] = {
        "/WINEGATE/bin/XFBDEV", ":0", "-ac", "-nolock", "-fp",
        "/WINEGATE/fonts/misc", NULL
    };
    pid_t server = launch(xargs, wine_env);
    if (server < 0) return 1;
    Display *display = NULL;
    for (int i = 0; i < 200 && !display; ++i) {
        display = XOpenDisplay(":0");
        if (!display) usleep(100000);
    }
    if (!display) {
        puts("[WINE:X11] XOpenDisplay FAILED"); fflush(stdout);
        kill(server, SIGTERM);
        return 2;
    }
    XSetErrorHandler(probe_x_error);
    puts("[WINE:X11] TinyX ready"); fflush(stdout);
    char *const console_args[] = {
        "/OPT/WINE/bin/wine", "Z:\\WINEGATE\\BIN\\WINE-CONSOLE.EXE", NULL
    };
    pid_t console = launch(console_args, wine_env);
    int console_status = wait_child(console);
    printf("[WINE:X11] console status=%d\n", console_status); fflush(stdout);
    if (console_status != 0) {
        XCloseDisplay(display); kill(server, SIGTERM); wait_child(server);
        return 3;
    }
    char *const gui_args[] = {
        "/OPT/WINE/bin/wine", "Z:\\WINEGATE\\BIN\\WINE-MESSAGEBOX.EXE", NULL
    };
    pid_t gui = launch(gui_args, wine_env);
    int mapped = 0, entered = 0, status = 0, completed = 0;
    for (int i = 0; i < 6000; ++i) {
        if (!entered && access("/WINEGATE/messagebox.enter", F_OK) == 0) {
            puts("[WINE:X11] MessageBoxA entered"); fflush(stdout);
            entered = 1;
        }
        if (!mapped && find_dialog(display, DefaultRootWindow(display), 3)) {
            puts("[WINE:X11] MessageBox mapped"); fflush(stdout);
            mapped = 1;
        }
        pid_t done = waitpid(gui, &status, WNOHANG);
        if (done == gui) { completed = 1; break; }
        usleep(100000);
    }
    if (!completed) {
        kill(gui, SIGTERM); wait_child(gui);
        status = -1;
    }
    printf("[WINE:X11] MessageBox status=%d entered=%d mapped=%d\n",
           status, entered, mapped);
    fflush(stdout);
    XCloseDisplay(display);
    char *const stop[] = {"/OPT/WINE/bin/wineserver", "-k15", NULL};
    pid_t stopper = launch(stop, wine_env);
    wait_child(stopper);
    kill(server, SIGTERM);
    wait_child(server);
    return entered && mapped && status == 0 ? 0 : 4;
}
