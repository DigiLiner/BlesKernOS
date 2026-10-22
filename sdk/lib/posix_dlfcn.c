#include <dlfcn.h>
#include "../include/bleskernos_lbs_stats.h"
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DL_MAGIC 0x424B444CU
#define DL_MAX_MODULES 64U
#define DL_MAX_DEPS 32U
#define DL_MAX_DEPTH 32U
#define DL_MAX_FILE (16U * 1024U * 1024U)
typedef struct {
    unsigned int magic, low, bias, size, refs;
    unsigned int state, global, nodelete;
    unsigned int vm_image, file_readonly_bytes;
    void (*fini_func)(void);
    unsigned char *image, *image_raw;
    char *name;
    Elf32_Phdr *phdr;
    unsigned short phnum;
    Elf32_Sym *symtab;
    const char *strtab;
    unsigned int strsz, symcount;
    void (**fini)(void);
    unsigned int fini_count;
    void *dependencies[DL_MAX_DEPS];
    unsigned int dependency_count;
} dl_module_t;
static dl_module_t *module_list[DL_MAX_MODULES];
static char *last_error;
static char e_cycle[] = "circular dynamic dependency";
static char e_depth[] = "dynamic dependency nesting exceeds safe stack limit";
static char e_busy[] = "module is unloading";
static char e_resources[] = "dynamic module still owns live resources";
static char e_tls[] = "late TLS/RELA is not supported";
static char e_handle[] = "invalid dynamic module handle";
static char e_elf[] = "not a supported ELF32 i386 ET_DYN module";
static char e_io[] = "cannot read dynamic module";
static char e_mem[] = "not enough memory for dynamic module";
static char e_needed[] = "DT_NEEDED dependency cannot be loaded";
static char e_reloc[] = "unsupported or unresolved ELF relocation";
static char e_symbol[] = "symbol not found";
static char e_slots[] = "too many dynamic modules";

/* Native clients linking liblbs opt into the real VM backend. Legacy SO-only
 * clients and the host portability harness keep their established allocator. */
extern int __bk_lbs_vm_available(void) __attribute__((weak));
extern void *__bk_lbs_vm_map(void *, unsigned int, unsigned int, unsigned int,
                            int, unsigned int) __attribute__((weak));
extern int __bk_lbs_vm_unmap(void *, unsigned int) __attribute__((weak));
static void release_image(dl_module_t *m) {
    if (m->vm_image) __bk_lbs_vm_unmap(m->image, m->size);
    else free(m->image_raw);
}

extern void *bk_lbs_host_symbol(const char *) __attribute__((weak));

#define EXPORT(n)                                                                                  \
    if (!strcmp(symbol, #n))                                                                       \
    return (void *)(n)
static void *static_symbol(const char *symbol) {
    if (!symbol)
        return 0;
    if (bk_lbs_host_symbol) {
        void *v = bk_lbs_host_symbol(symbol);
        if (v)
            return v;
    }
    EXPORT(malloc);
    EXPORT(calloc);
    EXPORT(realloc);
    EXPORT(reallocarray);
    EXPORT(free);
    EXPORT(memcpy);
    EXPORT(memmove);
    EXPORT(memset);
    EXPORT(memcmp);
    EXPORT(memchr);
    EXPORT(memmem);
    EXPORT(strlen);
    EXPORT(strnlen);
    EXPORT(strcmp);
    EXPORT(strncmp);
    EXPORT(strcpy);
    EXPORT(strncpy);
    EXPORT(strdup);
    EXPORT(strndup);
    EXPORT(strchr);
    EXPORT(strrchr);
    EXPORT(strstr);
    EXPORT(open);
    EXPORT(read);
    EXPORT(write);
    EXPORT(close);
    EXPORT(lseek);
    EXPORT(dup);
    EXPORT(dup2);
    EXPORT(fopen);
    EXPORT(fclose);
    EXPORT(fread);
    EXPORT(fwrite);
    EXPORT(fflush);
    EXPORT(printf);
    EXPORT(fprintf);
    EXPORT(snprintf);
    EXPORT(sscanf);
    EXPORT(pthread_create);
    EXPORT(pthread_join);
    EXPORT(pthread_detach);
    EXPORT(pthread_self);
    EXPORT(pthread_mutex_lock);
    EXPORT(pthread_mutex_unlock);
    EXPORT(dlopen);
    EXPORT(dlsym);
    EXPORT(dlclose);
    return 0;
}
#undef EXPORT
static int range_ok(unsigned int off, unsigned int n, unsigned int cap) {
    return off <= cap && n <= cap - off;
}
static void *address(dl_module_t *m, unsigned int va, unsigned int n) {
    unsigned int off;
    if (!m || va < m->low)
        return 0;
    off = va - m->low;
    return range_ok(off, n, m->size) ? m->image + off : 0;
}
static int image_range(dl_module_t *m, const void *p, unsigned int n) {
    unsigned int v = (unsigned int)p, base = (unsigned int)m->image;
    return v >= base && range_ok(v - base, n, m->size);
}
static int executable_pointer(dl_module_t *m, const void *p) {
    unsigned int i, va;
    if (!image_range(m, p, 1))
        return 0;
    va = m->low + (unsigned int)((const unsigned char *)p - m->image);
    for (i = 0; i < m->phnum; i++) {
        Elf32_Phdr *h = &m->phdr[i];
        if (h->p_type == PT_LOAD && (h->p_flags & PF_X) && va >= h->p_vaddr &&
            va - h->p_vaddr < h->p_memsz)
            return 1;
    }
    return 0;
}
static int string_ok(dl_module_t *m, unsigned int off) {
    return off < m->strsz && memchr(m->strtab + off, 0, m->strsz - off) != 0;
}
static dl_module_t *valid_handle(void *h) {
    unsigned int i;
    for (i = 0; i < DL_MAX_MODULES; i++)
        if (module_list[i] == h)
            return module_list[i];
    return 0;
}
static void *module_symbol(dl_module_t *m, const char *name) {
    unsigned int i;
    if (!m || !m->symtab || !m->strtab || !name)
        return 0;
    for (i = 1; i < m->symcount; i++) {
        Elf32_Sym *s = &m->symtab[i];
        if (!string_ok(m, s->st_name) || s->st_shndx == SHN_UNDEF ||
            ELF32_ST_BIND(s->st_info) == STB_LOCAL || (s->st_other & 3U) == 1U ||
            (s->st_other & 3U) == 2U)
            continue;
        if (!strcmp(m->strtab + s->st_name, name)) {
            if (s->st_shndx == SHN_ABS)
                return (void *)s->st_value;
            return address(m, s->st_value, s->st_size ? s->st_size : 1U);
        }
    }
    return 0;
}
/* Breadth-first scope, visiting each module once even in a diamond DAG. */
static unsigned int module_scope(dl_module_t *root, dl_module_t *scope[DL_MAX_MODULES]) {
    unsigned int count = 1, at, i, j;
    scope[0] = root;
    for (at = 0; at < count; at++)
        for (i = 0; i < scope[at]->dependency_count; i++) {
            dl_module_t *dependency = scope[at]->dependencies[i];
            for (j = 0; j < count && scope[j] != dependency; j++) {
            }
            if (j == count && count < DL_MAX_MODULES)
                scope[count++] = dependency;
        }
    return count;
}
static void *scope_symbol(dl_module_t *m, const char *name) {
    dl_module_t *scope[DL_MAX_MODULES];
    unsigned int i, count = module_scope(m, scope);
    void *value;
    for (i = 0; i < count; i++)
        if ((value = module_symbol(scope[i], name)))
            return value;
    return 0;
}
static int scope_executable_pointer(dl_module_t *m, const void *p) {
    dl_module_t *scope[DL_MAX_MODULES];
    unsigned int i, count = module_scope(m, scope);
    for (i = 0; i < count; i++)
        if (executable_pointer(scope[i], p))
            return 1;
    return 0;
}
static void *global_symbol(const char *name, dl_module_t *skip) {
    unsigned int i;
    void *v = static_symbol(name);
    if (v)
        return v;
    for (i = 0; i < DL_MAX_MODULES; i++)
        if (module_list[i] && module_list[i] != skip && module_list[i]->state == 2 &&
            module_list[i]->global && (v = scope_symbol(module_list[i], name)))
            return v;
    return 0;
}
static int native_name(const char *name) {
    unsigned int n = strlen(name);
    return n >= 4 && name[n - 4] == '.' && (name[n - 3] == 'L' || name[n - 3] == 'l') &&
           (name[n - 2] == 'B' || name[n - 2] == 'b') && (name[n - 1] == 'S' || name[n - 1] == 's');
}
static void *dependency_open(const char *parent, const char *name) {
    static const char *dirs[] = {"/SYSTEM/", "/SYSTEM/LIBS/POSIX/", "/SYSTEM/LIBS/"};
    char path[260];
    const char *slash;
    unsigned int n, i;
    void *h;
    if (!name || !name[0])
        return 0;
    if (strchr(name, '/'))
        return dlopen(name, RTLD_NOW);
    slash = parent ? strrchr(parent, '/') : 0;
    if (slash) {
        n = (unsigned int)(slash - parent) + 1U;
        if (n + strlen(name) < sizeof(path)) {
            memcpy(path, parent, n);
            strcpy(path + n, name);
            h = dlopen(path, RTLD_NOW);
            if (h || last_error != e_io)
                return h;
        }
    }
    for (i = native_name(name) ? 0U : 1U; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (strlen(name) + strlen(dirs[i]) < sizeof(path)) {
            strcpy(path, dirs[i]);
            strcat(path, name);
            h = dlopen(path, RTLD_NOW);
            if (h || last_error != e_io)
                return h;
        }
    return 0;
}
/* Bind a global provider for the lifetime of the importing module. */
static void *relocation_symbol(dl_module_t *m, const char *name) {
    unsigned int i;
    void *v = scope_symbol(m, name);
    dl_module_t *provider;
    if (v)
        return v;
    v = static_symbol(name);
    if (v)
        return v;
    for (i = 0; i < DL_MAX_MODULES; i++) {
        provider = module_list[i];
        if (!provider || provider == m || provider->state != 2 || !provider->global)
            continue;
        v = scope_symbol(provider, name);
        if (!v)
            continue;
        if (m->dependency_count == DL_MAX_DEPS)
            return 0;
        provider->refs++;
        m->dependencies[m->dependency_count++] = provider;
        return v;
    }
    return 0;
}
static int normalize_path(const char *path, char out[260]) {
    char input[520];
    unsigned int i = 0, n = 0, start, length;
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(input))
            return 0;
        strcpy(input, path);
    } else {
        if (!getcwd(input, 260))
            return 0;
        if (strlen(input) + strlen(path) + 2 >= sizeof(input))
            return 0;
        strcat(input, "/");
        strcat(input, path);
    }
    out[n++] = '/';
    while (input[i]) {
        while (input[i] == '/')
            i++;
        start = i;
        while (input[i] && input[i] != '/')
            i++;
        length = i - start;
        if (!length)
            break;
        if (length == 1 && input[start] == '.')
            continue;
        if (length == 2 && input[start] == '.' && input[start + 1] == '.') {
            if (n > 1) {
                while (n > 1 && out[n - 1] != '/')
                    n--;
                if (n > 1)
                    n--;
            }
            continue;
        }
        if (n > 1) {
            if (n >= 259)
                return 0;
            out[n++] = '/';
        }
        if (length >= 260 - n)
            return 0;
        memcpy(out + n, input + start, length);
        n += length;
    }
    out[n] = 0;
    return 1;
}
static int relocate(dl_module_t *m, Elf32_Rel *r, unsigned int bytes) {
    unsigned int i, count = bytes / sizeof(*r);
    if (bytes % sizeof(*r))
        return 0;
    if (bytes && (!r || (unsigned char *)r < m->image ||
                  !range_ok((unsigned int)((unsigned char *)r - m->image), bytes, m->size)))
        return 0;
    for (i = 0; i < count; i++) {
        unsigned int si = ELF32_R_SYM(r[i].r_info), type = ELF32_R_TYPE(r[i].r_info), sv = 0;
        unsigned int *p = (unsigned int *)address(m, r[i].r_offset, 4U);
        Elf32_Sym *s;
        if (!p)
            return 0;
        if (type == R_386_NONE)
            continue;
        if (m->vm_image) {
            unsigned int j;
            for (j = 0; j < m->phnum; j++) {
                Elf32_Phdr *seg = &m->phdr[j];
                if (seg->p_type == PT_LOAD && (seg->p_flags & PF_W) &&
                    r[i].r_offset >= seg->p_vaddr && seg->p_memsz >= 4U &&
                    r[i].r_offset - seg->p_vaddr <= seg->p_memsz - 4U) break;
            }
            /* Native shared text is never relocated or made writable. */
            if (j == m->phnum) return 0;
        }
        if (type == R_386_RELATIVE) {
            *p += m->bias;
            continue;
        }
        if (!m->symtab || si >= m->symcount)
            return 0;
        s = &m->symtab[si];
        if (s->st_shndx != SHN_UNDEF)
            sv = s->st_shndx == SHN_ABS
                     ? s->st_value
                     : (unsigned int)address(m, s->st_value, s->st_size ? s->st_size : 1U);
        else if (string_ok(m, s->st_name)) {
            sv = (unsigned int)relocation_symbol(m, m->strtab + s->st_name);
        }
        if (!sv && ELF32_ST_BIND(s->st_info) != STB_WEAK)
            return 0;
        if (type == R_386_32)
            *p += sv;
        else if (type == R_386_PC32)
            *p += sv - (unsigned int)p;
        else if (type == R_386_GLOB_DAT || type == R_386_JMP_SLOT)
            *p = sv;
        else
            return 0;
    }
    return 1;
}

static void *dlopen_locked(const char *path, int mode) {
    char canonical[260];
    struct stat st;
    unsigned char *file = 0;
    dl_module_t *m = 0;
    Elf32_Ehdr *h;
    Elf32_Phdr *ph;
    Elf32_Dyn *dyn = 0;
    Elf32_Rel *rel = 0, *jmp = 0;
    void (**init)(void) = 0;
    unsigned int needed[DL_MAX_DEPS], needed_count = 0, dync = 0, lo = ~0U, hi = 0, relsz = 0,
                                      jmpsz = 0, initc = 0, slot, i;
    unsigned int initva = 0, finiva = 0;
    void (*init_func)(void) = 0;
    int fd;
    last_error = 0;
    if (!path || !path[0])
        return (void *)1;
    if (!strchr(path, '/'))
        return dependency_open(native_name(path) ? 0 : "./module", path);
    if (!normalize_path(path, canonical)) {
        last_error = e_io;
        return 0;
    }
    if (!strncmp(canonical, "/SYSTEM/LIBS/NATIVE/", 20) && native_name(canonical))
        memmove(canonical + 8, canonical + 20, strlen(canonical + 20) + 1);
    path = canonical;
    for (slot = 0; slot < DL_MAX_MODULES; slot++)
        if (module_list[slot] && module_list[slot]->name &&
            !strcmp(module_list[slot]->name, path)) {
            if (module_list[slot]->state != 2) {
                last_error = module_list[slot]->state == 1 ? e_cycle : e_busy;
                return 0;
            }
            module_list[slot]->refs++;
            if (mode & RTLD_GLOBAL)
                module_list[slot]->global = 1;
            return module_list[slot];
        }
    for (slot = 0; slot < DL_MAX_MODULES && module_list[slot]; slot++) {
    }
    if (slot == DL_MAX_MODULES) {
        last_error = e_slots;
        return 0;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0) {
        if (fd >= 0)
            close(fd);
        last_error = e_io;
        return 0;
    }
    if (st.st_size < (int)sizeof(*h) || st.st_size > (int)DL_MAX_FILE) {
        close(fd);
        last_error = e_elf;
        return 0;
    }
    file = malloc((unsigned int)st.st_size);
    if (!file) {
        close(fd);
        last_error = e_mem;
        return 0;
    }
    {
        unsigned int got = 0;
        while (got < (unsigned int)st.st_size) {
            int amount = read(fd, file + got, (unsigned int)st.st_size - got);
            if (amount <= 0) {
                close(fd);
                free(file);
                last_error = e_io;
                return 0;
            }
            got += (unsigned int)amount;
        }
    }
    h = (Elf32_Ehdr *)(void *)file;
    if (memcmp(h->e_ident, "\177ELF", 4) || h->e_ident[EI_CLASS] != ELFCLASS32 ||
        h->e_ident[EI_DATA] != ELFDATA2LSB || h->e_type != ET_DYN || h->e_machine != EM_386 ||
        h->e_version != EV_CURRENT || h->e_ident[6] != EV_CURRENT || h->e_ehsize != sizeof(*h) ||
        !h->e_phnum || h->e_phentsize != sizeof(*ph) ||
        !range_ok(h->e_phoff, h->e_phnum * sizeof(*ph), (unsigned int)st.st_size)) {
        close(fd);
        free(file);
        last_error = e_elf;
        return 0;
    }
    ph = (Elf32_Phdr *)(void *)(file + h->e_phoff);
    for (i = 0; i < h->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            unsigned int end = ph[i].p_vaddr + ph[i].p_memsz;
            if (end < ph[i].p_vaddr || ph[i].p_filesz > ph[i].p_memsz ||
                !range_ok(ph[i].p_offset, ph[i].p_filesz, (unsigned int)st.st_size)) {
                close(fd);
        free(file);
                last_error = e_elf;
                return 0;
            }
            if (ph[i].p_vaddr < lo)
                lo = ph[i].p_vaddr;
            if (end > hi)
                hi = end;
        }
    if (lo == ~0U || hi > 0xfffff000U || hi - lo > DL_MAX_FILE) {
        close(fd);
        free(file);
        last_error = e_elf;
        return 0;
    }
    lo &= ~0xfffU;
    hi = (hi + 0xfffU) & ~0xfffU;
    if (lo == ~0U || hi <= lo) {
        close(fd);
        free(file);
        last_error = e_elf;
        return 0;
    }
    m = calloc(1, sizeof(*m));
    if (!m) {
        close(fd); free(file); last_error = e_mem; return 0;
    }
    m->low = lo;
    m->size = hi - lo;
    m->vm_image = native_name(path) && __bk_lbs_vm_available &&
                  __bk_lbs_vm_map && __bk_lbs_vm_unmap && __bk_lbs_vm_available();
    if (m->vm_image) {
        /* MAP_PRIVATE|MAP_ANONYMOUS: reserve contiguous virtual space. The
         * pager commits only touched private data; file views replace RO pages. */
        m->image = __bk_lbs_vm_map(0, m->size, 3U, 0x22U, -1, 0);
    } else {
        m->image_raw = calloc(1, m->size + 4095U);
        if (m->image_raw)
            m->image = (unsigned char *)(((unsigned int)m->image_raw + 4095U) & ~4095U);
    }
    if (!m->image) {
        close(fd); free(m); free(file); last_error = e_mem; return 0;
    }
    m->magic = DL_MAGIC;
    m->refs = 1U;
    m->state = 1;
    m->global = (mode & RTLD_GLOBAL) != 0;
    m->bias = (unsigned int)m->image - lo;
    m->name = strdup(path);
    m->phnum = h->e_phnum;
    if (!m->name) { last_error = e_mem; goto fail; }
    for (i = 0; i < h->e_phnum; i++) {
        Elf32_Phdr *seg = &ph[i];
        if (seg->p_type != PT_LOAD || !seg->p_memsz) continue;
        if (m->vm_image) {
            unsigned int first = seg->p_vaddr & ~4095U;
            unsigned int end = (seg->p_vaddr + seg->p_memsz + 4095U) & ~4095U;
            unsigned int j;
            if ((seg->p_vaddr & 4095U) != (seg->p_offset & 4095U) ||
                (seg->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
                last_error = e_elf; goto fail;
            }
            for (j = 0; j < i; j++) {
                Elf32_Phdr *other = &ph[j];
                if (other->p_type == PT_LOAD && other->p_memsz &&
                    first < ((other->p_vaddr + other->p_memsz + 4095U) & ~4095U) &&
                    (other->p_vaddr & ~4095U) < end) {
                    last_error = e_elf; goto fail;
                }
            }
            if (!(seg->p_flags & PF_W)) {
                unsigned int protection = (seg->p_flags & PF_R ? 1U : 0U) |
                                          (seg->p_flags & PF_X ? 4U : 0U);
                /* RO segments must be entirely file backed; zero-fill stays
                 * private in writable data/BSS segments. Reject unusual ELF. */
                if (!protection || seg->p_memsz != seg->p_filesz ||
                    __bk_lbs_vm_map(m->image + first - lo, end - first,
                        protection, 0x12U, fd, seg->p_offset & ~4095U) !=
                        m->image + first - lo ||
                    memcmp(m->image + seg->p_vaddr - lo, file + seg->p_offset,
                           seg->p_filesz)) {
                    last_error = e_elf; goto fail;
                }
                m->file_readonly_bytes += end - first;
                continue;
            }
        }
        memcpy(m->image + seg->p_vaddr - lo, file + seg->p_offset, seg->p_filesz);
    }
    close(fd);
    fd = -1;
    /* La copia de phdr vive con el modulo; el buffer del archivo se libera. */
    m->phdr = malloc(h->e_phnum * sizeof(*ph));
    if (!m->phdr) {
        last_error = e_mem;
        goto fail;
    }
    memcpy(m->phdr, ph, h->e_phnum * sizeof(*ph));
    for (i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) {
            last_error = e_tls;
            goto fail;
        }
        if (ph[i].p_type == PT_DYNAMIC) {
            if (dyn) {
                last_error = e_elf;
                goto fail;
            }
            dyn = (Elf32_Dyn *)address(m, ph[i].p_vaddr, ph[i].p_memsz);
            dync = ph[i].p_memsz / sizeof(*dyn);
        }
    }
    if (!dyn) {
        last_error = e_elf;
        goto fail;
    }
    for (i = 0; i < dync && dyn[i].d_tag != DT_NULL; i++) {
        unsigned int v = dyn[i].d_un.d_ptr;
        switch (dyn[i].d_tag) {
        case DT_INIT:
            init_func = (void (*)(void))address(m, v, 1);
            if (v && !init_func) {
                last_error = e_elf;
                goto fail;
            }
            break;
        case DT_FINI:
            m->fini_func = (void (*)(void))address(m, v, 1);
            if (v && !m->fini_func) {
                last_error = e_elf;
                goto fail;
            }
            break;
        case DT_RELA:
        case DT_RELASZ:
        case DT_RELAENT:
            last_error = e_tls;
            goto fail;
        case DT_RELENT:
            if (v != sizeof(Elf32_Rel)) {
                last_error = e_elf;
                goto fail;
            }
            break;
        case DT_PLTREL:
            if (v != DT_REL) {
                last_error = e_elf;
                goto fail;
            }
            break;
        case DT_SYMENT:
            if (v != sizeof(Elf32_Sym)) {
                last_error = e_elf;
                goto fail;
            }
            break;
        case DT_NEEDED:
            if (needed_count >= DL_MAX_DEPS) {
                last_error = e_slots;
                goto fail;
            }
            needed[needed_count++] = dyn[i].d_un.d_val;
            break;
        case DT_STRTAB:
            m->strtab = (char *)address(m, v, 1);
            break;
        case DT_SYMTAB:
            m->symtab = (Elf32_Sym *)address(m, v, sizeof(Elf32_Sym));
            break;
        case DT_STRSZ:
            m->strsz = dyn[i].d_un.d_val;
            break;
        case DT_HASH: {
            unsigned int *x = (unsigned int *)address(m, v, 8);
            if (x)
                m->symcount = x[1];
            break;
        }
        case DT_REL:
            rel = (Elf32_Rel *)address(m, v, sizeof(*rel));
            break;
        case DT_RELSZ:
            relsz = dyn[i].d_un.d_val;
            break;
        case DT_JMPREL:
            jmp = (Elf32_Rel *)address(m, v, sizeof(*jmp));
            break;
        case DT_PLTRELSZ:
            jmpsz = dyn[i].d_un.d_val;
            break;
        case DT_INIT_ARRAY:
            initva = v;
            init = (void (**)(void))address(m, v, sizeof(void *));
            break;
        case DT_INIT_ARRAYSZ:
            if (v % sizeof(void *)) {
                last_error = e_elf;
                goto fail;
            }
            initc = dyn[i].d_un.d_val / sizeof(void *);
            break;
        case DT_FINI_ARRAY:
            finiva = v;
            m->fini = (void (**)(void))address(m, v, sizeof(void *));
            break;
        case DT_FINI_ARRAYSZ:
            if (v % sizeof(void *)) {
                last_error = e_elf;
                goto fail;
            }
            m->fini_count = dyn[i].d_un.d_val / sizeof(void *);
            break;
        default:
            break;
        }
    }
    if (i == dync || !m->symtab || !m->strtab || !m->strsz || !m->symcount ||
        m->symcount > m->size / sizeof(Elf32_Sym) ||
        !image_range(m, m->symtab, m->symcount * sizeof(Elf32_Sym)) ||
        !image_range(m, m->strtab, m->strsz) ||
        (initc && (!initva || initc > m->size / sizeof(void *) ||
                   !address(m, initva, initc * sizeof(void *)))) ||
        (m->fini_count && (!finiva || m->fini_count > m->size / sizeof(void *) ||
                           !address(m, finiva, m->fini_count * sizeof(void *))))) {
        last_error = e_elf;
        goto fail;
    }
    module_list[slot] = m;
    for (i = 0; i < needed_count; i++) {
        if (!string_ok(m, needed[i])) {
            last_error = e_needed;
            goto fail;
        }
        m->dependencies[m->dependency_count] = dependency_open(path, m->strtab + needed[i]);
        if (!m->dependencies[m->dependency_count]) {
            if (last_error == e_io || !last_error)
                last_error = e_needed;
            goto fail;
        }
        m->dependency_count++;
    }
    if (!relocate(m, rel, relsz) || !relocate(m, jmp, jmpsz)) {
        last_error = e_reloc;
        goto fail;
    }
    /* Validate all callback targets before invoking any constructor. */
    for (i = 0; i < initc; i++)
        if (init[i] && !scope_executable_pointer(m, (void *)init[i])) {
            last_error = e_elf;
            goto fail;
        }
    for (i = 0; i < m->fini_count; i++)
        if (m->fini[i] && !scope_executable_pointer(m, (void *)m->fini[i])) {
            last_error = e_elf;
            goto fail;
        }
    if ((init_func && !scope_executable_pointer(m, (void *)init_func)) ||
        (m->fini_func && !scope_executable_pointer(m, (void *)m->fini_func))) {
        last_error = e_elf;
        goto fail;
    }
    free(file);
    last_error = 0;
    if (init_func)
        init_func();
    for (i = 0; i < initc; i++)
        if (init[i])
            init[i]();
    m->state = 2;
    return m;
fail:
    {
        char *saved_error = last_error;
        if (module_list[slot] == m)
            module_list[slot] = 0;
        while (m && m->dependency_count)
            dlclose(m->dependencies[--m->dependency_count]);
        if (m) {
            free(m->phdr);
            free(m->name);
            release_image(m);
            free(m);
        }
        if (fd >= 0) close(fd);
        free(file);
        last_error = saved_error;
        return 0;
    }
}
static void *dlsym_locked(void *handle, const char *symbol) {
    void *v;
    dl_module_t *m;
    last_error = 0;
    if (!symbol) {
        last_error = e_symbol;
        return 0;
    }
    if (handle == (void *)1 || handle == RTLD_DEFAULT)
        v = global_symbol(symbol, 0);
    else if ((m = valid_handle(handle)) && m->state == 2)
        v = scope_symbol(m, symbol);
    else {
        last_error = e_handle;
        return 0;
    }
    if (!v)
        last_error = e_symbol;
    return v;
}
static int release_plan(dl_module_t *m, unsigned int refs[DL_MAX_MODULES]) {
    unsigned int i, slot;
    for (slot = 0; slot < DL_MAX_MODULES && module_list[slot] != m; slot++) {
    }
    if (slot == DL_MAX_MODULES || !refs[slot])
        return 0;
    if (refs[slot] > 1) {
        refs[slot]--;
        return 1;
    }
    if (m->nodelete)
        return 1;
    {
        unsigned int (*live)(void) =
            (unsigned int (*)(void))module_symbol(m, "bk_lbs_live_resources");
        if (live && live())
            return 0;
    }
    refs[slot] = 0;
    for (i = m->dependency_count; i; i--)
        if (!release_plan(m->dependencies[i - 1], refs))
            return 0;
    return 1;
}
static int dlclose_locked(void *handle) {
    dl_module_t *m;
    unsigned int i;
    if (handle == (void *)1 || handle == RTLD_DEFAULT)
        return 0;
    m = valid_handle(handle);
    if (!m || m->state != 2) {
        last_error = e_handle;
        return -1;
    }
    if (m->refs > 1U) {
        m->refs--;
        return 0;
    }
    if (m->nodelete)
        return 0;
    {
        unsigned int refs[DL_MAX_MODULES];
        for (i = 0; i < DL_MAX_MODULES; i++)
            refs[i] = module_list[i] ? module_list[i]->refs : 0;
        if (!release_plan(m, refs)) {
            last_error = e_resources;
            return -1;
        }
    }
    m->state = 3;
    for (i = m->fini_count; m->fini && i; i--)
        if (m->fini[i - 1])
            m->fini[i - 1]();
    if (m->fini_func)
        m->fini_func();
    for (i = 0; i < DL_MAX_MODULES; i++)
        if (module_list[i] == m)
            module_list[i] = 0;
    m->magic = 0;
    while (m->dependency_count)
        dlclose(m->dependencies[--m->dependency_count]);
    free(m->phdr);
    free(m->name);
    release_image(m);
    free(m);
    return 0;
}
/* Pin modules containing live callbacks, threads or backend global state. */
static int bk_dl_pin_locked(void *handle) {
    dl_module_t *m = valid_handle(handle);
    if (!m || m->state != 2) {
        last_error = e_handle;
        return -1;
    }
    m->nodelete = 1;
    return 0;
}
static char *dlerror_locked(void) {
    char *r = last_error;
    last_error = 0;
    return r;
}
static int dl_iterate_phdr_locked(int (*cb)(struct dl_phdr_info *, unsigned int, void *),
                                  void *data) {
    unsigned int i;
    if (!cb)
        return 0;
    for (i = 0; i < DL_MAX_MODULES; i++)
        if (module_list[i]) {
            struct dl_phdr_info x;
            int r;
            x.dlpi_addr = module_list[i]->bias;
            x.dlpi_name = module_list[i]->name;
            x.dlpi_phdr = module_list[i]->phdr;
            x.dlpi_phnum = module_list[i]->phnum;
            r = cb(&x, sizeof(x), data);
            if (r)
                return r;
        }
    return 0;
}

/* Serializes mutations across threads. Constructors may re-enter on the same
   thread. They must not join a worker that is itself waiting for the loader. */
static volatile unsigned int dl_owner;
static unsigned int dl_depth;
static pthread_key_t dl_error_key;
static int dl_error_key_ready;
static void loader_lock(void) {
    unsigned int tid = (unsigned int)pthread_self();
    if (!tid)
        tid = ~0U;
    if (dl_owner == tid) {
        dl_depth++;
        return;
    }
    while (!__sync_bool_compare_and_swap(&dl_owner, 0, tid))
        sched_yield();
    dl_depth = 1;
    if (!dl_error_key_ready && pthread_key_create(&dl_error_key, 0) == 0)
        dl_error_key_ready = 1;
    if (dl_error_key_ready)
        last_error = pthread_getspecific(dl_error_key);
}
static void loader_unlock(void) {
    if (!--dl_depth) {
        if (dl_error_key_ready)
            (void)pthread_setspecific(dl_error_key, last_error);
        __sync_lock_release(&dl_owner);
    }
}
void *dlopen(const char *p, int mode) {
    void *r;
    loader_lock();
    if (dl_depth > DL_MAX_DEPTH) {
        last_error = e_depth;
        loader_unlock();
        return 0;
    }
    r = dlopen_locked(p, mode);
    loader_unlock();
    return r;
}
void *dlsym(void *h, const char *s) {
    void *r;
    loader_lock();
    r = dlsym_locked(h, s);
    loader_unlock();
    return r;
}
int dlclose(void *h) {
    int r;
    loader_lock();
    r = dlclose_locked(h);
    loader_unlock();
    return r;
}
int bk_dl_pin(void *h) {
    int r;
    loader_lock();
    r = bk_dl_pin_locked(h);
    loader_unlock();
    return r;
}
char *dlerror(void) {
    char *r;
    loader_lock();
    r = dlerror_locked();
    loader_unlock();
    return r;
}
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, unsigned int, void *), void *d) {
    int r;
    loader_lock();
    r = dl_iterate_phdr_locked(cb, d);
    loader_unlock();
    return r;
}

int bk_dl_statistics(bk_dl_statistics_t *out) {
    unsigned int i;
    if (!out || out->size < sizeof(*out))
        return -1;
    loader_lock();
    out->modules = out->image_bytes = out->references = out->pinned = 0;
    for (i = 0; i < DL_MAX_MODULES; i++)
        if (module_list[i]) {
            out->modules++;
            out->image_bytes += module_list[i]->size;
            out->references += module_list[i]->refs;
            if (module_list[i]->nodelete)
                out->pinned++;
        }
    loader_unlock();
    return 0;
}

/* Internal SDK helper: messages supplied here have static application lifetime. */
void __bk_dl_set_error(const char *message) {
    loader_lock();
    last_error = (char *)message;
    loader_unlock();
}

/* Metadata must belong to the requested module, not one of its dependencies. */
void *__bk_dl_own_symbol(void *handle, const char *symbol) {
    void *value = 0;
    dl_module_t *m;
    loader_lock();
    m = valid_handle(handle);
    if (m && m->state == 2 && symbol) {
        value = module_symbol(m, symbol);
        if (!value)
            last_error = e_symbol;
    } else
        last_error = e_handle;
    loader_unlock();
    return value;
}

int bk_dl_mapping_statistics(bk_dl_mapping_statistics_t *out) {
    unsigned int i;
    if (!out || out->size < sizeof(*out)) return -1;
    loader_lock();
    out->modules = out->virtual_bytes = out->file_readonly_bytes = out->private_bytes = 0;
    for (i = 0; i < DL_MAX_MODULES; i++) if (module_list[i]) {
        dl_module_t *m = module_list[i];
        out->modules++;
        out->virtual_bytes += m->size;
        out->file_readonly_bytes += m->file_readonly_bytes;
        out->private_bytes += m->size - m->file_readonly_bytes;
    }
    loader_unlock();
    return 0;
}
