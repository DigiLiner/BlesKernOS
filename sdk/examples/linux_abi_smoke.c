#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct linux_user_desc {
    int entry_number;
    unsigned int base_addr, limit, flags;
};
static long raw_linux_call3(unsigned int number, unsigned int a,
                            unsigned int b, unsigned int c) {
    long result;
    __asm__ volatile ("int $0x80" : "=a"(result)
                      : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
    return result;
}

int main(int argc, char **argv) {
    static const char payload[] = "LINUX ABI i386 OK\n";
    char readback[sizeof(payload)];
    int fd;
    unsigned char *mapping;
    void *break_before;
    static unsigned int tls_cell = 0x1234ABCDU;
    struct linux_user_desc tls = {-1, (unsigned int)&tls_cell,
                                  0xFFFFFU, 0x51U};
    unsigned int futex_word = 7U;
    (void)argc;
    (void)argv;

    fd = open("/TEMP/LINUXABI.TXT", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return 10;
    if (write(fd, payload, sizeof(payload) - 1U) !=
        (int)(sizeof(payload) - 1U)) return 11;
    if (lseek(fd, 0, SEEK_SET) != 0) return 12;
    memset(readback, 0, sizeof(readback));
    if (read(fd, readback, sizeof(payload) - 1U) !=
        (int)(sizeof(payload) - 1U)) return 13;
    if (strcmp(readback, payload) != 0) return 14;
    if (fsync(fd) != 0 || close(fd) != 0) return 15;
    if (getpid() <= 0 || getppid() <= 0) return 16;
    if (!getcwd(readback, sizeof(readback))) return 17;
    mapping=(unsigned char*)mmap(0,8192U,PROT_READ|PROT_WRITE,
                                 MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(mapping==MAP_FAILED)return 19;
    mapping[0]=0x5aU;mapping[8191]=0xa5U;
    if(mprotect(mapping,8192U,PROT_READ|PROT_EXEC)||
       munmap(mapping,8192U))return 20;
    break_before=sbrk(0);
    if(break_before==(void*)-1||sbrk(4096)!=(void*)break_before||
       sbrk(-4096)==(void*)-1)return 21;
    if (raw_linux_call3(243U, (unsigned int)&tls, 0U, 0U) != 0 ||
        tls.entry_number != 6) return 22;
    tls.base_addr = 0U;
    if (raw_linux_call3(244U, (unsigned int)&tls, 0U, 0U) != 0 ||
        tls.base_addr != (unsigned int)&tls_cell) return 23;
    if (raw_linux_call3(240U, (unsigned int)&futex_word, 0U, 8U) != -11)
        return 24;
    if (raw_linux_call3(240U, (unsigned int)&futex_word, 1U, 1U) != 0)
        return 25;
    if (write(STDOUT_FILENO, payload, sizeof(payload) - 1U) !=
        (int)(sizeof(payload) - 1U)) return 18;
    return 0;
}
