#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#ifdef WINEBOOT_PROFILE_PHASES
static struct timespec profile_origin;
static void profile_stamp(const char *phase) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return;
    long sec = now.tv_sec - profile_origin.tv_sec;
    long nsec = now.tv_nsec - profile_origin.tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    printf("[W3P:USER] phase=%s elapsed_ms=%ld\n", phase,
           sec * 1000L + nsec / 1000000L);
    fflush(stdout);
}
#else
#define profile_stamp(phase) do { (void)(phase); } while (0)
#endif

static int check_saved_marker(void) {
    const char *path = "/WINEGATE/prefix-w3/user.reg";
    const char *marker = "W3-PERSIST-v1";
    char buffer[4096 + 32];
    FILE *file = fopen(path, "rb");
    if (!file) { perror("[WINE:BOOT] open saved user.reg"); return 1; }
    size_t kept = 0, size;
    int found = 0;
    while ((size = fread(buffer + kept, 1, 4096, file)) != 0) {
        size += kept;
        buffer[size] = 0;
        if (strstr(buffer, marker)) { found = 1; break; }
        kept = strlen(marker) - 1;
        if (kept > size) kept = size;
        memmove(buffer, buffer + size - kept, kept);
    }
    int failed = ferror(file);
    fclose(file);
    if (!found || failed) {
        puts("[WINE:BOOT] persisted marker missing from saved user.reg");
        return 1;
    }
    return 0;
}

static int check_hives(void) {
    const char *registry[] = { "/WINEGATE/prefix-w3/system.reg",
        "/WINEGATE/prefix-w3/user.reg", "/WINEGATE/prefix-w3/userdef.reg" };
    int failed = 0;
    for (unsigned i = 0; i < sizeof(registry) / sizeof(registry[0]); ++i) {
        struct stat st;
        if (stat(registry[i], &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            printf("[WINE:BOOT] missing/empty registry: %s\n", registry[i]);
            failed = 1;
        }
    }
    return failed;
}

static int run(char **args, char **env) {
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { perror("wineboot validation fork"); return 1; }
    if (!child) {
        execve(args[0], args, env);
        perror(args[0]);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("wineboot validation waitpid");
        return 1;
    }
    printf("[WINE:BOOT] child=%s status=%d\n", args[0], status);
    fflush(stdout);
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

int main(void) {
#ifdef WINEBOOT_PROFILE_PHASES
    clock_gettime(CLOCK_MONOTONIC, &profile_origin);
#endif
    profile_stamp("launcher_start");
    char *env[] = {
        "LD_LIBRARY_PATH=/WINEGATE/lib",
        "FONTCONFIG_FILE=/WINEGATE/etc/fonts/fonts.conf",
        "FONTCONFIG_PATH=/WINEGATE/etc/fonts",
        "WINEDLLPATH=/OPT/WINE/lib/wine",
        "WINEPREFIX=/WINEGATE/prefix-w3",
        "HOME=/WINEGATE",
        "PATH=/OPT/WINE/bin:/WINEGATE/bin",
        "USER=root",
        "LOGNAME=root",
        /* W3-65: BlesKernOS does not ship /etc/localtime yet.  Without an
           explicit POSIX TZ, glibc retries that path during Wine setup and
           SetupAPI spends the end of the bootstrap in repeated ENOENT probes.
           Argentina is UTC-3 year-round; ART3 expresses that without tzdata. */
        "TZ=ART3",
        "TMPDIR=/tmp",
        "TEMP=/tmp",
        "TMP=/tmp",
        /* Keep detailed module/service traces behind the diagnostic switch.
           Serial output from every DLL load slows the normal guest boot. */
#if defined(WINEBOOT_DIAG_REUSE) || defined(WINEBOOT_TRACE_MODULES)
        "WINEDEBUG=+timestamp,+pid,err+all,trace+module,trace+loaddll,trace+service,trace+wineboot",
#else
        "WINEDEBUG=+timestamp,+pid,err+all,warn+module,warn+winstation,trace+wineboot",
#endif
        NULL
    };

    /* A Linux host normally provides /tmp before Wine starts.  The W3 runner
       stages that host directory in its disposable image; keep this fallback
       for direct launches, and always apply the Linux-visible sticky mode in
       the guest.  wineboot itself must still create the fresh WINEPREFIX. */
    if (mkdir("/tmp", 01777) < 0 && errno != EEXIST) {
        printf("[WINE:BOOT] mkdir /tmp errno=%d\n", errno);
        return 1;
    }
    if (chmod("/tmp", 01777) < 0) {
        printf("[WINE:BOOT] chmod /tmp errno=%d\n", errno);
        return 1;
    }
    char *registry_gate[] = { "/OPT/WINE/bin/wine",
        "Z:\\WINEGATE\\BIN\\WINE-REG-GATE.EXE", NULL, NULL };
#ifdef WINEBOOT_REUSE_PREFIX
    /* Fail before running init if the previous boot did not persist its
       hives. Query the marker without recreating it on this boot. */
    if (check_hives()) return 1;
    puts("[WINE:BOOT] reusing persisted prefix after cold boot");
    registry_gate[2] = "--reuse";
#elif !defined(WINEBOOT_DIAG_REUSE)
    struct stat existing;
    if (!stat("/WINEGATE/prefix-w3", &existing) || errno != ENOENT) {
        puts("[WINE:BOOT] fresh-prefix test requires an absent prefix");
        return 1;
    }
#endif
#ifdef WINEBOOT_NORMAL_START
    puts("[WINE:BOOT] normal startup using persisted prefix");
#else
    /* Wine 11.0's ntdll runs wineboot.exe --init before the first PE process
       of a fresh prefix. Launching wineboot.exe ourselves repeats the whole
       service startup after that automatic init. */
    puts("[WINE:BOOT] upstream automatic wineboot --init on first Wine process");
#endif

    /* One PE process checks both installed keys and writes/reads the marker
       through the registry API. Repeated wine reg.exe launches rebuilt DLL
       and font state four times and dominated the validation time. */
    profile_stamp("wine_start");
    int failed = run(registry_gate, env);
    profile_stamp("wine_done");
    /* SIGTERM's upstream handler flushes the registry. Wait for the server
       lock to be released before the host harness terminates QEMU. */
    char *stop[] = { "/OPT/WINE/bin/wineserver", "-k15", NULL };
    char *wait[] = { "/OPT/WINE/bin/wineserver", "-w", NULL };
    profile_stamp("wineserver_stop_start");
    failed |= run(stop, env);
    failed |= run(wait, env);
    profile_stamp("wineserver_stopped");
    failed |= check_hives();
    failed |= check_saved_marker();
    profile_stamp("hives_checked");
    if (!failed) {
#ifdef WINEBOOT_REUSE_PREFIX
#ifdef WINEBOOT_NORMAL_START
        puts("[WINE:BOOT] cold reboot, persisted registry and normal startup PASS");
#else
        puts("[WINE:BOOT] cold reboot, persisted registry and reused init PASS");
#endif
#else
        puts("[WINE:BOOT] init, registry queries and hive files PASS");
#endif
    }
    return failed;
}
