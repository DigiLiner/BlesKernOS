#include <bleskernos_api.h>

/* Freestanding glue for the upstream single-header decoder. pl_mpeg exposes
 * allocator hooks but still names a few libc routines internally; map all of
 * them to tiny local implementations so VIEWER.O stays independent of libc. */

typedef struct {
    uint32_t size;
    uint32_t magic;
} vv_plm_alloc_header_t;

#define VV_PLM_ALLOC_MAGIC 0x504C4D41U /* 'PLMA' */

static void *vv_plm_malloc(size_t size) {
    vv_plm_alloc_header_t *header;
    uint32_t wanted = (uint32_t)size;
    if (!wanted) wanted = 1U;
    if (wanted > 0xFFFFFFFFU - (uint32_t)sizeof(*header)) return NULL;
    header = (vv_plm_alloc_header_t *)bk_sys_alloc(
        wanted + (uint32_t)sizeof(*header));
    if (!header) return NULL;
    header->size = wanted;
    header->magic = VV_PLM_ALLOC_MAGIC;
    return (void *)(header + 1);
}

static void vv_plm_free(void *pointer) {
    vv_plm_alloc_header_t *header;
    if (!pointer) return;
    header = ((vv_plm_alloc_header_t *)pointer) - 1;
    if (header->magic != VV_PLM_ALLOC_MAGIC) return;
    header->magic = 0U;
    bk_sys_free(header);
}

static void *vv_plm_memcpy(void *dst, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (count--) *d++ = *s++;
    return dst;
}

static void *vv_plm_memmove(void *dst, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || !count) return dst;
    if (d < s) {
        while (count--) *d++ = *s++;
    } else {
        d += count;
        s += count;
        while (count--) *--d = *--s;
    }
    return dst;
}

static void *vv_plm_memset(void *dst, int value, size_t count) {
    uint8_t *d = (uint8_t *)dst;
    while (count--) *d++ = (uint8_t)value;
    return dst;
}

static int vv_plm_abs(int value) {
    return value < 0 ? -value : value;
}

static void *vv_plm_realloc(void *pointer, size_t size) {
    vv_plm_alloc_header_t *header;
    void *replacement;
    uint32_t old_size, copy_size;
    if (!pointer) return vv_plm_malloc(size);
    if (!size) {
        vv_plm_free(pointer);
        return NULL;
    }
    header = ((vv_plm_alloc_header_t *)pointer) - 1;
    if (header->magic != VV_PLM_ALLOC_MAGIC) return NULL;
    old_size = header->size;
    replacement = vv_plm_malloc(size);
    if (!replacement) return NULL;
    copy_size = old_size < (uint32_t)size ? old_size : (uint32_t)size;
    vv_plm_memcpy(replacement, pointer, copy_size);
    vv_plm_free(pointer);
    return replacement;
}

#define PLM_NO_STDIO 1
#define PLM_MALLOC(sz) vv_plm_malloc((size_t)(sz))
#define PLM_REALLOC(ptr, sz) vv_plm_realloc((ptr), (size_t)(sz))
#define PLM_FREE(ptr) vv_plm_free((ptr))
#define memcpy  vv_plm_memcpy
#define memmove vv_plm_memmove
#define memset  vv_plm_memset
#define abs     vv_plm_abs
#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg.h"
