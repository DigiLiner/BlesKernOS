#define BK_API_IMPLEMENTATION
#include "kernel/include/api.h"
#include "kernel/include/compat_mode.h"
#include "kernel/include/perfmon.h"
#include "kernel/include/vision_ipc.h"
#include "kernel/include/system_sounds.h"

/* Driver information crosses the boundary by value, never as a supervisor
 * BSS pointer. The syscall presenter owns hardware access and mode changes. */
const gfx_info_t *gfx_get_info(void) {
    static gfx_info_t info;
    return bk_gfx_info(&info)?&info:NULL;
}
#ifndef VISION_ENGINE_FRAME_HOOK
#define VISION_ENGINE_FRAME_HOOK() ((void)0)
#endif
void vision_windows_dispatch(gui_desktop_t *desktop);
void vision_native_frames_dispatch(gui_desktop_t *desktop);
gui_rect_t deskbar_clock_rect(const gui_desktop_t *desktop);
static bool initialized;
static gui_event_queue_t events;
static uint32_t last_clock,last_paint,generation;
#include "vision_pacing_trace.h"

bool vision_engine_step(gui_desktop_t *desktop) {
    bk_vision_frame_t frame;gui_event_t event;
    if(bk_proc_exit_requested())return false;
#ifdef VISION_PACING_TRACE
    uint32_t trace_begin=bk_sys_ticks();
#endif
    if(!bk_vision_frame_begin(&frame)) {bk_sys_sleep_ms(20U);return true;}
#ifdef VISION_PACING_TRACE
    uint32_t trace_dispatch=bk_sys_ticks();
    if(trace_dispatch-trace_begin>pacing.begin_max)pacing.begin_max=trace_dispatch-trace_begin;
#endif
    vision_windows_dispatch(desktop);
#ifdef VISION_PACING_TRACE
    uint32_t trace_dispatch_end=bk_sys_ticks();
    if(trace_dispatch_end-trace_dispatch>pacing.dispatch_max)pacing.dispatch_max=trace_dispatch_end-trace_dispatch;
#endif
    VISION_ENGINE_FRAME_HOOK();
    if(!initialized) {
        gui_event_init(&events);last_clock=frame.ticks;last_paint=frame.ticks;
        generation=0U;initialized=true;
        uint16_t cs;__asm__ volatile("mov %%cs,%0":"=r"(cs));
        kprintf("[VISION:ENGINE] compositor y ventanas CPL=%u\n",cs&3U);
    }
    bool needs_paint=frame.paint_generation!=generation;
    if(frame.layout_refresh)deskmanager_refresh_layout();
    perfmon_gui_loop();
    uint64_t events_started=perfmon_scope_begin();
    gui_event_poll(&events);
    while(gui_event_next(&events,&event)) {
        perfmon_gui_event(event.type==GUI_EVENT_MOUSE_MOVE);
        bk_vision_note_input();
        if(event.type==GUI_EVENT_MOUSE_WHEEL && event.dy)
            (void)system_sound_event_play(SYSTEM_SOUND_SCROLL);
        gui_desktop_handle_event(desktop,&event);
        if(event.type!=GUI_EVENT_MOUSE_MOVE || !gui_compositor_move_hardware_cursor(desktop))needs_paint=true;
    }
    perfmon_scope_end(PERF_SCOPE_GUI_EVENTS,events_started);
    if(gui_compositor_tick_cursor(desktop,frame.ticks) ||
       gui_desktop_tick_minimize_animation(desktop,frame.ticks))needs_paint=true;
    if(frame.paint_generation!=generation && !gui_desktop_has_dirty(desktop))gui_desktop_invalidate_all(desktop);
    uint32_t hz=frame.frequency?frame.frequency:100U;
    if(frame.ticks-last_clock>=hz) {
        last_clock=frame.ticks;
        /* The panel owns its clock geometry. The current launcher panel has
         * no clock: damaging an old hard-coded location rebuilt the entire
         * GPU desktop once per second during otherwise cached animation. */
        gui_rect_t clock=deskbar_clock_rect(desktop);
        if(clock.w>0 && clock.h>0) {
            gui_desktop_invalidate_rect(desktop,clock);needs_paint=true;
        }
    }
    needs_paint=needs_paint || gui_desktop_has_dirty(desktop);
    uint32_t fps=compat_mode_gui_target_fps();if(!fps)fps=30U;
    uint32_t interval=hz/fps;if(!interval)interval=1U;
    uint32_t paint_tick=bk_sys_ticks();
#ifdef VISION_PACING_TRACE
    if(pacing.owner && !pacing_alive(desktop))pacing_flush();
#endif
    if((needs_paint || bk_vision_native_pending()) && paint_tick-last_paint>=interval) {
        /* Copy the latest completed capture only when its desktop frame is
         * due, so producer wakeups cannot flood the GPU with unseen copies. */
        vision_native_frames_dispatch(desktop);
        /* An unfinished GPU capture is a reason to poll, not a displayed
         * frame. Do not spend a presentation period on an empty repaint. */
        if(needs_paint || gui_desktop_has_dirty(desktop)) {
#ifdef VISION_PACING_TRACE
            uint32_t trace_owner=pacing_owner(desktop);
            uint32_t trace_presentations=gui_compositor_gpu_content_presentations();
#endif
            uint64_t started=perfmon_scope_begin();
            gui_desktop_paint(desktop);
            perfmon_scope_end(PERF_SCOPE_GUI_FRAME,started);
#ifdef VISION_PACING_TRACE
            if(gui_compositor_gpu_content_presentations()!=trace_presentations)
                pacing_note(trace_owner,paint_tick,bk_sys_ticks());
#endif
#ifdef VISION_FRAME_TRACE
            uint32_t elapsed=bk_sys_ticks()-frame.ticks;
            if(elapsed>hz/2U)
                kprintf("[VISION:FRAME] slow ticks=%u scene_wait=%u dirty=%u\n",elapsed,gui_desktop_scene_waiting(desktop),gui_desktop_has_dirty(desktop));
#endif
            generation=frame.paint_generation;
            /* Keep the scheduled cadence rather than adding dispatch/render
             * lateness to every following frame. Skip missed periods; never
             * accumulate a burst of catch-up repaints after an idle/slow frame. */
            last_paint+=((paint_tick-last_paint)/interval)*interval;
        }
    }
    bool dirty=gui_desktop_has_dirty(desktop);
    bk_vision_frame_end();
    uint32_t idle=compat_mode_gui_idle_hz();if(!idle)idle=50U;
    bk_vision_ipc_wait(dirty?1U:(hz/idle?hz/idle:1U));
    return true;
}
