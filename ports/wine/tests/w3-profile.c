/* Linked only into --profile-w3 runs. No hot-path serial output. */
#include "../../../kernel/include/w3_profile.h"
#include "../../../kernel/include/pit.h"
#include "../../../kernel/include/vga.h"
#include "../../../kernel/include/memory.h"

typedef struct {
    uint64_t cycles;
    uint64_t maximum;
    uint32_t calls;
    uint32_t bytes;
    uint32_t sizes[5];
} w3p_stat_t;

static w3p_stat_t w3p_stats[W3P_LIMIT];
static uint64_t w3p_started;
static uint32_t w3p_tick_started;
static bool w3p_enabled;

void w3p_reset(void) {
    w3p_enabled = false;
    kmemset(w3p_stats, 0, sizeof(w3p_stats));
    w3p_tick_started = pit_get_ticks();
    w3p_started = w3p_clock();
    w3p_enabled = true;
    kprintf("[W3P:START] tick=%u hz=%u\n", w3p_tick_started,
            pit_get_frequency_hz());
}

void w3p_record(uint32_t id, uint64_t started, uint32_t bytes) {
    if (!w3p_enabled || id >= W3P_LIMIT) return;
    uint64_t elapsed = w3p_clock() - started;
    w3p_stat_t *stat = &w3p_stats[id];
    stat->calls++;
    stat->cycles += elapsed;
    if (elapsed > stat->maximum) stat->maximum = elapsed;
    stat->bytes += bytes;
    if (bytes) {
        uint32_t bin = bytes < 512U ? 0U : bytes <= 4096U ? 1U :
                       bytes <= 16384U ? 2U : bytes <= 65536U ? 3U : 4U;
        stat->sizes[bin]++;
    }
}

void w3p_dump(void) {
    uint64_t total = w3p_clock() - w3p_started;
    uint32_t hz = pit_get_frequency_hz();
    uint32_t elapsed_ms = hz ?
        (uint32_t)(((uint64_t)(pit_get_ticks() - w3p_tick_started) * 1000U) / hz) : 0U;
    w3p_enabled = false;
    kprintf("[W3P:END] elapsed_ms=%u\n", elapsed_ms);
    for (uint32_t i = 0U; i < W3P_LIMIT; i++) {
        const w3p_stat_t *s = &w3p_stats[i];
        if (!s->calls) continue;
        uint32_t ms = total ? (uint32_t)((s->cycles * elapsed_ms) / total) : 0U;
        uint32_t max_ms = total ? (uint32_t)((s->maximum * elapsed_ms) / total) : 0U;
        kprintf("[W3P] id=%u n=%u ms=%u max_ms=%u bytes=%u bins=%u,%u,%u,%u,%u\n",
                i, s->calls, ms, max_ms, s->bytes,
                s->sizes[0], s->sizes[1], s->sizes[2], s->sizes[3], s->sizes[4]);
    }
}
