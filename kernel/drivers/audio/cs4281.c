/* SPDX-License-Identifier: MIT */
/* Cirrus CS4281: PCI IDs, codec and playback registers. */
#include "legacy_pcm.h"
#define LEGACY_PCM_NAME "Cirrus CS4281"
#define LEGACY_PCM_RATE 48000U

static bool identify(const pci_device_t *d) {
    if (d->class_code != 4 || d->subclass != 1)
        return false;
    return d->vendor_id == 0x1013 && d->device_id == 0x6005;
}

static bool codec_write(uint8_t reg, uint16_t value) {
    if (!audio_wait(0x460, 8, 0))
        return false;
    audio_write(0x46c, reg);
    audio_write(0x470, value);
    audio_write(0x460, 14);
    return audio_wait(0x460, 8, 0);
}

static bool codec_init(void) {
    audio_write(0x3e0, 0x4281);
    audio_write(0x3f0, 1);
    if (audio_read(0x3f0) != 1)
        return false;
    if (audio_read(0x428) != 3 || audio_read(0x42c) != 3)
        return false;
    audio_write(0x740, 0x74);
    audio_write(0x400, 0);
    audio_write(0x420, 0);
    audio_write(0x460, 0);
    audio_write(0x3ec, 0);
    codec_delay();
    audio_write(0x3ec, 1);
    codec_delay();
    audio_write(0x420, 0x10003);
    audio_write(0x400, 0x10);
    codec_delay();
    audio_write(0x400, 0x30);
    if (!audio_wait(0x400, 1U << 24, 1U << 24))
        return false;
    audio_write(0x460, 2);
    if (!audio_wait(0x464, 1, 1))
        return false;
    audio_write(0x460, 6);
    if (!audio_wait(0x474, 3, 3))
        return false;
    audio_write(0x468, 3);
    audio_write(0x75c, 0x0b0a0100);
    audio_write(0x760, 0);
    audio_write(0x764, 0);
    audio_write(0xc, 0xffffffffU);
    audio_write(8, 2);
    return codec_write(0x26, 0) && codec_write(2, 0) && codec_write(4, 0) &&
           codec_write(0x18, 0x0808);
}

static void hardware_stop(void) {
    audio_write(0x154, 1);
    audio_write(0x150, 0);
}

static void hardware_program_dma(uint32_t frames) {
    uint32_t data = (uint32_t)((uintptr_t)(audio.dma.data + PCM_OFFSET));
    legacy_barrier();
    audio_write(0x118, data);
    audio_write(0x11c, frames - 1);
    audio_write(0x744, 0);
    audio_write(0x180, 0x81002000);
    audio_write(0x150, 0x20000048);
    audio_write(0x154, 0);
}

static bool map_bars(void) {
    bool ok = legacy_mmio_bar(audio.pci, 0, 0x1000, &audio.mmio);
    return ok;
}

#include "legacy_pcm_runtime.inc"
