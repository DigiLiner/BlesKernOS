#include "../../include/driver.h"
#include "../../include/memory.h"
#include "../../include/pci.h"
#include "../../include/paging.h"
#include "../../include/pic.h"
#include "../../include/pit.h"
#include "../../include/sound.h"
#include "../../include/task.h"
#include "../../include/vga.h"

#define AC97_NAM_RESET       0x00U
#define AC97_NAM_MASTER_VOL  0x02U
#define AC97_NAM_PCM_VOL     0x18U
#define AC97_NAM_VENDOR1     0x7CU
#define AC97_NAM_VENDOR2     0x7EU

#define AC97_PO_BDBAR        0x10U
#define AC97_PO_CIV          0x14U
#define AC97_PO_LVI          0x15U
#define AC97_PO_SR           0x16U
#define AC97_PO_SIS_SR       0x18U
#define AC97_PO_CR           0x1BU
#define AC97_GLOB_CNT        0x2CU
#define AC97_GLOB_STA        0x30U

#define AC97_SR_DCH          0x0001U
#define AC97_SR_FIFOE        0x0010U
#define AC97_SR_CLEAR        0x001CU
#define AC97_CR_RUN          0x01U
#define AC97_CR_RESET        0x02U
#define AC97_GLOB_COLD       0x00000002U
#define AC97_GLOB_PCR        0x00000100U

#define AC97_BDL_COUNT       32U
#define AC97_BUFFER_FRAMES   2048U
#define AC97_PRELOAD_SHORT   4U
#define AC97_PRELOAD_LONG    12U
#define AC97_REFILL_WATERMARK 2U /* ~85 ms de mix-ahead a 48 kHz */
#define AC97_OUTPUT_HZ       48000U
#define AC97_MIX_VOICES      8U
#define AC97_VOICE_MAX_BYTES  65536U
#define AC97_BDL_BUP         0x4000U
#define AC97_TONE_MAX_MS     1000U
#define AC97_DIRECT_MIN_FRAMES 1U /* GL117 audiofix5: 22050 Hz direct-only safe path */

typedef struct PACKED {
    uint32_t address;
    uint16_t samples;
    uint16_t control;
} ac97_bdl_entry_t;

typedef struct {
    /* Copy short PCM into kernel-owned storage. Ring-3 callers may be
     * preempted or exit while the AC97 worker is still rendering ahead;
     * never retain a user pointer in an asynchronous kernel voice. */
    uint8_t samples[AC97_VOICE_MAX_BYTES];
    uint32_t length;
    uint64_t position_fp;
    uint32_t step_fp;
    uint8_t volume;
    volatile bool active;
} ac97_voice_t;

typedef enum {
    AC97_CONTROLLER_ICH = 0,
    AC97_CONTROLLER_NFORCE,
    AC97_CONTROLLER_SIS7012
} ac97_controller_type_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    ac97_controller_type_t type;
    const char *name;
} ac97_controller_id_t;

/* Motores PCM bus-master derivados de Intel ICH. Tener un codec AC'97 no
 * basta: ALi, VIA y ESS usan registros/DSP diferentes. */
static const ac97_controller_id_t g_ac97_controller_ids[] = {
    {0x8086, 0x2415, AC97_CONTROLLER_ICH, "Intel ICH"},
    {0x8086, 0x2425, AC97_CONTROLLER_ICH, "Intel ICH0"},
    {0x8086, 0x2445, AC97_CONTROLLER_ICH, "Intel ICH2"},
    {0x8086, 0x2485, AC97_CONTROLLER_ICH, "Intel ICH3"},
    {0x8086, 0x24C5, AC97_CONTROLLER_ICH, "Intel ICH4"},
    {0x8086, 0x24D5, AC97_CONTROLLER_ICH, "Intel ICH5"},
    {0x8086, 0x25A6, AC97_CONTROLLER_ICH, "Intel 6300ESB"},
    {0x8086, 0x266E, AC97_CONTROLLER_ICH, "Intel ICH6"},
    {0x8086, 0x27DE, AC97_CONTROLLER_ICH, "Intel ICH7"},
    {0x8086, 0x2698, AC97_CONTROLLER_ICH, "Intel ESB2"},
    {0x8086, 0x7195, AC97_CONTROLLER_ICH, "Intel 440MX"},
    {0x1022, 0x7445, AC97_CONTROLLER_ICH, "AMD-768"},
    {0x1022, 0x746D, AC97_CONTROLLER_ICH, "AMD-8111"},
    {0x10DE, 0x01B1, AC97_CONTROLLER_NFORCE, "NVIDIA nForce"},
    {0x10DE, 0x003A, AC97_CONTROLLER_NFORCE, "NVIDIA MCP04"},
    {0x10DE, 0x006A, AC97_CONTROLLER_NFORCE, "NVIDIA nForce2"},
    {0x10DE, 0x0059, AC97_CONTROLLER_NFORCE, "NVIDIA CK804"},
    {0x10DE, 0x008A, AC97_CONTROLLER_NFORCE, "NVIDIA CK8"},
    {0x10DE, 0x00DA, AC97_CONTROLLER_NFORCE, "NVIDIA nForce3"},
    {0x10DE, 0x00EA, AC97_CONTROLLER_NFORCE, "NVIDIA CK8S"},
    {0x10DE, 0x026B, AC97_CONTROLLER_NFORCE, "NVIDIA MCP51"},
    {0x1039, 0x7012, AC97_CONTROLLER_SIS7012, "SiS 7012"}
};

static const pci_device_t *g_ac97_pci;
static const ac97_controller_id_t *g_ac97_controller;
static uint32_t g_ac97_nam;
static uint32_t g_ac97_nabm;
static bool g_ac97_mmio;
static uint16_t g_ac97_po_sr = AC97_PO_SR;
static ac97_bdl_entry_t g_ac97_bdl[AC97_BDL_COUNT]
    __attribute__((aligned(128)));
static int16_t g_ac97_buffers[AC97_BDL_COUNT][AC97_BUFFER_FRAMES * 2U]
    __attribute__((aligned(4096)));
static ac97_voice_t g_ac97_voices[AC97_MIX_VOICES];
static uint8_t g_ac97_tone[AC97_OUTPUT_HZ]
    __attribute__((aligned(16)));
static volatile bool g_ac97_present;
static volatile bool g_ac97_streaming;
static bool g_ac97_observed_running;
static uint32_t g_ac97_start_tick;
static uint32_t g_ac97_producer_seq;
static uint32_t g_ac97_current_seq;
static uint8_t g_ac97_last_civ;
static uint32_t g_ac97_fifo_errors;
static uint32_t g_ac97_dch_restarts;
static uint32_t g_ac97_start_retries;
static bool g_ac97_continuous_ring_reported;
static bool g_ac97_short_ring_started_reported;
static bool g_ac97_short_ring_done_reported;
static bool g_ac97_direct_reported;
static int16_t *g_ac97_direct_buffer;
static volatile bool g_ac97_direct_active;

/*
 * AC97 state is shared by the PCM API caller and the ac97-mixer worker.
 * task_preempt_disable() only protects the current CPU, so it is insufficient
 * once BlesKernOS runs the scheduler on several CPUs.  Keep the whole DMA /
 * voice state behind a tiny cross-CPU spin lock.  None of these critical
 * sections sleep; the only comparatively long path is the one-time direct
 * conversion of a WAV, which is still bounded and avoids exposing half-
 * initialized DMA state to another CPU.
 */
static volatile uint32_t g_ac97_state_lock;

static void ac97_state_lock(void) {
    /* A task must never be scheduled away while owning this raw SMP lock.
     * Otherwise ac97-mixer can run on the same CPU and spin waiting for a
     * lock whose owner cannot resume. */
    task_preempt_disable();
    while (__sync_lock_test_and_set(&g_ac97_state_lock, 1U)) {
        while (g_ac97_state_lock)
            __asm__ volatile ("pause" : : : "memory");
    }
    __asm__ volatile ("" : : : "memory");
}

static void ac97_state_unlock(void) {
    __asm__ volatile ("" : : : "memory");
    __sync_lock_release(&g_ac97_state_lock);
    task_preempt_enable();
}

/*
 * El buffer de DMA directo puede ser finalizado por ac97_worker() al mismo
 * tiempo que sound_stop() lo cancela desde otra CPU.  El xchg de 32 bits es
 * atomico en i486+ y transfiere la propiedad del buffer a un solo liberador.
 */
static int16_t *ac97_take_direct_buffer(void) {
    int16_t *buffer = (int16_t *)__sync_lock_test_and_set(
        &g_ac97_direct_buffer, (int16_t *)0);
    g_ac97_direct_active = false;
    return buffer;
}

static void ac97_channel_reset(void);
static uint16_t ac97_descriptor_length(uint32_t frames);
static uint32_t ac97_max_descriptor_frames(void);

/* ICH4 exposes native memory BAR2/BAR3 as well as legacy I/O BAR0/BAR1.
 * Keep register offsets identical; never truncate an MMIO address to 16 bits. */
static uint8_t ac97_inb(uint32_t address) {
    if(g_ac97_mmio)return *(volatile uint8_t *)(uintptr_t)address;
    return inb((uint16_t)address);
}
static uint16_t ac97_inw(uint32_t address) {
    if(g_ac97_mmio)return *(volatile uint16_t *)(uintptr_t)address;
    uint16_t value;
    __asm__ volatile("inw %1, %0":"=a"(value):"Nd"((uint16_t)address));
    return value;
}
static uint32_t ac97_inl(uint32_t address) {
    if(g_ac97_mmio)return *(volatile uint32_t *)(uintptr_t)address;
    return inl((uint16_t)address);
}
static void ac97_outb(uint32_t address,uint8_t value) {
    if(g_ac97_mmio)*(volatile uint8_t *)(uintptr_t)address=value;
    else outb((uint16_t)address,value);
}
static void ac97_outw(uint32_t address,uint16_t value) {
    if(g_ac97_mmio)*(volatile uint16_t *)(uintptr_t)address=value;
    else __asm__ volatile("outw %0, %1"::"a"(value),"Nd"((uint16_t)address));
}
static void ac97_outl(uint32_t address,uint32_t value) {
    if(g_ac97_mmio)*(volatile uint32_t *)(uintptr_t)address=value;
    else outl((uint16_t)address,value);
}

static inline void ac97_io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static inline void ac97_dma_publish(void) {
    /* Publicar contenido/BDL antes de que el dispositivo vea un LVI nuevo. */
    __asm__ volatile ("" : : : "memory");
}

/*
 * Intel ICH permite extender/rearmar una lista detenida escribiendo LVI
 * mientras RPBM sigue activo. QEMU implementa la misma semantica. El kick
 * evita esperar al timeout si el motor queda DCH al arrancar.
 */
static void ac97_run_with_lvi_kick(uint8_t lvi) {
    ac97_outb(g_ac97_nabm + AC97_PO_CR, AC97_CR_RUN);
    if (ac97_inw((g_ac97_nabm + g_ac97_po_sr)) & AC97_SR_DCH)
        ac97_outb(g_ac97_nabm + AC97_PO_LVI, lvi);
}

static uint32_t ac97_ring_queued(void) {
    if (g_ac97_producer_seq < g_ac97_current_seq) return 0;
    return g_ac97_producer_seq - g_ac97_current_seq;
}

static void ac97_prepare_ring_bdl(void) {
    for (uint32_t i = 0; i < AC97_BDL_COUNT; i++) {
        g_ac97_bdl[i].address =
            (uint32_t)(uintptr_t)g_ac97_buffers[i];
        g_ac97_bdl[i].samples = ac97_descriptor_length(AC97_BUFFER_FRAMES);
        g_ac97_bdl[i].control = AC97_BDL_BUP;
    }
    ac97_dma_publish();
}

static const ac97_controller_id_t *ac97_match_controller(
        const pci_device_t *device) {
    if (!device) return NULL;
    for (uint32_t i = 0; i < sizeof(g_ac97_controller_ids) /
                                  sizeof(g_ac97_controller_ids[0]); i++) {
        const ac97_controller_id_t *id = &g_ac97_controller_ids[i];
        if (device->vendor_id == id->vendor_id &&
            device->device_id == id->device_id) return id;
    }
    return NULL;
}

static const pci_device_t *ac97_find_device(void) {
    uint32_t count = pci_device_count();
    for (uint32_t i = 0; i < count; i++) {
        const pci_device_t *dev = pci_device_at(i);
        if (!dev || dev->class_code != 0x04U || dev->subclass != 0x01U)
            continue;
        g_ac97_controller = ac97_match_controller(dev);
        if (g_ac97_controller) return dev;
        if (dev->vendor_id == 0x125DU &&
            (dev->device_id == 0x1988U || dev->device_id == 0x1998U ||
             dev->device_id == 0x199AU)) {
            SOUND_LOG("[AC97] ESS %x:%x requiere Maestro3 + firmware DSP; no es ICH\n",
                    dev->vendor_id, dev->device_id);
        } else {
            SOUND_LOG("[AC97] audio PCI %x:%x usa otro motor DMA AC'97\n",
                    dev->vendor_id, dev->device_id);
        }
    }
    return NULL;
}

static uint16_t ac97_descriptor_length(uint32_t frames) {
    uint32_t length = frames * 2U; /* muestras stereo para ICH/nForce */
    if (g_ac97_controller &&
        g_ac97_controller->type == AC97_CONTROLLER_SIS7012)
        length *= sizeof(int16_t); /* SiS cuenta bytes */
    return (uint16_t)length;
}

static uint32_t ac97_max_descriptor_frames(void) {
    return g_ac97_controller &&
           g_ac97_controller->type == AC97_CONTROLLER_SIS7012
         ? 16383U : 32767U;
}

static bool ac97_has_active_voice(void) {
    for (uint32_t i = 0; i < AC97_MIX_VOICES; i++)
        if (g_ac97_voices[i].active) return true;
    return false;
}

static uint32_t ac97_preload_target(void) {
    uint64_t short_limit =
        (uint64_t)AC97_BUFFER_FRAMES * 8U;
    for (uint32_t i = 0; i < AC97_MIX_VOICES; i++) {
        const ac97_voice_t *voice = &g_ac97_voices[i];
        uint64_t remaining_fp;
        uint64_t short_source_fp;
        if (!voice->active || !voice->step_fp) continue;
        remaining_fp = ((uint64_t)voice->length << 16) -
                       voice->position_fp;
        short_source_fp = short_limit * voice->step_fp;
        if (remaining_fp > short_source_fp) return AC97_PRELOAD_LONG;
    }
    return AC97_PRELOAD_SHORT;
}

static void ac97_clear_voices(void) {
    for (uint32_t i = 0; i < AC97_MIX_VOICES; i++) {
        g_ac97_voices[i].active = false;
        g_ac97_voices[i].length = 0;
        g_ac97_voices[i].position_fp = 0;
        g_ac97_voices[i].step_fp = 0;
        g_ac97_voices[i].volume = 0;
    }
}

static bool ac97_add_voice(const uint8_t *samples, uint32_t length,
                           uint16_t rate, uint8_t volume) {
    uint32_t slot = AC97_MIX_VOICES;

    if (!samples || !length || length > AC97_VOICE_MAX_BYTES ||
        rate < 4000U || rate > 48000U) return false;

    /* ac97_play_pcm_u8() already owns g_ac97_state_lock here. Do not toggle
     * preemption inside that lock: on SMP it could schedule the lock owner
     * away while ac97-mixer is spinning for the same state. Eight voices are
     * enough for GL-117; when saturated, dropping a sound is safer than
     * replacing storage currently being consumed by DMA mix-ahead. */
    for (uint32_t i = 0; i < AC97_MIX_VOICES; i++) {
        if (!g_ac97_voices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot >= AC97_MIX_VOICES) return false;

    {
        ac97_voice_t *voice = &g_ac97_voices[slot];
        kmemcpy(voice->samples, samples, length);
        voice->length = length;
        voice->position_fp = 0;
        voice->step_fp = ((uint32_t)rate << 16) / AC97_OUTPUT_HZ;
        if (!voice->step_fp) voice->step_fp = 1;
        voice->volume = volume;
        __asm__ volatile ("" : : : "memory");
        voice->active = true;
    }
    return true;
}

static int ac97_clamp_mix(int value) {
    if (value < -128) return -128;
    if (value > 127) return 127;
    return value;
}

static uint32_t ac97_render(int16_t *output, uint32_t max_frames) {
    uint32_t produced = 0;
    if (!output) return 0;

    for (uint32_t frame = 0; frame < max_frames; frame++) {
        int mixed = 0;
        bool any = false;
        for (uint32_t v = 0; v < AC97_MIX_VOICES; v++) {
            ac97_voice_t *voice = &g_ac97_voices[v];
            uint32_t position;
            uint32_t fraction;
            uint32_t next_position;
            int first;
            int second;
            int sample;
            if (!voice->active) continue;
            position = (uint32_t)(voice->position_fp >> 16);
            if (position >= voice->length) {
                voice->active = false;
                continue;
            }
            fraction = (uint32_t)(voice->position_fp & 0xFFFFU);
            next_position = position + 1U;
            if (next_position >= voice->length) next_position = position;
            first = (int)voice->samples[position] - 128;
            second = (int)voice->samples[next_position] - 128;
            sample = first + (((second - first) * (int)fraction) / 65536);
            mixed += (sample * (int)voice->volume) / 255;
            voice->position_fp += voice->step_fp;
            if ((voice->position_fp >> 16) >= voice->length)
                voice->active = false;
            any = true;
        }
        if (!any) break;
        mixed = ac97_clamp_mix(mixed) * 256;
        output[frame * 2U] = (int16_t)mixed;
        output[frame * 2U + 1U] = (int16_t)mixed;
        produced++;
    }
    return produced;
}

static uint32_t ac97_output_frames(uint32_t source_samples, uint16_t rate) {
    uint32_t seconds;
    uint32_t remainder;
    uint32_t frames;
    if (!rate) return 0;
    seconds = source_samples / rate;
    remainder = source_samples % rate;
    if (seconds > (AC97_BDL_COUNT * ac97_max_descriptor_frames()) /
                  AC97_OUTPUT_HZ) return 0;
    frames = seconds * AC97_OUTPUT_HZ;
    frames += (remainder * AC97_OUTPUT_HZ + rate - 1U) / rate;
    return frames;
}

static bool ac97_start_direct(const uint8_t *samples, uint32_t length,
                              uint16_t rate, uint8_t volume) {
    uint32_t frames;
    uint32_t step_fp;
    uint64_t position_fp = 0;
    uint32_t descriptor_count;
    uint32_t frame_offset = 0;
    uint32_t max_descriptor_frames;
    int16_t *output;

    if (!samples || !length || rate < 4000U || rate > 48000U)
        return false;
    max_descriptor_frames = ac97_max_descriptor_frames();
    frames = ac97_output_frames(length, rate);
    if ((rate != 22050U && frames < AC97_DIRECT_MIN_FRAMES) ||
        frames > AC97_BDL_COUNT * max_descriptor_frames || g_ac97_streaming ||
        ac97_has_active_voice()) return false;
    output = (int16_t *)kmalloc(frames * 2U * sizeof(int16_t));
    if (!output) return false;
    /* Direct DMA outlives the syscall/task that submitted it. Keep the
     * converted buffer owned by the kernel until ac97_poll_stream() retires
     * it, instead of letting mm_release_process() reclaim it asynchronously. */
    if (!mm_set_allocation_owner(output, 0U)) {
        kfree(output);
        return false;
    }
    step_fp = ((uint32_t)rate << 16) / AC97_OUTPUT_HZ;
    if (!step_fp) step_fp = 1;

    for (uint32_t frame = 0; frame < frames; frame++) {
        uint32_t position = (uint32_t)(position_fp >> 16);
        uint32_t next_position;
        uint32_t fraction;
        int first;
        int second;
        int value;
        if (position >= length) position = length - 1U;
        next_position = position + 1U;
        if (next_position >= length) next_position = position;
        fraction = (uint32_t)(position_fp & 0xFFFFU);
        first = (int)samples[position] - 128;
        second = (int)samples[next_position] - 128;
        value = first + (((second - first) * (int)fraction) / 65536);
        value = (value * (int)volume) / 255;
        value = ac97_clamp_mix(value) * 256;
        output[frame * 2U] = (int16_t)value;
        output[frame * 2U + 1U] = (int16_t)value;
        position_fp += step_fp;
    }

    descriptor_count = (frames + max_descriptor_frames - 1U) /
                       max_descriptor_frames;
    kmemset(g_ac97_bdl, 0, sizeof(g_ac97_bdl));
    for (uint32_t i = 0; i < descriptor_count; i++) {
        uint32_t chunk_frames = frames - frame_offset;
        if (chunk_frames > max_descriptor_frames)
            chunk_frames = max_descriptor_frames;
        g_ac97_bdl[i].address = (uint32_t)(uintptr_t)
            &output[frame_offset * 2U];
        g_ac97_bdl[i].samples = ac97_descriptor_length(chunk_frames);
        g_ac97_bdl[i].control = AC97_BDL_BUP;
        frame_offset += chunk_frames;
    }

    ac97_channel_reset();
    g_ac97_direct_buffer = output;
    g_ac97_direct_active = true;
    g_ac97_producer_seq = descriptor_count;
    g_ac97_current_seq = 0;
    g_ac97_last_civ = 0;
    ac97_dma_publish();
    ac97_outl(g_ac97_nabm + AC97_PO_BDBAR,
         (uint32_t)(uintptr_t)g_ac97_bdl);
    ac97_outb(g_ac97_nabm + AC97_PO_LVI,
         (uint8_t)(descriptor_count - 1U));
    ac97_run_with_lvi_kick((uint8_t)(descriptor_count - 1U));
    g_ac97_observed_running = false;
    g_ac97_start_retries = 0;
    g_ac97_start_tick = pit_get_ticks();
    g_ac97_streaming = true;
    if (!g_ac97_direct_reported) {
        SOUND_LOG("[AC97] DMA directo: %u frames, %u descriptores\n",
                frames, descriptor_count);
        g_ac97_direct_reported = true;
    }
    return true;
}

static bool ac97_fill_descriptor(uint32_t sequence) {
    uint32_t index = sequence & (AC97_BDL_COUNT - 1U);
    uint32_t frames = ac97_render(g_ac97_buffers[index],
                                  AC97_BUFFER_FRAMES);
    if (!frames) return false;
    /* A short PCM fragment must end at its last valid sample. Playing the
     * zero padding until 2048 frames inserted silence between OSS chunks. */
    g_ac97_bdl[index].samples = ac97_descriptor_length(frames);
    if (frames < AC97_BUFFER_FRAMES) {
        kmemset(&g_ac97_buffers[index][frames * 2U], 0,
                (AC97_BUFFER_FRAMES - frames) * 2U * sizeof(int16_t));
    }
    return true;
}

static void ac97_channel_reset(void) {
    ac97_outb(g_ac97_nabm + AC97_PO_CR, 0);
    ac97_outb(g_ac97_nabm + AC97_PO_CR, AC97_CR_RESET);
    for (uint32_t i = 0; i < 100000U; i++) {
        if (!(ac97_inb(g_ac97_nabm + AC97_PO_CR) & AC97_CR_RESET))
            break;
        ac97_io_wait();
    }
    ac97_outw((g_ac97_nabm + g_ac97_po_sr), AC97_SR_CLEAR);
}

static bool ac97_start_stream(void) {
    uint32_t filled = 0;
    uint32_t target;
    ac97_channel_reset();
    g_ac97_producer_seq = 0;
    g_ac97_current_seq = 0;
    g_ac97_last_civ = 0;

    target = ac97_preload_target();
    while (filled < target && ac97_fill_descriptor(filled)) filled++;
    if (!filled) return false;
    g_ac97_producer_seq = filled;
    ac97_dma_publish();
    ac97_outl(g_ac97_nabm + AC97_PO_BDBAR,
         (uint32_t)(uintptr_t)g_ac97_bdl);
    ac97_outb(g_ac97_nabm + AC97_PO_LVI,
         (uint8_t)((filled - 1U) & (AC97_BDL_COUNT - 1U)));
    ac97_run_with_lvi_kick(
        (uint8_t)((filled - 1U) & (AC97_BDL_COUNT - 1U)));
    g_ac97_observed_running = false;
    g_ac97_start_retries = 0;
    g_ac97_start_tick = pit_get_ticks();
    g_ac97_streaming = true;
    if (!g_ac97_short_ring_started_reported) {
        SOUND_LOG("[AC97] PCM ring corto iniciado: %u descriptor(es) precargados\n",
                filled);
        g_ac97_short_ring_started_reported = true;
    }
    return true;
}

static void ac97_fill_ahead(void) {
    uint32_t target = ac97_preload_target();
    while (g_ac97_producer_seq - g_ac97_current_seq < target) {
        if (!ac97_fill_descriptor(g_ac97_producer_seq)) break;
        g_ac97_producer_seq++;
        ac97_dma_publish();
        ac97_outb(g_ac97_nabm + AC97_PO_LVI,
             (uint8_t)((g_ac97_producer_seq - 1U) &
                       (AC97_BDL_COUNT - 1U)));
    }
}

static void ac97_poll_stream(void) {
    uint16_t status;
    uint8_t civ;
    uint8_t delta;
    uint32_t timeout_ticks;

    if (!g_ac97_streaming) return;
    status = ac97_inw((g_ac97_nabm + g_ac97_po_sr));
    civ = ac97_inb(g_ac97_nabm + AC97_PO_CIV) & 0x1FU;
    if (status & AC97_SR_FIFOE) {
        g_ac97_fifo_errors++;
        if (g_ac97_fifo_errors <= 4U)
            SOUND_LOG("[AC97] FIFO underrun #%u SR=%x CIV=%u LVI=%u\n",
                    g_ac97_fifo_errors, status, (uint32_t)civ,
                    (uint32_t)ac97_inb(g_ac97_nabm + AC97_PO_LVI));
    }
    if (!(status & AC97_SR_DCH)) {
        g_ac97_observed_running = true;
        g_ac97_start_retries = 0;
    }
    delta = (uint8_t)((civ - g_ac97_last_civ) & 0x1FU);
    if (delta && delta < 16U) {
        g_ac97_current_seq += delta;
        g_ac97_last_civ = civ;
    }
    if (status & AC97_SR_CLEAR)
        ac97_outw((g_ac97_nabm + g_ac97_po_sr),
                  (uint16_t)(status & AC97_SR_CLEAR));

    if ((status & AC97_SR_DCH) && g_ac97_observed_running) {
        if (g_ac97_direct_active) {
            int16_t *finished = ac97_take_direct_buffer();
            g_ac97_streaming = false;
            ac97_prepare_ring_bdl();
            if (finished) kfree(finished);
            return;
        }
        if (ac97_has_active_voice()) {
            g_ac97_dch_restarts++;
            if (g_ac97_dch_restarts <= 4U)
                SOUND_LOG("[AC97] DMA alcanzo LVI antes de tiempo #%u CIV=%u LVI=%u\n",
                        g_ac97_dch_restarts, (uint32_t)civ,
                        (uint32_t)ac97_inb(g_ac97_nabm + AC97_PO_LVI));
        }
        g_ac97_current_seq = g_ac97_producer_seq;
        g_ac97_streaming = false;
        if (!g_ac97_short_ring_done_reported) {
            SOUND_LOG("[AC97] PCM ring corto completado CIV=%u LVI=%u\n",
                    (uint32_t)civ,
                    (uint32_t)ac97_inb(g_ac97_nabm + AC97_PO_LVI));
            g_ac97_short_ring_done_reported = true;
        }
        return;
    }
    if (!g_ac97_direct_active && delta && delta < 16U)
        ac97_fill_ahead();
    timeout_ticks = pit_get_frequency_hz();
    if (!timeout_ticks) timeout_ticks = 100U;
    timeout_ticks /= 10U; /* 100 ms, no un segundo entero de silencio */
    if (!timeout_ticks) timeout_ticks = 1U;
    if (!g_ac97_observed_running &&
        (uint32_t)(pit_get_ticks() - g_ac97_start_tick) > timeout_ticks) {
        if (!g_ac97_direct_active &&
            g_ac97_start_retries < 2U &&
            g_ac97_producer_seq > g_ac97_current_seq) {
            uint8_t lvi = (uint8_t)((g_ac97_producer_seq - 1U) &
                                    (AC97_BDL_COUNT - 1U));
            g_ac97_start_retries++;
            ac97_channel_reset();
            ac97_dma_publish();
            ac97_outl(g_ac97_nabm + AC97_PO_BDBAR,
                 (uint32_t)(uintptr_t)g_ac97_bdl);
            ac97_outb(g_ac97_nabm + AC97_PO_LVI, lvi);
            ac97_run_with_lvi_kick(lvi);
            g_ac97_start_tick = pit_get_ticks();
            return;
        }

        SOUND_LOG("[AC97] DMA no inicio tras reintentos, SR=%x CR=%x\n",
                status, ac97_inb(g_ac97_nabm + AC97_PO_CR));
        int16_t *direct;
        ac97_channel_reset();
        ac97_clear_voices();
        direct = ac97_take_direct_buffer();
        ac97_prepare_ring_bdl();
        g_ac97_streaming = false;
        if (direct) kfree(direct);
    }
}

static void ac97_worker(void *argument UNUSED) {
    while (true) {
        bool active = false;
        uint32_t sleep_ticks = 1U;
        ac97_state_lock();
        if (g_ac97_present) {
            ac97_poll_stream();
            if (!g_ac97_streaming && ac97_has_active_voice())
                (void)ac97_start_stream();
            active = g_ac97_streaming || ac97_has_active_voice();
        }
        ac97_state_unlock();
        if (!active) {
            uint32_t hz = pit_get_frequency_hz();
            sleep_ticks = hz / 100U;
            if (!sleep_ticks) sleep_ticks = 1U;
        }
        task_sleep(sleep_ticks);
    }
}

static void ac97_pc_play(uint32_t frequency_hz) {
    uint32_t divisor;
    uint8_t speaker;
    if (!frequency_hz) return;
    divisor = 1193182U / frequency_hz;
    if (!divisor || divisor > 0xFFFFU) return;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFFU));
    outb(0x42, (uint8_t)(divisor >> 8));
    speaker = inb(0x61);
    outb(0x61, (uint8_t)(speaker | 0x03U));
}

static void ac97_stop_locked(void) {
    int16_t *direct;
    uint8_t speaker = inb(0x61);
    outb(0x61, (uint8_t)(speaker & 0xFCU));

    /* Retire ownership before the channel reset can expose DCH. */
    direct = ac97_take_direct_buffer();
    g_ac97_streaming = false;
    ac97_clear_voices();
    if (g_ac97_present) ac97_channel_reset();
    ac97_prepare_ring_bdl();
    if (direct) kfree(direct);
}

static void ac97_stop(void) {
    ac97_state_lock();
    ac97_stop_locked();
    ac97_state_unlock();
}

static bool ac97_play_pcm_u8(const uint8_t *samples, uint32_t length,
                             uint16_t rate, uint8_t volume) {
    bool result = false;
    ac97_state_lock();
    if (g_ac97_present) {
        /* GL117_AUDIOFIX6_22050_DIRECT
         * GL-117 ships its effects as 22050-Hz U8 mono.  The older
         * audiofix5 selected ac97_start_direct(), but ac97_start_direct()
         * still rejected clips shorter than AC97_DIRECT_MIN_FRAMES (2 s),
         * so plane1.wav fell through to the 12-descriptor software-mix ring.
         *
         * 22050-Hz effects are now truly direct-only: convert/copy them to
         * kernel-owned S16 stereo, hand that buffer to bus-master DMA, and
         * never fall back to the ring.  If another PCM owns the device, the
         * effect is simply dropped.  This trades overlap for deterministic
         * gameplay timing and keeps the render thread out of the mixer path.
         */
        if (rate == 22050U) {
            if (!g_ac97_streaming && !g_ac97_direct_active &&
                !ac97_has_active_voice())
                result = ac97_start_direct(samples, length, rate, volume);
        } else if (ac97_start_direct(samples, length, rate, volume)) {
            result = true;
        } else if (!g_ac97_direct_active) {
            result = ac97_add_voice(samples, length, rate, volume);
            if (result) {
                if (!g_ac97_streaming) {
                    result = ac97_start_stream();
                } else {
                    ac97_fill_ahead();
                    if (!g_ac97_continuous_ring_reported) {
                        SOUND_LOG("[AC97] PCM ring continuo: refill <= %u descriptor(es)\n",
                                AC97_REFILL_WATERMARK);
                        g_ac97_continuous_ring_reported = true;
                    }
                }
            }
        }
    }
    ac97_state_unlock();
    return result;
}


static bool ac97_start_tone(uint32_t frequency_hz, uint32_t duration_ms) {
    uint32_t samples;
    uint32_t period;
    bool result;
    if (!frequency_hz || !duration_ms) return false;
    if (duration_ms > AC97_TONE_MAX_MS) duration_ms = AC97_TONE_MAX_MS;
    samples = (AC97_OUTPUT_HZ * duration_ms) / 1000U;
    period = AC97_OUTPUT_HZ / frequency_hz;
    if (period < 2U) period = 2U;
    ac97_state_lock();
    ac97_stop_locked();
    for (uint32_t i = 0; i < samples; i++)
        g_ac97_tone[i] = (i % period) < (period / 2U) ? 224U : 32U;
    result = ac97_add_voice(g_ac97_tone, samples, AC97_OUTPUT_HZ, 200U);
    ac97_state_unlock();
    return result;
}

static void ac97_beep(uint32_t frequency_hz, uint32_t duration_ms) {
    if (!duration_ms) {
        ac97_pc_play(frequency_hz);
        return;
    }
    if (!ac97_start_tone(frequency_hz, duration_ms)) return;
    while (g_ac97_streaming || ac97_has_active_voice()) task_sleep(1);
}

/* sound_poll() corre desde la IRQ del PIT; el trabajo pesado vive en worker. */
static void ac97_poll(void) {}
static bool ac97_pcm_available(void) { return g_ac97_present; }
static bool ac97_pcm_busy(void) {
    bool busy = false;
    ac97_state_lock();
    if (g_ac97_present) {
        uint32_t queued;
        ac97_poll_stream();
        if (!g_ac97_streaming && ac97_has_active_voice())
            (void)ac97_start_stream();

        if (g_ac97_direct_active) {
            busy = g_ac97_streaming || ac97_has_active_voice();
        } else if (g_ac97_streaming) {
            /*
             * Dejar que el productor entregue el proximo bloque cuando quedan
             * <=2 descriptores (~85 ms). play_pcm_u8 lo renderiza por delante
             * y mueve LVI, manteniendo el DMA continuo.
             */
            queued = ac97_ring_queued();
            busy = ac97_has_active_voice() ||
                   queued > AC97_REFILL_WATERMARK;
        } else {
            busy = ac97_has_active_voice();
        }
    }
    ac97_state_unlock();
    return busy;
}
static bool ac97_has_sb16(void) { return false; }
static bool ac97_sb16_tone(uint32_t hz UNUSED, uint32_t ms UNUSED) {
    return false;
}
static bool ac97_sb16_busy(void) { return false; }
static const char *ac97_pcm_name(void) {
    return g_ac97_controller ? g_ac97_controller->name
                             : "AC'97 ICH-compatible";
}

static bool ac97_codec_ready(void) {
    uint32_t ready = 0;
    for (uint32_t i = 0; i < 200000U; i++) {
        ready = ac97_inl(g_ac97_nabm + AC97_GLOB_STA);
        if (ready & AC97_GLOB_PCR) return true;
        ac97_io_wait();
    }
    return false;
}

static uint8_t ac97_config8(const pci_device_t *d,uint8_t offset) {
    return (uint8_t)(pci_config_read32(d->bus,d->slot,d->function,offset&~3U) >>
        ((offset&3U)*8U));
}
static void ac97_wake_device(void) {
    const pci_device_t *d=g_ac97_pci;
    if(!(pci_config_read16(d->bus,d->slot,d->function,0x06U)&0x10U))return;
    uint8_t cap=ac97_config8(d,0x34U)&~3U;
    for(unsigned n=0;n<48 && cap>=0x40U && cap<=0xF8U;n++) {
        if(ac97_config8(d,cap)==1U) {
            uint16_t pm=pci_config_read16(d->bus,d->slot,d->function,cap+4U);
            if(pm&3U) {
                /* D0; PME status is W1C and must not be acknowledged here. */
                pci_config_write16(d->bus,d->slot,d->function,cap+4U,pm&~0x8003U);
                for(unsigned i=0;i<10000U;i++)ac97_io_wait();
            }
            return;
        }
        cap=ac97_config8(d,cap+1U)&~3U;
    }
}

static bool ac97_init(void) {
    uint32_t nam_bar;
    uint32_t nabm_bar;
    uint16_t vendor1;
    uint16_t vendor2;
    static const sound_driver_ops_t ops = {
        ac97_poll,
        ac97_pc_play,
        ac97_stop,
        ac97_beep,
        ac97_start_tone,
        ac97_play_pcm_u8,
        ac97_pcm_available,
        ac97_pcm_busy,
        ac97_has_sb16,
        ac97_sb16_tone,
        ac97_sb16_busy,
        ac97_pcm_name,
        200U
    };

    g_ac97_pci = ac97_find_device();
    if (!g_ac97_pci) {
        SOUND_LOG("[AC97] controlador ICH-compatible no detectado\n");
        return false;
    }
    SOUND_LOG("[AC97] PCI candidato %x:%x bus=%u slot=%u fn=%u "
            "BAR0=%x BAR1=%x IRQ=%u cmd=%x\n",
            g_ac97_pci->vendor_id, g_ac97_pci->device_id,
            g_ac97_pci->bus, g_ac97_pci->slot, g_ac97_pci->function,
            g_ac97_pci->bars[0], g_ac97_pci->bars[1],
            g_ac97_pci->interrupt_line, g_ac97_pci->command);
    g_ac97_po_sr = g_ac97_controller->type == AC97_CONTROLLER_SIS7012
                 ? AC97_PO_SIS_SR : AC97_PO_SR;
    nam_bar = g_ac97_pci->bars[0];
    nabm_bar = g_ac97_pci->bars[1];
    uint32_t mem_nam=g_ac97_pci->bars[2], mem_bm=g_ac97_pci->bars[3];
    g_ac97_mmio=g_ac97_pci->vendor_id==0x8086U &&
        g_ac97_pci->device_id==0x24C5U && mem_nam && mem_bm &&
        !(mem_nam&1U) && !(mem_bm&1U) &&
        (mem_nam&~15U) && (mem_bm&~15U);
    if(g_ac97_mmio &&
       (!paging_kernel_mmio_uncached(mem_nam&~15U,512U) ||
        !paging_kernel_mmio_uncached(mem_bm&~15U,256U)))g_ac97_mmio=false;
    if(g_ac97_mmio) {
        g_ac97_nam=mem_nam&~15U; g_ac97_nabm=mem_bm&~15U;
    } else {
        if(!(nam_bar&1U) || !(nabm_bar&1U) || !(nam_bar&~3U) ||
           !(nabm_bar&~3U) || (nam_bar&~3U)>0xFFFFU ||
           (nabm_bar&~3U)>0xFFFFU) {
            SOUND_LOG("[AC97] sin BARs de registros utilizables\n");
            return false;
        }
        g_ac97_nam=nam_bar&~3U; g_ac97_nabm=nabm_bar&~3U;
    }
    ac97_wake_device();
    if(!pci_enable_command(g_ac97_pci, PCI_COMMAND_BUSMASTER |
        (g_ac97_mmio?PCI_COMMAND_MEMORY:PCI_COMMAND_IO)))return false;
    /* Preserve routing bits; bring AC-link up in stereo, 16-bit mode.
     * An already running link needs a warm reset, not another cold assertion. */
    uint32_t control=ac97_inl(g_ac97_nabm+AC97_GLOB_CNT);
    control &= ~(0x00300000U | 0x00400000U | 0x08U);
    control |= (control&AC97_GLOB_COLD)?4U:AC97_GLOB_COLD;
    ac97_outl(g_ac97_nabm+AC97_GLOB_CNT,control);
    for(uint32_t i=0;i<200000U && (ac97_inl(g_ac97_nabm+AC97_GLOB_CNT)&4U);i++)
        ac97_io_wait();
    if (!ac97_codec_ready()) {
        kprintf("[AC97] codec primario no responde, GLOB_STA=%x\n",
                ac97_inl(g_ac97_nabm + AC97_GLOB_STA));
        return false;
    }
    ac97_outw((g_ac97_nam + AC97_NAM_RESET), 0);
    /* Wake DAC, mixer and external amplifier before unmuting output paths. */
    ac97_outw(g_ac97_nam+0x26U,0);
    for(uint32_t i=0;i<200000U;i++) {
        if((ac97_inw(g_ac97_nam+0x26U)&0x0FU)==0x0FU)break;
        ac97_io_wait();
    }
    ac97_outw(g_ac97_nam+0x04U,0); /* headphone/aux out */
    ac97_outw(g_ac97_nam+0x06U,0); /* mono out */
    uint16_t extended=ac97_inw(g_ac97_nam+0x2AU);
    if(extended!=0xFFFFU)ac97_outw(g_ac97_nam+0x2AU,extended&~0x0009U);

    ac97_outw((g_ac97_nam + AC97_NAM_MASTER_VOL), 0);
    ac97_outw((g_ac97_nam + AC97_NAM_PCM_VOL), 0);
    if (g_ac97_controller->type == AC97_CONTROLLER_SIS7012) {
        /* SiS intercambia SR/PICB y requiere habilitar la salida PCM. */
        uint16_t sis_output = ac97_inw((g_ac97_nabm + 0x4CU));
        ac97_outw((g_ac97_nabm + 0x4CU),
                  (uint16_t)(sis_output | 1U));
    }
    vendor1 = ac97_inw((g_ac97_nam + AC97_NAM_VENDOR1));
    vendor2 = ac97_inw((g_ac97_nam + AC97_NAM_VENDOR2));
    if ((vendor1 == 0xFFFFU && vendor2 == 0xFFFFU) ||
        (!vendor1 && !vendor2)) {
        SOUND_LOG("[AC97] codec presente pero ID invalido\n");
        return false;
    }

    if(vendor1==0x4352U && (vendor2&0xFFF8U)==0x5930U) {
        /* CS4299 AC mode default, as specified by the Cirrus codec patch. */
        ac97_outw(g_ac97_nam+0x5EU,0x0080U);
    }
    kprintf("[AC97] access=%s power=%x master=%x headphone=%x PCM=%x\n",
        g_ac97_mmio?"MMIO":"PIO",ac97_inw(g_ac97_nam+0x26U),
        ac97_inw(g_ac97_nam+2U),ac97_inw(g_ac97_nam+4U),ac97_inw(g_ac97_nam+0x18U));
    kmemset(g_ac97_buffers, 0, sizeof(g_ac97_buffers));
    ac97_prepare_ring_bdl();
    ac97_clear_voices();
    g_ac97_streaming = false;
    g_ac97_observed_running = false;
    g_ac97_direct_buffer = NULL;
    g_ac97_direct_active = false;
    g_ac97_fifo_errors = 0;
    g_ac97_dch_restarts = 0;
    g_ac97_start_retries = 0;
    g_ac97_continuous_ring_reported = false;
    g_ac97_short_ring_started_reported = false;
    g_ac97_short_ring_done_reported = false;
    g_ac97_direct_reported = false;
    g_ac97_state_lock = 0U;
    ac97_channel_reset();
    g_ac97_present = true;
    if (task_create("ac97-mixer", ac97_worker, NULL) < 0) {
        g_ac97_present = false;
        return false;
    }
    if (!sound_register_driver(&ops)) {
        g_ac97_present = false;
        return false;
    }
    SOUND_LOG("[AC97] %s %x:%x NAM=%x NABM=%x codec=%x:%x\n",
            g_ac97_controller->name,
            g_ac97_pci->vendor_id, g_ac97_pci->device_id,
            g_ac97_nam, g_ac97_nabm, vendor1, vendor2);
    SOUND_LOG("[AC97] PCM 48000 Hz S16LE stereo, BDL=%x buffers=%u x %u frames\n",
            (uint32_t)(uintptr_t)g_ac97_bdl, AC97_BDL_COUNT,
            AC97_BUFFER_FRAMES);
    SOUND_LOG("[AC97] SMP-safe PCM state serialization active\n");
    SOUND_LOG("[AC97] PCM ring sync-drive fallback active\n");
    /* GL117_AUDIOFIX8_DVR_REAL */
    SOUND_LOG("[AC97] GL117 audiofix8 DVR ACTIVE\n");
    SOUND_LOG("[AC97] GL117 audiofix7 kernel RELINK ACTIVE\n");
    SOUND_LOG("[AC97] PCM safe-copy voices: %u x %u bytes\n",
            AC97_MIX_VOICES, AC97_VOICE_MAX_BYTES);
    return true;
}

static void ac97_shutdown(void) {
    ac97_stop();
    ac97_state_lock();
    g_ac97_present = false;
    ac97_state_unlock();
}

const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module = {
        BK_DRIVER_ABI_VERSION,
        sizeof(bk_driver_module_t),
        1U, 0U,
        "ac97",
        "Intel/AMD/NVIDIA/SiS AC'97 PCM bus-master",
        ac97_init,
        ac97_shutdown
    };
    return &module;
}
