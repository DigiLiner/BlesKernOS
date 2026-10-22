.text
.globl __lbs_call_bk_about_attach
.type __lbs_call_bk_about_attach,@function
__lbs_call_bk_about_attach:
 jmp *__bk_lbs_app_slots+0
.size __lbs_call_bk_about_attach,.-__lbs_call_bk_about_attach
.globl __lbs_call_bk_app_launch
.type __lbs_call_bk_app_launch,@function
__lbs_call_bk_app_launch:
 jmp *__bk_lbs_app_slots+4
.size __lbs_call_bk_app_launch,.-__lbs_call_bk_app_launch
.globl __lbs_call_bk_file_list_dir
.type __lbs_call_bk_file_list_dir,@function
__lbs_call_bk_file_list_dir:
 jmp *__bk_lbs_app_slots+8
.size __lbs_call_bk_file_list_dir,.-__lbs_call_bk_file_list_dir
.globl __lbs_call_bk_file_read_all
.type __lbs_call_bk_file_read_all,@function
__lbs_call_bk_file_read_all:
 jmp *__bk_lbs_app_slots+12
.size __lbs_call_bk_file_read_all,.-__lbs_call_bk_file_read_all
.globl __lbs_call_bk_gui_create_window
.type __lbs_call_bk_gui_create_window,@function
__lbs_call_bk_gui_create_window:
 jmp *__bk_lbs_app_slots+16
.size __lbs_call_bk_gui_create_window,.-__lbs_call_bk_gui_create_window
.globl __lbs_call_bk_gui_set_window_content
.type __lbs_call_bk_gui_set_window_content,@function
__lbs_call_bk_gui_set_window_content:
 jmp *__bk_lbs_app_slots+20
.size __lbs_call_bk_gui_set_window_content,.-__lbs_call_bk_gui_set_window_content
.globl __lbs_call_bk_gui_set_window_event_handler
.type __lbs_call_bk_gui_set_window_event_handler,@function
__lbs_call_bk_gui_set_window_event_handler:
 jmp *__bk_lbs_app_slots+24
.size __lbs_call_bk_gui_set_window_event_handler,.-__lbs_call_bk_gui_set_window_event_handler
.globl __lbs_call_bk_gui_set_window_min_size
.type __lbs_call_bk_gui_set_window_min_size,@function
__lbs_call_bk_gui_set_window_min_size:
 jmp *__bk_lbs_app_slots+28
.size __lbs_call_bk_gui_set_window_min_size,.-__lbs_call_bk_gui_set_window_min_size
.globl __lbs_call_bk_gui_widget_set_icon
.type __lbs_call_bk_gui_widget_set_icon,@function
__lbs_call_bk_gui_widget_set_icon:
 jmp *__bk_lbs_app_slots+32
.size __lbs_call_bk_gui_widget_set_icon,.-__lbs_call_bk_gui_widget_set_icon
.globl __lbs_call_bk_ngs_checker
.type __lbs_call_bk_ngs_checker,@function
__lbs_call_bk_ngs_checker:
 jmp *__bk_lbs_app_slots+36
.size __lbs_call_bk_ngs_checker,.-__lbs_call_bk_ngs_checker
.globl __lbs_call_bk_ngs_image_blit
.type __lbs_call_bk_ngs_image_blit,@function
__lbs_call_bk_ngs_image_blit:
 jmp *__bk_lbs_app_slots+40
.size __lbs_call_bk_ngs_image_blit,.-__lbs_call_bk_ngs_image_blit
.globl __lbs_call_bk_proc_bind_window
.type __lbs_call_bk_proc_bind_window,@function
__lbs_call_bk_proc_bind_window:
 jmp *__bk_lbs_app_slots+44
.size __lbs_call_bk_proc_bind_window,.-__lbs_call_bk_proc_bind_window
.globl __lbs_call_bk_proc_exit
.type __lbs_call_bk_proc_exit,@function
__lbs_call_bk_proc_exit:
 jmp *__bk_lbs_app_slots+48
.size __lbs_call_bk_proc_exit,.-__lbs_call_bk_proc_exit
.globl __lbs_call_bk_proc_exit_requested
.type __lbs_call_bk_proc_exit_requested,@function
__lbs_call_bk_proc_exit_requested:
 jmp *__bk_lbs_app_slots+52
.size __lbs_call_bk_proc_exit_requested,.-__lbs_call_bk_proc_exit_requested
.globl __lbs_call_bk_proc_spawn_thread
.type __lbs_call_bk_proc_spawn_thread,@function
__lbs_call_bk_proc_spawn_thread:
 jmp *__bk_lbs_app_slots+56
.size __lbs_call_bk_proc_spawn_thread,.-__lbs_call_bk_proc_spawn_thread
.globl __lbs_call_bk_sys_alloc
.type __lbs_call_bk_sys_alloc,@function
__lbs_call_bk_sys_alloc:
 jmp *__bk_lbs_app_slots+60
.size __lbs_call_bk_sys_alloc,.-__lbs_call_bk_sys_alloc
.globl __lbs_call_bk_sys_alloc_zero
.type __lbs_call_bk_sys_alloc_zero,@function
__lbs_call_bk_sys_alloc_zero:
 jmp *__bk_lbs_app_slots+64
.size __lbs_call_bk_sys_alloc_zero,.-__lbs_call_bk_sys_alloc_zero
.globl __lbs_call_bk_sys_free
.type __lbs_call_bk_sys_free,@function
__lbs_call_bk_sys_free:
 jmp *__bk_lbs_app_slots+68
.size __lbs_call_bk_sys_free,.-__lbs_call_bk_sys_free
.globl __lbs_call_bk_sys_getpid
.type __lbs_call_bk_sys_getpid,@function
__lbs_call_bk_sys_getpid:
 jmp *__bk_lbs_app_slots+72
.size __lbs_call_bk_sys_getpid,.-__lbs_call_bk_sys_getpid
.globl __lbs_call_bk_sys_sleep_ticks
.type __lbs_call_bk_sys_sleep_ticks,@function
__lbs_call_bk_sys_sleep_ticks:
 jmp *__bk_lbs_app_slots+76
.size __lbs_call_bk_sys_sleep_ticks,.-__lbs_call_bk_sys_sleep_ticks
.globl __lbs_call_bk_sys_ticks
.type __lbs_call_bk_sys_ticks,@function
__lbs_call_bk_sys_ticks:
 jmp *__bk_lbs_app_slots+80
.size __lbs_call_bk_sys_ticks,.-__lbs_call_bk_sys_ticks
.globl __lbs_call_bk_sys_uptime_ms
.type __lbs_call_bk_sys_uptime_ms,@function
__lbs_call_bk_sys_uptime_ms:
 jmp *__bk_lbs_app_slots+84
.size __lbs_call_bk_sys_uptime_ms,.-__lbs_call_bk_sys_uptime_ms
.globl __lbs_call_gui_desktop_remove_window
.type __lbs_call_gui_desktop_remove_window,@function
__lbs_call_gui_desktop_remove_window:
 jmp *__bk_lbs_app_slots+88
.size __lbs_call_gui_desktop_remove_window,.-__lbs_call_gui_desktop_remove_window
.globl __lbs_call_gui_font_draw_string
.type __lbs_call_gui_font_draw_string,@function
__lbs_call_gui_font_draw_string:
 jmp *__bk_lbs_app_slots+92
.size __lbs_call_gui_font_draw_string,.-__lbs_call_gui_font_draw_string
.globl __lbs_call_gui_font_draw_string_clipped
.type __lbs_call_gui_font_draw_string_clipped,@function
__lbs_call_gui_font_draw_string_clipped:
 jmp *__bk_lbs_app_slots+96
.size __lbs_call_gui_font_draw_string_clipped,.-__lbs_call_gui_font_draw_string_clipped
.globl __lbs_call_gui_gfx_draw_rect
.type __lbs_call_gui_gfx_draw_rect,@function
__lbs_call_gui_gfx_draw_rect:
 jmp *__bk_lbs_app_slots+100
.size __lbs_call_gui_gfx_draw_rect,.-__lbs_call_gui_gfx_draw_rect
.globl __lbs_call_gui_gfx_fill_rect
.type __lbs_call_gui_gfx_fill_rect,@function
__lbs_call_gui_gfx_fill_rect:
 jmp *__bk_lbs_app_slots+104
.size __lbs_call_gui_gfx_fill_rect,.-__lbs_call_gui_gfx_fill_rect
.globl __lbs_call_gui_gif_animation_free
.type __lbs_call_gui_gif_animation_free,@function
__lbs_call_gui_gif_animation_free:
 jmp *__bk_lbs_app_slots+108
.size __lbs_call_gui_gif_animation_free,.-__lbs_call_gui_gif_animation_free
.globl __lbs_call_gui_gif_load_animation_limited
.type __lbs_call_gui_gif_load_animation_limited,@function
__lbs_call_gui_gif_load_animation_limited:
 jmp *__bk_lbs_app_slots+112
.size __lbs_call_gui_gif_load_animation_limited,.-__lbs_call_gui_gif_load_animation_limited
.globl __lbs_call_gui_image_free
.type __lbs_call_gui_image_free,@function
__lbs_call_gui_image_free:
 jmp *__bk_lbs_app_slots+116
.size __lbs_call_gui_image_free,.-__lbs_call_gui_image_free
.globl __lbs_call_gui_rect_contains
.type __lbs_call_gui_rect_contains,@function
__lbs_call_gui_rect_contains:
 jmp *__bk_lbs_app_slots+120
.size __lbs_call_gui_rect_contains,.-__lbs_call_gui_rect_contains
.globl __lbs_call_gui_widget_create
.type __lbs_call_gui_widget_create,@function
__lbs_call_gui_widget_create:
 jmp *__bk_lbs_app_slots+124
.size __lbs_call_gui_widget_create,.-__lbs_call_gui_widget_create
.globl __lbs_call_gui_window_content_rect
.type __lbs_call_gui_window_content_rect,@function
__lbs_call_gui_window_content_rect:
 jmp *__bk_lbs_app_slots+128
.size __lbs_call_gui_window_content_rect,.-__lbs_call_gui_window_content_rect
.globl __lbs_call_gui_window_destroy
.type __lbs_call_gui_window_destroy,@function
__lbs_call_gui_window_destroy:
 jmp *__bk_lbs_app_slots+132
.size __lbs_call_gui_window_destroy,.-__lbs_call_gui_window_destroy
.globl __lbs_call_kmemcpy
.type __lbs_call_kmemcpy,@function
__lbs_call_kmemcpy:
 jmp *__bk_lbs_app_slots+136
.size __lbs_call_kmemcpy,.-__lbs_call_kmemcpy
.globl __lbs_call_kstrcmp
.type __lbs_call_kstrcmp,@function
__lbs_call_kstrcmp:
 jmp *__bk_lbs_app_slots+140
.size __lbs_call_kstrcmp,.-__lbs_call_kstrcmp
.globl __lbs_call_kstrlen
.type __lbs_call_kstrlen,@function
__lbs_call_kstrlen:
 jmp *__bk_lbs_app_slots+144
.size __lbs_call_kstrlen,.-__lbs_call_kstrlen
.globl __lbs_call_kstrncpy
.type __lbs_call_kstrncpy,@function
__lbs_call_kstrncpy:
 jmp *__bk_lbs_app_slots+148
.size __lbs_call_kstrncpy,.-__lbs_call_kstrncpy
.globl __lbs_call_program_launch_arg
.type __lbs_call_program_launch_arg,@function
__lbs_call_program_launch_arg:
 jmp *__bk_lbs_app_slots+152
.size __lbs_call_program_launch_arg,.-__lbs_call_program_launch_arg
.section .note.GNU-stack,"",@progbits
