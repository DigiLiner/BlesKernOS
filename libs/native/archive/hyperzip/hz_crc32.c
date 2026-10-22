#include "hz_core.h"

hz_u32 hz_crc32(const void *data, hz_u32 size) {
    const hz_u8 *bytes = (const hz_u8 *)data;
    hz_u32 crc = 0xFFFFFFFFU;
    hz_u32 i;

    for (i = 0; i < size; ++i) {
        hz_u32 value = crc ^ bytes[i];
        hz_u32 bit;
        for (bit = 0; bit < 8U; ++bit) {
            hz_u32 mask = (hz_u32)-(hz_i32)(value & 1U);
            value = (value >> 1) ^ (0xEDB88320U & mask);
        }
        crc = value;
    }
    return ~crc;
}
