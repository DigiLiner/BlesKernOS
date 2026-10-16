#ifndef BLESKERNOS_LINK_H
#define BLESKERNOS_LINK_H
#include <elf.h>
struct dl_phdr_info { Elf32_Addr dlpi_addr; const char *dlpi_name; const Elf32_Phdr *dlpi_phdr; Elf32_Half dlpi_phnum; };
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, unsigned int, void *), void *data);
#endif
