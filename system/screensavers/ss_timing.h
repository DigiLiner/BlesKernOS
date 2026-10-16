#ifndef BLESK_SS_TIMING_H
#define BLESK_SS_TIMING_H

/* Milliseconds keep the animation rate independent of the 100/300 Hz PIT.
 * Retain the fractional frame remainder instead of accumulating scheduler
 * delays. Render at most once after a stall, without a catch-up burst. */
#define SS_FRAME_MS 33U
static inline bool ss_frame_due(uint32_t now, uint32_t *last) {
    uint32_t elapsed = now - *last;
    if (elapsed < SS_FRAME_MS) return false;
    *last = now - elapsed % SS_FRAME_MS;
    return true;
}
#endif
