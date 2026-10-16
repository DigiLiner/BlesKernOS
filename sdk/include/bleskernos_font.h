#ifndef BLESKERNOS_FONT_TYPES_H
#define BLESKERNOS_FONT_TYPES_H
/* Stable font metadata shared by the kernel and application SDK. */
#define BK_FONT_MIN_SIZE 8U
#define BK_FONT_MAX_SIZE 72U
#define BK_FONT_MONOSPACE 1U
#define BK_FONT_MAX_FACES 64U
#define BK_FONT_STYLE_BOLD 1U
#define BK_FONT_STYLE_ITALIC 2U
#define BK_FONT_STYLE_ANTIALIAS 4U

typedef struct {
    unsigned int id;
    unsigned int flags;
    unsigned short min_size;
    unsigned short max_size;
    char name[48];
} bk_font_info_t;
#endif
