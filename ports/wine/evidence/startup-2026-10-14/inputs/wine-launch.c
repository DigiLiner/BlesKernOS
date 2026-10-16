/* Wine 11 desktop launcher for the BlesKernOS Linux i386 ABI.
 * GPL-3.0-or-later, matching the TinyX port. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"

#define BLES_DISPLAY_FIRST 10
#define BLES_DISPLAY_COUNT 48
#define BLES_TRACKED_WINDOWS 64
#define BLES_EMPTY_GRACE_MS 5000UL

typedef struct {
    Window id;
    unsigned generation;
    unsigned x, y, width, height, parent_id, flags, z_index;
    int active;
    char title[48];
} tracked_window_t;

static char display_env[32] = "DISPLAY=:10";
static char *environment[] = {
    "LD_LIBRARY_PATH=/WINEGATE/lib",
    "FONTCONFIG_FILE=/WINEGATE/etc/fonts/fonts.conf",
    "FONTCONFIG_PATH=/WINEGATE/etc/fonts",
    "WINEDLLPATH=/OPT/WINE/lib/wine",
    "XLOCALEDIR=/WINEGATE/share/X11/locale",
    "WINEPREFIX=/WINEGATE/prefix", display_env, "HOME=/WINEGATE",
    "PATH=/OPT/WINE/bin:/WINEGATE/bin", "USER=root", "LOGNAME=root",
    "TZ=ART3", "TMPDIR=/tmp", "TEMP=/tmp", "TMP=/tmp",
    /* No glibc locale archive is installed. Wine uses LC_ALL as its own
     * locale-name fallback when libc reports C; without it, the empty name
     * selects Windows' invariant locale and old installers reject it. */
    "LC_ALL=en_US.UTF-8",
    "WINEDLLOVERRIDES=mscoree=",
    /* Keep COM1 for kernel failures and launcher status. Wine's default
     * fixme/warn stream includes expected probes on every application run. */
    "WINEDEBUG=-all",
    "BLES_TINYX_ROOTLESS=1",
    "BLES_WINE_GL=native",
    "GALLIUM_DRIVER=softpipe",
    "LP_NUM_THREADS=1",
    NULL
};

/* Normal boots stay quiet; diagnostic runs retain the full timing stream. */
static int diagnostics;
static char wine_debug_env[512];
static void wine_trace(const char *format, ...)
{
    va_list args;
    if (!diagnostics) return;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static tracked_window_t tracked[BLES_TRACKED_WINDOWS];
static Display *window_manager_display;
static void process_wm_event(Display *display, const XEvent *event);
static unsigned generation;
static struct timespec launch_started;
static int first_surface_reported;
static unsigned long perf_x_ready_ms;
static unsigned long perf_prefix_ready_ms;
static unsigned long perf_theme_ready_ms;
static unsigned long perf_target_begin_ms;
static unsigned long perf_target_spawn_ms;
static bles_gfx_wine_vm_perf_t perf_vm_baseline;
static int perf_vm_baseline_valid;

static unsigned long elapsed_ms(void)
{
    struct timespec now;
    long seconds, nanos;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    seconds = now.tv_sec - launch_started.tv_sec;
    nanos = now.tv_nsec - launch_started.tv_nsec;
    if (nanos < 0) { --seconds; nanos += 1000000000L; }
    return (unsigned long)seconds * 1000UL + (unsigned long)nanos / 1000000UL;
}


static int wine_vm_perf_snapshot(int control_fd, bles_gfx_wine_vm_perf_t *out)
{
    if (!diagnostics || control_fd < 0 || !out) return -1;
    memset(out, 0, sizeof(*out));
    /* pid=0 asks BlesKernOS for cumulative counters. This deliberately
     * includes Wine helper children and wineserver, even if they exit before
     * the first visible window. */
    out->pid = 0;
    return ioctl(control_fd, BLES_GFX_WINE_VM_PERF, out);
}

static void wine_cache_perf_print(int control_fd,const char *phase)
{
    bles_gfx_wine_cache_perf_t stats;
    if (!diagnostics) return;
    memset(&stats,0,sizeof(stats));
    if(ioctl(control_fd,BLES_GFX_WINE_CACHE_PERF,&stats)<0)return;
    wine_trace("[WINE:CACHE-PERF] phase=%s cache_entries=%u cache_bytes=%u "
           "cache_evictions=%u vfs_handles_peak=%u vfs_handles=%u cache_writeback_fds=%u\n",
           phase,stats.cache_entries,stats.cache_bytes,stats.cache_evictions,
           stats.vfs_handles_peak,stats.vfs_handles,stats.private_fds);
}

static void wine_boot_report(int control_fd,int status)
{
    if(control_fd>=0)(void)ioctl(control_fd,BLES_GFX_WINE_BOOT_READY,&status);
}

static unsigned perf_delta_u32(uint32_t after, uint32_t before)
{
    return (unsigned)(after - before);
}

static void wine_vm_perf_print_delta(const char *phase,
                                     const bles_gfx_wine_vm_perf_t *before,
                                     const bles_gfx_wine_vm_perf_t *after)
{
    if (!before || !after) return;
    wine_trace("[WINE:VM-PERF] phase=%s mmap=%u file=%u cached=%u "
           "hits=%u misses=%u fallback=%u file_io_kib=%u file_io_ms=%u\n",
           phase ? phase : "?",
           perf_delta_u32(after->mmap_calls, before->mmap_calls),
           perf_delta_u32(after->file_maps, before->file_maps),
           perf_delta_u32(after->cached_file_maps, before->cached_file_maps),
           perf_delta_u32(after->cache_hits, before->cache_hits),
           perf_delta_u32(after->cache_misses, before->cache_misses),
           perf_delta_u32(after->fallback_reads, before->fallback_reads),
           perf_delta_u32(after->file_io_kib, before->file_io_kib),
           perf_delta_u32(after->file_io_ms, before->file_io_ms));
}

/* Windows may disappear between XQueryTree() and a control action.  A normal
 * window manager treats those races as benign; do the same instead of letting
 * Xlib terminate the rootless bridge with BadWindow/BadMatch. */
static int ignore_x_error(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    return 0;
}

static void select_display(int number)
{
    snprintf(display_env, sizeof(display_env), "DISPLAY=:%d", number);
}

static pid_t start_at(char *const argv[], const char *directory)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* TinyX probes transports absent from this local-only port. The
         * launcher checks exit status/connection failure; detailed server
         * output belongs to an explicitly requested diagnostic run. */
        if (!diagnostics && !strcmp(argv[0], "/WINEGATE/bin/XFBDEV")) {
            int quiet = open("/dev/null", O_WRONLY);
            if (quiet >= 0) {
                (void)dup2(quiet, STDOUT_FILENO);
                (void)dup2(quiet, STDERR_FILENO);
                if (quiet > STDERR_FILENO) close(quiet);
            }
        }
        if (directory && chdir(directory) < 0) {
            perror("[WINE] application directory");
            _exit(127);
        }
        execve(argv[0], argv, environment);
        perror(argv[0]);
        _exit(127);
    }
    return pid;
}

static pid_t start(char *const argv[])
{
    return start_at(argv, NULL);
}

static int run_wait(char *const argv[])
{
    int status = 0;
    pid_t pid = start(argv);
    if (pid < 0) return -1;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0) return -1;
        if (window_manager_display) {
            for (unsigned i = 0; i < 64 && XPending(window_manager_display); ++i) {
                XEvent event;
                XNextEvent(window_manager_display, &event);
                process_wm_event(window_manager_display, &event);
            }
            XFlush(window_manager_display);
        }
        {
            struct timespec pause = {0, 10000000L};
            nanosleep(&pause, NULL);
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int ends_with(const char *value, const char *suffix)
{
    size_t a = strlen(value), b = strlen(suffix);
    return a >= b && !strcasecmp(value + a - b, suffix);
}

/* This international Quake II script rejects the US/Canadian locales.
 * Fingerprint the script, not just SETUP.EXE: other editions and unrelated
 * installers must keep their normal locale. No registry changes are needed. */
static int quake2_international_installer(const char *application)
{
    char directory[4096], script[4096];
    const char *suffixes[] = {"/SETUP.INS", "/INSTALL/SETUP.INS"};
    if (!application || !ends_with(application, "/setup.exe") ||
        strlen(application) >= sizeof(directory)) return 0;
    strcpy(directory, application);
    *strrchr(directory, '/') = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (snprintf(script, sizeof(script), "%s%s", directory, suffixes[i]) >=
            (int)sizeof(script)) continue;
        FILE *file = fopen(script, "rb");
        if (!file) continue;
        unsigned char bytes[1024];
        uint32_t hash = 2166136261U;
        size_t count, total = 0;
        while ((count = fread(bytes, 1, sizeof(bytes), file)) != 0) {
            total += count;
            if (total > 67519U) break;
            for (size_t j = 0; j < count; ++j)
                hash = (hash ^ bytes[j]) * 16777619U;
        }
        int matched = !ferror(file) && total == 67519U && hash == 0x5295e9ddU;
        fclose(file);
        if (matched) return 1;
    }
    return 0;
}

/* Prefix setup and appearance are intentionally separate stages.
 * The old rootless v2 code applied the palette with many `wine reg add`
 * invocations. On a fresh prefix the first one implicitly bootstrapped Wine,
 * so a theme operation could spend minutes running wineboot/services and the
 * launcher had no way to tell that apart from launching the requested EXE.
 *
 * v4 makes that lifecycle explicit:
 *   1. wineboot --init once per prefix;
 *   2. one .reg import for the Vision palette;
 *   3. launch the requested Windows program.
 */
/* Cross-process POSIX advisory lock.  Separate X servers are allowed, but
 * only one launcher may initialize/reset the shared prefix or import its
 * registry theme. The kernel implements F_SETLK and releases a lock when the
 * owning process exits, including after an interrupted QEMU session. */
#define BLES_PREFIX_INIT_LOCK "/WINEGATE/.bles-prefix-init.lock"

static int wine_prefix_lock(void)
{
    struct flock request;
    int fd = open(BLES_PREFIX_INIT_LOCK, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("[WINE:STAGE] PREFIX lock open");
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    /* Poll instead of F_SETLKW: BlesKernOS' current blocking fcntl path
     * holds the VFS kernel domain while waiting, starving the lock owner
     * when wineboot needs to create files. */
    for (unsigned retry = 0; retry < 3000; ++retry) {
        if (fcntl(fd, F_SETLK, &request) == 0) {
            wine_trace("[WINE:STAGE] PREFIX exclusive lock acquired\n");
            return fd;
        }
        if (errno != EAGAIN && errno != EACCES) {
            perror("[WINE:STAGE] PREFIX lock fcntl");
            break;
        }
        usleep(100000);
    }
    fprintf(stderr, "[WINE:STAGE] PREFIX busy/unavailable; refusing unsafe reset\n");
    close(fd);
    return -1;
}

#define BLES_PREFIX_MARKER "/WINEGATE/prefix/.bles-prefix-ready-v1"
#define BLES_UPDATE_MARKER "/WINEGATE/prefix/.update-timestamp"
#define BLES_WINE_C_ROOT   "/SYSTEM/WINE"
#define BLES_PREFIX_DRIVE_C "/WINEGATE/prefix/drive_c"
#define BLES_DOSDEV_C "/WINEGATE/prefix/dosdevices/c:"
#define BLES_DOSDEV_Z "/WINEGATE/prefix/dosdevices/z:"
#define BLES_THEME_MARKER  "/WINEGATE/prefix/.bles-vision-theme-v5"
#define BLES_THEME_UNIX    "/SYSTEM/WINE/windows/temp/BLESVIS.REG"
#define BLES_THEME_WIN     "C:\\windows\\temp\\BLESVIS.REG"

static int touch_marker(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static int write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!written) return -1;
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int prefix_registry_present(void)
{
    return access("/WINEGATE/prefix/system.reg", F_OK) == 0 &&
           access("/WINEGATE/prefix/user.reg", F_OK) == 0;
}

/* FAT timestamps do not track Wine's Unix-side update probe reliably.  The
 * validated BlesKernOS prefix uses Wine's "disable" sentinel so that each
 * program launch does not run wineboot again over an already initialized
 * prefix. */
static int disable_automatic_prefix_update(void)
{
    static const char value[] = "disable\n";
    int fd = open(BLES_UPDATE_MARKER, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    if (write_all(fd, value, sizeof(value) - 1) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

/* mtools cannot copy names containing ':' into a FAT image. Create the
 * Wine drive links on first launch, including their FAT POSIX symlink mode. */
static int ensure_dosdevice(const char *path, const char *target)
{
    char current[64];
    ssize_t size;

    if (symlink(target, path) == 0) return 0;
    if (errno != EEXIST) return -1;
    size = readlink(path, current, sizeof(current));
    if (size == (ssize_t)strlen(target) &&
        memcmp(current, target, (size_t)size) == 0) return 0;
    if (unlink(path) < 0) return -1;
    return symlink(target, path);
}

static int ensure_prefix_drive_c_alias(void)
{
    struct stat st;
    char current[64];
    ssize_t size;

    if (mkdir("/SYSTEM", 0755) < 0 && errno != EEXIST) return -1;
    if (mkdir(BLES_WINE_C_ROOT, 0755) < 0 && errno != EEXIST) return -1;
    if (mkdir("/WINEGATE/prefix", 0755) < 0 && errno != EEXIST) return -1;

    if (lstat(BLES_PREFIX_DRIVE_C, &st) < 0) {
        if (errno != ENOENT) return -1;
        return symlink(BLES_WINE_C_ROOT, BLES_PREFIX_DRIVE_C);
    }
    if (!S_ISLNK(st.st_mode)) {
        /* Legacy images kept drive_c inside the private prefix.  New images
         * never enter this branch; leave the old tree intact rather than
         * risking destructive migration during boot.  C: itself is still
         * redirected to /SYSTEM/WINE by dosdevices below. */
        return 0;
    }
    size = readlink(BLES_PREFIX_DRIVE_C, current, sizeof(current));
    if (size == (ssize_t)strlen(BLES_WINE_C_ROOT) &&
        memcmp(current, BLES_WINE_C_ROOT, (size_t)size) == 0) return 0;
    if (unlink(BLES_PREFIX_DRIVE_C) < 0) return -1;
    return symlink(BLES_WINE_C_ROOT, BLES_PREFIX_DRIVE_C);
}

/* A real Windows drive root matters to multi-volume installers. Z:\\CDROM
 * makes CD-relative paths resolve against the system root instead of the CD. */
static char optical_drive_letter;
static int ensure_optical_drive(void)
{
    struct stat st;char link[80],target[64];
    optical_drive_letter=0;
    if(stat("/CDROM",&st)<0 || !S_ISDIR(st.st_mode))return 0;
    /* Reuse a configured optical mapping before taking a free letter. */
    for(char letter='d';letter<='y';++letter) {
        snprintf(link,sizeof(link),"/WINEGATE/prefix/dosdevices/%c:",letter);
        ssize_t length=readlink(link,target,sizeof(target));
        if(length==6 && !memcmp(target,"/CDROM",6)) {optical_drive_letter=letter-'a'+'A';return 0;}
    }
    for(char letter='d';letter<='y';++letter) {
        snprintf(link,sizeof(link),"/WINEGATE/prefix/dosdevices/%c:",letter);
        if(lstat(link,&st)==0)continue; /* Preserve user-configured drives. */
        if(errno!=ENOENT)return -1;
        if(symlink("/CDROM",link)==0) {optical_drive_letter=letter-'a'+'A';return 0;}
        if(errno!=EEXIST)return -1; /* Another launcher may have reserved it. */
        ssize_t length=readlink(link,target,sizeof(target));
        if(length==6 && !memcmp(target,"/CDROM",6)) {optical_drive_letter=letter-'a'+'A';return 0;}
    }
    return 0;
}
static int optical_windows_path(const char *source,char *target,size_t capacity)
{
    if(!optical_drive_letter || strncasecmp(source,"/CDROM",6) ||
       (source[6] && source[6]!='/'))return 0;
    if(snprintf(target,capacity,"%c:\\%s",optical_drive_letter,source[6]?source+7:"")>=(int)capacity)return -1;
    for(char *p=target+2;*p;++p)if(*p=='/')*p='\\';
    return 1;
}

static int ensure_dosdevices(void)
{
    if (ensure_prefix_drive_c_alias() < 0) return -1;
    if (mkdir("/WINEGATE/prefix/dosdevices", 0755) < 0 && errno != EEXIST)
        return -1;
    if (ensure_dosdevice(BLES_DOSDEV_C, BLES_WINE_C_ROOT) < 0) return -1;
    if(ensure_dosdevice(BLES_DOSDEV_Z,"/")<0)return -1;
    return ensure_optical_drive();
}

/* wineboot creates system.reg/user.reg well before the whole prefix is ready.
 * If QEMU is stopped while Wine is still copying system32, a later wineboot
 * sees those registry files and tries to continue over a half-populated tree.
 * That produced misleading loader errors such as STATUS_INVALID_FILE_FOR_SECTION
 * and missing shell32/user32 even though the packaged Wine files were present.
 *
 * Treat our own ready marker as the commit record.  Any prefix directory without
 * it is an interrupted transaction and is rebuilt from a clean directory. */
static int remove_tree(const char *path)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;

    if (lstat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);

    dir = opendir(path);
    if (!dir) return -1;
    while ((entry = readdir(dir)) != NULL) {
        char child[1024];
        int rc;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child)) {
            errno = ENAMETOOLONG;
            closedir(dir);
            return -1;
        }
        rc = remove_tree(child);
        if (rc < 0) {
            int saved = errno;
            closedir(dir);
            errno = saved;
            return -1;
        }
    }
    if (closedir(dir) < 0) return -1;
    return rmdir(path);
}

static int ensure_wine_prefix(void)
{
    int rc;
    struct stat prefix_st;
    int prefix_exists = lstat("/WINEGATE/prefix", &prefix_st) == 0;

    if (access(BLES_PREFIX_MARKER, F_OK) == 0 && prefix_registry_present()) {
        if (ensure_dosdevices() < 0) return -1;
        if (disable_automatic_prefix_update() < 0) return -1;
        wine_trace("[WINE:STAGE] PREFIX ready marker=1\n");
        fflush(stdout);
        return 0;
    }

    if (prefix_exists) {
        wine_trace("[WINE:STAGE] PREFIX recovery reason=no-ready-marker action=reset\n");
        fflush(stdout);
        if (remove_tree("/WINEGATE/prefix") < 0) {
            perror("[WINE:STAGE] PREFIX reset");
            return -1;
        }
        wine_trace("[WINE:STAGE] PREFIX recovery reset=done\n");
        fflush(stdout);
    }

    /* Create the public C: mapping before wineboot.  A fresh Wine prefix then
     * populates /SYSTEM/WINE directly instead of recreating a private drive_c. */
    if (ensure_dosdevices() < 0) {
        perror("[WINE:STAGE] PREFIX layout");
        return -1;
    }

    wine_trace("[WINE:STAGE] PREFIX begin mode=wineboot-init existing=0\n");
    fflush(stdout);
    {
        char *const args[] = {
            "/OPT/WINE/bin/wineboot", "--init", NULL
        };
        rc = run_wait(args);
    }
    wine_trace("[WINE:STAGE] PREFIX wineboot-exit=%d registry=%d\n",
           rc, prefix_registry_present());
    fflush(stdout);

    if (rc != 0 || !prefix_registry_present()) {
        fprintf(stderr,
                "[WINE:STAGE] PREFIX failed wineboot=%d system-user-reg=%d\n",
                rc, prefix_registry_present());
        return -1;
    }
    if (ensure_dosdevices() < 0) {
        perror("[WINE:STAGE] PREFIX dosdevices");
        return -1;
    }
    if (disable_automatic_prefix_update() < 0) {
        perror("[WINE:STAGE] PREFIX disable-update");
        return -1;
    }
    if (touch_marker(BLES_PREFIX_MARKER) < 0) {
        perror("[WINE:STAGE] PREFIX marker");
        return -1;
    }
    wine_trace("[WINE:STAGE] PREFIX ready marker=created\n");
    fflush(stdout);
    return 0;
}

/* One registry import replaces the old sequence of Wine processes. Keeping
 * the file under the public C: tree avoids depending on a Z: mapping and also
 * keeps the generated registry fragment visible to native BlesKernOS tools. */
static int write_bles_theme_reg(void)
{
    static const char registry[] =
        "Windows Registry Editor Version 5.00\r\n"
        "\r\n"
        "[HKEY_CURRENT_USER\\Software\\Wine\\Drivers]\r\n"
        "\"Audio\"=\"oss\"\r\n"
        "\r\n"
        "[HKEY_CURRENT_USER\\Control Panel\\Colors]\r\n"
        "\"ActiveTitle\"=\"75 147 168\"\r\n"
        "\"InactiveTitle\"=\"169 160 160\"\r\n"
        "\"ButtonFace\"=\"196 179 171\"\r\n"
        "\"ButtonHilight\"=\"241 229 223\"\r\n"
        "\"ButtonLight\"=\"216 199 191\"\r\n"
        "\"ButtonShadow\"=\"127 116 110\"\r\n"
        "\"ButtonDkShadow\"=\"77 76 79\"\r\n"
        "\"ButtonText\"=\"26 23 22\"\r\n"
        "\"Menu\"=\"196 179 171\"\r\n"
        "\"MenuText\"=\"26 23 22\"\r\n"
        "\"Hilight\"=\"75 147 168\"\r\n"
        "\"HilightText\"=\"255 255 255\"\r\n"
        "\"Window\"=\"255 255 255\"\r\n"
        "\"WindowText\"=\"26 23 22\"\r\n"
        "\r\n"
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager]\r\n"
        "\"ThemeActive\"=\"0\"\r\n"
        "\r\n"
        /* Vision has no XDG MIME/menu consumer. Keep Windows associations;
         * avoid exporting every association to Linux desktop metadata. */
        "[HKEY_CURRENT_USER\\Software\\Wine\\FileOpenAssociations]\r\n"
        "\"Enable\"=\"N\"\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\Software\\Classes\\Folder\\DefaultIcon]\r\n"
        "@=\"C:\\\\windows\\\\system32\\\\BLESICONS.DLL,-1\"\r\n"
        "[HKEY_LOCAL_MACHINE\\Software\\Classes\\Drive\\DefaultIcon]\r\n"
        "@=\"C:\\\\windows\\\\system32\\\\BLESICONS.DLL,-3\"\r\n"
        "[HKEY_LOCAL_MACHINE\\Software\\Classes\\Unknown\\DefaultIcon]\r\n"
        "@=\"C:\\\\windows\\\\system32\\\\BLESICONS.DLL,-4\"\r\n"
        "[HKEY_LOCAL_MACHINE\\Software\\Classes\\txtfile\\DefaultIcon]\r\n"
        "@=\"C:\\\\windows\\\\system32\\\\BLESICONS.DLL,-4\"\r\n"
        "[HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\DefaultIcon]\r\n"
        "@=\"C:\\\\windows\\\\system32\\\\BLESICONS.DLL,-5\"\r\n";
    int fd;

    mkdir(BLES_WINE_C_ROOT "/windows", 0755);
    mkdir(BLES_WINE_C_ROOT "/windows/temp", 0755);
    fd = open(BLES_THEME_UNIX, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    if (write_all(fd, registry, sizeof(registry) - 1) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int apply_bles_theme(void)
{
    int rc;
    if (access(BLES_THEME_MARKER, F_OK) == 0) {
        wine_trace("[WINE:STAGE] THEME ready marker=1\n");
        fflush(stdout);
        return 0;
    }
    if (write_bles_theme_reg() < 0) {
        perror("[WINE:STAGE] THEME write-reg");
        return -1;
    }

    wine_trace("[WINE:STAGE] THEME begin import=%s\n", BLES_THEME_WIN);
    fflush(stdout);
    {
        char *const args[] = {
            "/OPT/WINE/bin/wine", "regedit", "/S", BLES_THEME_WIN, NULL
        };
        rc = run_wait(args);
    }
    wine_trace("[WINE:STAGE] THEME regedit-exit=%d\n", rc);
    fflush(stdout);
    if (rc != 0) return -1;
    if (touch_marker(BLES_THEME_MARKER) < 0) {
        perror("[WINE:STAGE] THEME marker");
        return -1;
    }
    wine_trace("[WINE:STAGE] THEME ready marker=created\n");
    fflush(stdout);
    return 0;
}

static Display *start_private_x(pid_t *server_out, int *display_out)
{
    int seed = BLES_DISPLAY_FIRST + ((int)getpid() % BLES_DISPLAY_COUNT);
    for (int attempt = 0; attempt < BLES_DISPLAY_COUNT; ++attempt) {
        int number = BLES_DISPLAY_FIRST +
            ((seed - BLES_DISPLAY_FIRST + attempt) % BLES_DISPLAY_COUNT);
        Display *probe;
        pid_t server;
        int child_status = 0;
        int server_exited = 0;
        char display_arg[16];

        select_display(number);
        probe = XOpenDisplay(display_env + strlen("DISPLAY="));
        if (probe) { XCloseDisplay(probe); continue; }
        snprintf(display_arg, sizeof(display_arg), ":%d", number);
        {
            char *const xargs[] = {"/WINEGATE/bin/XFBDEV", display_arg, "-ac",
                                   "-nolock", "-dumbSched", "-fp", "/WINEGATE/fonts/misc", NULL};
            server = start(xargs);
        }
        if (server < 0) continue;
        for (int retry = 0; retry < 300; ++retry) {
            probe = XOpenDisplay(display_arg);
            if (probe) {
                *server_out = server;
                *display_out = number;
                return probe;
            }
            if (waitpid(server, &child_status, WNOHANG) == server) {
                server_exited = 1;
                break;
            }
            usleep(100000);
        }
        if (!server_exited) {
            kill(server, SIGTERM);
            waitpid(server, NULL, 0);
        }
    }
    return NULL;
}

static tracked_window_t *tracked_slot(Window window)
{
    tracked_window_t *free_slot = NULL;
    for (unsigned i = 0; i < BLES_TRACKED_WINDOWS; ++i) {
        if (tracked[i].id == window) return &tracked[i];
        if (!tracked[i].id && !free_slot) free_slot = &tracked[i];
    }
    return free_slot;
}

static void window_title(Display *display, Window window, char *out, size_t size)
{
    char *name = NULL;
    XClassHint hint;
    if (!size) return;
    out[0] = '\0';
    if (XFetchName(display, window, &name) && name) {
        snprintf(out, size, "%s", name);
        XFree(name);
        if (out[0]) return;
    }
    memset(&hint, 0, sizeof(hint));
    if (XGetClassHint(display, window, &hint)) {
        const char *fallback = hint.res_name ? hint.res_name : hint.res_class;
        if (fallback) snprintf(out, size, "%s", fallback);
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
    }
    if (!out[0]) snprintf(out, size, "Windows application");
}

static unsigned surface_flags(Display *display, Window window,
                              const XWindowAttributes *attr)
{
    XSizeHints hints;
    long supplied = 0;
    unsigned flags = 0;
    Window transient = None;
    if (XGetWMNormalHints(display, window, &hints, &supplied) &&
        (hints.flags & PMinSize) && (hints.flags & PMaxSize) &&
        hints.min_width == hints.max_width && hints.min_height == hints.max_height)
        flags |= BLES_GFX_SURFACE_FIXED;
    if (XGetTransientForHint(display, window, &transient) && transient != None)
        flags |= BLES_GFX_SURFACE_DIALOG;
    (void)attr;
    return flags;
}

static unsigned sync_surfaces(Display *display, int control_fd,
                              unsigned instance_id)
{
    Window root = DefaultRootWindow(display), parent, returned_root;
    Window *children = NULL;
    unsigned count = 0, visible = 0;
    ++generation;
    if (!generation) ++generation;

    if (!XQueryTree(display, root, &returned_root, &parent, &children, &count))
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes attr;
        Window ignored, transient = None;
        tracked_window_t *slot;
        bles_gfx_surface_t surface;
        char title[48];
        int x, y;

        if (!XGetWindowAttributes(display, children[i], &attr)) continue;
        if (attr.map_state != IsViewable || attr.class != InputOutput ||
            attr.override_redirect || attr.width < 16 || attr.height < 16)
            continue;
        if (!XTranslateCoordinates(display, children[i], root, 0, 0,
                                   &x, &y, &ignored))
            continue;
        int screen_width = DisplayWidth(display, DefaultScreen(display));
        int screen_height = DisplayHeight(display, DefaultScreen(display));
        if (x < 0 || y < 0 || x >= screen_width || y >= screen_height) continue;
        if (attr.width > screen_width - x) attr.width = screen_width - x;
        if (attr.height > screen_height - y) attr.height = screen_height - y;
        if (attr.width < 1 || attr.height < 1) continue;

        slot = tracked_slot(children[i]);
        if (!slot) continue;
#ifdef BLES_GFX_TRACE_MOVE
        if (!slot->active)
            XSelectInput(display, children[i],
                         ButtonPressMask | ButtonReleaseMask | StructureNotifyMask);
#endif
        slot->id = children[i];
        slot->generation = generation;
        ++visible;

        memset(&surface, 0, sizeof(surface));
        surface.instance_id = instance_id;
        surface.surface_id = (uint32_t)children[i];
        if (XGetTransientForHint(display, children[i], &transient) && transient != None)
            surface.parent_id = (uint32_t)transient;
        surface.x = (uint32_t)x; surface.y = (uint32_t)y;
        surface.width = (uint32_t)attr.width; surface.height = (uint32_t)attr.height;
        surface.flags = surface_flags(display, children[i], &attr);
        surface.z_index = i;
        window_title(display, children[i], title, sizeof(title));
        snprintf(surface.title, sizeof(surface.title), "%s", title);
        if (!slot->active || slot->x != surface.x || slot->y != surface.y ||
            slot->width != surface.width || slot->height != surface.height ||
            slot->parent_id != surface.parent_id || slot->flags != surface.flags ||
            slot->z_index != surface.z_index ||
            strcmp(slot->title, surface.title)) {
#ifdef BLES_GFX_TRACE_MOVE
            if (slot->active && (slot->x != surface.x || slot->y != surface.y)) {
                wine_trace("[WINE:XSURFACE] id=%lu old=%u,%u new=%u,%u\n",
                       (unsigned long)children[i],slot->x,slot->y,
                       surface.x,surface.y);
                fflush(stdout);
            }
#endif
            if (ioctl(control_fd, BLES_GFX_SURFACE_UPSERT, &surface) == 0) {
                if (!first_surface_reported) {
                    first_surface_reported = 1;
                    unsigned long ready_ms = elapsed_ms();
                    wine_trace("[WINE:TIME] first-surface ms=%lu title=%s\n",
                           ready_ms, surface.title);
                    wine_trace("[WINE:PERF] ready total_ms=%lu x_ms=%lu prefix_ms=%lu "
                           "theme_ms=%lu target_begin_ms=%lu spawn_ms=%lu "
                           "spawn_to_surface_ms=%lu\n",
                           ready_ms, perf_x_ready_ms, perf_prefix_ready_ms,
                           perf_theme_ready_ms, perf_target_begin_ms,
                           perf_target_spawn_ms,
                           ready_ms >= perf_target_spawn_ms
                               ? ready_ms - perf_target_spawn_ms : 0UL);
                    if (control_fd >= 0 && perf_vm_baseline_valid) {
                        bles_gfx_wine_vm_perf_t vmperf;
                        if (wine_vm_perf_snapshot(control_fd, &vmperf) == 0)
                            wine_vm_perf_print_delta("spawn-to-surface",
                                                     &perf_vm_baseline, &vmperf);
                        else
                            wine_trace("[WINE:VM-PERF] phase=spawn-to-surface unavailable errno=%d\n",
                                   errno);
                    }
                    wine_cache_perf_print(control_fd,"first-surface");
                    fflush(stdout);
                }
                slot->active = 1;
                slot->x = surface.x; slot->y = surface.y;
                slot->width = surface.width; slot->height = surface.height;
                slot->parent_id = surface.parent_id; slot->flags = surface.flags;
                slot->z_index = surface.z_index;
                snprintf(slot->title, sizeof(slot->title), "%s", surface.title);
            }
        }
    }
    if (children) XFree(children);

    for (unsigned i = 0; i < BLES_TRACKED_WINDOWS; ++i) {
        if (tracked[i].id && tracked[i].generation != generation) {
            bles_gfx_surface_t surface;
            memset(&surface, 0, sizeof(surface));
            surface.instance_id = instance_id;
            surface.surface_id = (uint32_t)tracked[i].id;
            (void)ioctl(control_fd, BLES_GFX_SURFACE_REMOVE, &surface);
            tracked[i].id = None;
            tracked[i].generation = 0;
            tracked[i].active = 0;
        }
    }
    return visible;
}

static void request_close(Display *display, Window window)
{
    Atom wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    Atom *protocols = NULL;
    int count = 0, supports_delete = 0;

    if (XGetWMProtocols(display, window, &protocols, &count)) {
        for (int i = 0; i < count; ++i)
            if (protocols[i] == wm_delete) supports_delete = 1;
        if (protocols) XFree(protocols);
    }
    if (supports_delete) {
        XEvent event;
        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = wm_protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = (long)wm_delete;
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(display, window, False, NoEventMask, &event);
    } else {
        XDestroyWindow(display, window);
    }
    XFlush(display);
}

/* Wine expects a window manager to acknowledge map requests with WM_STATE.
 * Without one, moving an X window changes its pixels but leaves Win32's
 * window rectangle at its old coordinates, breaking hit testing. */
static void set_wm_state(Display *display, Window window, long state)
{
    long value[2] = {state, None};
    Atom atom = XInternAtom(display, "WM_STATE", False);
    XChangeProperty(display, window, atom, atom, 32, PropModeReplace,
                    (unsigned char *)value, 2);
}

/* Wine requests ICCCM WM_TAKE_FOCUS and deliberately ignores bare FocusIn.
 * Deliver the protocol so Windows activates the dialog and its input queue. */
static void focus_x_window(Display *display, Window window)
{
    Atom *protocols = NULL;
    int count = 0, takes_focus = 0;
    Atom take_focus = XInternAtom(display, "WM_TAKE_FOCUS", False);
    if (XGetWMProtocols(display, window, &protocols, &count)) {
        for (int i = 0; i < count; ++i)
            if (protocols[i] == take_focus) takes_focus = 1;
        XFree(protocols);
    }
    if (takes_focus) {
        XEvent event;
        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
        event.xclient.format = 32;
        event.xclient.data.l[0] = take_focus;
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(display, window, False, NoEventMask, &event);
    } else {
        XWMHints *hints = XGetWMHints(display, window);
        int accepts_input = !hints || !(hints->flags & InputHint) || hints->input;
        if (hints) XFree(hints);
        if (accepts_input) XSetInputFocus(display, window, RevertToParent, CurrentTime);
    }
}

static void process_wm_event(Display *display, const XEvent *event)
{
    if (event->type == MapRequest) {
        XMapWindow(display, event->xmaprequest.window);
        set_wm_state(display, event->xmaprequest.window, NormalState);
        focus_x_window(display, event->xmaprequest.window);
    } else if (event->type == ConfigureRequest) {
        XWindowChanges changes;
        const XConfigureRequestEvent *request = &event->xconfigurerequest;
        memset(&changes, 0, sizeof(changes));
        changes.x = request->x; changes.y = request->y;
        changes.width = request->width; changes.height = request->height;
        changes.border_width = request->border_width;
        changes.sibling = request->above; changes.stack_mode = request->detail;
        XConfigureWindow(display, request->window, request->value_mask, &changes);
    } else if (event->type == UnmapNotify &&
               event->xunmap.event == DefaultRootWindow(display)) {
        Atom atom = XInternAtom(display, "WM_STATE", False), type;
        int format;
        unsigned long count, remaining;
        unsigned char *property = NULL;
        int iconic = XGetWindowProperty(display, event->xunmap.window, atom,
                                       0, 2, False, atom, &type, &format,
                                       &count, &remaining, &property) == Success &&
                     type == atom && format == 32 && count >= 1 &&
                     ((long *)property)[0] == IconicState;
        if (property) XFree(property);
        if (!iconic)
            set_wm_state(display, event->xunmap.window, WithdrawnState);
    } else if (event->type == ClientMessage &&
               event->xclient.message_type ==
                   XInternAtom(display, "WM_CHANGE_STATE", False) &&
               event->xclient.format == 32 &&
               event->xclient.data.l[0] == IconicState) {
        XUnmapWindow(display, event->xclient.window);
        set_wm_state(display, event->xclient.window, IconicState);
    }
}

static void pump_surface_events(Display *display, int control_fd,
                                unsigned instance_id)
{
    for (unsigned i = 0; i < 32; ++i) {
        bles_gfx_surface_event_t event;
        memset(&event, 0, sizeof(event));
        event.instance_id = instance_id;
        if (ioctl(control_fd, BLES_GFX_SURFACE_EVENT, &event) <= 0) break;
        if (!event.surface_id) continue;
        if (event.type == BLES_GFX_SURFACE_CLOSE) {
            request_close(display, (Window)event.surface_id);
        } else if (event.type == BLES_GFX_SURFACE_RESIZE) {
            if (event.width && event.height)
                XResizeWindow(display, (Window)event.surface_id,
                              event.width, event.height);
        } else if (event.type == BLES_GFX_SURFACE_MOVE) {
            XMoveWindow(display, (Window)event.surface_id,event.x,event.y);
#ifdef BLES_GFX_TRACE_MOVE
            XSync(display, False);
            {
                XWindowAttributes attr;
                Window child;
                int x = -1, y = -1;
                if (XGetWindowAttributes(display, (Window)event.surface_id, &attr))
                    XTranslateCoordinates(display, (Window)event.surface_id,
                                          DefaultRootWindow(display), 0, 0,
                                          &x, &y, &child);
                wine_trace("[WINE:MOVE] id=%lu requested=%d,%d actual=%d,%d\n",
                       (unsigned long)event.surface_id,event.x,event.y,x,y);
                fflush(stdout);
            }
#endif
        } else if (event.type == BLES_GFX_SURFACE_FOCUS) {
            XRaiseWindow(display, (Window)event.surface_id);
            focus_x_window(display, (Window)event.surface_id);
        }
    }
    XFlush(display);
}

/* Quake II's packaged default.cfg selects fullscreen DirectDraw. Start its
 * first session with the tested windowed software renderer; once the game
 * saves config.cfg, the user's video preferences take precedence. */
static int quake2_first_video_setup(const char *application, const char *directory)
{
    char path[4096], magic[4];
    struct stat st;
    const char *name = application ? strrchr(application, '/') : NULL;
    name = name ? name + 1 : application;
    if (!name || strcasecmp(name, "quake2.exe") || !directory) return 0;
    if (snprintf(path, sizeof(path), "%s/baseq2/config.cfg", directory) >= (int)sizeof(path))
        return 0;
    if (!stat(path, &st) || errno != ENOENT) return 0;
    if (snprintf(path, sizeof(path), "%s/baseq2/autoexec.cfg", directory) >= (int)sizeof(path))
        return 0;
    if (!stat(path, &st) || errno != ENOENT) return 0;
    if (snprintf(path, sizeof(path), "%s/baseq2/pak0.pak", directory) >= (int)sizeof(path))
        return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    int valid = fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
                !memcmp(magic, "PACK", sizeof(magic));
    fclose(file);
    return valid;
}

int main(int argc, char **argv)
{
    pid_t server = -1, app = -1;
    Display *display;
    int display_number = -1, status = 0;
    int control_fd = -1;
    unsigned instance_id;
    unsigned visible = 0;
    unsigned long empty_since_ms = 0, last_sync_ms = 0;
    int surfaces_synced = 0;
    int structure_changed = 0;
    int app_done = 0;
    int warmup;
    int full_warmup;
    char application_path[4096], application_directory[4096], optical_target[4096];
    const char *application_cwd = NULL;
    char virtual_desktop[64] = {0};

    diagnostics = getenv("BLES_WINE_DIAGNOSTICS") &&
                  !strcmp(getenv("BLES_WINE_DIAGNOSTICS"), "1");
    while (argc > 2) {
        if (!strcmp(argv[1], "--diagnostics")) diagnostics = 1;
        else if (!strcmp(argv[1], "--software-gl")) {
            for (unsigned i = 0; environment[i]; ++i)
                if (!strncmp(environment[i], "BLES_WINE_GL=", 13))
                    environment[i] = "BLES_WINE_GL=software";
        } else if (!strncmp(argv[1], "--desktop=", 10)) {
            unsigned width, height; char extra;
            if (sscanf(argv[1]+10, "%ux%u%c", &width, &height, &extra) != 2 ||
                width < 320 || height < 200 || width > 2048 || height > 2048) {
                fprintf(stderr, "wine-launch: escritorio virtual invalido (320x200 a 2048x2048)\n");
                return 2;
            }
            snprintf(virtual_desktop, sizeof(virtual_desktop), "/desktop=BlesKernOS,%ux%u", width, height);
        } else break;
        ++argv;
        --argc;
    }
    if (diagnostics && getenv("WINEDEBUG")) {
        snprintf(wine_debug_env, sizeof(wine_debug_env), "WINEDEBUG=%s", getenv("WINEDEBUG"));
        for (unsigned i = 0; environment[i]; ++i)
            if (!strncmp(environment[i], "WINEDEBUG=", 10)) environment[i] = wine_debug_env;
    }
    if (argc != 2 || !argv[1][0]) {
        fprintf(stderr, "uso: wine-launch [--diagnostics] [--software-gl] [--desktop=800x600] <programa.exe|script.bat|winecfg>\n");
        return 2;
    }
    warmup = !strcmp(argv[1], "--warmup") || !strcmp(argv[1], "--warmup-full");
    full_warmup = !strcmp(argv[1], "--warmup-full");
    /* CD autorun installers and batch files use paths relative to their own
     * directory. Resolve the argument before changing the child's cwd; keep
     * Wine services and the launcher in their original working directory. */
    if (!warmup && strchr(argv[1], '/') && !strchr(argv[1], ':')) {
        if (argv[1][0] == '/') {
            if (snprintf(application_path, sizeof(application_path), "%s", argv[1]) >=
                (int)sizeof(application_path)) return 2;
        } else {
            if (!getcwd(application_directory, sizeof(application_directory))) {
                perror("[WINE] current directory");
                return 2;
            }
            if (snprintf(application_path, sizeof(application_path), "%s/%s",
                         application_directory, argv[1]) >= (int)sizeof(application_path)) return 2;
        }
        strcpy(application_directory, application_path);
        char *slash = strrchr(application_directory, '/');
        if (slash == application_directory) slash[1] = 0;
        else *slash = 0;
        application_cwd = application_directory;
        argv[1] = application_path;
    }
    if (!warmup && quake2_international_installer(argv[1])) {
        for (unsigned i = 0; environment[i]; ++i)
            if (!strncmp(environment[i], "LC_ALL=", 7))
                environment[i] = "LC_ALL=en_GB.UTF-8";
        wine_trace("[WINE:COMPAT] Quake II international installer locale=en-GB\n");
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &launch_started);
    wine_trace("[WINE:DESKTOP] rootless start %s\n", argv[1]);
    fflush(stdout);
    mkdir("/tmp", 01777);
    chmod("/tmp", 01777);
    mkdir("/tmp/.X11-unix", 01777);

    display = start_private_x(&server, &display_number);
    if (!display) return 4;
    XSetErrorHandler(ignore_x_error);
    XSelectInput(display, DefaultRootWindow(display),
                 SubstructureNotifyMask | SubstructureRedirectMask);
    XSync(display, False);
    window_manager_display = display;
    {
        int event_base, error_base;
        if (XFixesQueryExtension(display, &event_base, &error_base)) {
            XFixesHideCursor(display, DefaultRootWindow(display));
            XFlush(display);
        }
    }
    instance_id = (unsigned)server;
    wine_trace("[WINE:DESKTOP] TinyX rootless ready on :%d pid=%u\n",
           display_number, instance_id);
    perf_x_ready_ms = elapsed_ms();
    wine_trace("[WINE:TIME] x-ready ms=%lu\n", perf_x_ready_ms);
    fflush(stdout);

    control_fd = open("/dev/blesgfx", O_RDWR);
    if (control_fd < 0) perror("/dev/blesgfx control");

    /* Prefix bootstrap and theme import are one serialized transaction.
     * Previously two launches could both decide the marker was missing;
     * one then removed /WINEGATE/prefix while the other was in wineboot. */
    {
        int prefix_lock_fd = wine_prefix_lock();
        int prefix_ok;
        if (prefix_lock_fd < 0) {
            fprintf(stderr, "[WINE:STAGE] ABORT reason=prefix-lock\n");
            status = 7;
            app = -1;
            goto cleanup;
        }
        prefix_ok = ensure_wine_prefix();
        perf_prefix_ready_ms = elapsed_ms();
        if (prefix_ok == 0 && apply_bles_theme() < 0) {
            /* The appearance import is best effort, as before. */
            fprintf(stderr,
                    "[WINE:STAGE] THEME warning=import-failed continue=1\n");
        }
        /* Closing the descriptor releases the process-owned POSIX lock. */
        close(prefix_lock_fd);
        if (prefix_ok < 0) {
            fprintf(stderr, "[WINE:STAGE] ABORT reason=prefix-init\n");
            status = 7;
            app = -1;
            goto cleanup;
        }
    }
    wine_trace("[WINE:TIME] prefix-ready ms=%lu delta_ms=%lu\n",
           perf_prefix_ready_ms,
           perf_prefix_ready_ms >= perf_x_ready_ms
               ? perf_prefix_ready_ms - perf_x_ready_ms : 0UL);
    fflush(stdout);
    perf_theme_ready_ms = elapsed_ms();
    wine_trace("[WINE:TIME] theme-ready ms=%lu delta_ms=%lu\n",
           perf_theme_ready_ms,
           perf_theme_ready_ms >= perf_prefix_ready_ms
               ? perf_theme_ready_ms - perf_prefix_ready_ms : 0UL);
    fflush(stdout);
    if (warmup) {
        char *const persistent[] = {"/OPT/WINE/bin/wineserver", "-p", NULL};
        char *const prime[] = {"/OPT/WINE/bin/wine", "Z:\\WINEGATE\\bin\\wine-ready.exe", NULL};
        char *const probe[] = {"/OPT/WINE/bin/wine", "reg", "query",
                               "HKCU\\Control Panel\\Colors", NULL};
        bles_gfx_wine_vm_perf_t vm_before, vm_after;
        int vm_before_valid = wine_vm_perf_snapshot(control_fd, &vm_before) == 0;
        unsigned long server_start_ms = elapsed_ms();
        int keep;
        if (!vm_before_valid) {
            wine_trace("[WINE:VM-PERF] phase=warmup-baseline unavailable errno=%d\n",
                   errno);
            fflush(stdout);
        }
        keep = run_wait(persistent);
        unsigned long server_ready_ms = elapsed_ms();
        wine_trace("[WINE:TIME] warmup-wineserver ms=%lu delta_ms=%lu status=%d\n",
               server_ready_ms,
               server_ready_ms >= server_start_ms
                   ? server_ready_ms - server_start_ms : 0UL, keep);
        if (vm_before_valid && wine_vm_perf_snapshot(control_fd, &vm_after) == 0)
            wine_vm_perf_print_delta("warmup-wineserver", &vm_before, &vm_after);
        fflush(stdout);

        /* `wineserver -p` alone is cheap, but the first real EXE then pays
         * Wine's client/services startup cost. The old `reg query` did prime
         * that state, but it spent ~106 seconds doing far more work than we
         * need. Run a small GUI client that checks user32/GDI instead.
         * cmd /c exit also starts conhost, whose initialization can stall
         * indefinitely on this ABI even though ordinary GUI EXEs work.
         *
         * The kernel waits for the process-scoped boot completion ioctl before
         * handing the framebuffer from the splash to Vision. Keep the
         * private TinyX display alive afterwards: Wine helper processes created
         * during the prime may still own X connections, and killing their X
         * server immediately would throw away part of the warm state we just
         * paid to build. --warmup-full retains the old registry probe as a
         * diagnostic comparison. */
        if (!full_warmup) {
            unsigned long prime_start_ms;
            unsigned long prime_ready_ms;
            int prime_status;

            vm_before_valid = wine_vm_perf_snapshot(control_fd, &vm_before) == 0;
            if (!vm_before_valid) {
                wine_trace("[WINE:VM-PERF] phase=warmup-prime-baseline unavailable errno=%d\n",
                       errno);
                fflush(stdout);
            }
            prime_start_ms = elapsed_ms();
            prime_status = keep == 0 ? run_wait(prime) : -1;
            prime_ready_ms = elapsed_ms();
            wine_trace("[WINE:TIME] warmup-prime ms=%lu delta_ms=%lu status=%d\n",
                   prime_ready_ms,
                   prime_ready_ms >= prime_start_ms
                       ? prime_ready_ms - prime_start_ms : 0UL,
                   prime_status);
            if (vm_before_valid && wine_vm_perf_snapshot(control_fd, &vm_after) == 0)
                wine_vm_perf_print_delta("warmup-prime", &vm_before, &vm_after);
            else if (vm_before_valid)
                wine_trace("[WINE:VM-PERF] phase=warmup-prime unavailable errno=%d\n",
                       errno);

            wine_trace("[WINE:WARMUP] mode=core server=%d prime=%d probe=skipped elapsed_ms=%lu\n",
                   keep, prime_status, prime_ready_ms);
            wine_trace("[WINE:PERF] warmup-ready total_ms=%lu x_ms=%lu "
                   "prefix_ms=%lu theme_ms=%lu wineserver_ms=%lu "
                   "prime_ms=%lu probe_ms=0\n",
                   prime_ready_ms, perf_x_ready_ms, perf_prefix_ready_ms,
                   perf_theme_ready_ms,
                   server_ready_ms >= server_start_ms
                       ? server_ready_ms - server_start_ms : 0UL,
                   prime_ready_ms >= prime_start_ms
                       ? prime_ready_ms - prime_start_ms : 0UL);
            if (prime_status != 0)
                fprintf(stderr, "[WINE:WARMUP] error=core-prime-failed status=%d\n",
                       prime_status);
            fflush(stdout);
            status = keep == 0 && prime_status == 0 ? 0 : 8;
            app = -2;
            if (status) goto cleanup;
        } else {
            unsigned long probe_start_ms;
            unsigned long probe_ready_ms;
            int probe_status;

            vm_before_valid = wine_vm_perf_snapshot(control_fd, &vm_before) == 0;
            if (!vm_before_valid) {
                wine_trace("[WINE:VM-PERF] phase=warmup-probe-baseline unavailable errno=%d\n",
                       errno);
                fflush(stdout);
            }
            probe_start_ms = elapsed_ms();
            probe_status = keep == 0 ? run_wait(probe) : -1;
            probe_ready_ms = elapsed_ms();
            wine_trace("[WINE:TIME] warmup-probe ms=%lu delta_ms=%lu status=%d\n",
                   probe_ready_ms,
                   probe_ready_ms >= probe_start_ms
                       ? probe_ready_ms - probe_start_ms : 0UL, probe_status);
            if (vm_before_valid && wine_vm_perf_snapshot(control_fd, &vm_after) == 0)
                wine_vm_perf_print_delta("warmup-probe", &vm_before, &vm_after);
            else if (vm_before_valid)
                wine_trace("[WINE:VM-PERF] phase=warmup-probe unavailable errno=%d\n",
                       errno);
            wine_trace("[WINE:WARMUP] mode=full server=%d probe=%d elapsed_ms=%lu\n",
                   keep, probe_status, probe_ready_ms);
            wine_trace("[WINE:PERF] warmup-ready total_ms=%lu x_ms=%lu "
                   "prefix_ms=%lu theme_ms=%lu wineserver_ms=%lu "
                   "prime_ms=0 probe_ms=%lu\n",
                   probe_ready_ms, perf_x_ready_ms, perf_prefix_ready_ms,
                   perf_theme_ready_ms,
                   server_ready_ms >= server_start_ms
                       ? server_ready_ms - server_start_ms : 0UL,
                   probe_ready_ms >= probe_start_ms
                       ? probe_ready_ms - probe_start_ms : 0UL);
            fflush(stdout);
            status = keep == 0 && probe_status == 0 ? 0 : 8;
            app = -2;
            if (status) goto cleanup;
        }

        wine_cache_perf_print(control_fd,"warmup-ready");
        fflush(stdout);
        wine_boot_report(control_fd,status);

        /* Keep the hidden rootless X server alive after reporting readiness.
         * The kernel can now release the desktop; this preserves the
         * Wine services/helpers initialized above instead of forcing the first
         * application to recreate them after cleanup tears X down. */
        for (;;) {
            int child_status;
            if (waitpid(server, &child_status, WNOHANG) == server) {
                server = -1;
                status = 9;
                break;
            }
            for (unsigned i = 0; i < 64 && XPending(display); ++i) {
                XEvent event;
                XNextEvent(display, &event);
                process_wm_event(display, &event);
            }
            XFlush(display);
            usleep(100000);
        }
        goto cleanup;
    }
    int optical_path=optical_windows_path(argv[1],optical_target,sizeof(optical_target));
    if(optical_path<0) {status=5;goto cleanup;}
    if(optical_path>0)argv[1]=optical_target;
    wine_trace("[WINE:STAGE] TARGET begin path=%s kind=%s\n", argv[1],
           ends_with(argv[1], ".bat") ? "bat" : "exe");
    perf_target_begin_ms = elapsed_ms();
    wine_trace("[WINE:TIME] target-begin ms=%lu\n", perf_target_begin_ms);
    fflush(stdout);
    perf_vm_baseline_valid =
        wine_vm_perf_snapshot(control_fd, &perf_vm_baseline) == 0;
    if (!perf_vm_baseline_valid) {
        wine_trace("[WINE:VM-PERF] phase=spawn-baseline unavailable errno=%d\n",
               errno);
        fflush(stdout);
    }
    {
        char *const batargs[] = {"/OPT/WINE/bin/wine", "cmd", "/c", argv[1], NULL};
        char *const exeargs[] = {"/OPT/WINE/bin/wine", argv[1], NULL};
        char *const desktopargs[] = {"/OPT/WINE/bin/wine", "explorer", virtual_desktop, argv[1], NULL};
        char *const first_video_args[] = {"/OPT/WINE/bin/wine", argv[1],
            "+set", "vid_fullscreen", "0", "+set", "vid_ref", "soft", NULL};
        app = start_at(virtual_desktop[0] ? desktopargs : (ends_with(argv[1], ".bat") || ends_with(argv[1], ".cmd"))
                       ? batargs : quake2_first_video_setup(argv[1], application_cwd)
                       ? first_video_args : exeargs, application_cwd);
    }
    if (app < 0) {
        perror("[WINE:STAGE] TARGET spawn");
        status = 5;
    } else {
        perf_target_spawn_ms = elapsed_ms();
        wine_trace("[WINE:STAGE] TARGET spawned pid=%d\n", (int)app);
        wine_trace("[WINE:TIME] target-spawned ms=%lu delta_ms=%lu\n",
               perf_target_spawn_ms,
               perf_target_spawn_ms >= perf_target_begin_ms
                   ? perf_target_spawn_ms - perf_target_begin_ms : 0UL);
        fflush(stdout);
        for (;;) {
            pid_t result;
            unsigned long now_ms = elapsed_ms();
            for (unsigned i = 0; i < 64 && XPending(display); ++i) {
                XEvent event;
                XNextEvent(display, &event);
                process_wm_event(display, &event);
                if (event.type == CreateNotify || event.type == DestroyNotify ||
                    event.type == MapNotify || event.type == UnmapNotify ||
                    event.type == ConfigureNotify || event.type == ReparentNotify ||
                    event.type == MapRequest || event.type == ConfigureRequest)
                    structure_changed = 1;
#ifdef BLES_GFX_TRACE_MOVE
                if (event.type == ButtonPress || event.type == ButtonRelease) {
                    wine_trace("[WINE:XBUTTON] id=%lu type=%d root=%d,%d local=%d,%d\n",
                           (unsigned long)event.xbutton.window,event.type,
                           event.xbutton.x_root,event.xbutton.y_root,
                           event.xbutton.x,event.xbutton.y);
                    fflush(stdout);
                }
#endif
            }
            if (!app_done) {
                result = waitpid(app, &status, WNOHANG);
                if (result == app) {
                    app_done = 1;
                    if (!WIFEXITED(status) || WEXITSTATUS(status))
                        fprintf(stderr, "[WINE] application failed raw=%d\n", status);
                    wine_trace("[WINE:STAGE] TARGET exited raw=%d code=%d signal=%d\n",
                           status,
                           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                    fflush(stdout);
                } else if (result < 0) {
                    status = 6;
                    app_done = 1;
                    perror("[WINE:STAGE] TARGET waitpid");
                }
            }
            if (control_fd >= 0 &&
                (!surfaces_synced || structure_changed ||
                 now_ms - last_sync_ms >= 1000UL)) {
                visible = sync_surfaces(display, control_fd, instance_id);
                last_sync_ms = now_ms;
                surfaces_synced = 1;
                structure_changed = 0;
            }
            if (control_fd >= 0)
                pump_surface_events(display, control_fd, instance_id);
#ifdef BLES_GFX_TRACE_MOVE
            {
                static int previous_x = -1, previous_y = -1;
                Window root_return, child_return;
                int root_x, root_y, window_x, window_y;
                unsigned mask;
                if (XQueryPointer(display, DefaultRootWindow(display),
                                  &root_return, &child_return,
                                  &root_x, &root_y, &window_x, &window_y,
                                  &mask) &&
                    (root_x != previous_x || root_y != previous_y)) {
                    previous_x = root_x;
                    previous_y = root_y;
                    wine_trace("[WINE:XPOINTER] root=%d,%d child=%lu mask=%u\n",
                           root_x, root_y, (unsigned long)child_return, mask);
                    fflush(stdout);
                }
            }
#endif

            if (app_done) {
                if (visible) empty_since_ms = 0;
                else if (!empty_since_ms) empty_since_ms = now_ms;
                else if (now_ms - empty_since_ms >= BLES_EMPTY_GRACE_MS)
                    break;
            }
            usleep(20000);
        }
    }

cleanup:
    if(warmup)wine_boot_report(control_fd,status?status:8);
    if (control_fd >= 0) {
        /* One last pass removes any stale host surfaces before XFBDEV exits. */
        ++generation;
        for (unsigned i = 0; i < BLES_TRACKED_WINDOWS; ++i) {
            if (tracked[i].id) {
                bles_gfx_surface_t surface;
                memset(&surface, 0, sizeof(surface));
                surface.instance_id = instance_id;
                surface.surface_id = (uint32_t)tracked[i].id;
                (void)ioctl(control_fd, BLES_GFX_SURFACE_REMOVE, &surface);
                tracked[i].id = None;
            }
        }
        close(control_fd);
    }
    XCloseDisplay(display);
    if (server > 0) {
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
    }
    if (warmup) return status;
    if (app < 0) return status == 7 ? 7 : 5;
    wine_trace("[WINE:DESKTOP] rootless app status=%d display=:%d\n",
           status, display_number);
    fflush(stdout);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 6;
}
