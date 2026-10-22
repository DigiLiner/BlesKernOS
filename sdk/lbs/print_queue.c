#include <bleskernos_print_lbs.h>
#include <string.h>
static int stem(const char *path, char *name) {
    uint32_t i;
    if (!path || strlen(path) != 25 || strncmp(path, "/TEMP/SPOOL/J", 13) ||
        strcmp(path + 21, ".BPJ"))
        return 0;
    for (i = 13; i < 21; i++)
        if (!((path[i] >= '0' && path[i] <= '9') || (path[i] >= 'A' && path[i] <= 'F')))
            return 0;
    memcpy(name, path + 12, 9);
    name[9] = 0;
    return 1;
}
static int read_status(const char *path, char *text, uint32_t capacity) {
    int fd = bk_file_open(path, BK_FILE_READ_ONLY), n;
    if (fd < 0)
        return -2;
    n = bk_file_read(fd, text, capacity - 1);
    bk_file_close(fd);
    if (n < 0)
        return -5;
    text[n] = 0;
    return 0;
}
int bk_print_job_status(const char *queue, char *text, uint32_t capacity) {
    char name[10], path[80];
    int r;
    if (!stem(queue, name) || !text || capacity < 2)
        return BK_LBS_INVALID;
    strcpy(path, "/TEMP/SPOOL/");
    strcat(path, name);
    strcat(path, ".STS");
    r = read_status(path, text, capacity);
    if (r != -2)
        return r;
    strcpy(path, "/TEMP/SPOOL/HISTORY/");
    strcat(path, name);
    strcat(path, ".STS");
    return read_status(path, text, capacity);
}
int bk_print_job_cancel_queued(const char *queue) {
    char name[10], path[64];
    bk_file_entry_t info;
    if (!stem(queue, name))
        return BK_LBS_INVALID;
    strcpy(path, "/TEMP/SPOOL/");
    strcat(path, name);
    strcat(path, ".RUN");
    if (bk_file_stat(path, &info))
        return BK_LBS_BUSY;
    if (!bk_file_stat(queue, &info))
        return -2;
    strcpy(path, "/TEMP/SPOOL/");
    strcat(path, name);
    strcat(path, ".CAN");
    return bk_file_write_all(path, "cancel\n", 7) ? 0 : -5;
}
