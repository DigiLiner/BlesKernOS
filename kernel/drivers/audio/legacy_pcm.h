#ifndef BK_LEGACY_PCM_H
#define BK_LEGACY_PCM_H
/* SPDX-License-Identifier: MIT
 * Mono-u8 input copied/resampled into supervisor DMA stereo-s16 storage.
 * Playback registers studied in Linux ALSA; implementation and ABI are local.
 */
#include "../../include/driver.h"
#include "../../include/sound.h"
#include "../../include/task.h"
#include "../../include/vga.h"
#include "../legacy_io.h"
#define PCM_FRAMES 65536U
#define PCM_OFFSET 4096U
#define DMA_BYTES (PCM_OFFSET + PCM_FRAMES * 4U)
static struct {
    const pci_device_t *pci;
    uint16_t io, saved_command;
    volatile uint8_t *mmio;
    legacy_dma_t dma;
    volatile uint32_t lock;
    bool ready, busy;
    uint32_t end_tick;
} audio;
static inline uint32_t audio_read(uint32_t off) {
    return audio.mmio ? *(volatile uint32_t *)(audio.mmio + off)
                      : legacy_in32(audio.io + (uint16_t)off);
}
static inline void audio_write(uint32_t off, uint32_t value) {
    if (audio.mmio)
        *(volatile uint32_t *)(audio.mmio + off) = value;
    else
        legacy_out32(audio.io + (uint16_t)off, value);
}
static inline bool audio_wait(uint32_t off, uint32_t mask, uint32_t value) {
    for (unsigned i = 0; i < 500000U; i++) {
        if ((audio_read(off) & mask) == value)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}
static void codec_delay(void) {
    for (unsigned i = 0; i < 10000; i++)
        __asm__ volatile("pause");
}

#endif
