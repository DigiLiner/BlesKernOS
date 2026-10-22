/* Optional diagnostics: completed desktop submissions containing new native
 * content, NOT application swaps or physical monitor refresh timestamps.
 * Buffer the histogram; serial output never runs for each frame. */
#ifdef VISION_PACING_TRACE
uint32_t gui_compositor_gpu_content_presentations(void);
static struct {
    uint32_t owner, last, count, gaps[1001], paint_max, begin_max, dispatch_max, gap_max, gap_max_at;
    uint64_t gap_total;
} pacing;
static void pacing_flush(void) {
    if(pacing.count) {
        kprintf("[VISION:PACING] owner=%u intervals=%u hz=%u paint_max_ticks=%u begin_max=%u dispatch_max=%u gap_max=%u gap_max_at=%u gap_total=%u gaps=",
            pacing.owner,pacing.count,bk_sys_tick_frequency(),pacing.paint_max,pacing.begin_max,pacing.dispatch_max,pacing.gap_max,pacing.gap_max_at,(uint32_t)pacing.gap_total);
        for(unsigned i=0;i<1001;i++)if(pacing.gaps[i])kprintf("%u:%u,",i,pacing.gaps[i]);
        kprintf("\n");
    }
    kmemset(&pacing,0,sizeof(pacing));
}
static uint32_t pacing_owner(gui_desktop_t *desktop) {
    for(gui_window_t *w=desktop->first_window;w;w=w->next)
        if(w->visible && w->gpu_view_visible && w->gpu_view_frame_dirty)return w->owner_pid;
    return 0;
}
static bool pacing_alive(gui_desktop_t *desktop) {
    for(gui_window_t *w=desktop->first_window;w;w=w->next)
        if(w->owner_pid==pacing.owner && w->gpu_view_visible)return true;
    return false;
}
static void pacing_note(uint32_t owner,uint32_t started,uint32_t ended) {
    if(!owner)return;
    if(pacing.owner!=owner)pacing_flush();
    pacing.owner=owner;
    if(pacing.last) {
        uint32_t gap=ended-pacing.last;
        pacing.gaps[gap<1000U?gap:1000U]++;pacing.count++;pacing.gap_total+=gap;
        if(gap>pacing.gap_max){pacing.gap_max=gap;pacing.gap_max_at=pacing.count;}
    }
    pacing.last=ended;
    if(ended-started>pacing.paint_max)pacing.paint_max=ended-started;
}
#endif
