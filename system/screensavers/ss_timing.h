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
/* Bound simulation work after a long stall without tying motion to renders. */
static inline uint32_t ss_steps_due(uint32_t now, uint32_t *last,
                                    uint32_t interval, uint32_t maximum) {
    uint32_t elapsed = now - *last;
    uint32_t steps = elapsed / interval;
    if (steps) *last = now - elapsed % interval;
    return steps < maximum ? steps : maximum;
}
#endif
