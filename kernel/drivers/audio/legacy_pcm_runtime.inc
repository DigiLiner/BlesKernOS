/* SPDX-License-Identifier: MIT */
/* Shared copied PCM conversion, tone generation, locks and lifecycle. */
static void stop_locked(void) {
    if (!audio.pci)
        return;
    hardware_stop();
    legacy_barrier();
    audio.busy = false;
}
static void stop(void) {
    while (!__sync_bool_compare_and_swap(&audio.lock, 0, 1))
        __asm__ volatile("pause");
    stop_locked();
    __sync_lock_release(&audio.lock);
}
static void poll(void) {
    if (audio.busy && (int32_t)(pit_get_ticks() - audio.end_tick) >= 0 &&
        __sync_bool_compare_and_swap(&audio.lock, 0, 1)) {
        stop_locked();
        __sync_lock_release(&audio.lock);
    }
}
static uint32_t output_rate(void) { return LEGACY_PCM_RATE; }
static void program_dma(uint32_t frames) {
    hardware_program_dma(frames);
    uint32_t hz = pit_get_frequency_hz();
    if (!hz)
        hz = 100;
    audio.end_tick = pit_get_ticks() + (frames * hz + output_rate() - 1) / output_rate() + 1;
    audio.busy = true;
}
static bool pcm(const uint8_t *samples, uint32_t length, uint16_t rate, uint8_t volume) {
    if (!audio.ready || !samples || !length || rate < 4000 || rate > 48000 ||
        !__sync_bool_compare_and_swap(&audio.lock, 0, 1))
        return false;
    uint64_t frames64 = ((uint64_t)length * output_rate() + rate - 1) / rate;
    if (frames64 > PCM_FRAMES) {
        __sync_lock_release(&audio.lock);
        return false;
    }
    stop_locked();
    uint32_t frames = (uint32_t)frames64;
    int16_t *dst = (int16_t *)(audio.dma.data + PCM_OFFSET);
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t source = (uint32_t)((uint64_t)i * rate / output_rate());
        if (source >= length)
            source = length - 1;
        int32_t value = ((int32_t)samples[source] - 128) * 256 * volume / 255;
        dst[2 * i] = dst[2 * i + 1] = (int16_t)value;
    }
    program_dma(frames);
    __sync_lock_release(&audio.lock);
    return true;
}
static bool start_tone(uint32_t frequency, uint32_t ms) {
    if (!audio.ready || frequency < 20 || frequency > 20000 || !ms || ms > 1000 ||
        !__sync_bool_compare_and_swap(&audio.lock, 0, 1))
        return false;
    stop_locked();
    uint32_t frames = output_rate() * ms / 1000;
    int16_t *dst = (int16_t *)(audio.dma.data + PCM_OFFSET);
    uint32_t phase = 0, step = (uint32_t)((uint64_t)frequency * 0x100000000ULL / output_rate());
    for (uint32_t i = 0; i < frames; i++) {
        int16_t value = (phase & 0x80000000U) ? 4096 : -4096;
        dst[2 * i] = dst[2 * i + 1] = value;
        phase += step;
    }
    program_dma(frames);
    __sync_lock_release(&audio.lock);
    return true;
}
static void play(uint32_t hz) { (void)start_tone(hz, 1000); }
static void beep(uint32_t hz, uint32_t ms) { (void)start_tone(hz, ms); }
static bool available(void) { return audio.ready; }
static bool busy(void) {
    poll();
    return audio.busy;
}
static bool has_sb16(void) { return false; }
static bool sb_tone(uint32_t hz, uint32_t ms) { return start_tone(hz, ms); }
static const char *pcm_name(void) { return LEGACY_PCM_NAME; }
static void shutdown_audio(void) {
    if (!audio.pci)
        return;
    stop();
    audio.ready = false;
    pci_config_write16(audio.pci->bus, audio.pci->slot, audio.pci->function, 4,
                       audio.saved_command & ~PCI_COMMAND_BUSMASTER);
    codec_delay();
    if (!(pci_config_read16(audio.pci->bus, audio.pci->slot, audio.pci->function, 4) &
          PCI_COMMAND_BUSMASTER))
        legacy_dma_free(&audio.dma);
    else
        kprintf("[LEGACYAUDIO] DMA quarantined: bus master could not be disabled\n");
    pci_config_write16(audio.pci->bus, audio.pci->slot, audio.pci->function, 4,
                       audio.saved_command & ~PCI_COMMAND_BUSMASTER);
}
static bool driver_init(void) {
    kmemset(&audio, 0, sizeof(audio));
    for (unsigned i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d && identify(d)) {
            audio.pci = d;
            break;
        }
    }
    if (!audio.pci)
        return false;
    audio.saved_command =
        pci_config_read16(audio.pci->bus, audio.pci->slot, audio.pci->function, 4);
    bool ok = map_bars();
    if (!ok || !pci_enable_command(audio.pci, (audio.mmio ? PCI_COMMAND_MEMORY : PCI_COMMAND_IO) |
                                                  PCI_COMMAND_BUSMASTER))
        return false;
    if (!legacy_dma_alloc(&audio.dma, DMA_BYTES) || !codec_init()) {
        shutdown_audio();
        return false;
    }
    static sound_driver_ops_t ops = {poll, play,     stop,    beep, start_tone, pcm, available,
                                     busy, has_sb16, sb_tone, busy, pcm_name,   200};
    audio.ready = true;
    if (!sound_register_driver(&ops)) {
        shutdown_audio();
        return false;
    }
    kprintf("[LEGACYAUDIO] %s PCI %x:%x PCM stereo16 %u Hz (DMA32/poll)\n", LEGACY_PCM_NAME,
            audio.pci->vendor_id, audio.pci->device_id, output_rate());
    return true;
}
const bk_driver_module_t *bleskernos_driver_query(void) {
    static bk_driver_module_t module = {BK_DRIVER_ABI_VERSION,
                                        sizeof(bk_driver_module_t),
                                        1,
                                        0,
                                        NULL,
                                        "Legacy PCI PCM, supervisor DMA, copied/resampled u8 input",
                                        driver_init,
                                        shutdown_audio};
    module.name = LEGACY_PCM_NAME;
    return &module;
}
