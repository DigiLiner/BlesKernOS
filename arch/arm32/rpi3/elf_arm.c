#include "elf_arm.h"
#include "task_arm.h"
#include "cache.h"
#include "../../../kernel/include/vfs.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/libc/include/stdio.h"

#define EI_NIDENT 16U
#define ET_REL 1U
#define EM_ARM 40U
#define EV_CURRENT 1U
#define ELFCLASS32 1U
#define ELFDATA2LSB 1U
#define SHN_UNDEF 0U
#define SHN_ABS 0xFFF1U
#define SHF_ALLOC 0x2U
#define SHT_PROGBITS 1U
#define SHT_SYMTAB 2U
#define SHT_STRTAB 3U
#define SHT_NOBITS 8U
#define SHT_REL 9U
#define SHT_ARM_EXIDX 0x70000001U
#define STB_WEAK 2U
#define ELF32_ST_BIND(x) ((x) >> 4)
#define ELF32_R_SYM(x) ((x) >> 8)
#define ELF32_R_TYPE(x) ((x) & 0xFFU)
#define R_ARM_NONE 0U
#define R_ARM_PC24 1U
#define R_ARM_ABS32 2U
#define R_ARM_REL32 3U
#define R_ARM_CALL 28U
#define R_ARM_JUMP24 29U
#define R_ARM_TARGET1 38U
#define R_ARM_V4BX 40U
#define R_ARM_PREL31 42U
#define R_ARM_MOVW_ABS_NC 43U
#define R_ARM_MOVT_ABS 44U
#define ARM_ELF_MAX_SECTIONS 256U
#define ARM_ELF_IMAGE_OFFSET 0x00001000U
#define ARM_TRAMPOLINE_BYTES 16U

/* User-mode return trampoline: mov r7,#SYS_EXIT; mov r0,#0; svc #0; b . */
static const uint32_t g_user_exit_trampoline[4] = {
    0xE3A07000U, 0xE3A00000U, 0xEF000000U, 0xEAFFFFFEU
};

/* mov r7,#SYS_UPCALL_RETURN (25); svc #0; b .; b . */
static const uint32_t g_user_upcall_trampoline[4] = {
    0xE3A07019U, 0xEF000000U, 0xEAFFFFFEU, 0xEAFFFFFEU
};

typedef struct {
    uint8_t ident[EI_NIDENT];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf32_header_t;

typedef struct {
    uint32_t name;
    uint32_t type;
    uint32_t flags;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
    uint32_t info;
    uint32_t addralign;
    uint32_t entsize;
} __attribute__((packed)) elf32_section_t;

typedef struct {
    uint32_t name;
    uint32_t value;
    uint32_t size;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
} __attribute__((packed)) elf32_symbol_t;

typedef struct {
    uint32_t offset;
    uint32_t info;
} __attribute__((packed)) elf32_rel_t;

static const char *g_error = "sin error";

const char *rpi3_elf_last_error(void) { return g_error; }

static bool range_ok(uint32_t offset, uint32_t bytes, uint32_t size) {
    return offset <= size && bytes <= size - offset;
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    if (alignment < 4U) alignment = 4U;
    if ((alignment & (alignment - 1U)) != 0U) return 0U;
    if (value > 0xFFFFFFFFU - (alignment - 1U)) return 0U;
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int32_t sign_extend24(uint32_t value) {
    value &= 0x00FFFFFFU;
    if (value & 0x00800000U) value |= 0xFF000000U;
    return (int32_t)value;
}

static uint32_t mov_imm16(uint32_t instruction) {
    return ((instruction >> 4) & 0xF000U) | (instruction & 0x0FFFU);
}

static uint32_t patch_mov_imm16(uint32_t instruction, uint32_t value) {
    instruction &= ~0x000F0FFFU;
    instruction |= ((value & 0xF000U) << 4) | (value & 0x0FFFU);
    return instruction;
}

static bool symbol_value(const elf32_symbol_t *symbols, uint32_t symbol_count,
                         const char *strings, uint32_t strings_size,
                         const uint32_t *runtime, uint32_t section_count,
                         uint32_t index, uint32_t *value_out,
                         const char **name_out) {
    const elf32_symbol_t *symbol;
    if (!symbols || index >= symbol_count) return false;
    symbol = &symbols[index];
    if (name_out) {
        *name_out = symbol->name < strings_size ? strings + symbol->name : "(bad-name)";
    }
    if (symbol->shndx == SHN_UNDEF) {
        if (ELF32_ST_BIND(symbol->info) == STB_WEAK) {
            if (value_out) *value_out = 0U;
            return true;
        }
        return false;
    }
    if (symbol->shndx == SHN_ABS) {
        if (value_out) *value_out = symbol->value;
        return true;
    }
    if (symbol->shndx >= section_count || !runtime[symbol->shndx]) return false;
    if (value_out) *value_out = runtime[symbol->shndx] + symbol->value;
    return true;
}

static bool apply_relocation(uint32_t type, uint32_t place, uint32_t symbol) {
    uint32_t *word = (uint32_t *)(uintptr_t)place;
    uint32_t instruction = *word;
    int32_t addend;
    int32_t delta;
    uint32_t value;
    switch (type) {
        case R_ARM_NONE:
            return true;
        case R_ARM_ABS32:
        case R_ARM_TARGET1:
            *word = symbol + instruction;
            return true;
        case R_ARM_REL32:
            *word = symbol + instruction - place;
            return true;
        case R_ARM_PC24:
        case R_ARM_CALL:
        case R_ARM_JUMP24:
            addend = sign_extend24(instruction) << 2;
            delta = (int32_t)(symbol - place) + addend;
            if ((delta & 3) != 0 || delta < -(1 << 25) || delta >= (1 << 25)) return false;
            *word = (instruction & 0xFF000000U) | (((uint32_t)delta >> 2) & 0x00FFFFFFU);
            return true;
        case R_ARM_V4BX:
            /* ARMv7 supports BX natively; no rewrite is required. */
            return true;
        case R_ARM_PREL31:
            value = (symbol + (instruction & 0x7FFFFFFFU) - place) & 0x7FFFFFFFU;
            *word = (instruction & 0x80000000U) | value;
            return true;
        case R_ARM_MOVW_ABS_NC:
            value = symbol + mov_imm16(instruction);
            *word = patch_mov_imm16(instruction, value & 0xFFFFU);
            return true;
        case R_ARM_MOVT_ABS:
            value = symbol + (mov_imm16(instruction) << 16);
            *word = patch_mov_imm16(instruction, (value >> 16) & 0xFFFFU);
            return true;
        default:
            return false;
    }
}

static const char *short_name(const char *path) {
    const char *result = path ? path : "arm-user";
    const char *p = result;
    while (*p) {
        if (*p == '/' || *p == '\\') result = p + 1;
        p++;
    }
    return result[0] ? result : "arm-user";
}

int rpi3_elf_spawn_memory(const char *name, const void *data_void, uint32_t size,
                          const char *launch_arg) {
    const uint8_t *data = (const uint8_t *)data_void;
    const elf32_header_t *header;
    const elf32_section_t *sections;
    const elf32_symbol_t *symbols = NULL;
    const char *strings = NULL;
    uint32_t strings_size = 0U;
    uint32_t symbol_count = 0U;
    uint32_t runtime[ARM_ELF_MAX_SECTIONS];
    uint32_t cursor, slot, base, slot_bytes, i, entry = 0U;
    uint32_t exit_pc, upcall_pc, image_end;
    int pid;
    g_error = "ELF ARM invalido";
    if (!data || size < sizeof(*header)) return -1;
    header = (const elf32_header_t *)data;
    if (header->ident[0] != 0x7FU || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != ELFCLASS32 || header->ident[5] != ELFDATA2LSB) {
        g_error = "no es ELF32 little-endian";
        return -2;
    }
    if (header->type != ET_REL) { g_error = "se requiere ELF ET_REL"; return -3; }
    if (header->machine != EM_ARM) { g_error = "arquitectura ELF no es EM_ARM"; return -4; }
    if (header->version != EV_CURRENT || header->shentsize != sizeof(elf32_section_t) ||
        !header->shnum || header->shnum > ARM_ELF_MAX_SECTIONS) {
        g_error = "cabecera ELF ARM incompatible";
        return -5;
    }
    if (!range_ok(header->shoff, (uint32_t)header->shnum * sizeof(*sections), size)) {
        g_error = "tabla de secciones fuera del archivo";
        return -6;
    }
    sections = (const elf32_section_t *)(data + header->shoff);
    kmemset(runtime, 0, sizeof(runtime));

    if (rpi3_task_user_slot_acquire(&slot, &base, &slot_bytes) != 0) {
        g_error = "sin ventanas de memoria USR libres";
        return -7;
    }
    kmemset((void *)(uintptr_t)base, 0, slot_bytes);
    cursor = base + ARM_ELF_IMAGE_OFFSET;

    for (i = 1U; i < header->shnum; ++i) {
        const elf32_section_t *section = &sections[i];
        uint32_t alignment, next;
        if (!(section->flags & SHF_ALLOC) || section->type == SHT_ARM_EXIDX) continue;
        if (section->type != SHT_PROGBITS && section->type != SHT_NOBITS) continue;
        alignment = section->addralign ? section->addralign : 4U;
        next = align_up(cursor, alignment);
        if (!next || next < base || next > base + RPI3_USER_HEAP_END_OFF ||
            section->size > base + RPI3_USER_HEAP_END_OFF - next) {
            g_error = "imagen ELF ARM excede la ventana USR";
            rpi3_task_user_slot_release(slot);
            return -8;
        }
        runtime[i] = next;
        if (section->type == SHT_PROGBITS) {
            if (!range_ok(section->offset, section->size, size)) {
                g_error = "seccion ELF truncada";
                rpi3_task_user_slot_release(slot);
                return -9;
            }
            kmemcpy((void *)(uintptr_t)next, data + section->offset, section->size);
        } else {
            kmemset((void *)(uintptr_t)next, 0, section->size);
        }
        cursor = next + section->size;
    }

    for (i = 1U; i < header->shnum; ++i) {
        const elf32_section_t *section = &sections[i];
        if (section->type != SHT_SYMTAB || !section->entsize ||
            section->entsize != sizeof(elf32_symbol_t) || section->link >= header->shnum)
            continue;
        if (!range_ok(section->offset, section->size, size)) continue;
        if (sections[section->link].type != SHT_STRTAB ||
            !range_ok(sections[section->link].offset, sections[section->link].size, size))
            continue;
        symbols = (const elf32_symbol_t *)(data + section->offset);
        symbol_count = section->size / sizeof(elf32_symbol_t);
        strings = (const char *)(data + sections[section->link].offset);
        strings_size = sections[section->link].size;
        break;
    }
    if (!symbols || !strings) {
        g_error = "ELF ARM sin tabla de simbolos";
        rpi3_task_user_slot_release(slot);
        return -10;
    }

    for (i = 1U; i < header->shnum; ++i) {
        const elf32_section_t *relsec = &sections[i];
        const elf32_rel_t *rels;
        uint32_t count, r;
        if (relsec->type != SHT_REL || relsec->info >= header->shnum ||
            relsec->entsize != sizeof(elf32_rel_t) || !runtime[relsec->info]) continue;
        if (!range_ok(relsec->offset, relsec->size, size)) {
            g_error = "relocaciones ELF truncadas";
            rpi3_task_user_slot_release(slot);
            return -11;
        }
        rels = (const elf32_rel_t *)(data + relsec->offset);
        count = relsec->size / sizeof(*rels);
        for (r = 0U; r < count; ++r) {
            uint32_t sym_index = ELF32_R_SYM(rels[r].info);
            uint32_t type = ELF32_R_TYPE(rels[r].info);
            uint32_t symbol, place;
            const char *sym_name = NULL;
            if (rels[r].offset > sections[relsec->info].size - 4U) {
                g_error = "relocacion ARM fuera de seccion";
                rpi3_task_user_slot_release(slot);
                return -12;
            }
            if (!symbol_value(symbols, symbol_count, strings, strings_size,
                              runtime, header->shnum, sym_index, &symbol, &sym_name)) {
                kprintf("[ELF:ARM] unresolved symbol=%s\n", sym_name ? sym_name : "?");
                g_error = "simbolo externo ARM no resuelto (Stage 12)";
                rpi3_task_user_slot_release(slot);
                return -13;
            }
            place = runtime[relsec->info] + rels[r].offset;
            if (!apply_relocation(type, place, symbol)) {
                kprintf("[ELF:ARM] unsupported relocation type=%u symbol=%s place=%p\n",
                        type, sym_name ? sym_name : "?", (void *)(uintptr_t)place);
                g_error = "relocacion ARM no soportada";
                rpi3_task_user_slot_release(slot);
                return -14;
            }
        }
    }

    for (i = 0U; i < symbol_count; ++i) {
        const elf32_symbol_t *symbol = &symbols[i];
        const char *sym_name;
        uint32_t value;
        if (symbol->name >= strings_size) continue;
        sym_name = strings + symbol->name;
        if ((kstrcmp(sym_name, "_start") != 0 && kstrcmp(sym_name, "main") != 0) ||
            !symbol_value(symbols, symbol_count, strings, strings_size,
                          runtime, header->shnum, i, &value, NULL)) continue;
        entry = value;
        if (kstrcmp(sym_name, "_start") == 0) break;
    }
    if (!entry) {
        g_error = "ELF ARM sin _start/main";
        rpi3_task_user_slot_release(slot);
        return -15;
    }

    exit_pc = align_up(cursor, 4U);
    if (!exit_pc || exit_pc + ARM_TRAMPOLINE_BYTES * 2U >= base + RPI3_USER_HEAP_END_OFF) {
        g_error = "sin espacio para trampoline USR";
        rpi3_task_user_slot_release(slot);
        return -16;
    }
    kmemcpy((void *)(uintptr_t)exit_pc, g_user_exit_trampoline,
            ARM_TRAMPOLINE_BYTES);
    upcall_pc = exit_pc + ARM_TRAMPOLINE_BYTES;
    kmemcpy((void *)(uintptr_t)upcall_pc, g_user_upcall_trampoline,
            ARM_TRAMPOLINE_BYTES);
    image_end = upcall_pc + ARM_TRAMPOLINE_BYTES;
    rpi3_cache_sync_code((const void *)(uintptr_t)(base + ARM_ELF_IMAGE_OFFSET),
                         image_end - (base + ARM_ELF_IMAGE_OFFSET));

    pid = rpi3_task_create_user_image(short_name(name), entry, exit_pc,
                                      upcall_pc, slot, image_end, launch_arg);
    if (pid < 0) {
        g_error = "no se pudo crear tarea USR ARM";
        rpi3_task_user_slot_release(slot);
        return -17;
    }
    g_error = "sin error";
    kprintf("[ELF:ARM] EM_ARM ET_REL loaded pid=%u entry=%p image=%p..%p\n",
            (uint32_t)pid, (void *)(uintptr_t)entry,
            (void *)(uintptr_t)(base + ARM_ELF_IMAGE_OFFSET),
            (void *)(uintptr_t)image_end);
    return pid;
}

int rpi3_elf_spawn(const char *path, const char *launch_arg) {
    void *file = NULL;
    uint32_t size = 0U;
    int result;
    if (!path || !path[0]) { g_error = "ruta vacia"; return -1; }
    if (!vfs_read_all(path, &file, &size) || !file || !size) {
        g_error = "no se pudo leer el ELF ARM";
        return -2;
    }
    result = rpi3_elf_spawn_memory(path, file, size, launch_arg);
    kfree(file);
    if (result < 0)
        kprintf("[ELF:ARM] load FAIL path=%s rc=%d reason=%s\n",
                path, result, g_error);
    return result;
}
