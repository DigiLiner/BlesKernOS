#include "audio_pwm.h"
#include "mmio.h"
#include "timer.h"
#include "cache.h"
#include "../../../kernel/include/sound.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/libc/include/stdio.h"

#define PERIPHERAL_BASE 0x3F000000U
#define GPIO_BASE       (PERIPHERAL_BASE + 0x200000U)
#define PWM_BASE        (PERIPHERAL_BASE + 0x20C000U)
#define CM_BASE         (PERIPHERAL_BASE + 0x101000U)
#define DMA_BASE        (PERIPHERAL_BASE + 0x007000U)
#define DMA_CHANNEL     5U
#define DMA_CH_BASE     (DMA_BASE + DMA_CHANNEL * 0x100U)

#define GPIO_GPFSEL4    (GPIO_BASE + 0x10U)
#define CM_PWMCTL       (CM_BASE + 0xA0U)
#define CM_PWMDIV       (CM_BASE + 0xA4U)
#define CM_PASSWORD     0x5A000000U
#define CM_CTL_KILL     (1U << 5)
#define CM_CTL_ENAB     (1U << 4)
#define CM_CTL_BUSY     (1U << 7)
#define CM_SRC_PLLD     6U

#define PWM_CTL         (PWM_BASE + 0x00U)
#define PWM_STA         (PWM_BASE + 0x04U)
#define PWM_DMAC        (PWM_BASE + 0x08U)
#define PWM_RNG1        (PWM_BASE + 0x10U)
#define PWM_FIFO        (PWM_BASE + 0x18U)
#define PWM_RNG2        (PWM_BASE + 0x20U)
#define PWM_CTL_PWEN1   (1U << 0)
#define PWM_CTL_USEF1   (1U << 5)
#define PWM_CTL_CLRF    (1U << 6)
#define PWM_CTL_PWEN2   (1U << 8)
#define PWM_CTL_USEF2   (1U << 13)

#define DMA_CS          (DMA_CH_BASE + 0x00U)
#define DMA_CONBLK_AD   (DMA_CH_BASE + 0x04U)
#define DMA_DEBUG       (DMA_CH_BASE + 0x20U)
#define DMA_CS_ACTIVE   (1U << 0)
#define DMA_CS_END      (1U << 1)
#define DMA_CS_INT      (1U << 2)
#define DMA_CS_RESET    (1U << 31)
#define DMA_TI_DEST_DREQ (1U << 6)
#define DMA_TI_SRC_INC   (1U << 8)
#define DMA_TI_WAIT_RESP (1U << 3)
#define DMA_TI_PERMAP(x) ((uint32_t)(x) << 16)

#define PWM_RANGE       256U
#define PCM_MAX_SAMPLES 16384U

typedef struct dma_control_block {
    uint32_t transfer_info;
    uint32_t source;
    uint32_t destination;
    uint32_t length;
    uint32_t stride;
    uint32_t next;
    uint32_t reserved[2];
} __attribute__((aligned(32))) dma_control_block_t;

static dma_control_block_t g_cb;
static uint32_t g_pcm[PCM_MAX_SAMPLES * 2U] __attribute__((aligned(64)));
static volatile bool g_busy;
static uint32_t g_tone_end;

static uint32_t dma_bus(const void *pointer) {
    return 0xC0000000U | ((uint32_t)(uintptr_t)pointer & 0x3FFFFFFFU);
}

static void pwm_stop(void) {
    mmio_write(DMA_CS, DMA_CS_RESET);
    rpi3_timer_delay_us(20U);
    mmio_write(PWM_CTL, 0U);
    g_busy = false;
    g_tone_end = 0U;
}

static bool pwm_clock(uint32_t sample_rate) {
    uint32_t divisor;
    if (sample_rate < 4000U || sample_rate > 48000U) return false;
    divisor = 500000000U / (sample_rate * PWM_RANGE);
    if (divisor < 2U) divisor = 2U;
    if (divisor > 4095U) return false;
    mmio_write(CM_PWMCTL, CM_PASSWORD | CM_CTL_KILL);
    rpi3_timer_delay_us(20U);
    mmio_write(CM_PWMDIV, CM_PASSWORD | (divisor << 12));
    mmio_write(CM_PWMCTL, CM_PASSWORD | CM_SRC_PLLD | CM_CTL_ENAB);
    rpi3_timer_delay_us(20U);
    return (mmio_read(CM_PWMCTL) & CM_CTL_BUSY) != 0U;
}

static bool pwm_play_pcm(const uint8_t *samples, uint32_t length,
                         uint16_t rate, uint8_t volume) {
    uint32_t i, words;
    if (!samples || !length || length > PCM_MAX_SAMPLES || !pwm_clock(rate))
        return false;
    pwm_stop();
    if (!pwm_clock(rate)) return false;
    for (i = 0U; i < length; ++i) {
        int32_t centered = (int32_t)samples[i] - 128;
        uint32_t duty = (uint32_t)(128 + (centered * (int32_t)volume) / 255);
        g_pcm[i * 2U] = duty;
        g_pcm[i * 2U + 1U] = duty;
    }
    words = length * 2U;
    mmio_write(PWM_CTL, 0U);
    rpi3_timer_delay_us(10U);
    mmio_write(PWM_RNG1, PWM_RANGE); mmio_write(PWM_RNG2, PWM_RANGE);
    mmio_write(PWM_DMAC, (1U << 31) | (7U << 8) | 7U);
    mmio_write(PWM_CTL, PWM_CTL_CLRF);
    rpi3_timer_delay_us(10U);

    kmemset(&g_cb, 0, sizeof(g_cb));
    g_cb.transfer_info = DMA_TI_DEST_DREQ | DMA_TI_SRC_INC |
                         DMA_TI_WAIT_RESP | DMA_TI_PERMAP(5U);
    g_cb.source = dma_bus(g_pcm);
    g_cb.destination = 0x7E20C018U;
    g_cb.length = words * sizeof(uint32_t);
    rpi3_cache_clean_range(g_pcm, g_cb.length);
    rpi3_cache_clean_range(&g_cb, sizeof(g_cb));
    mmio_write(DMA_CS, DMA_CS_RESET);
    rpi3_timer_delay_us(10U);
    mmio_write(DMA_DEBUG, 7U);
    mmio_write(DMA_CONBLK_AD, dma_bus(&g_cb));
    mmio_write(PWM_CTL, PWM_CTL_USEF1 | PWM_CTL_USEF2 |
                        PWM_CTL_PWEN1 | PWM_CTL_PWEN2);
    mmio_write(DMA_CS, DMA_CS_END | DMA_CS_INT);
    mmio_write(DMA_CS, DMA_CS_ACTIVE | (8U << 16));
    g_busy = true;
    return true;
}

static void pwm_poll(void) {
    uint32_t cs;
    if (!g_busy) return;
    cs = mmio_read(DMA_CS);
    if ((cs & DMA_CS_END) || !(cs & DMA_CS_ACTIVE)) pwm_stop();
    if (g_tone_end && (int32_t)(rpi3_timer_ticks() - g_tone_end) >= 0)
        pwm_stop();
}

static bool pwm_tone(uint32_t hz, uint32_t milliseconds) {
    static uint8_t tone[4096];
    uint32_t rate = 22050U, period, length, i;
    if (!hz || !milliseconds) return false;
    period = rate / hz; if (period < 2U) period = 2U;
    length = (rate * milliseconds) / 1000U;
    if (!length) length = 1U; if (length > sizeof(tone)) length = sizeof(tone);
    for (i = 0U; i < length; ++i) tone[i] = (i % period) < period / 2U ? 208U : 48U;
    if (!pwm_play_pcm(tone, length, (uint16_t)rate, 192U)) return false;
    g_tone_end = rpi3_timer_ticks() +
        (milliseconds * RPI3_TIMER_HZ + 999U) / 1000U;
    return true;
}

static void pwm_play(uint32_t hz) { (void)pwm_tone(hz, 1000U); }
static void pwm_beep(uint32_t hz, uint32_t ms) { (void)pwm_tone(hz, ms); }
static bool pwm_available(void) { return true; }
static bool pwm_busy(void) { pwm_poll(); return g_busy; }
static bool no_sb16(void) { return false; }
static bool no_sb16_tone(uint32_t hz, uint32_t ms) { (void)hz; (void)ms; return false; }
static const char *pwm_name(void) { return "bcm2837-pwm-dma-pcm"; }

bool rpi3_audio_pwm_init(void) {
    static const sound_driver_ops_t ops = {
        pwm_poll, pwm_play, pwm_stop, pwm_beep, pwm_tone, pwm_play_pcm,
        pwm_available, pwm_busy, no_sb16, no_sb16_tone, pwm_busy,
        pwm_name, 200U
    };
    uint32_t select = mmio_read(GPIO_GPFSEL4);
    select &= ~((7U << 0) | (7U << 15));
    select |= (4U << 0) | (4U << 15); /* GPIO40/45: ALT0 PWM0/1. */
    mmio_write(GPIO_GPFSEL4, select);
    pwm_stop();
    if (!sound_register_driver(&ops)) return false;
    kprintf("[AUDIO:ARM] BCM2837 PWM stereo + DMA PCM online\n");
    return true;
}
