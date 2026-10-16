#!/usr/bin/env python3
"""Menu commands keep their original target when another row or tab becomes selected."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'programs/processmanager.c').read_text()
f=s[s.index('static void menu_action('):s.index('static void show_actions(')]
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define UNUSED __attribute__((unused))
typedef void gui_window_t;
typedef struct {
 void *window;
 uint32_t expanded,selected_pid,selected_id,tab;
 uint32_t menu_pid,menu_id,menu_tab,events_pid,events_id,events_tab;
 uint32_t pending_action,pending_id,pending_pid,pending_toggle_id,pending_edit_id,pending_remove_id;
 bool pending_events,pending_toggle,pending_edit,pending_remove;
} processmanager_state_t;
static void collect(processmanager_state_t *s){(void)s;}
static void bk_gui_window_invalidate(void *w){(void)w;}
'''+f+r'''
int main(void){
 processmanager_state_t s={.menu_pid=42,.menu_id=7,.menu_tab=0,.selected_pid=999,.selected_id=12,.tab=3};
 menu_action(NULL,10,&s);assert(s.pending_action==10&&s.pending_pid==42);
 menu_action(NULL,43,&s);assert(s.pending_action==43&&s.pending_pid==42);
 menu_action(NULL,21,&s);assert(s.pending_events&&s.events_pid==42&&s.events_tab==0);
 menu_action(NULL,2,&s);assert(s.pending_action==2&&s.pending_id==7);
 menu_action(NULL,30,&s);assert(s.pending_toggle_id==7);
 menu_action(NULL,32,&s);assert(s.pending_edit_id==7);
 menu_action(NULL,33,&s);assert(s.pending_remove_id==7);
 menu_action(NULL,20,&s);assert(s.expanded==42);
 menu_action(NULL,31,&s);assert(s.pending_edit_id==0);
 puts("PASS process, service, startup and event actions retain the menu's original target");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-menu-target-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
