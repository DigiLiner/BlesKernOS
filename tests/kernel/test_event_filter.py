#!/usr/bin/env python3
"""Run the viewer's real filtering/scroll anchoring against changing snapshots."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'programs/event_viewer.c').read_text()
refresh=s[s.index('static bk_gui_rect_t log_rect('):s.index('static void action(')]
header=(root/'sdk/include/bleskernos_admin.h').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
'''+header+r'''
typedef struct {int x,y,w,h;} bk_gui_rect_t;
typedef struct {void *window;char query[96];bk_gui_rect_t client;uint32_t source,level,offset,count,filter_pid,visual_count,layout_columns;uint16_t visual_event[BK_EVENT_CAPACITY*8],visual_part[BK_EVENT_CAPACITY*8];bk_event_info_t rows[BK_EVENT_CAPACITY];} event_state_t;
static bk_event_info_t feed[BK_EVENT_CAPACITY];static uint32_t total;
static uint32_t st_length(const char *s){return strlen(s);}
static void event_line(bk_event_info_t *e,char *line,uint32_t capacity){snprintf(line,capacity,"%s",e->message);}
uint32_t bk_event_count(void){return total;}
bool bk_event_get(uint32_t i,bk_event_info_t *out){if(i>=total)return false;*out=feed[i];return true;}
static bool st_contains_ci(const char *s,const char *q){if(!*q)return true;for(;*s;s++){const char *a=s,*b=q;while(*a&&*b&&toupper(*a)==toupper(*b)){a++;b++;}if(!*b)return true;}return false;}
'''+refresh+r'''
int main(void){
 event_state_t s={.source=7,.level=3,.filter_pid=42,.client={100,80,700,300}};total=40;
 for(uint32_t i=0;i<total;i++){feed[i].sequence=100-i;feed[i].pid=i%2?42:99;strcpy(feed[i].message,"worker report");}
 refresh(&s);assert(s.count==20);for(uint32_t i=0;i<s.count;i++)assert(s.rows[i].pid==42);
 s.offset=3;uint32_t anchor=s.rows[s.offset].sequence;
 memmove(feed+1,feed,total*sizeof(feed[0]));total++;feed[0].sequence=101;feed[0].pid=42;
 refresh(&s);assert(s.rows[s.offset].sequence==anchor&&s.offset==4);
 strcpy(s.query,"missing");refresh(&s);assert(s.count==0&&s.offset==0&&s.filter_pid==42);
 strcpy(s.query,"WORKER");refresh(&s);assert(s.count==21);
 s.source=5;refresh(&s);assert(s.count==0&&s.filter_pid==42);
 s.source=7;s.level=2;feed[0].severity=2;refresh(&s);assert(s.count==1&&s.rows[0].pid==42);
 s.level=3;s.query[0]=0;total=1;memset(feed[0].message,'x',sizeof(feed[0].message)-1);feed[0].message[sizeof(feed[0].message)-1]=0;
 refresh(&s);assert(s.visual_count>1);
 for(uint32_t i=0;i<s.visual_count;i++)assert(s.visual_event[i]==0&&s.visual_part[i]==i);
 s.client.h=40;s.offset=1;uint32_t sequence=s.rows[0].sequence;
 memmove(feed+1,feed,sizeof(feed[0]));total=2;feed[0].sequence=sequence+1;strcpy(feed[0].message,"new");
 refresh(&s);assert(s.rows[s.visual_event[s.offset]].sequence==sequence&&s.visual_part[s.offset]==1);
 puts("PASS strict process filter, empty results, source/severity/search and scroll anchoring");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-event-filter-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
