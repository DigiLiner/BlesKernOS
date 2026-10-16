#include "common.h"

static bool exists(const char *path) {
    int fd = bk_file_open(path, BK_FILE_READ_ONLY);
    if (fd < 0) return false;
    (void)bk_file_close(fd);
    return true;
}

static bool has_separator(const char *text) {
    while (text && *text) {
        if (*text == '/' || *text == '\\') return true;
        text++;
    }
    return false;
}

static bool has_extension(const char *text) {
    const char *last = text;
    while (text && *text) {
        if (*text == '/' || *text == '\\') last = text + 1;
        text++;
    }
    while (last && *last) if (*last++ == '.') return true;
    return false;
}

static bool arguments(char *out,uint32_t cap,int argc,char **argv) {
    uint32_t used=0;
    out[0]=0;
    for(int i=2;i<argc;i++) {
        if(used+3>=cap)return false;
        if(i>2)out[used++]=' ';
        out[used++]='"';
        for(const char *p=argv[i];*p;p++) {
            if(used+3>=cap)return false;
            if(*p=='"'||*p=='\\')out[used++]='\\';
            out[used++]=*p;
        }
        out[used++]='"';
        out[used]=0;
    }
    return true;
}

static int launch(const char *path, const char *args) {
    if (!exists(path)) return -1;
    if (!bk_app_launch(path, args))
        return command_error("start", "@H3F5209AC");
    kprintf("Iniciado: %s\n", path);
    return 0;
}

static int run(int argc, char **argv) {
    char path[VFS_MAX_PATH];
    char args[COMMAND_ARG_MAX];
    int result;
    if (argc < 2)
        return command_error("start", "@HF5BBD4DE");
    if(!arguments(args,sizeof(args),argc,argv))return command_error("start","argumentos demasiado largos");
    if (has_separator(argv[1]) || has_extension(argv[1])) {
        result = launch(argv[1], args);
        return result >= 0 ? result : command_error("start", "@HC6CAD505");
    }
    bk_runtime_strncpy(path, "/SYSTEM/PROGRAMS/", sizeof(path) - 1U);
    path[sizeof(path) - 1U] = '\0';
    bk_runtime_strcat(path, argv[1]);
    bk_runtime_strcat(path, ".BEX");
    result = launch(path, args);
    if (result >= 0) return result;
    bk_runtime_strncpy(path, "/SYSTEM/WIN32/", sizeof(path) - 1U);
    path[sizeof(path) - 1U] = '\0';
    bk_runtime_strcat(path, argv[1]);
    bk_runtime_strcat(path, ".EXE");
    result = launch(path, args);
    if (result >= 0) return result;
    return command_error("start", "@HD98B340A");
}

BK_COMMAND_MAIN(run)
