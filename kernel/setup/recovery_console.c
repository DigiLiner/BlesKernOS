#include "../include/recovery_console.h"
#include "../include/types.h"
#include "../include/gfx.h"
#include "../include/vga.h"

#define VGA_MISC_WRITE       0x3C2
#define VGA_SEQ_INDEX        0x3C4
#define VGA_SEQ_DATA         0x3C5
#define VGA_CRTC_INDEX       0x3D4
#define VGA_CRTC_DATA        0x3D5
#define VGA_GC_INDEX         0x3CE
#define VGA_GC_DATA          0x3CF
#define VGA_AC_INDEX         0x3C0
#define VGA_INPUT_STATUS_1   0x3DA
#define VGA_TEXT_MEMORY      ((volatile uint16_t *)0xB8000)

static inline void recovery_outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t recovery_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void vga_write_indexed(uint16_t index_port, uint16_t data_port,
                              uint8_t index, uint8_t value) {
    recovery_outb(index_port, index);
    recovery_outb(data_port, value);
}

/* Registros VGA para modo texto color 80x25, fuente 8x16. */
static const uint8_t seq_regs[5] = {
    0x03, 0x00, 0x03, 0x00, 0x02
};

static const uint8_t crtc_regs[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};

static const uint8_t gc_regs[9] = {
    0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};

static const uint8_t ac_regs[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

/* Graficos de rescate: no depende de Vision, drivers .DVR, heap,
 * framebuffer secundario ni BIOS interrupts. El logo ya comprobó que el
 * LFB funciona en el Intel 852GME de la TravelMate 250. */
#define RECOVERY_MAX_COLS 120U
#define RECOVERY_MAX_ROWS 72U
#define RECOVERY_CELL_W     8U
#define RECOVERY_CELL_H    12U
#define RECOVERY_LEFT       8U
#define RECOVERY_TOP       32U
#define RECOVERY_BOTTOM    12U

static bool recovery_lfb = false;
static uint16_t recovery_cols = 0U, recovery_rows = 0U;
static uint16_t recovery_x = 0U, recovery_y = 0U;
static char recovery_cells[RECOVERY_MAX_ROWS][RECOVERY_MAX_COLS];
static uint8_t recovery_colors[RECOVERY_MAX_ROWS][RECOVERY_MAX_COLS];

bool recovery_console_graphics_active(void) {
    return recovery_lfb;
}

static void recovery_draw_cell(uint16_t row, uint16_t col) {
    int x = (int)(RECOVERY_LEFT + col * RECOVERY_CELL_W);
    int y = (int)(RECOVERY_TOP + row * RECOVERY_CELL_H);
    char character = recovery_cells[row][col];
    gfx_fill_rect_rgb(x, y, RECOVERY_CELL_W, RECOVERY_CELL_H, 0x00000000U);
    if (character >= 32 && character <= 126)
        gfx_draw_char(x, y + 2, character, recovery_colors[row][col], 0, false);
}

static void recovery_scroll(void) {
    uint16_t row, col;
    for (row = 1; row < recovery_rows; ++row) {
        for (col = 0; col < recovery_cols; ++col) {
            recovery_cells[row - 1][col] = recovery_cells[row][col];
            recovery_colors[row - 1][col] = recovery_colors[row][col];
        }
    }
    for (col = 0; col < recovery_cols; ++col) {
        recovery_cells[recovery_rows - 1][col] = ' ';
        recovery_colors[recovery_rows - 1][col] = 7U;
    }
    gfx_fill_rect_rgb(0, RECOVERY_TOP, gfx_get_info()->width,
                      (int)gfx_get_info()->height - RECOVERY_TOP,
                      0x00000000U);
    for (row = 0; row < recovery_rows; ++row)
        for (col = 0; col < recovery_cols; ++col)
            if (recovery_cells[row][col] != ' ')
                recovery_draw_cell(row, col);
    recovery_y = recovery_rows - 1U;
    (void)gfx_flush();
}

void recovery_console_clear(void) {
    uint16_t row, col;
    if (!recovery_lfb) return;
    for (row = 0; row < recovery_rows; ++row)
        for (col = 0; col < recovery_cols; ++col) {
            recovery_cells[row][col] = ' ';
            recovery_colors[row][col] = 7U;
        }
    recovery_x = recovery_y = 0U;
    gfx_clear_rgb(0x00000000U);
    gfx_fill_rect_rgb(0, 0, gfx_get_info()->width, 23, 0x00203648U);
    gfx_draw_string(8, 7, "BlesKernOS 1.0 | Recovery / Hardware Diagnostics",
                    15, 0, false);
    gfx_draw_string(8, (int)gfx_get_info()->height - 10,
                    "Type: help | bootlog | mem | pci | disks | video | reboot",
                    10, 0, false);
    (void)gfx_flush();
}

void recovery_console_set_cursor(int x, int y) {
    if (!recovery_lfb) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    recovery_x = (uint16_t)((uint32_t)x < recovery_cols ? (uint32_t)x : (uint32_t)recovery_cols - 1U);
    recovery_y = (uint16_t)((uint32_t)y < recovery_rows ? (uint32_t)y : (uint32_t)recovery_rows - 1U);
}

void recovery_console_get_cursor(int *x, int *y) {
    if (x) *x = recovery_x;
    if (y) *y = recovery_y;
}

void recovery_console_putchar(char character, uint8_t foreground) {
    if (!recovery_lfb) return;
    if (character == '\r') {
        recovery_x = 0;
        return;
    }
    if (character == '\n') {
        recovery_x = 0;
        recovery_y++;
    } else if (character == '\t') {
        uint16_t next = (uint16_t)((recovery_x + 4U) & ~3U);
        recovery_x = next < recovery_cols ? next : recovery_cols - 1U;
    } else if (character == '\b') {
        if (recovery_x) {
            recovery_x--;
            recovery_cells[recovery_y][recovery_x] = ' ';
            recovery_draw_cell(recovery_y, recovery_x);
        }
    } else if ((uint8_t)character >= 32U) {
        recovery_cells[recovery_y][recovery_x] = character;
        recovery_colors[recovery_y][recovery_x] = foreground;
        recovery_draw_cell(recovery_y, recovery_x);
        recovery_x++;
        if (recovery_x >= recovery_cols) {
            recovery_x = 0;
            recovery_y++;
        }
    }
    if (recovery_y >= recovery_rows) recovery_scroll();
    /* For VirtIO/VMware, pixels are not guaranteed visible until flushed.
     * In the Intel VESA path this simply writes to the existing LFB. */
    if (gfx_get_info()->mode != GFX_MODE_VESA_LFB) (void)gfx_flush();
}

void recovery_console_enter(void) {
    const gfx_info_t *video = gfx_get_info();
    uint32_t bytes_per_pixel;
    uint32_t i;
    bool valid;

    bytes_per_pixel = video && video->bpp ? (uint32_t)video->bpp / 8U : 0U;
    valid = video && video->mode != GFX_MODE_TEXT &&
        video->mode != GFX_MODE_VGA_12H && video->mode != GFX_MODE_VGA_13H &&
        video->framebuffer >= 0x100000U &&
        video->width >= 320U && video->width <= 1920U &&
        video->height >= 200U && video->height <= 1200U &&
        (video->bpp == 16U || video->bpp == 32U) &&
        video->pitch >= (uint32_t)video->width * bytes_per_pixel &&
        video->pitch <= 8192U;
    if (valid) {
        recovery_cols = (uint16_t)((video->width - RECOVERY_LEFT * 2U) /
                                   RECOVERY_CELL_W);
        recovery_rows = (uint16_t)((video->height - RECOVERY_TOP -
                                   RECOVERY_BOTTOM) / RECOVERY_CELL_H);
        if (recovery_cols > RECOVERY_MAX_COLS) recovery_cols = RECOVERY_MAX_COLS;
        if (recovery_rows > RECOVERY_MAX_ROWS) recovery_rows = RECOVERY_MAX_ROWS;
        recovery_lfb = true;
        vga_init(); /* Reutiliza kprintf()/shell_run() sin escribir a 0xB8000. */
        return;
    }

    /* Si la BIOS nunca entregó LFB, conservar la ruta de texto histórica. */
    recovery_lfb = false;
    /* Apaga momentaneamente la pantalla durante la reprogramacion. */

    vga_write_indexed(VGA_SEQ_INDEX, VGA_SEQ_DATA, 0x01, 0x20);

    recovery_outb(VGA_MISC_WRITE, 0x67);

    for (i = 0; i < 5; ++i)
        vga_write_indexed(VGA_SEQ_INDEX, VGA_SEQ_DATA, (uint8_t)i, seq_regs[i]);

    /* Desbloquea CRTC 0-7. */
    vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA, 0x11,
                      (uint8_t)(recovery_inb(VGA_CRTC_DATA) & 0x7F));

    for (i = 0; i < 25; ++i)
        vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA,
                          (uint8_t)i, crtc_regs[i]);

    for (i = 0; i < 9; ++i)
        vga_write_indexed(VGA_GC_INDEX, VGA_GC_DATA, (uint8_t)i, gc_regs[i]);

    for (i = 0; i < 21; ++i) {
        (void)recovery_inb(VGA_INPUT_STATUS_1);
        recovery_outb(VGA_AC_INDEX, (uint8_t)i);
        recovery_outb(VGA_AC_INDEX, ac_regs[i]);
    }

    /* Reactiva video y deshabilita blanking del Attribute Controller. */
    (void)recovery_inb(VGA_INPUT_STATUS_1);
    recovery_outb(VGA_AC_INDEX, 0x20);
    vga_write_indexed(VGA_SEQ_INDEX, VGA_SEQ_DATA, 0x01, seq_regs[1]);

    /* Limpia la consola y deja cursor visible en la esquina superior. */
    for (i = 0; i < 80u * 25u; ++i)
        VGA_TEXT_MEMORY[i] = (uint16_t)0x0720;

    vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA, 0x0A, 0x0E);
    vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA, 0x0B, 0x0F);
    vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA, 0x0E, 0x00);
    vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA, 0x0F, 0x00);
    vga_init();
}
