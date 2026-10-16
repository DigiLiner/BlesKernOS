#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>

static void *worker(void *argument) {
    int *value = (int *)argument;
    *value += 7;
    return (void *)0x1234U;
}
static volatile int signal_seen;
static void signal_probe(int number) { signal_seen = number; }

void bleskernos_program_main(void *unused) {
    static const char message[] = "POSIX x86 OK\n";
    struct stat info;
    struct timespec now;
    char buffer[sizeof(message)];
    DIR *directory;
    pthread_t thread;
    void *thread_result = 0;
    unsigned char *mapping;
    int worker_value = 5;
    int pipe_fds[2];
    struct pollfd polled;
    fd_set readable;
    struct timeval immediate = {0, 0};
    struct utsname system_name;
    sem_t semaphore;
    int fd;
    int duplicate;
    (void)unused;

    /* Como servicio de autoprueba, esperar a que el escritorio termine sus
     * lecturas iniciales antes de ejercer las escrituras FAT. */
    sleep(5U);

    fd = open("/TEMP/POSIX.TXT", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) _exit(10);
    if (write(fd, message, sizeof(message) - 1U) !=
        (ssize_t)(sizeof(message) - 1U)) _exit(11);
    if (lseek(fd, 0, SEEK_SET) != 0) _exit(12);
    memset(buffer, 0, sizeof(buffer));
    if (read(fd, buffer, sizeof(message) - 1U) !=
        (ssize_t)(sizeof(message) - 1U)) _exit(13);
    if (strcmp(buffer, message) != 0) _exit(14);
    if (lseek(fd, 0, SEEK_SET) != 0 || (duplicate = dup(fd)) < 0) _exit(36);
    if (read(duplicate, buffer, 5U) != 5 || lseek(fd, 0, SEEK_CUR) != 5 ||
        close(duplicate) != 0) _exit(37);
    if (fstat(fd, &info) != 0 || info.st_size !=
        (off_t)(sizeof(message) - 1U)) _exit(15);
    if (fsync(fd) != 0 || close(fd) != 0) _exit(16);
    if (stat("/TEMP/POSIX.TXT", &info) != 0 || !S_ISREG(info.st_mode))
        _exit(17);
    directory = opendir("/TEMP");
    if (!directory || !readdir(directory)) _exit(18);
    if (closedir(directory) != 0) _exit(19);
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) _exit(20);
    mapping = (unsigned char *)mmap(0, 8192U, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) _exit(21);
    mapping[0] = 0x5AU; mapping[8191] = 0xA5U;
    if (mapping[0] != 0x5AU || mapping[8191] != 0xA5U ||
        munmap(mapping, 8192U) != 0) _exit(22);
    (void)write(STDOUT_FILENO, "POSIX mmap OK\n", 14U);
    if (pipe(pipe_fds) != 0 || write(pipe_fds[1], "pipe", 4U) != 4) _exit(26);
    polled.fd=pipe_fds[0]; polled.events=POLLIN; polled.revents=0;
    if(poll(&polled,1,0)!=1||!(polled.revents&POLLIN))_exit(27);
    FD_ZERO(&readable); FD_SET(pipe_fds[0],&readable);
    if(select(pipe_fds[0]+1,&readable,0,0,&immediate)!=1||
       !FD_ISSET(pipe_fds[0],&readable))_exit(28);
    memset(buffer,0,sizeof(buffer));
    if(read(pipe_fds[0],buffer,4U)!=4||memcmp(buffer,"pipe",4U))_exit(29);
    if(close(pipe_fds[0])||close(pipe_fds[1]))_exit(30);
    if(sem_init(&semaphore,0,1U)||sem_wait(&semaphore)||sem_post(&semaphore)||
       sem_destroy(&semaphore))_exit(31);
    if(uname(&system_name)||strcmp(system_name.sysname,"BlesKernOS"))_exit(32);
    if(time((time_t*)0)==(time_t)-1)_exit(33);
    if(sqrt(81.0)!=9.0||fabs(sin(0.0))>0.0001)_exit(34);
    {
        int parsed=0;char word[8];char temporary[]="/TEMP/PXXXXXX";
        if(sscanf("42 ready","%d %7s",&parsed,word)!=2||parsed!=42||strcmp(word,"ready"))_exit(38);
        duplicate=mkstemp(temporary);if(duplicate<0||close(duplicate)||unlink(temporary))_exit(39);
        if(signal(SIGUSR1,signal_probe)==SIG_ERR||raise(SIGUSR1)||signal_seen!=SIGUSR1)_exit(40);
        signal_seen=0;if(kill(getpid(),SIGUSR1))_exit(41);
        for(int spin=0;spin<1000&&!signal_seen;spin++)sched_yield();
        if(signal_seen!=SIGUSR1)_exit(42);
    }
    {
        Display *display=XOpenDisplay(0);XGCValues values;GC gc;Window window;
        if(!display)_exit(43);
        window=XCreateSimpleWindow(display,DefaultRootWindow(display),40,40,160,90,1,0,WhitePixel(display,0));
        if(!window)_exit(44);
        XStoreName(display,window,"Xlib Ring 3");
        XSelectInput(display,window,ExposureMask|KeyPressMask|StructureNotifyMask);
        if(XMapWindow(display,window))_exit(45);
        values.foreground=0x2050A0UL;gc=XCreateGC(display,window,GCForeground,&values);
        if(!gc)_exit(46);
        XFillRectangle(display,window,gc,8,8,40,24);
        XDrawLine(display,window,gc,0,0,159,89);
        XFlush(display);XFreeGC(display,gc);XDestroyWindow(display,window);XCloseDisplay(display);
    }
    {
        char small[8];char *formatted=0;void*runtime=dlopen(0,RTLD_NOW);
        void*module;int(*answer)(void);
        if(strlcpy(small,"abcdef",sizeof(small))!=6U||strcmp(small,"abcdef"))_exit(47);
        if(asprintf(&formatted,"%s %d","value",7)!=7||strcmp(formatted,"value 7"))_exit(48);
        free(formatted);
        if(!runtime||!dlsym(runtime,"malloc")||dlclose(runtime))_exit(49);
        module=dlopen("/SYSTEM/TESTS/NATIVE/DLMOD.SO",RTLD_NOW);
        if(!module)_exit(50);
        answer=(int(*)(void))dlsym(module,"blesk_dynamic_answer");
        if(!answer||answer()!=42||dlclose(module))_exit(51);
        (void)write(STDOUT_FILENO,"POSIX dlopen ET_DYN OK\n",23U);
    }
    {
        char *arguments[]={(char*)"probe",(char*)"--mode",(char*)"gui",0};
        struct option options[]={{"mode",required_argument,0,'m'},{0,0,0,0}};
        optind=1;
        if(getopt_long(3,arguments,"",options,0)!='m'||strcmp(optarg,"gui"))
            _exit(35);
    }
    (void)write(STDOUT_FILENO, "POSIX IPC/compat OK\n", 20U);
    if (pthread_create(&thread, 0, worker, &worker_value) != 0) _exit(23);
    (void)write(STDOUT_FILENO, "POSIX pthread create OK\n", 24U);
    if (pthread_join(thread, &thread_result) != 0) _exit(24);
    (void)write(STDOUT_FILENO, "POSIX pthread join OK\n", 22U);
    if (worker_value != 12 || thread_result != (void *)0x1234U) _exit(25);
    (void)write(STDOUT_FILENO, "POSIX pthread OK\n", 17U);
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1U);
    _exit(0);
}
