#ifndef NSBK_FONT_H
#define NSBK_FONT_H
#include <bleskernos_api.h>
#define NSBK_FONT_NONE 65535U
#define NSBK_FAMILY_SANS 0U
#define NSBK_FAMILY_SERIF 1U
#define NSBK_FAMILY_MONO 2U
void nsbk_font_init(void);
uint16_t nsbk_font_named(const char *name, uint32_t length);
uint16_t nsbk_font_default(unsigned family);
uint16_t nsbk_font_height(uint16_t face, uint16_t size);
uint32_t nsbk_utf8_next(const char *text, uint32_t length, uint32_t offset);
int32_t nsbk_font_width(uint16_t face, uint16_t size, bool bold, bool mono,
                       const char *text, uint32_t length);
/* Return a byte offset at a character boundary. */
uint32_t nsbk_font_fit(uint16_t face, uint16_t size, bool bold, bool mono,
                       const char *text, uint32_t length, int32_t available,
                       int letter_spacing, int word_spacing);
void nsbk_font_draw(bk_gui_surface_t *surface, bk_gui_rect_t clip,
                    uint16_t face, uint16_t size, int x, int y,
                    const char *text, uint32_t length, uint32_t color,
                    bool bold, bool italic, bool mono);
void nsbk_font_draw_spaced(bk_gui_surface_t *surface, bk_gui_rect_t clip,
                    uint16_t face, uint16_t size, int x, int y,
                    const char *text, uint32_t length, uint32_t color,
                    bool bold, bool italic, bool mono, int letter, int word);
int32_t nsbk_font_spacing(const char *text, uint32_t length, int letter, int word);
#endif
