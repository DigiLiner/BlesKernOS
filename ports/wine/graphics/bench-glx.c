/* SPDX-License-Identifier: MIT
 * Diagnostic only: actual local GLX render/readback/X11 presentation cost.
 * This is a synthetic workload, not a prediction of game frame rates. */
#include <GL/glx.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

extern Bool bles_glx_copy_front(Display *, GLXDrawable, Pixmap);

static unsigned long long micros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    pid_t server = fork();
    if (server < 0) { perror("fork"); return 1; }
    if (!server) {
        execl("/WINEGATE/bin/XFBDEV", "XFBDEV", ":30", "-ac", "-nolock",
              "-dumbSched", "-fp", "/WINEGATE/fonts/misc", NULL);
        _exit(127);
    }
    Display *d = NULL;
    for (int i = 0; i < 100 && !d; ++i) {
        usleep(50000);
        d = XOpenDisplay(":30");
    }
    int result = 1;
    if (!d) goto done;
    #define LOAD(name) __typeof__(&name) bk_##name = (__typeof__(&name))glXGetProcAddress((const GLubyte *)#name); if (!bk_##name) goto close
    LOAD(glViewport);
    LOAD(glMatrixMode);
    LOAD(glLoadIdentity);
    LOAD(glDrawBuffer);
    LOAD(glClearColor);
    LOAD(glClear);
    LOAD(glColor3f);
    LOAD(glBegin);
    LOAD(glVertex2f);
    LOAD(glEnd);
    LOAD(glFinish);
    #undef LOAD
    int count;
    GLXFBConfig *configs = glXGetFBConfigs(d, DefaultScreen(d), &count);
    if (!configs || count < 2) goto close;
    GLXContext c = glXCreateNewContext(d, configs[1], GLX_RGBA_TYPE, NULL, True);
    free(configs);
    if (!c) goto close;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 640, 480, 0, 0, 0);
    XMapWindow(d, w);
    XSync(d, False);
    if (!glXMakeCurrent(d, w, c)) goto destroy;
    printf("[GLX-BENCH] renderer=%s version=%s glsl=%s\n", glGetString(GL_RENDERER),
           glGetString(GL_VERSION), glGetString(0x8b8c));
    const unsigned sizes[][2] = {{320,240}, {640,480}, {1024,768}};
    const unsigned batches[] = {1,128,1024};
    for (unsigned s = 0; s < 3; ++s) {
        unsigned width = sizes[s][0], height = sizes[s][1];
        XResizeWindow(d, w, width, height);
        XSync(d, False);
        if (!glXMakeCurrent(d, w, c)) goto destroy;
        bk_glViewport(0, 0, width, height);
        bk_glMatrixMode(GL_PROJECTION); bk_glLoadIdentity();
        bk_glMatrixMode(GL_MODELVIEW); bk_glLoadIdentity();
        bk_glDrawBuffer(GL_BACK);
        for (unsigned b = 0; b < 3; ++b) {
            for (unsigned phase = 0; phase < 2; ++phase) {
            unsigned frames = 24;
            unsigned long long elapsed = 0;
            for (unsigned frame = 0; frame < frames + 4; ++frame) {
                if (frame == 4) printf("[GLX-BENCH] begin phase=%s size=%ux%u batches=%u\n",
                    phase ? "present" : "render", width, height, batches[b]);
                unsigned long long start = micros();
                bk_glClearColor(0, 0, 1, 1);
                bk_glClear(GL_COLOR_BUFFER_BIT);
                /* Many small batches stress translation/submission without
                 * turning this into a full-screen overdraw benchmark. */
                for (unsigned i = 0; i < batches[b]; ++i) {
                    float x = (float)(i % 32) / 16.0f - 1.0f;
                    float y = (float)((i / 32) % 32) / 16.0f - 1.0f;
                    bk_glColor3f(1, 0, 0);
                    bk_glBegin(GL_TRIANGLES);
                    bk_glVertex2f(x,y); bk_glVertex2f(x+0.05f,y); bk_glVertex2f(x,y+0.05f);
                    bk_glEnd();
                }
                if (phase) { glXSwapBuffers(d, w); XSync(d, False); }
                else bk_glFinish(); /* Include server consumption of XPutImage. */
                unsigned long long end = micros();
                if (frame >= 4) elapsed += end - start;
            }
            printf("[GLX-BENCH] phase=%s size=%ux%u batches=%u frames=%u us=%llu\n",
                   phase ? "present" : "render", width, height, batches[b], frames, elapsed);
            }
        }
        Pixmap front = XCreatePixmap(d, DefaultRootWindow(d), width, height, DefaultDepth(d, DefaultScreen(d)));
        if (!bles_glx_copy_front(d, w, front)) goto destroy;
        XImage *image = XGetImage(d, front, width-1, height-1, 1, 1, AllPlanes, ZPixmap);
        unsigned long pixel = image ? XGetPixel(image,0,0) & 0xffffff : ~0UL;
        if (image) XDestroyImage(image);
        XFreePixmap(d, front);
        printf("[GLX-BENCH] corner=%06lx expected=0000ff\n", pixel);
        if (pixel != 0x0000ff) goto destroy;
    }
    result = 0;
destroy:
    glXMakeCurrent(d, None, NULL);
    glXDestroyContext(d, c);
    XDestroyWindow(d, w);
close:
    XCloseDisplay(d);
done:
    kill(server, 15);
    waitpid(server, NULL, 0);
    printf("[GLX-BENCH] %s\n", result ? "FAIL" : "PASS");
    return result;
}
