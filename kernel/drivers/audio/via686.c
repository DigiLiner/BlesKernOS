/* SPDX-License-Identifier: MIT */
/* VIA VT82C686 AC97: PCI IDs, codec and playback registers. */
#include "legacy_pcm.h"
#define LEGACY_PCM_NAME "VIA VT82C686 AC97"
#define LEGACY_PCM_RATE 48000U

static void config_byte(uint8_t offset, uint8_t value) {
    uint8_t aligned = offset & 0xfc;
    uint32_t shift = (offset & 3) * 8;
    uint32_t old = pci_config_read32(audio.pci->bus, audio.pci->slot, audio.pci->function, aligned);
    pci_config_write32(audio.pci->bus, audio.pci->slot, audio.pci->function, aligned,
                       (old & ~(0xffU << shift)) | ((uint32_t)value << shift));
}

static bool identify(const pci_device_t *d) {
    if (d->class_code != 4 || d->subclass != 1)
        return false;
    return d->vendor_id == 0x1106 && d->device_id == 0x3058;
}

static bool codec_write(uint8_t reg, uint16_t value) {
    if (!audio_wait(0x80, 1U << 24, 0))
        return false;
    audio_write(0x80, ((uint32_t)reg << 16) | value);
    return audio_wait(0x80, 1U << 24, 0);
}

static bool codec_init(void) {
    config_byte(0x41, 0);
    codec_delay();
    config_byte(0x41, 0xc0);
    codec_delay();
    config_byte(0x41, 0xe0);
    codec_delay();
    bool ready = false;
    for (unsigned i = 0; i < 500000; i++)
        if (pci_config_read32(audio.pci->bus, audio.pci->slot, audio.pci->function, 0x40) & 1) {
            ready = true;
            break;
        }

    if (!ready)
        return false;
    legacy_out8(audio.io + 1, 0x40);
    legacy_out8(audio.io, 7);
    return codec_write(0x26, 0) && codec_write(2, 0) && codec_write(4, 0) &&
           codec_write(0x18, 0x0808);
}

static void hardware_stop(void) { legacy_out8(audio.io + 1, 0x40); }

static void hardware_program_dma(uint32_t frames) {
    uint32_t data = (uint32_t)((uintptr_t)(audio.dma.data + PCM_OFFSET));
    legacy_barrier();
    uint32_t *table = (uint32_t *)audio.dma.data;
    table[0] = data;
    table[1] = 0x80000000U | (frames * 4U);
    audio_write(4, (uint32_t)((uintptr_t)table));
    legacy_out8(audio.io + 2, 0x30);
    legacy_out8(audio.io, 7);
    legacy_out8(audio.io + 1, 0x80);
}

static bool map_bars(void) {
    bool ok = legacy_io_bar(audio.pci, 0, 0x100, &audio.io);
    return ok;
}

#include "legacy_pcm_runtime.inc"
