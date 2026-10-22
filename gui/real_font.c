/* FreeType strikes, rasterized at build time: no floating point in kernel. */
#include "gui.h"
#include "../kernel/include/vfs.h"
#include "../kernel/include/memory.h"
#include "../kernel/include/klock.h"
#include "../kernel/libc/include/string.h"

#define FONT_CACHE_SLOTS 32
#define FONT_LARGE_CACHE_SLOTS 8
#define FONT_NAME_SIZE 16
#define FONT_NAME_MAX_BLOCK (128U * 1024U)
#define FONT_MAX_BLOCK (2U * 1024U * 1024U)
#define FONT_MAX_FILE (64U * 1024U * 1024U)
#define FONT_MAX_GLYPHS 1024U
#define FONT_TEXT_LIMIT 4096U

typedef struct PACKED {
    char magic[4]; uint16_t version, count;
    char name[48]; uint32_t flags, reserved;
} font_header_t;
typedef struct PACKED {
    uint16_t size, line; int16_t ascent; uint16_t reserved;
    uint32_t offset, bytes;
} font_strike_t;
typedef struct PACKED {
    uint32_t codepoint, offset; uint16_t width, height;
    int16_t left, top; uint32_t advance;
} font_glyph_t;
typedef struct {
    bk_font_info_t info;
    char path[VFS_MAX_PATH];
    uint32_t bytes;
    uint16_t count, version;
    font_strike_t strikes[BK_FONT_MAX_SIZE - BK_FONT_MIN_SIZE + 1];
} font_face_t;
typedef struct {
    uint32_t face, stamp, count;
    uint16_t size, line; int16_t ascent;
    uint8_t *data;
} font_cache_t;

static font_face_t faces[BK_FONT_MAX_FACES];
static uint32_t face_count, stamp;
static font_cache_t cache[FONT_CACHE_SLOTS];
static kmutex_t font_lock = KMUTEX_INITIALIZER;

static bool read_exact(int fd, void *data, uint32_t bytes) {
    uint8_t *p = data;
    while (bytes) {
        int n = vfs_read(fd, p, bytes);
        if (n <= 0 || (uint32_t)n > bytes) return false;
        p += n; bytes -= (uint32_t)n;
    }
    return true;
}

static bool font_header(const char *path, font_face_t *face) {
    font_header_t h;
    int fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) return false;
    int32_t length = vfs_size(fd);
    bool valid = length >= (int32_t)sizeof(h) && length <= (int32_t)FONT_MAX_FILE &&
                 read_exact(fd, &h, sizeof(h)) && !memcmp(h.magic, "BFN1", 4) &&
                 (h.version == 1 || h.version == 2) && h.count &&
                 h.count <= BK_FONT_MAX_SIZE - BK_FONT_MIN_SIZE + 1 &&
                 h.name[47] == 0 && h.name[0];
    if (valid) valid = read_exact(fd, face->strikes, h.count * sizeof(font_strike_t));
    if (valid) for (uint32_t i = 0; i < h.count; i++) {
        font_strike_t *s = &face->strikes[i];
        if (s->size < BK_FONT_MIN_SIZE || s->size > BK_FONT_MAX_SIZE ||
            (i && s->size != face->strikes[i-1].size + 1) ||
            !s->line || s->line > 256 || s->ascent < 0 || s->ascent > s->line ||
            s->offset < sizeof(h) + h.count * sizeof(*s) ||
            s->bytes < 4 || s->bytes > FONT_MAX_BLOCK ||
            s->offset > (uint32_t)length || s->bytes > (uint32_t)length - s->offset)
            valid = false;
    }
    vfs_close(fd);
    if (!valid) return false;
    face->bytes = (uint32_t)length; face->count = h.count; face->version = h.version;
    face->info.flags = h.flags & BK_FONT_MONOSPACE;
    face->info.min_size = face->strikes[0].size;
    face->info.max_size = face->strikes[h.count-1].size;
    memcpy(face->info.name, h.name, 48);
    strcpy(face->path, path);
    return true;
}

static void discover(void) {
    /* Retry if called before the disk is mounted. Catalogue is stable thereafter. */
    if (face_count) return;
    vfs_dir_entry_t *entries = kmalloc(128 * sizeof(*entries));
    uint32_t count = 0;
    if (!entries) return;
    if (vfs_listdir("/SYSTEM/FONTS", entries, 128, &count)) {
        /* Sort by filename so IDs do not depend on filesystem insertion order. */
        for (uint32_t i = 0; i < count; i++) for (uint32_t j = i+1; j < count; j++)
            if (strcmp(entries[i].name, entries[j].name) > 0) {
                vfs_dir_entry_t tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp;
            }
        for (uint32_t i = 0; i < count && face_count < BK_FONT_MAX_FACES; i++) {
            uint32_t n = strlen(entries[i].name);
            char path[VFS_MAX_PATH];
            if (entries[i].type != VFS_NODE_FILE || n < 5 || n > VFS_MAX_PATH - 15 ||
                (strcmp(entries[i].name+n-4, ".BFN") && strcmp(entries[i].name+n-4, ".bfn"))) continue;
            strcpy(path, "/SYSTEM/FONTS/"); strcat(path, entries[i].name);
            if (font_header(path, &faces[face_count])) {
                faces[face_count].info.id = face_count;
                face_count++;
            }
        }
    }
    kfree(entries);
}

/* Decode byte runs into a separately bounded allocation. Reject trailing/short data. */
static uint8_t *unpack_strike(const uint8_t *encoded, uint32_t bytes, uint32_t *decoded_bytes) {
    if (bytes < 4) return NULL;
    uint32_t total; memcpy(&total,encoded,4);
    if (total < 4 || total > FONT_MAX_BLOCK) return NULL;
    uint8_t *out=kmalloc(total);
    if (!out) return NULL;
    uint32_t input=4, output=0;
    while (input < bytes) {
        uint8_t tag=encoded[input++]; uint32_t count=(tag&127)+1;
        if (count > total-output) goto invalid;
        if (tag&128) {
            if (input >= bytes) goto invalid;
            memset(out+output,encoded[input++],count);
        } else {
            if (count > bytes-input) goto invalid;
            memcpy(out+output,encoded+input,count); input+=count;
        }
        output+=count;
    }
    if (output != total) goto invalid;
    *decoded_bytes=total; return out;
invalid:
    kfree(out); return NULL;
}

static font_cache_t *load_strike(uint32_t id, uint16_t size) {
    discover();
    if (id >= face_count || size < faces[id].info.min_size || size > faces[id].info.max_size) return NULL;
    font_strike_t *s = &faces[id].strikes[size - faces[id].info.min_size];
    /* Names at 16 px have a separate small cache; opening a list must not
     * evict the selected preview or repeatedly read all its rows from disk. */
    uint32_t first=0, end=FONT_LARGE_CACHE_SLOTS;
    if (size == FONT_NAME_SIZE && s->bytes <= FONT_NAME_MAX_BLOCK) {
        first=FONT_LARGE_CACHE_SLOTS; end=FONT_CACHE_SLOTS;
    }
    for (uint32_t i=0;i<FONT_CACHE_SLOTS;i++) {
        if (cache[i].data && cache[i].face == id && cache[i].size == size) {
            cache[i].stamp=++stamp;return &cache[i];
        }
    }
    uint32_t oldest = first;
    for (uint32_t i = first; i < end; i++) {
        if (cache[i].data && cache[i].face == id && cache[i].size == size) {
            cache[i].stamp = ++stamp; return &cache[i];
        }
        if (!cache[i].data || cache[i].stamp < cache[oldest].stamp) oldest = i;
    }
    uint8_t *data = kmalloc(s->bytes);
    if (!data) return NULL;
    (void)mm_set_allocation_subsystem(data,MM_SUBSYSTEM_GUI);
    int fd = vfs_open(faces[id].path, VFS_O_RDONLY);
    bool valid = fd >= 0 && vfs_size(fd) == (int32_t)faces[id].bytes &&
        vfs_seek(fd, (int32_t)s->offset, 0) == (int32_t)s->offset && read_exact(fd, data, s->bytes);
    if (fd >= 0) vfs_close(fd);
    uint32_t block_bytes=s->bytes;
    if (valid && faces[id].version == 2) {
        uint8_t *decoded=unpack_strike(data,s->bytes,&block_bytes);
        kfree(data); data=decoded; valid=data != NULL;
        if (valid && first == FONT_LARGE_CACHE_SLOTS && block_bytes > FONT_NAME_MAX_BLOCK) {
            /* Large custom strikes still render, but cannot fill the small cache. */
            oldest=0;
            for (uint32_t i=0;i<FONT_LARGE_CACHE_SLOTS;i++)
                if (!cache[i].data || cache[i].stamp<cache[oldest].stamp) oldest=i;
        }
    }
    uint32_t count = 0;
    if (valid) {
        memcpy(&count, data, 4);
        valid = count && count <= FONT_MAX_GLYPHS && count <= (block_bytes-4)/sizeof(font_glyph_t);
    }
    if (valid) {
        font_glyph_t *g = (font_glyph_t *)(data+4);
        uint32_t start = 4 + count * sizeof(*g);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t bytes = (uint32_t)g[i].width * g[i].height;
            if ((i && g[i].codepoint <= g[i-1].codepoint) || g[i].codepoint > 0x10ffff ||
                g[i].width > 256 || g[i].height > 256 || g[i].left < -256 || g[i].left > 256 ||
                g[i].top < -256 || g[i].top > 256 || g[i].advance > 256*64 ||
                g[i].offset < start || g[i].offset > block_bytes || bytes > block_bytes-g[i].offset)
                valid = false;
        }
    }
    if (!valid) { kfree(data); return NULL; }
    /* Strikes belong to the global font cache, not to the application that
     * happened to request this face/size first. Process teardown must leave
     * cached glyph data valid for subsequent clients. */
#ifndef BLES_VISION_SERVICE
    /* The kernel cache outlives requesting clients. The private Ring-3
     * service cache instead retains its service owner for restart cleanup. */
    if (!mm_set_allocation_owner(data, 0U)) {
        kfree(data);
        return NULL;
    }
#endif
    (void)mm_set_allocation_subsystem(data, MM_SUBSYSTEM_GUI);
    font_cache_t *slot = &cache[oldest];
    kfree(slot->data);
    *slot = (font_cache_t){id, ++stamp, count, size, s->line, s->ascent, data};
    return slot;
}

static uint32_t decode(const uint8_t **cursor, const uint8_t *end) {
    const uint8_t *p = *cursor;
    uint32_t cp = *p++;
    uint32_t need, minimum;
    if (cp < 0x80) { *cursor = p; return cp; }
    if (cp >= 0xc2 && cp <= 0xdf) { need=1; minimum=0x80; cp &= 31; }
    else if (cp >= 0xe0 && cp <= 0xef) { need=2; minimum=0x800; cp &= 15; }
    else if (cp >= 0xf0 && cp <= 0xf4) { need=3; minimum=0x10000; cp &= 7; }
    else { *cursor=p; return 0xfffd; }
    if ((uint32_t)(end-p) < need) { *cursor=p; return 0xfffd; }
    for (uint32_t i=0; i<need; i++) {
        if ((p[i]&0xc0) != 0x80) { *cursor=p; return 0xfffd; }
        cp=(cp<<6)|(p[i]&63);
    }
    *cursor=p+need;
    return cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ? 0xfffd : cp;
}

static const font_glyph_t *glyph(font_cache_t *c, uint32_t cp) {
    const font_glyph_t *g = (const font_glyph_t *)(c->data+4);
    uint32_t lo=0, hi=c->count;
    while (lo < hi) { uint32_t mid=lo+(hi-lo)/2; if (g[mid].codepoint < cp) lo=mid+1; else hi=mid; }
    if (lo < c->count && g[lo].codepoint == cp) return &g[lo];
    return cp == '?' ? NULL : glyph(c, '?');
}

uint32_t bk_font_count(void) {
    kmutex_lock(&font_lock); discover(); uint32_t n=face_count; kmutex_unlock(&font_lock); return n;
}
bool bk_font_info(uint32_t id, bk_font_info_t *out) {
    if (!out) return false;
    kmutex_lock(&font_lock); discover(); bool ok=id < face_count;
    if (ok) *out=faces[id].info;
    kmutex_unlock(&font_lock); return ok;
}
uint16_t bk_font_line_height(uint32_t id, uint16_t size) {
    kmutex_lock(&font_lock); font_cache_t *c=load_strike(id,size);
    uint16_t line=c ? c->line : 0; kmutex_unlock(&font_lock); return line;
}
uint16_t bk_font_text_width(uint32_t id, uint16_t size, const char *text, uint32_t length) {
    if (!text || length > FONT_TEXT_LIMIT) return 0;
    kmutex_lock(&font_lock); font_cache_t *c=load_strike(id,size);
    uint32_t pen=0, width=0;
    if (c) {
        const uint8_t *p=(const uint8_t *)text, *end=p+length;
        while (p < end && *p) {
            uint32_t cp=decode(&p,end);
            if (cp == '\n') { if (pen > width) width=pen; pen=0; continue; }
            if (cp == '\r') continue;
            const font_glyph_t *g=glyph(c,cp == '\t' ? ' ' : cp);
            if (g) pen+=g->advance*(cp == '\t' ? 4 : 1);
        }
    }
    if (pen > width) width=pen;
    kmutex_unlock(&font_lock); width=(width+63)/64; return width > 65535 ? 65535 : (uint16_t)width;
}

/* Export advances in a batch so application layout can retain fractional
 * metrics and cache glyphs without a syscall for every word or prefix. */
uint32_t bk_font_text_advances(uint32_t id, uint16_t size,
                              const char *text, uint32_t length,
                              uint32_t *advances, uint32_t capacity) {
    if (!text || !advances || length>FONT_TEXT_LIMIT || !capacity || capacity>FONT_TEXT_LIMIT) return 0;
    kmutex_lock(&font_lock);
    font_cache_t *c=load_strike(id,size);
    uint32_t count=0;
    if (c) {
        const uint8_t *p=(const uint8_t *)text,*end=p+length;
        while (p<end && *p) {
            if (count==capacity) {count=0;break;}
            uint32_t cp=decode(&p,end);
            const font_glyph_t *g=glyph(c,cp=='\t'?' ':cp);
            advances[count++]=(cp=='\n'||cp=='\r'||!g)?0U:
                (uint32_t)g->advance*(cp=='\t'?4U:1U);
        }
    }
    kmutex_unlock(&font_lock);return count;
}

bool bk_font_draw_text_styled(gui_surface_t *surface, uint32_t id, uint16_t size,
                       int x, int y, const char *text, uint32_t length,
                       uint32_t color, gui_rect_t clip, uint32_t style,
                       int letter_spacing, int word_spacing) {
    if (!surface || !surface->pixels || !text || length > FONT_TEXT_LIMIT ||
        surface->pitch < (surface->storage_width ? surface->storage_width : surface->width) || x < -1048576 || x > 1048576 ||
        y < -1048576 || y > 1048576 || clip.w <= 0 || clip.h <= 0) return false;
    /* Intersect using wide arithmetic before accepting caller coordinates. */
    int64_t left=clip.x, top=clip.y, right=left+(int64_t)clip.w, bottom=top+(int64_t)clip.h;
    if (left < surface->origin_x) left=surface->origin_x;
    if (top < surface->origin_y) top=surface->origin_y;
    uint32_t sw=surface->storage_width ? surface->storage_width : surface->width;
    uint32_t sh=surface->storage_height ? surface->storage_height : surface->height;
    if (right > (int64_t)surface->origin_x+sw) right=(int64_t)surface->origin_x+sw;
    if (bottom > (int64_t)surface->origin_y+sh) bottom=(int64_t)surface->origin_y+sh;
    if (left < surface->clip.x) left=surface->clip.x;
    if (top < surface->clip.y) top=surface->clip.y;
    int64_t sr=(int64_t)surface->clip.x+surface->clip.w;
    int64_t sb=(int64_t)surface->clip.y+surface->clip.h;
    if (right > sr) right=sr;
    if (bottom > sb) bottom=sb;
    kmutex_lock(&font_lock); font_cache_t *c=load_strike(id,size);
    if (!c) { kmutex_unlock(&font_lock); return false; }
    int32_t pen=0;
    if (letter_spacing < -8 || letter_spacing > 64 || word_spacing < -8 || word_spacing > 64) {
        kmutex_unlock(&font_lock); return false;
    }
    const uint8_t *p=(const uint8_t *)text, *end=p+length;
    while (p < end && *p) {
        uint32_t cp=decode(&p,end);
        if (cp == '\n') { y+=c->line; pen=0; continue; }
        if (cp == '\r') continue;
        const font_glyph_t *g=glyph(c,cp == '\t' ? ' ' : cp);
        if (!g) continue;
        int gx=x+(int)((pen+32)/64)+g->left, gy=y+c->ascent+g->top;
        if (cp != '\t') for (uint32_t row=0; row<g->height; row++) {
            int py=gy+(int)row;
            if (py < top || py >= bottom) continue;
            for (uint32_t col=0; col<g->width; col++) {
                int slant = (style & BK_FONT_STYLE_ITALIC) ?
                    ((int)size - (gy + (int)row - y)) / 4 : 0;
                if (slant < 0) slant = 0;
                int px=gx+(int)col+slant;
                uint32_t a=c->data[g->offset+row*g->width+col];
                if (!(style & BK_FONT_STYLE_ANTIALIAS)) a = a < 128U ? 0U : 255U;
                if (!a) continue;
                unsigned copies = (style & BK_FONT_STYLE_BOLD) ? 2U : 1U;
                for (unsigned copy=0; copy<copies; copy++) {
                    int dx=px+(int)copy;
                    if (dx < left || dx >= right) continue;
                    uint32_t *dst=&surface->pixels[(py-surface->origin_y)*surface->pitch+(dx-surface->origin_x)];
                    if (a == 255U) *dst=color & 0x00ffffff;
                    else {
                        uint32_t r=((color>>16&255U)*a+(*dst>>16&255U)*(255U-a)+127U)/255U;
                        uint32_t g=((color>>8&255U)*a+(*dst>>8&255U)*(255U-a)+127U)/255U;
                        uint32_t b=((color&255U)*a+(*dst&255U)*(255U-a)+127U)/255U;
                        *dst=(r<<16)|(g<<8)|b;
                    }
                }
            }
        }
        pen+=(int32_t)g->advance*(cp == '\t' ? 4 : 1)+letter_spacing*64;
        if (cp == ' ') pen+=word_spacing*64;
    }
    kmutex_unlock(&font_lock); return true;
}

/* Preserve the pixel output of existing native applications. */
bool bk_font_draw_text(gui_surface_t *surface, uint32_t id, uint16_t size,
                       int x, int y, const char *text, uint32_t length,
                       uint32_t color, gui_rect_t clip) {
    return bk_font_draw_text_styled(surface,id,size,x,y,text,length,color,clip,0U,0,0);
}
