#include "hz_core.h"

#define HZ_MAX_BITS 15
#define HZ_MAX_LITLEN 288
#define HZ_MAX_DIST 32
#define HZ_MAX_CODELEN 19
#define HZ_MAX_CODE_LENGTHS (HZ_MAX_LITLEN + HZ_MAX_DIST)

typedef struct {
    const hz_u8 *source;
    hz_u32 size;
    hz_u32 position;
    hz_u32 bit_buffer;
    hz_u8 bit_count;
} hz_bits_t;

typedef struct {
    hz_u16 count[HZ_MAX_BITS + 1];
    hz_u16 symbol[HZ_MAX_LITLEN];
} hz_huffman_t;

static int hz_bits_read(hz_bits_t *bits, hz_u8 count, hz_u32 *value) {
    hz_u32 mask;
    if (!bits || !value || count > 24U) return 0;
    while (bits->bit_count < count) {
        if (bits->position >= bits->size) return 0;
        bits->bit_buffer |= ((hz_u32)bits->source[bits->position++]) << bits->bit_count;
        bits->bit_count = (hz_u8)(bits->bit_count + 8U);
    }
    if (count == 0U) {
        *value = 0U;
        return 1;
    }
    mask = (1U << count) - 1U;
    *value = bits->bit_buffer & mask;
    bits->bit_buffer >>= count;
    bits->bit_count = (hz_u8)(bits->bit_count - count);
    return 1;
}

static void hz_bits_align_byte(hz_bits_t *bits) {
    hz_u8 drop;
    if (!bits) return;
    drop = (hz_u8)(bits->bit_count & 7U);
    bits->bit_buffer >>= drop;
    bits->bit_count = (hz_u8)(bits->bit_count - drop);
}

static int hz_huffman_build(hz_huffman_t *table, const hz_u8 *lengths,
                            hz_u16 symbols, hz_u16 symbol_capacity) {
    hz_u16 offsets[HZ_MAX_BITS + 1];
    hz_i32 left = 1;
    hz_u16 i;
    hz_u8 len;

    if (!table || !lengths || symbols > symbol_capacity) return 0;
    for (i = 0; i <= HZ_MAX_BITS; ++i) table->count[i] = 0;
    for (i = 0; i < symbols; ++i) {
        if (lengths[i] > HZ_MAX_BITS) return 0;
        table->count[lengths[i]]++;
    }

    if (table->count[0] == symbols) return 0;
    for (len = 1; len <= HZ_MAX_BITS; ++len) {
        left <<= 1;
        left -= table->count[len];
        if (left < 0) return 0;
    }

    offsets[1] = 0;
    for (len = 1; len < HZ_MAX_BITS; ++len)
        offsets[len + 1] = (hz_u16)(offsets[len] + table->count[len]);

    for (i = 0; i < symbols; ++i) {
        len = lengths[i];
        if (len != 0U) table->symbol[offsets[len]++] = i;
    }
    return 1;
}

static int hz_huffman_decode(hz_bits_t *bits, const hz_huffman_t *table,
                             hz_u16 *symbol) {
    hz_u32 code = 0;
    hz_u32 first = 0;
    hz_u32 index = 0;
    hz_u8 len;

    if (!bits || !table || !symbol) return 0;
    for (len = 1; len <= HZ_MAX_BITS; ++len) {
        hz_u32 bit;
        hz_u32 count;
        if (!hz_bits_read(bits, 1, &bit)) return 0;
        code |= bit;
        count = table->count[len];
        if (code < first + count) {
            *symbol = table->symbol[index + (code - first)];
            return 1;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return 0;
}

static void hz_fixed_lengths(hz_u8 *litlen, hz_u8 *dist) {
    hz_u16 i;
    for (i = 0; i <= 143; ++i) litlen[i] = 8;
    for (; i <= 255; ++i) litlen[i] = 9;
    for (; i <= 279; ++i) litlen[i] = 7;
    for (; i <= 287; ++i) litlen[i] = 8;
    for (i = 0; i < 32; ++i) dist[i] = 5;
}

static int hz_dynamic_tables(hz_bits_t *bits, hz_huffman_t *litlen_table,
                             hz_huffman_t *dist_table) {
    static const hz_u8 order[HZ_MAX_CODELEN] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    hz_u8 code_lengths[HZ_MAX_CODELEN];
    hz_u8 all_lengths[HZ_MAX_CODE_LENGTHS];
    hz_huffman_t code_table;
    hz_u32 value;
    hz_u16 hlit, hdist, hclen;
    hz_u16 total;
    hz_u16 i;

    for (i = 0; i < HZ_MAX_CODELEN; ++i) code_lengths[i] = 0;
    for (i = 0; i < HZ_MAX_CODE_LENGTHS; ++i) all_lengths[i] = 0;

    if (!hz_bits_read(bits, 5, &value)) return HZ_INFLATE_TRUNCATED;
    hlit = (hz_u16)(value + 257U);
    if (!hz_bits_read(bits, 5, &value)) return HZ_INFLATE_TRUNCATED;
    hdist = (hz_u16)(value + 1U);
    if (!hz_bits_read(bits, 4, &value)) return HZ_INFLATE_TRUNCATED;
    hclen = (hz_u16)(value + 4U);
    if (hlit > 286U || hdist > 32U) return HZ_INFLATE_BAD_HUFFMAN;

    for (i = 0; i < hclen; ++i) {
        if (!hz_bits_read(bits, 3, &value)) return HZ_INFLATE_TRUNCATED;
        code_lengths[order[i]] = (hz_u8)value;
    }
    if (!hz_huffman_build(&code_table, code_lengths, HZ_MAX_CODELEN,
                          HZ_MAX_LITLEN))
        return HZ_INFLATE_BAD_HUFFMAN;

    total = (hz_u16)(hlit + hdist);
    i = 0;
    while (i < total) {
        hz_u16 symbol;
        if (!hz_huffman_decode(bits, &code_table, &symbol))
            return HZ_INFLATE_BAD_HUFFMAN;
        if (symbol <= 15U) {
            all_lengths[i++] = (hz_u8)symbol;
        } else if (symbol == 16U) {
            hz_u8 previous;
            hz_u16 repeat;
            if (i == 0) return HZ_INFLATE_BAD_HUFFMAN;
            previous = all_lengths[i - 1];
            if (!hz_bits_read(bits, 2, &value)) return HZ_INFLATE_TRUNCATED;
            repeat = (hz_u16)(value + 3U);
            if ((hz_u32)i + repeat > total) return HZ_INFLATE_BAD_HUFFMAN;
            while (repeat--) all_lengths[i++] = previous;
        } else if (symbol == 17U) {
            hz_u16 repeat;
            if (!hz_bits_read(bits, 3, &value)) return HZ_INFLATE_TRUNCATED;
            repeat = (hz_u16)(value + 3U);
            if ((hz_u32)i + repeat > total) return HZ_INFLATE_BAD_HUFFMAN;
            while (repeat--) all_lengths[i++] = 0;
        } else if (symbol == 18U) {
            hz_u16 repeat;
            if (!hz_bits_read(bits, 7, &value)) return HZ_INFLATE_TRUNCATED;
            repeat = (hz_u16)(value + 11U);
            if ((hz_u32)i + repeat > total) return HZ_INFLATE_BAD_HUFFMAN;
            while (repeat--) all_lengths[i++] = 0;
        } else {
            return HZ_INFLATE_BAD_HUFFMAN;
        }
    }

    if (all_lengths[256] == 0U) return HZ_INFLATE_BAD_HUFFMAN;
    if (!hz_huffman_build(litlen_table, all_lengths, hlit, HZ_MAX_LITLEN))
        return HZ_INFLATE_BAD_HUFFMAN;
    if (!hz_huffman_build(dist_table, all_lengths + hlit, hdist, HZ_MAX_LITLEN))
        return HZ_INFLATE_BAD_HUFFMAN;
    return HZ_INFLATE_OK;
}

static int hz_decode_compressed(hz_bits_t *bits, hz_u8 *destination,
                                hz_u32 capacity, hz_u32 *out_position,
                                const hz_huffman_t *litlen_table,
                                const hz_huffman_t *dist_table) {
    static const hz_u16 length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
        67,83,99,115,131,163,195,227,258
    };
    static const hz_u8 length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,
        4,4,4,4,5,5,5,5,0
    };
    static const hz_u16 dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,
        16385,24577
    };
    static const hz_u8 dist_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };

    for (;;) {
        hz_u16 symbol;
        if (!hz_huffman_decode(bits, litlen_table, &symbol))
            return HZ_INFLATE_BAD_HUFFMAN;
        if (symbol < 256U) {
            if (*out_position >= capacity) return HZ_INFLATE_OUTPUT_FULL;
            destination[(*out_position)++] = (hz_u8)symbol;
        } else if (symbol == 256U) {
            return HZ_INFLATE_OK;
        } else if (symbol >= 257U && symbol <= 285U) {
            hz_u16 length_index = (hz_u16)(symbol - 257U);
            hz_u32 extra = 0;
            hz_u32 length = length_base[length_index];
            hz_u16 distance_symbol;
            hz_u32 distance;
            hz_u32 i;

            if (length_extra[length_index] &&
                !hz_bits_read(bits, length_extra[length_index], &extra))
                return HZ_INFLATE_TRUNCATED;
            length += extra;
            if (!hz_huffman_decode(bits, dist_table, &distance_symbol))
                return HZ_INFLATE_BAD_HUFFMAN;
            if (distance_symbol >= 30U) return HZ_INFLATE_BAD_DISTANCE;
            extra = 0;
            distance = dist_base[distance_symbol];
            if (dist_extra[distance_symbol] &&
                !hz_bits_read(bits, dist_extra[distance_symbol], &extra))
                return HZ_INFLATE_TRUNCATED;
            distance += extra;
            if (distance == 0U || distance > *out_position)
                return HZ_INFLATE_BAD_DISTANCE;
            if (length > capacity - *out_position)
                return HZ_INFLATE_OUTPUT_FULL;
            for (i = 0; i < length; ++i) {
                destination[*out_position] = destination[*out_position - distance];
                (*out_position)++;
            }
        } else {
            return HZ_INFLATE_BAD_HUFFMAN;
        }
    }
}

int hz_inflate_raw(const hz_u8 *source, hz_u32 source_size,
                   hz_u8 *destination, hz_u32 destination_capacity,
                   hz_u32 *written) {
    hz_bits_t bits;
    hz_u32 out_position = 0;
    hz_u32 final_block = 0;

    if (written) *written = 0;
    if ((!source && source_size) || (!destination && destination_capacity))
        return HZ_INFLATE_BAD_ARGUMENT;

    bits.source = source;
    bits.size = source_size;
    bits.position = 0;
    bits.bit_buffer = 0;
    bits.bit_count = 0;

    while (!final_block) {
        hz_u32 block_type;
        if (!hz_bits_read(&bits, 1, &final_block) ||
            !hz_bits_read(&bits, 2, &block_type))
            return HZ_INFLATE_TRUNCATED;

        if (block_type == 0U) {
            hz_u32 len, nlen;
            hz_u32 i;
            hz_bits_align_byte(&bits);
            if (!hz_bits_read(&bits, 16, &len) ||
                !hz_bits_read(&bits, 16, &nlen))
                return HZ_INFLATE_TRUNCATED;
            if (((len ^ 0xFFFFU) & 0xFFFFU) != (nlen & 0xFFFFU))
                return HZ_INFLATE_BAD_BLOCK;
            if (len > destination_capacity - out_position)
                return HZ_INFLATE_OUTPUT_FULL;
            for (i = 0; i < len; ++i) {
                hz_u32 byte;
                if (!hz_bits_read(&bits, 8, &byte)) return HZ_INFLATE_TRUNCATED;
                destination[out_position++] = (hz_u8)byte;
            }
        } else if (block_type == 1U || block_type == 2U) {
            hz_u8 litlen_lengths[HZ_MAX_LITLEN];
            hz_u8 dist_lengths[HZ_MAX_DIST];
            hz_huffman_t litlen_table;
            hz_huffman_t dist_table;
            int result;

            if (block_type == 1U) {
                hz_fixed_lengths(litlen_lengths, dist_lengths);
                if (!hz_huffman_build(&litlen_table, litlen_lengths,
                                      HZ_MAX_LITLEN, HZ_MAX_LITLEN) ||
                    !hz_huffman_build(&dist_table, dist_lengths,
                                      HZ_MAX_DIST, HZ_MAX_LITLEN))
                    return HZ_INFLATE_BAD_HUFFMAN;
            } else {
                result = hz_dynamic_tables(&bits, &litlen_table, &dist_table);
                if (result != HZ_INFLATE_OK) return result;
            }

            result = hz_decode_compressed(&bits, destination,
                                          destination_capacity, &out_position,
                                          &litlen_table, &dist_table);
            if (result != HZ_INFLATE_OK) return result;
        } else {
            return HZ_INFLATE_BAD_BLOCK;
        }
    }

    if (written) *written = out_position;
    return HZ_INFLATE_OK;
}
