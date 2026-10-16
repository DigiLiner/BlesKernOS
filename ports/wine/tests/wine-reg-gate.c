#include <windows.h>
#include <stdio.h>
#include <string.h>

static int query(HKEY root, const char *path, const char *name,
                 const char *expected) {
    HKEY key;
    char value[512];
    DWORD type = 0, size = sizeof(value) - 1;
    LONG status = RegOpenKeyExA(root, path, 0, KEY_READ, &key);
    if (status != ERROR_SUCCESS) {
        printf("[WINE:REG] open %s failed=%ld\n", path, status);
        return 1;
    }
    status = RegQueryValueExA(key, name, NULL, &type, (BYTE *)value, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || size >= sizeof(value) ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        printf("[WINE:REG] query %s/%s failed=%ld type=%lu size=%lu\n",
               path, name, status, type, size);
        return 1;
    }
    value[size] = 0;
    if (expected && strcmp(value, expected)) {
        printf("[WINE:REG] %s/%s unexpected: %s\n", path, name, value);
        return 1;
    }
    printf("[WINE:REG] %s/%s=%s\n", path, name, value);
    return 0;
}

int main(int argc, char **argv) {
    const char *marker_path = "Software\\BlesKernOS\\WineValidation";
    const char *marker = "W3-PERSIST-v1";
    int reuse = argc > 1 && !strcmp(argv[1], "--reuse");
    int failed = 0;
    failed |= query(HKEY_LOCAL_MACHINE,
                    "System\\CurrentControlSet\\Services\\MountMgr",
                    "ImagePath", NULL);
    failed |= query(HKEY_LOCAL_MACHINE,
                    "Software\\Microsoft\\Windows NT\\CurrentVersion",
                    "ProductName", NULL);
    if (!reuse) {
        HKEY key;
        DWORD disposition;
        LONG status = RegCreateKeyExA(HKEY_CURRENT_USER, marker_path, 0, NULL,
                                      0, KEY_WRITE, NULL, &key, &disposition);
        if (status == ERROR_SUCCESS) {
            status = RegSetValueExA(key, "Persisted", 0, REG_SZ,
                                    (const BYTE *)marker,
                                    (DWORD)strlen(marker) + 1);
            RegCloseKey(key);
        }
        if (status != ERROR_SUCCESS) {
            printf("[WINE:REG] set marker failed=%ld\n", status);
            failed = 1;
        }
    }
    failed |= query(HKEY_CURRENT_USER, marker_path, "Persisted", marker);
    if (!failed) puts("[WINE:REG] registry API validation PASS");
    return failed;
}
