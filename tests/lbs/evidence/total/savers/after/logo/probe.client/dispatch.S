.text
.globl __lbs_call_bk_gui_desktop
.type __lbs_call_bk_gui_desktop,@function
__lbs_call_bk_gui_desktop:
 jmp *__bk_lbs_app_slots+0
.size __lbs_call_bk_gui_desktop,.-__lbs_call_bk_gui_desktop
.globl __lbs_call_bk_gui_invalidate_all
.type __lbs_call_bk_gui_invalidate_all,@function
__lbs_call_bk_gui_invalidate_all:
 jmp *__bk_lbs_app_slots+4
.size __lbs_call_bk_gui_invalidate_all,.-__lbs_call_bk_gui_invalidate_all
.globl __lbs_call_bk_gui_request_paint
.type __lbs_call_bk_gui_request_paint,@function
__lbs_call_bk_gui_request_paint:
 jmp *__bk_lbs_app_slots+8
.size __lbs_call_bk_gui_request_paint,.-__lbs_call_bk_gui_request_paint
.globl __lbs_call_bk_proc_exit_requested
.type __lbs_call_bk_proc_exit_requested,@function
__lbs_call_bk_proc_exit_requested:
 jmp *__bk_lbs_app_slots+12
.size __lbs_call_bk_proc_exit_requested,.-__lbs_call_bk_proc_exit_requested
.globl __lbs_call_bk_sys_alloc_zero
.type __lbs_call_bk_sys_alloc_zero,@function
__lbs_call_bk_sys_alloc_zero:
 jmp *__bk_lbs_app_slots+16
.size __lbs_call_bk_sys_alloc_zero,.-__lbs_call_bk_sys_alloc_zero
.globl __lbs_call_bk_sys_free
.type __lbs_call_bk_sys_free,@function
__lbs_call_bk_sys_free:
 jmp *__bk_lbs_app_slots+20
.size __lbs_call_bk_sys_free,.-__lbs_call_bk_sys_free
.globl __lbs_call_bk_sys_sleep_ms
.type __lbs_call_bk_sys_sleep_ms,@function
__lbs_call_bk_sys_sleep_ms:
 jmp *__bk_lbs_app_slots+24
.size __lbs_call_bk_sys_sleep_ms,.-__lbs_call_bk_sys_sleep_ms
.globl __lbs_call_bk_sys_uptime_ms
.type __lbs_call_bk_sys_uptime_ms,@function
__lbs_call_bk_sys_uptime_ms:
 jmp *__bk_lbs_app_slots+28
.size __lbs_call_bk_sys_uptime_ms,.-__lbs_call_bk_sys_uptime_ms
.globl __lbs_call_gui_font_draw_string
.type __lbs_call_gui_font_draw_string,@function
__lbs_call_gui_font_draw_string:
 jmp *__bk_lbs_app_slots+32
.size __lbs_call_gui_font_draw_string,.-__lbs_call_gui_font_draw_string
.globl __lbs_call_gui_font_draw_string_scaled
.type __lbs_call_gui_font_draw_string_scaled,@function
__lbs_call_gui_font_draw_string_scaled:
 jmp *__bk_lbs_app_slots+36
.size __lbs_call_gui_font_draw_string_scaled,.-__lbs_call_gui_font_draw_string_scaled
.globl __lbs_call_gui_gfx_draw_rect
.type __lbs_call_gui_gfx_draw_rect,@function
__lbs_call_gui_gfx_draw_rect:
 jmp *__bk_lbs_app_slots+40
.size __lbs_call_gui_gfx_draw_rect,.-__lbs_call_gui_gfx_draw_rect
.globl __lbs_call_gui_gfx_fill_rect
.type __lbs_call_gui_gfx_fill_rect,@function
__lbs_call_gui_gfx_fill_rect:
 jmp *__bk_lbs_app_slots+44
.size __lbs_call_gui_gfx_fill_rect,.-__lbs_call_gui_gfx_fill_rect
.section .note.GNU-stack,"",@progbits
