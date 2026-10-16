#include "font.h"
#include <string.h>
#include <stdlib.h>

#define WIDTH_SLOTS 256U
#define WIDTH_TEXT_MAX 96U
static bk_font_info_t faces[BK_FONT_MAX_FACES];
static uint32_t face_count;
static uint16_t defaults[3];
static bool initialized;
static uint16_t heights[BK_FONT_MAX_FACES][BK_FONT_MAX_SIZE + 1U];
typedef struct {
    uint32_t hash, length;
    uint16_t face, size, width;
    bool bold, mono, valid;
    char text[WIDTH_TEXT_MAX];
} width_entry_t;
static width_entry_t widths[WIDTH_SLOTS];

typedef struct {uint32_t codepoint, advance;uint16_t face,size;bool valid;} advance_entry_t;
static advance_entry_t glyph_advances[1024];
static uint32_t codepoint(const char *text,uint32_t start,uint32_t next) {
    uint32_t cp=(uint8_t)text[start],n=next-start;
    if (n>1) {
        cp&=(1U<<(7U-n))-1U;
        for (uint32_t i=start+1;i<next;i++)cp=(cp<<6)|((uint8_t)text[i]&63U);
    }
    return cp;
}
static advance_entry_t *advance_slot(uint16_t face,uint16_t size,uint32_t cp) {
    uint32_t hash=cp*2654435761U^(uint32_t)face*374761393U^(uint32_t)size*668265263U;
    return &glyph_advances[hash%1024U];
}
static uint16_t native_width(uint16_t face,uint16_t size,const char *text,uint32_t length) {
    uint32_t pen=0,maximum=0;
    bool missing=false;
    for (uint32_t i=0;i<length;) {
        uint32_t next=nsbk_utf8_next(text,length,i),cp=codepoint(text,i,next);
        advance_entry_t *e=advance_slot(face,size,cp);
        if (!e->valid||e->face!=face||e->size!=size||e->codepoint!=cp) {missing=true;break;}
        if (cp=='\n') {if(pen>maximum)maximum=pen;pen=0;}
        else pen+=e->advance;
        i=next;
    }
    if (missing) {
        uint32_t local[128];
        uint32_t *advances=length<=128U?local:malloc(length*sizeof(uint32_t));
        if (!advances) return bk_font_text_width(face,size,text,length);
        uint32_t count=bk_font_text_advances(face,size,text,length,advances,length);
        if (!count) {
            if (advances!=local) free(advances);
            return bk_font_text_width(face,size,text,length);
        }
        pen=0;maximum=0;
        for (uint32_t i=0,n=0;i<length && n<count;n++) {
            uint32_t next=nsbk_utf8_next(text,length,i),cp=codepoint(text,i,next);
            advance_entry_t *e=advance_slot(face,size,cp);
            *e=(advance_entry_t){cp,advances[n],face,size,true};
            if (cp=='\n') {if(pen>maximum)maximum=pen;pen=0;}
            else pen+=advances[n];
            i=next;
        }
        if (advances!=local) free(advances);
    }
    if (pen>maximum) maximum=pen;
    maximum=(maximum+63U)/64U;
    return maximum>65535U?65535U:(uint16_t)maximum;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; }
static bool equal_name(const char *a, uint32_t n, const char *b) {
    uint32_t i=0;
    for (; i<n && b[i]; i++) if (lower(a[i]) != lower(b[i])) return false;
    return i==n && !b[i];
}
static uint16_t named(const char *name, uint32_t length) {
    for (uint32_t i=0;i<face_count;i++)
        if (equal_name(name,length,faces[i].name)) return (uint16_t)faces[i].id;
    return NSBK_FONT_NONE;
}
void nsbk_font_init(void) {
    if (initialized) return;
    uint32_t count=bk_font_count();
    if (count>BK_FONT_MAX_FACES) count=BK_FONT_MAX_FACES;
    for (uint32_t i=0;i<count;i++)
        if (bk_font_info(i,&faces[face_count])) face_count++;
    defaults[0]=named("DejaVu Sans",11);
    defaults[1]=named("DejaVu Serif",12);
    defaults[2]=named("DejaVu Sans Mono",16);
    for (unsigned f=0;f<3;f++) if (defaults[f]==NSBK_FONT_NONE) {
        for (uint32_t i=0;i<face_count;i++)
            if (!!(faces[i].flags & BK_FONT_MONOSPACE)==(f==NSBK_FAMILY_MONO)) {
                defaults[f]=(uint16_t)faces[i].id; break;
            }
    }
    initialized=true;
}
uint16_t nsbk_font_named(const char *name, uint32_t length) {
    nsbk_font_init();
    uint16_t id=named(name,length);
    if (id!=NSBK_FONT_NONE) return id;
    if (equal_name(name,length,"Arial") || equal_name(name,length,"Helvetica")) {
        id=named("Arimo",5); return id!=NSBK_FONT_NONE?id:defaults[0];
    }
    if (equal_name(name,length,"Times New Roman") || equal_name(name,length,"Times")) return defaults[1];
    if (equal_name(name,length,"Courier New") || equal_name(name,length,"Courier")) return defaults[2];
    if (equal_name(name,length,"system-ui")) return defaults[0];
    return NSBK_FONT_NONE;
}
uint16_t nsbk_font_default(unsigned family) {
    nsbk_font_init();return defaults[family<3?family:0];
}
uint16_t nsbk_font_height(uint16_t face, uint16_t size) {
    if (face==NSBK_FONT_NONE || face>=BK_FONT_MAX_FACES || size>BK_FONT_MAX_SIZE) return size+3U;
    if (!heights[face][size]) heights[face][size]=bk_font_line_height(face,size);
    return heights[face][size]?heights[face][size]:size+3U;
}
uint32_t nsbk_utf8_next(const char *text, uint32_t length, uint32_t offset) {
    if (offset>=length) return length;
    uint8_t a=(uint8_t)text[offset];
    uint32_t n=a>=0xc2U&&a<=0xdfU?2U:a>=0xe0U&&a<=0xefU?3U:a>=0xf0U&&a<=0xf4U?4U:1U;
    if (offset+n>length) return offset+1U;
    for (uint32_t i=1;i<n;i++) if (((uint8_t)text[offset+i]&0xc0U)!=0x80U) return offset+1U;
    return offset+n;
}
/* The legacy bitmap API consumes Latin-1. Convert only at that fallback boundary. */
static uint32_t legacy(const char *text, uint32_t length, char out[4096]) {
    uint32_t used=0;
    for (uint32_t i=0;i<length && used<4096;) {
        uint32_t next=nsbk_utf8_next(text,length,i), cp=(uint8_t)text[i];
        if (next-i>1) {
            cp&=(1U<<(7U-(next-i)))-1U;
            for (uint32_t j=i+1;j<next;j++) cp=(cp<<6)|((uint8_t)text[j]&63U);
        }
        char c=cp<=255U?(char)cp:'?';
        if (cp==0x2013U || cp==0x2014U) c='-';
        if (cp==0x2018U || cp==0x2019U) c='\'';
        if (cp==0x201cU || cp==0x201dU) c='"';
        out[used++]=c;i=next;
    }
    return used;
}
int32_t nsbk_font_width(uint16_t face, uint16_t size, bool bold, bool mono,
                       const char *text, uint32_t length) {
    if (!text || !length) return 0;
    if (length>4096U) length=4096U;
    uint32_t hash=2166136261U;
    for (uint32_t i=0;i<length;i++) hash=(hash^(uint8_t)text[i])*16777619U;
    hash^=(uint32_t)face*65537U+(uint32_t)size*17U+bold*3U+mono;
    width_entry_t *e=&widths[hash%WIDTH_SLOTS];
    if (length<=WIDTH_TEXT_MAX && e->valid && e->hash==hash && e->length==length &&
        e->face==face && e->size==size && e->bold==bold && e->mono==mono &&
        !memcmp(e->text,text,length)) return e->width;
    uint16_t width;
    if (face!=NSBK_FONT_NONE) width=native_width(face,size,text,length);
    else {
        char converted[4096];uint32_t n=legacy(text,length,converted);
        width=bk_gui_text_width_px(converted,n,size,mono,bold);
    }
    if (face!=NSBK_FONT_NONE && bold && width && width<65535U) width++;
    if (length<=WIDTH_TEXT_MAX) {
        e->valid=false;e->hash=hash;e->length=length;e->face=face;e->size=size;
        e->bold=bold;e->mono=mono;e->width=width;memcpy(e->text,text,length);e->valid=true;
    }
    return width;
}
int32_t nsbk_font_spacing(const char *text, uint32_t length, int letter, int word) {
    uint32_t count=0,spaces=0;
    for (uint32_t i=0;i<length;i=nsbk_utf8_next(text,length,i)) {count++;if (text[i]==' ') spaces++;}
    return (count?(int32_t)(count-1)*letter:0)+(int32_t)spaces*word;
}
uint32_t nsbk_font_fit(uint16_t face, uint16_t size, bool bold, bool mono,
                       const char *text, uint32_t length, int32_t available,
                       int letter, int word) {
    /* First measure the entire word: normal words fit with one measurement.
     * Only oversized runs require a logarithmic prefix search. */
    if (nsbk_font_width(face,size,bold,mono,text,length)+nsbk_font_spacing(text,length,letter,word)<=available) return length;
    uint32_t low=0,high=length;
    if (letter<0 || word<0) { /* Negative spacing need not be monotonic. */
        for (uint32_t n=nsbk_utf8_next(text,length,0); n; n=nsbk_utf8_next(text,length,n)) {
            if (nsbk_font_width(face,size,bold,mono,text,n)+nsbk_font_spacing(text,n,letter,word)>available) break;
            low=n;if (n==length) break;
        }
        return low;
    }
    while (low<high) {
        uint32_t mid=low+(high-low+1U)/2U;
        while (mid>low && mid<length && ((uint8_t)text[mid]&0xc0U)==0x80U) mid--;
        if (mid==low) mid=nsbk_utf8_next(text,length,low);
        if (nsbk_font_width(face,size,bold,mono,text,mid)+nsbk_font_spacing(text,mid,letter,word)<=available) low=mid;
        else {
            high=mid-1U;
            while (high>low && ((uint8_t)text[high]&0xc0U)==0x80U) high--;
        }
    }
    return low;
}
void nsbk_font_draw_spaced(bk_gui_surface_t *surface, bk_gui_rect_t clip,
                    uint16_t face, uint16_t size, int x, int y,
                    const char *text, uint32_t length, uint32_t color,
                    bool bold, bool italic, bool mono, int letter, int word) {
    if (!text || !length || clip.w<=0 || clip.h<=0) return;
    if (length>4096U) length=4096U;
    if (face!=NSBK_FONT_NONE) {
        uint32_t flags=BK_FONT_STYLE_ANTIALIAS|(bold?BK_FONT_STYLE_BOLD:0U)|(italic?BK_FONT_STYLE_ITALIC:0U);
        /* A discovered pack that cannot load must not silently use other metrics. */
        (void)bk_font_draw_text_styled(surface,face,size,x,y,text,length,color,clip,flags,letter,word);
    } else {
        char converted[4096];uint32_t n=legacy(text,length,converted);
        if (!letter && !word)
            bk_gui_surface_draw_text_px(surface,x,y,converted,n,color,size,bold,italic,mono,clip);
        else for (uint32_t i=0;i<n;i++) {
            bk_gui_surface_draw_text_px(surface,x,y,converted+i,1U,color,size,bold,italic,mono,clip);
            x+=bk_gui_text_width_px(converted+i,1U,size,mono,bold)+letter;
            if (converted[i]==' ') x+=word;
        }
    }
}

void nsbk_font_draw(bk_gui_surface_t *surface, bk_gui_rect_t clip,
                    uint16_t face, uint16_t size, int x, int y,
                    const char *text, uint32_t length, uint32_t color,
                    bool bold, bool italic, bool mono) {
    nsbk_font_draw_spaced(surface,clip,face,size,x,y,text,length,color,bold,italic,mono,0,0);
}
