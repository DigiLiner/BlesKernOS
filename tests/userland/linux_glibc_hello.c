extern int puts(const char *text);

int main(int argc,char **argv) {
    if(argc<1 || !argv || !argv[0]) return 73;
    return puts("LINUX GCC ET_EXEC + libc.so.6 OK")<0 ? 74 : 0;
}
