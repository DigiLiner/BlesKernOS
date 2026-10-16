#!/usr/bin/env python3
"""Exercise live preferences, PS/2 + HID layout and desktop repeat timing."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(source,name):
 pos=source.index(name+'(');start=source.rfind('\n',0,pos)+1;opening=source.index('{',pos);depth=1;end=opening+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
header=(ROOT/'sdk/include/bleskernos_input.h').read_text()
constants='\n'.join(line for line in header.splitlines() if line.startswith('#define BK_'))
keys='\n'.join(line for line in (ROOT/'kernel/include/keyboard.h').read_text().splitlines() if line.startswith('#define KEY_'))
prefs=(ROOT/'kernel/ui/input_preferences.c').read_text();prefs=prefs[prefs.index('static uint32_t'):]
keyboard=(ROOT/'kernel/drivers/input/keyboard.c').read_text()
events=(ROOT/'gui/event.c').read_text();events=re.sub(r'^#include.*\n','',events,flags=re.M)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {bool extended_events;} gui_window_t;
typedef struct {gui_window_t *focused_window;} gui_desktop_t;
static gui_desktop_t desktop;static int invalidations;
static gui_desktop_t *gui_get_desktop(void){return &desktop;}
static void gui_desktop_invalidate_all(gui_desktop_t*d){(void)d;invalidations++;}
static void gui_request_paint(void){}
static uint32_t now;
static uint32_t pit_get_ticks(void){return now;}
static uint32_t pit_get_frequency_hz(void){return 1000;}
static void task_preempt_disable(void){}
static void task_preempt_enable(void){}
static void gui_widget_tick_caret(uint32_t ms){(void)ms;}
typedef struct {uint8_t key;bool pressed;} kbd_key_event_t;
typedef struct {bool shift,ctrl,alt,caps_lock,num_lock;} kbd_modifiers_t;
static kbd_modifiers_t mods;
static kbd_key_event_t input[8];static int input_pos,input_count;
static bool kbd_next_event(kbd_key_event_t*e){if(input_pos==input_count)return false;*e=input[input_pos++];return true;}
typedef struct {int x,y,wheel;uint8_t buttons;} mouse_state_t;
static void mouse_get_state(mouse_state_t*m){memset(m,0,sizeof(*m));}
enum {GUI_EVENT_MOUSE_MOVE,GUI_EVENT_MOUSE_DOWN,GUI_EVENT_MOUSE_UP,GUI_EVENT_MOUSE_WHEEL,GUI_EVENT_KEY,GUI_EVENT_KEY_UP};
#define MOUSE_LEFT_BUTTON 1
#define MOUSE_RIGHT_BUTTON 2
#define MOUSE_MIDDLE_BUTTON 4
#define GUI_MAX_EVENTS 64
typedef struct {int type,x,y,dx,dy;uint8_t buttons,button;char key;bool shift,ctrl,alt;} gui_event_t;
typedef struct {gui_event_t queue[GUI_MAX_EVENTS];uint32_t head,tail;int last_mouse_x,last_mouse_y,last_mouse_wheel;uint8_t last_mouse_buttons;} gui_event_queue_t;
'''+constants+'\n'+keys+'\n'+prefs
for name in ['kbd_map_scancode','kbd_hid_usage_to_char']:code+=function(keyboard,name)
code+=events
code+=r'''
static void key(uint8_t k,bool down){input_count=1;input_pos=0;input[0]=(kbd_key_event_t){k,down};}
static int drain(gui_event_queue_t*q){int n=0;gui_event_t e;while(gui_event_next(q,&e))n++;return n;}
int main(void){
 assert(!bk_input_preference_set(100,1));assert(!bk_input_preference_set(BK_INPUT_DOUBLE_CLICK,10));
 assert(!bk_input_preference_set(BK_INPUT_REPEAT_INTERVAL,0));assert(!bk_input_preference_set(BK_INPUT_CARET_BLINK,1));
 assert(bk_input_preference_set(BK_INPUT_CURSOR_SCHEME,2));assert(invalidations==1);
 assert(bk_input_preference_set(BK_INPUT_CURSOR_SCHEME,2));assert(invalidations==1);
 assert(kbd_map_scancode(0x27,false,false)==';');assert(kbd_hid_usage_to_char(0x33)==';');
 assert(bk_input_preference_set(BK_INPUT_LAYOUT,1));assert((uint8_t)kbd_map_scancode(0x27,false,false)==0xF1);
 assert((uint8_t)kbd_hid_usage_to_char(0x33)==0xF1);mods.shift=true;
 assert(kbd_map_scancode(0x03,true,false)=='"');assert(kbd_hid_usage_to_char(0x1F)=='"');mods.shift=false;
 assert(bk_input_preference_set(BK_INPUT_LAYOUT,0));
 gui_window_t w={0},other={0};desktop.focused_window=&w;gui_event_queue_t q;gui_event_init(&q);
 now=0;key('a',true);gui_event_poll(&q);assert(drain(&q)==1);
 now=100;key('a',true);gui_event_poll(&q);assert(drain(&q)==0); /* Ignore hardware repeats. */
 now=499;gui_event_poll(&q);assert(drain(&q)==0);
 now=500;gui_event_poll(&q);assert(drain(&q)==1);
 now=560;gui_event_poll(&q);assert(drain(&q)==1);
 now=570;key('a',false);gui_event_poll(&q);assert(drain(&q)==0);
 now=1000;gui_event_poll(&q);assert(drain(&q)==0);
 key('A',true);gui_event_poll(&q);assert(drain(&q)==1);key('a',false);gui_event_poll(&q);
 now=2000;gui_event_poll(&q);assert(drain(&q)==0);
 key('!',true);gui_event_poll(&q);assert(drain(&q)==1);key('1',false);gui_event_poll(&q);
 now=2600;gui_event_poll(&q);assert(drain(&q)==0);
 key('b',true);gui_event_poll(&q);assert(drain(&q)==1);desktop.focused_window=&other;
 now=3000;gui_event_poll(&q);assert(drain(&q)==0); /* Never repeat into a newly focused app. */
 assert(bk_input_preference_set(BK_INPUT_REPEAT_DELAY,250));assert(bk_input_preference_set(BK_INPUT_REPEAT_INTERVAL,30));
 key('c',true);gui_event_poll(&q);assert(drain(&q)==1);now=3250;gui_event_poll(&q);assert(drain(&q)==1);
 now=3280;gui_event_poll(&q);assert(drain(&q)==1);
 puts("PASS desktop input: validated global preferences, stock cursor invalidation, PS/2 + USB Spanish layout, repeat delay/rate, key release and focus isolation");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-input-preferences-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
