/* SPDX-License-Identifier: GPL-2.0-or-later
 * Local BPJ raster -> HP1020/1018 ZjStream adapter. Protocol facts from
 * OpenPrinting foo2zjs; compression supplied by Markus Kuhn's JBIG-KIT.
 */
#include "../../third_party/jbigkit/jbig.h"
/* Existing public-domain IBM/Daniel Hepper bitmap face, ASCII only. */
#define GLbyte signed char
#include "../../libs/src/font8x8_basic.h"
#undef GLbyte
static bool zjs_u32(output_buffer_t *out, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                        (uint8_t)value};
    return output_append(out, bytes, 4);
}
static bool zjs_record(output_buffer_t *out, uint32_t type, uint32_t items, uint32_t payload,
                       uint16_t reserved) {
    uint8_t tail[4] = {(uint8_t)(reserved >> 8), (uint8_t)reserved, 0x5a, 0x5a};
    return payload <= OUTPUT_MAX - 16U && zjs_u32(out, 16U + payload) && zjs_u32(out, type) &&
           zjs_u32(out, items) && output_append(out, tail, 4);
}
static bool zjs_item(output_buffer_t *out, uint16_t key, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)(key >> 8), (uint8_t)key, 1, 0};
    return zjs_u32(out, 12) && output_append(out, bytes, 4) && zjs_u32(out, value);
}
typedef struct {
    output_buffer_t stream;
} zjs_encoded_t;
static void zjs_compressed(unsigned char *data, size_t length, void *context) {
    zjs_encoded_t *encoded = context;
    if (length > OUTPUT_MAX || !output_append(&encoded->stream, data, (uint32_t)length))
        encoded->stream.failed = true;
}
static bool zjs_page(output_buffer_t *out, uint8_t *pixels, uint32_t width, uint32_t height,
                     uint32_t paper) {
    struct jbg_enc_state encoder;
    zjs_encoded_t encoded;
    memory_zero(&encoded, sizeof(encoded));
    unsigned char *planes[1] = {pixels};
    jbg_enc_init(&encoder, (width + 127U) & ~127U, height, 1, planes, zjs_compressed, &encoded);
    jbg_enc_options(&encoder, JBG_ILEAVE | JBG_SMID,
                    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON, 128, 16, 0);
    jbg_enc_out(&encoder);
    jbg_enc_free(&encoder);
    bool ok = !encoded.stream.failed && encoded.stream.size >= 20;
    if (ok) {
        uint32_t keys[13] = {23, 17, 18, 16, 12, 13, 7, 8, 9, 5, 4, 3, 6};
        uint32_t values[13] = {0, width, height, 1, width, height, 1, 600, 600, 7, 1, paper, 1};
        ok = zjs_record(out, 2, 13, 156, 0x9c);
        for (unsigned i = 0; i < 13 && ok; i++)
            ok = zjs_item(out, (uint16_t)keys[i], values[i]);
        uint32_t data_size = encoded.stream.size - 20;
        if (ok)
            ok = zjs_record(out, 4, 0, 20, 0) && output_append(out, encoded.stream.data, 20);
        /* HP engines accept bounded BID records, with padding only on the last. */
        for (uint32_t offset = 0; offset < data_size && ok;) {
            uint32_t count = data_size - offset;
            if (count > 65536U)
                count = 65536U;
            uint32_t pad = offset + count == data_size ? 16U + ((4U - (count & 3U)) & 3U) : 0;
            ok = zjs_record(out, 5, 0, count + pad, 0) &&
                 output_append(out, encoded.stream.data + 20 + offset, count);
            for (unsigned i = 0; i < pad && ok; i++)
                ok = output_byte(out, 0);
            offset += count;
        }
        if (ok)
            ok = zjs_record(out, 6, 0, 0, 0) && zjs_record(out, 3, 0, 0, 0);
    }
    if (encoded.stream.data)
        bk_sys_free(encoded.stream.data);
    return ok;
}
static void zjs_dot(uint8_t *bits, uint32_t width, uint32_t height, int x, int y) {
    if (x >= 0 && y >= 0 && (uint32_t)x < width && (uint32_t)y < height)
        bits[(uint32_t)y * (((width + 127U) & ~127U) / 8U) + (uint32_t)x / 8] |=
            (uint8_t)(0x80U >> ((uint32_t)x & 7));
}
static int zjs_px(int points) { return points * 600 / 72; }
static bool render_zjs(const uint8_t *job, uint32_t size, output_buffer_t *out) {
    job_iterator_t iterator;
    job_command_t command;
    page_command_t page;
    uint8_t *bits = NULL;
    uint32_t width = 0, height = 0, stride = 0, paper = 9;
    font_state_t font = {BK_PRINT_FONT_MONO, 12, 0};
    bool ok = false;
    memory_zero(&page, sizeof(page));
    if (!job_iterator_init(&iterator, job, size) ||
        !output_text(out, "\033%-12345X@PJL JOB\n@PJL SET JAMRECOVERY=OFF\n@PJL SET DENSITY=3\n"
                          "@PJL SET ECONOMODE=OFF\n@PJL SET RET=MEDIUM\n@PJL INFO STATUS\n"
                          "@PJL USTATUS DEVICE = ON\n@PJL USTATUS JOB = ON\n"
                          "@PJL USTATUS PAGE = ON\n@PJL USTATUS TIMED = 30\n"
                          "@PJL SET JOBATTR=\"JobAttr4=19700101000000\"") ||
        !output_byte(out, 0) || !output_text(out, "\033%-12345XJZJZ") ||
        !zjs_record(out, 0, 3, 36, 0x24) || !zjs_item(out, 1, 0) || !zjs_item(out, 2, 1) ||
        !zjs_item(out, 0, 0))
        return false;
    while (job_iterator_next(&iterator, &command)) {
        if (command.type == BK_PRINT_CMD_BEGIN_PAGE) {
            if (bits || !command_page_valid(&command, &page))
                goto done;
            /* Only A4 and Letter are implemented, simplex, fixed 600 dpi. */
            if (page.width_points >= 594 && page.width_points <= 596 && page.height_points >= 841 &&
                page.height_points <= 843)
                paper = 9;
            else if (page.width_points == 612 && page.height_points == 792)
                paper = 1;
            else
                goto done;
            width = (uint32_t)zjs_px((int)page.width_points);
            height = (uint32_t)zjs_px((int)page.height_points);
            stride = ((width + 127U) & ~127U) / 8U;
            bits = bk_sys_alloc(stride * height);
            if (!bits)
                goto done;
            memory_zero(bits, stride * height);
        } else if (command.type == BK_PRINT_CMD_SET_FONT) {
            if (!bits || command.payload_size != 8)
                goto done;
            font.family = read_u16(command.payload);
            font.points = read_u16(command.payload + 2);
            font.flags = read_u32(command.payload + 4);
            if (font.family > BK_PRINT_FONT_MONO || font.points < 4 || font.points > 144)
                goto done;
        } else if (command.type == BK_PRINT_CMD_TEXT) {
            const uint8_t *text;
            uint32_t length;
            int32_t x, y;
            if (!bits || !command_text_valid(&command, &x, &y, &text, &length) ||
                !coordinate_in_page(x, y, &page))
                goto done;
            int px = zjs_px(x), py = zjs_px(y), em = zjs_px(font.points);
            if (em < 8)
                em = 8;
            for (unsigned c = 0; c < length; c++) {
                if (text[c] < 32 || text[c] > 126)
                    goto done; /* Refuse unsupported UTF-8 rather than drop it. */
                for (int dy = 0; dy < em; dy++)
                    for (int dx = 0; dx < em; dx++)
                        if (font8x8_basic[text[c]][dy * 8 / em] & (1U << (dx * 8 / em))) {
                            zjs_dot(bits, width, height,
                                    px + dx +
                                        ((font.flags & BK_PRINT_FONT_ITALIC) ? (em - dy) / 4 : 0),
                                    py + dy);
                            if (font.flags & BK_PRINT_FONT_BOLD)
                                zjs_dot(
                                    bits, width, height,
                                    px + dx + 1 +
                                        ((font.flags & BK_PRINT_FONT_ITALIC) ? (em - dy) / 4 : 0),
                                    py + dy);
                        }
                if (font.flags & BK_PRINT_FONT_UNDERLINE)
                    for (int dx = 0; dx < em; dx++)
                        zjs_dot(bits, width, height, px + dx, py + em - 1);
                px += em;
                if (px > (int)width)
                    break;
            }
        } else if (command.type == BK_PRINT_CMD_LINE) {
            line_command_t line;
            if (!bits || !command_line_valid(&command, &page, &line))
                goto done;
            int x = zjs_px(line.x1), y = zjs_px(line.y1), x2 = zjs_px(line.x2),
                y2 = zjs_px(line.y2);
            int dx = x2 > x ? x2 - x : x - x2, sx = x < x2 ? 1 : -1, dy = y2 > y ? y - y2 : y2 - y,
                sy = y < y2 ? 1 : -1, error = dx + dy;
            unsigned thickness = (unsigned)zjs_px(line.width_points);
            if (!thickness)
                thickness = 1;
            if (thickness > 64)
                goto done;
            for (;;) {
                for (unsigned ty = 0; ty < thickness; ty++)
                    for (unsigned tx = 0; tx < thickness; tx++)
                        zjs_dot(bits, width, height, x + (int)tx, y + (int)ty);
                if (x == x2 && y == y2)
                    break;
                int twice = 2 * error;
                if (twice >= dy) {
                    error += dy;
                    x += sx;
                }
                if (twice <= dx) {
                    error += dx;
                    y += sy;
                }
            }
        } else if (command.type == BK_PRINT_CMD_BITMAP_MONO) {
            bitmap_command_t bitmap;
            if (!bits || !command_bitmap_valid(&command, &page, &bitmap))
                goto done;
            int x = zjs_px(bitmap.x), y = zjs_px(bitmap.y);
            uint32_t w = (bitmap.width * 600U + bitmap.dpi - 1) / bitmap.dpi,
                     h = (bitmap.height * 600U + bitmap.dpi - 1) / bitmap.dpi;
            if (w > width || h > height)
                goto done;
            for (uint32_t dy = 0; dy < h; dy++)
                for (uint32_t dx = 0; dx < w; dx++) {
                    uint32_t bx = dx * bitmap.dpi / 600U, by = dy * bitmap.dpi / 600U;
                    if (bx < bitmap.width && by < bitmap.height &&
                        (bitmap.pixels[by * bitmap.stride + bx / 8] & (0x80U >> (bx & 7))))
                        zjs_dot(bits, width, height, x + (int)dx, y + (int)dy);
                }
        } else if (command.type == BK_PRINT_CMD_END_PAGE) {
            if (!bits || command.payload_size || !zjs_page(out, bits, width, height, paper))
                goto done;
            bk_sys_free(bits);
            bits = NULL;
        } else
            goto done;
    }
    ok = job_iterator_complete(&iterator) && !bits && !out->failed && zjs_record(out, 1, 0, 0, 0) &&
         output_text(out, "\033%-12345X@PJL EOJ\n\033%-12345X");
done:
    if (bits)
        bk_sys_free(bits);
    return ok;
}
