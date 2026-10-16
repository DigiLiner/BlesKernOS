/* SPDX-License-Identifier: MIT */
/* C-Media CMI8338/8738: PCI IDs, codec and playback registers. */
#include "legacy_pcm.h"
#define LEGACY_PCM_NAME "C-Media CMI8338/8738"
#define LEGACY_PCM_RATE 48000U

static bool identify(const pci_device_t *d) {
    if (d->class_code != 4 || d->subclass != 1)
        return false;
    return d->vendor_id == 0x13f6 && (d->device_id == 0x0100 || d->device_id == 0x0101 ||
                                      d->device_id == 0x0111 || d->device_id == 0x0112);
}

static bool codec_init(void) {
    audio_write(0, 0x000c0000);
    audio_write(0, 0);
    audio_write(0x0c, 0);
    audio_write(4, 0x10);
    audio_write(0x18, audio_read(0x18) & ~0x00c00000U);
    legacy_out8(audio.io + 0x23, 0x30);
    legacy_out8(audio.io + 0x22, 0xf8);
    legacy_out8(audio.io + 0x23, 0x31);
    legacy_out8(audio.io + 0x22, 0xf8);
    legacy_out8(audio.io + 0x23, 0x32);
    legacy_out8(audio.io + 0x22, 0xf8);
    legacy_out8(audio.io + 0x23, 0x33);
    legacy_out8(audio.io + 0x22, 0xf8);
    return (audio_read(0) & 0xffffffffU) != 0xffffffffU;
}

static void hardware_stop(void) {
    audio_write(0, audio_read(0) & ~0x30000U);
    audio_write(0, audio_read(0) | 0x40000U);
    audio_write(0, audio_read(0) & ~0x40000U);
}

static void hardware_program_dma(uint32_t frames) {
    uint32_t data = (uint32_t)((uintptr_t)(audio.dma.data + PCM_OFFSET));
    legacy_barrier();
    audio_write(4, (audio_read(4) & ~0x1c00U) | (7U << 10) | 0x10);
    audio_write(8, (audio_read(8) & ~3U) | 3);
    audio_write(0x80, data);
    audio_write(0x84, (frames - 1) | ((frames - 1) << 16));
    audio_write(0, audio_read(0) | 0x10000);
}

static bool map_bars(void) {
    bool ok = legacy_io_bar(audio.pci, 0, 0x100, &audio.io);
    return ok;
}

#include "legacy_pcm_runtime.inc"
