/* Stage 1 application smoke test on real Wine/TinyX, GPL-3.0-or-later. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef STAGE1_DEFAULT_PROGRAM
#define STAGE1_DEFAULT_PROGRAM "notepad.exe"
#endif
#ifndef STAGE1_DEFAULT_SECONDS
#define STAGE1_DEFAULT_SECONDS 180
#endif
#ifndef STAGE1_STARTUP_SECONDS
#define STAGE1_STARTUP_SECONDS 600
#endif

static char *const wine_env[] = {
    "LD_LIBRARY_PATH=/WINEGATE/lib",
    "FONTCONFIG_FILE=/WINEGATE/etc/fonts/fonts.conf",
    "FONTCONFIG_PATH=/WINEGATE/etc/fonts",
    "WINEDLLPATH=/OPT/WINE/lib/wine",
    "XLOCALEDIR=/WINEGATE/share/X11/locale",
    "WINEPREFIX=/WINEGATE/prefix-w3", "DISPLAY=:0", "HOME=/WINEGATE",
    "PATH=/OPT/WINE/bin:/WINEGATE/bin", "USER=root", "LOGNAME=root",
    "TZ=ART3", "TMPDIR=/tmp", "TEMP=/tmp", "TMP=/tmp",
#ifdef STAGE2_TRACE_MSI
    "WINEDEBUG=+timestamp,+pid,err+all,warn+module,trace+msi,trace+cabinet",
#elif defined(STAGE1_TRACE_X11)
    "WINEDEBUG=+timestamp,+pid,err+all,+x11drv",
#elif defined(STAGE1_TRACE_KEYBOARD)
    "WINEDEBUG=+timestamp,+pid,err+all,+keyboard",
#else
    "WINEDEBUG=+timestamp,+pid,err+all,warn+module,warn+winstation",
#endif
    "WINEDLLOVERRIDES=mscoree=", NULL
};

static pid_t launch(char *const args[])
{
    pid_t child = fork();
    if (child == 0) {
        execve(args[0], args, wine_env);
        perror(args[0]);
        _exit(127);
    }
    return child;
}

static int x_error(Display *display, XErrorEvent *error)
{
    (void)display;
    if (error->error_code != BadWindow)
        fprintf(stderr, "[STAGE1] X error=%u request=%u\n",
                error->error_code, error->request_code);
    return 0;
}

static int scan_windows(Display *display, Window parent, int depth,
                        const char *application)
{
    Window root, ignored, *children = NULL;
    unsigned count = 0;
    int mapped = 0;
    if (!depth || !XQueryTree(display, parent, &root, &ignored, &children, &count))
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(display, children[i], &attr)) continue;
        char *title = NULL;
        int owned_title = 0;
        XFetchName(display, children[i], &title);
        if (!title || !*title) {
            Atom type;
            int format;
            unsigned long count, remaining;
            unsigned char *value = NULL;
            Atom property = XInternAtom(display, "_NET_WM_NAME", False);
            if (XGetWindowProperty(display, children[i], property, 0, 256,
                                   False, AnyPropertyType, &type, &format,
                                   &count, &remaining, &value) == Success && value) {
                if (title) XFree(title);
                title = strndup((char *)value, count);
                owned_title = 1;
                XFree(value);
            }
        }
        XClassHint class_hint = {0};
        XGetClassHint(display, children[i], &class_hint);
        /* With no external X window manager Wine can map a borderless
         * full-size top-level without WM_NAME.  It still has the client
         * geometry, unlike wineboot's small progress dialog. */
        if (attr.map_state == IsViewable &&
            ((title && strcasestr(title, application)) ||
             (class_hint.res_name && strcasestr(class_hint.res_name, application)) ||
             (class_hint.res_class && strcasestr(class_hint.res_class, application)) ||
             (attr.width >= 400 && attr.height >= 250))) {
            printf("[STAGE1] window id=%lu size=%dx%d title=%s class=%s\n",
                   children[i], attr.width, attr.height, title ? title : "",
                   class_hint.res_class ? class_hint.res_class : "");
            mapped++;
        }
        if (title) {
            if (owned_title) free(title);
            else XFree(title);
        }
        if (class_hint.res_name) XFree(class_hint.res_name);
        if (class_hint.res_class) XFree(class_hint.res_class);
        mapped += scan_windows(display, children[i], depth - 1, application);
    }
    if (children) XFree(children);
    return mapped;
}

int main(int argc, char **argv)
{
    const char *program = argc > 1 ? argv[1] : STAGE1_DEFAULT_PROGRAM;
    char application[256];
    snprintf(application, sizeof(application), "%s", program);
    char *extension = strrchr(application, '.');
    if (extension && !strcasecmp(extension, ".exe")) *extension = 0;
    int duration = argc > 2 ? atoi(argv[2]) : STAGE1_DEFAULT_SECONDS;
    if (duration < 10 || duration > 600) duration = STAGE1_DEFAULT_SECONDS;
    mkdir("/tmp", 01777); chmod("/tmp", 01777);
    mkdir("/tmp/.X11-unix", 01777);
    setenv("LD_LIBRARY_PATH", "/WINEGATE/lib", 1);
    setenv("DISPLAY", ":0", 1);
    char *const xargs[] = {"/WINEGATE/bin/XFBDEV", ":0", "-ac", "-nolock",
                           "-fp", "/WINEGATE/fonts/misc", NULL};
    pid_t server = launch(xargs);
    if (server < 0) return 1;
    Display *display = NULL;
    for (int i = 0; i < 200 && !display; ++i) {
        display = XOpenDisplay(":0");
        if (!display) usleep(100000);
    }
    if (!display) { puts("[STAGE1] TinyX failed"); kill(server, SIGTERM); return 2; }
    XSetErrorHandler(x_error);
    puts("[STAGE1] TinyX ready"); fflush(stdout);
#ifdef STAGE2_PROBE_EXTENSIONS
    {
        static const char *const extensions[] = {
            "RENDER", "SHAPE", "XFIXES", "RANDR", "XInputExtension"
        };
        for (unsigned i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
            int opcode, event_base, error_base;
            int present = XQueryExtension(display, extensions[i],
                                          &opcode, &event_base, &error_base);
            printf("[STAGE2] X extension %s=%d\n", extensions[i], present);
        }
        fflush(stdout);
    }
#endif
#ifdef STAGE2_MSI_INSTALL
    char *const wineargs[] = {"/OPT/WINE/bin/wine", "msiexec", "/i",
                              (char *)program, NULL};
#elif defined(STAGE2_PROGRAM_ARGUMENT)
    char *const wineargs[] = {"/OPT/WINE/bin/wine", (char *)program,
                              STAGE2_PROGRAM_ARGUMENT, NULL};
#else
    char *const wineargs[] = {"/OPT/WINE/bin/wine", (char *)program, NULL};
#endif
    pid_t app = launch(wineargs);
    if (app < 0) { XCloseDisplay(display); kill(server, SIGTERM); return 3; }
    printf("[STAGE1] started %s\n", program); fflush(stdout);
    int mapped = 0, mapped_tick = -1, status = 0, exited = 0;
    for (int tick = 0; tick < (STAGE1_STARTUP_SECONDS + duration) * 10; ++tick) {
        if (!mapped && scan_windows(display, DefaultRootWindow(display), 3,
                                    application)) {
            mapped = 1;
            mapped_tick = tick;
            puts("[STAGE1] application mapped"); fflush(stdout);
        }
        if (mapped && tick - mapped_tick >= duration * 10) break;
        if (!mapped && tick >= STAGE1_STARTUP_SECONDS * 10) break;
        if (waitpid(app, &status, WNOHANG) == app) { exited = 1; break; }
        if (tick % 300 == 0) {
            printf("[STAGE1] heartbeat seconds=%d mapped=%d\n", tick / 10, mapped);
            fflush(stdout);
        }
        usleep(100000);
    }
    printf("[STAGE1] result mapped=%d exited=%d status=%d\n", mapped, exited, status);
    fflush(stdout);
    if (!exited) { kill(app, SIGTERM); waitpid(app, &status, 0); }
    XCloseDisplay(display);
    char *const stop[] = {"/OPT/WINE/bin/wineserver", "-k", NULL};
    pid_t stopper = launch(stop);
    if (stopper > 0) waitpid(stopper, NULL, 0);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
#ifdef STAGE2_ACCEPT_EXIT
    return mapped && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0
           ? 0 : 4;
#else
    return mapped && !exited ? 0 : 4;
#endif
}
