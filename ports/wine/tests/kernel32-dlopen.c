#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int probe_file(const char *tag, const char *path)
{
    struct stat st;
    unsigned char magic[4] = {0};
    int fd;
    ssize_t n;

    errno = 0;
    if (stat(path, &st) < 0) {
        printf("[K32DLOPEN] %s stat FAIL errno=%d (%s) path=%s\n",
               tag, errno, strerror(errno), path);
        return 1;
    }
    printf("[K32DLOPEN] %s stat PASS size=%ld path=%s\n",
           tag, (long)st.st_size, path);

    errno = 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[K32DLOPEN] %s open FAIL errno=%d (%s)\n",
               tag, errno, strerror(errno));
        return 1;
    }
    n = read(fd, magic, sizeof(magic));
    close(fd);
    if (n != (ssize_t)sizeof(magic)) {
        printf("[K32DLOPEN] %s read FAIL n=%ld errno=%d (%s)\n",
               tag, (long)n, errno, strerror(errno));
        return 1;
    }
    printf("[K32DLOPEN] %s open/read PASS magic=%02x%02x%02x%02x\n",
           tag, magic[0], magic[1], magic[2], magic[3]);
    return 0;
}

int main(void)
{
    const char *pe = "/OPT/WINE/lib/wine/i386-windows/kernel32.dll";
    const char *so = "/OPT/WINE/lib/wine/i386-unix/kernel32.dll.so";
    void *handle;
    const char *err;

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("[K32DLOPEN] begin");

    if (probe_file("PE", pe)) return 10;
    if (probe_file("SO", so)) return 11;

    dlerror();
    handle = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        err = dlerror();
        printf("[K32DLOPEN] dlopen NOW FAIL error=%s\n",
               err ? err : "(null)");
        return 12;
    }
    printf("[K32DLOPEN] dlopen NOW PASS handle=%p\n", handle);

    dlerror();
    if (!dlsym(handle, "__wine_spec_nt_header")) {
        err = dlerror();
        printf("[K32DLOPEN] dlsym __wine_spec_nt_header FAIL error=%s\n",
               err ? err : "(null)");
        dlclose(handle);
        return 13;
    }
    puts("[K32DLOPEN] dlsym __wine_spec_nt_header PASS");

    if (dlclose(handle) != 0) {
        err = dlerror();
        printf("[K32DLOPEN] dlclose FAIL error=%s\n",
               err ? err : "(null)");
        return 14;
    }

    puts("[K32DLOPEN] kernel32.dll.so dlopen PASS");
    return 0;
}
