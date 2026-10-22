#ifndef HZ_CORE_H
#define HZ_CORE_H

typedef unsigned char      hz_u8;
typedef unsigned short     hz_u16;
typedef unsigned int       hz_u32;
typedef signed int         hz_i32;

#ifndef HZ_NULL
#define HZ_NULL ((void *)0)
#endif

typedef enum {
    HZ_INFLATE_OK = 0,
    HZ_INFLATE_BAD_ARGUMENT = -1,
    HZ_INFLATE_TRUNCATED = -2,
    HZ_INFLATE_BAD_BLOCK = -3,
    HZ_INFLATE_BAD_HUFFMAN = -4,
    HZ_INFLATE_OUTPUT_FULL = -5,
    HZ_INFLATE_BAD_DISTANCE = -6
} hz_inflate_result_t;

hz_u32 hz_crc32(const void *data, hz_u32 size);
int hz_inflate_raw(const hz_u8 *source, hz_u32 source_size,
                   hz_u8 *destination, hz_u32 destination_capacity,
                   hz_u32 *written);

#endif
