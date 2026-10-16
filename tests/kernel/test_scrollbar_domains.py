#!/usr/bin/env python3
"""Guard the lock boundary between private scrollbar state and compositor GUI state."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/sched/kernel_domains.c').read_text()
helpers=s[s.index('static bool text_starts'):s.index('void kernel_domains_init')]
function=s[s.index('uint32_t kernel_domain_mask_for_api'):s.index('void kernel_domains_enter')]
header=(root/'kernel/include/kernel_domains.h').read_text()
enum=header[header.index('typedef enum'):header.index('#define KDOMAIN_COUNT')]
code='#include <stdint.h>\n#include <stdbool.h>\n#include <assert.h>\n#include <stdio.h>\n'+enum+helpers+function+r'''
int main(void){
 assert(kernel_domain_mask_for_api("gui_scrollbar_init_vertical")==0);
 assert(kernel_domain_mask_for_api("gui_scrollbar_handle_event_vertical")==0);
 assert(kernel_domain_mask_for_api("gui_scrollbar_paint_vertical")==KDOMAIN_GFX);
 assert(kernel_domain_mask_for_api("bk_gui_scrollbar_init_vertical")==0);
 assert(kernel_domain_mask_for_api("bk_gui_scrollbar_handle_event_vertical")==0);
 uint32_t paint=kernel_domain_mask_for_api("bk_gui_scrollbar_paint_vertical");
 assert(paint==KDOMAIN_GFX && !(paint&KDOMAIN_GUI));
 assert(kernel_domain_mask_for_api("bk_gui_add_menu_item")==KDOMAIN_GUI);
 assert(kernel_domain_mask_for_api("bk_gui_window_content_rect")==KDOMAIN_GUI);
 assert(kernel_domain_mask_for_api("bk_gui_destroy_window")==KDOMAIN_GUI);
 assert(kernel_domain_mask_for_api("bk_gui_surface_draw_text")==KDOMAIN_GFX);
 assert(kernel_domain_mask_for_api("program_execute_path")== (KDOMAIN_TASK|KDOMAIN_VFS));
 assert(kernel_domain_mask_for_api("program_execute_path_arg")== (KDOMAIN_TASK|KDOMAIN_VFS));
 assert(kernel_domain_mask_for_api("program_execute_path_async")==0);
 assert(kernel_domain_mask_for_api("program_execute_path_async_arg")==0);
 assert(kernel_domain_mask_for_api("bk_vision_driver_window_event")==0);
 puts("PASS caller-owned scrollbar helpers avoid GUI/GFX inversion; window mutations remain guarded");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-scroll-lock-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
