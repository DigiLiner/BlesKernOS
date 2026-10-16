/* SPDX-License-Identifier: MIT. HP download-container validation, not firmware. */
#ifndef BK_HP_FIRMWARE_H
#define BK_HP_FIRMWARE_H
#include "../../include/types.h"
static const char *hp_firmware_path(uint16_t vendor, uint16_t product) {
    if (vendor != 0x03f0)
        return NULL;
    switch (product) {
    case 0x0517:
        return "/SYSTEM/FIRMWARE/SIHP1000.DL";
    case 0x1317:
        return "/SYSTEM/FIRMWARE/SIHP1005.DL";
    case 0x4117:
        return "/SYSTEM/FIRMWARE/SIHP1018.DL";
    case 0x2b17:
        return "/SYSTEM/FIRMWARE/SIHP1020.DL";
    default:
        return NULL;
    }
}
static bool hp_firmware_valid(const uint8_t *data, uint32_t size) {
    static const uint8_t magic[] = {0xbe, 0xef, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    if (!data || size < 33 || size > 2U * 1024U * 1024U)
        return false;
    for (unsigned i = 0; i < sizeof(magic); i++)
        if (data[i] != magic[i])
            return false;
    uint32_t payload = ((uint32_t)data[10] << 24) | ((uint32_t)data[11] << 16) |
                       ((uint32_t)data[12] << 8) | data[13];
    return payload == size - 17U && data[17] == 0xc0 && data[18] == 0xde;
}
static bool hp_firmware_ready_id(const char *id) {
    if (!id)
        return false;
    for (unsigned i = 0; id[i]; i++)
        if (id[i] == 'F' && id[i + 1] == 'W' && id[i + 2] == 'V' && id[i + 3] == 'E' &&
            id[i + 4] == 'R' && id[i + 5] == ':')
            return true;
    return false;
}
#endif
