#!/usr/bin/env python3
"""PS/2 absence/stuck ports must not block USB-only system boot."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/input/keyboard.c').read_text();s=s[s.index('static bool kbd_wait_input('):s.index('\n#endif',s.index('static bool kbd_wait_input('))]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define KBD_STATUS_PORT 0x64
#define KBD_DATA_PORT 0x60
static unsigned mode,reads,writes;static bool reply_ready;
static uint8_t inb(uint16_t p){reads++;if(mode==1)return 0xff;if(mode==2)return p==0x64?2:0;if(mode==3)return 0;if(mode==4)return 1;if(p==0x64)return reply_ready?1:0;reply_ready=false;return 0xfa;}
static void outb(uint16_t p,uint8_t v){assert(p==0x60&&v==0xf4);writes++;reply_ready=true;}
'''+s+r'''
int main(void){
 mode=1;assert(!kbd_flush()&&!kbd_wait_input()&&!kbd_wait_output());assert(reads==3);
 mode=2;reads=0;assert(!kbd_wait_input()&&reads==200000);
 mode=3;reads=0;assert(!kbd_wait_output()&&reads==200000);
 mode=4;reads=0;assert(!kbd_flush()&&reads==512);
 mode=0;assert(kbd_flush()&&kbd_send_cmd(0xf4)&&writes==1);
 puts("PASS: missing/stuck 8042 is bounded; normal keyboard enable/ACK preserved");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';e=Path(tmp)/'test';c.write_text(code)
 subprocess.run(['gcc','-O1','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
