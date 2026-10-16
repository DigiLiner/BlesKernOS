#ifndef BLESKERNOS_ELF_H
#define BLESKERNOS_ELF_H

#include <stdint.h>

#define EI_NIDENT 16
#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_386 3
#define EV_CURRENT 1
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define ELF32_R_SYM(info) ((info) >> 8)
#define ELF32_R_TYPE(info) ((unsigned char)(info))
#define ELF32_ST_BIND(info) ((info) >> 4)

typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;

typedef struct { unsigned char e_ident[EI_NIDENT]; Elf32_Half e_type,e_machine; Elf32_Word e_version; Elf32_Addr e_entry; Elf32_Off e_phoff,e_shoff; Elf32_Word e_flags; Elf32_Half e_ehsize,e_phentsize,e_phnum,e_shentsize,e_shnum,e_shstrndx; } Elf32_Ehdr;
typedef struct { Elf32_Word p_type; Elf32_Off p_offset; Elf32_Addr p_vaddr,p_paddr; Elf32_Word p_filesz,p_memsz,p_flags,p_align; } Elf32_Phdr;
typedef struct { Elf32_Word st_name; Elf32_Addr st_value; Elf32_Word st_size; unsigned char st_info,st_other; Elf32_Half st_shndx; } Elf32_Sym;
typedef struct { Elf32_Addr r_offset; Elf32_Word r_info; } Elf32_Rel;
typedef struct { Elf32_Sword d_tag; union { Elf32_Word d_val; Elf32_Addr d_ptr; } d_un; } Elf32_Dyn;

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_STRSZ 10
#define DT_REL 17
#define DT_RELSZ 18
#define DT_RELENT 19
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#endif
