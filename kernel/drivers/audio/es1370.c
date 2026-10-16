/* SPDX-License-Identifier: MIT */
/* Ensoniq AudioPCI ES1370: PCI IDs, codec and playback registers. */
#include "legacy_pcm.h"
#define LEGACY_PCM_NAME "Ensoniq AudioPCI ES1370"
#define LEGACY_PCM_RATE 44100U

static bool identify(const pci_device_t *d) {
    if (d->class_code != 4 || d->subclass != 1)
        return false;
    return d->vendor_id == 0x1274 && d->device_id == 0x5000;
}

static bool codec_write(uint8_t reg, uint16_t value) {
    if (!audio_wait(4, 0x400, 0))
        return false;
    legacy_out16(audio.io + 0x10, (uint16_t)((reg << 8) | (value & 255)));
    return audio_wait(4, 0x400, 0);
}

static bool codec_init(void) {
    audio_write(0, 0x00000003U);
    audio_write(0x20, 0);
    if (!codec_write(0x16, 2))
        return false;
    codec_delay();
    if (!codec_write(0x16, 3))
        return false;
    codec_delay();
    if (!codec_write(0, 0) || !codec_write(1, 0) || !codec_write(2, 0) || !codec_write(3, 0) ||
        !codec_write(0x10, 0x40) || !codec_write(0x11, 0))
        return false;
    return true;
}

static void hardware_stop(void) { audio_write(0, audio_read(0) & ~0x70U); }

static void hardware_program_dma(uint32_t frames) {
    uint32_t data = (uint32_t)((uintptr_t)(audio.dma.data + PCM_OFFSET));
    legacy_barrier();
    audio_write(0x0c, 0x0c);
    audio_write(0x38, data);
    audio_write(0x3c, frames - 1);
    audio_write(0x28, frames - 1);
    audio_write(0x20, (1U << 14) | (2U << 19) | 12U);
    audio_write(0, (30U << 16) | 0x23U);
}

static bool map_bars(void) {
    bool ok = legacy_io_bar(audio.pci, 0, 0x100, &audio.io);
    return ok;
}

#include "legacy_pcm_runtime.inc"
