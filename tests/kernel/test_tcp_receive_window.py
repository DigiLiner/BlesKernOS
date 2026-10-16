#!/usr/bin/env python3
"""Actual TCP receive path reopens zero windows without flooding small updates."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/network/netstack.c').read_text()
types=s[s.index('typedef enum {\n    TCP_CLOSED'):s.index('#define UDP_HANDLE_BASE')]
f=s[s.index('static int32_t stack_socket_receive('):s.index('static int32_t stack_socket_sendto(')]
h=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define kmemcpy memmove
'''+types+r'''
static tcp_socket_t g_sockets[2];
static uint32_t g_tcp_buffer_lock,updates,last_window,ticks;
static bool udp_socket_valid(int h){return h==256;}
static bool socket_valid(int h){return h>=0 && h<2;}
static int32_t stack_socket_receivefrom(int h,void *a,void *p,void *d,uint32_t n,uint32_t t){return -17;}
static uint32_t pit_get_ticks(void){return ticks;}
static uint32_t timeout_ticks(uint32_t t){return t;}
static void wait_one_tick(void){ticks++;}
static void net_spin_lock(uint32_t *p){assert(!*p);*p=1;}
static void net_spin_unlock(uint32_t *p){assert(*p);*p=0;}
static bool tcp_send_segment(tcp_socket_t *s,uint8_t flags,const void *d,uint16_t n,uint32_t seq){
 assert(!g_tcp_buffer_lock && flags==TCP_FLAG_ACK && !d && !n && seq==s->send_next);
 updates++;last_window=TCP_RECEIVE_SIZE-s->receive_length;return true;
}
'''+f+r'''
int main(void){
 tcp_socket_t *s=&g_sockets[0];uint8_t out[8192];
 s->state=TCP_ESTABLISHED;s->peer=-1;s->receive_length=8192;s->send_next=42;
 for(unsigned i=0;i<8192;i++)s->receive[i]=(uint8_t)i;
 assert(stack_socket_receive(0,out,2048,10)==2048 && updates==0);
 for(unsigned i=0;i<2048;i++)assert(out[i]==(uint8_t)i);
 assert(stack_socket_receive(0,out,2048,10)==2048 && updates==1 && last_window==4096);
 for(unsigned i=0;i<2048;i++)assert(out[i]==(uint8_t)(i+2048));
 assert(stack_socket_receive(0,out,4096,10)==4096 && updates==1);
 assert(stack_socket_receive(0,out,4096,10)==0 && updates==1);
 s->receive_length=8192;
 assert(stack_socket_receive(0,out,8192,10)==8192 && updates==2 && last_window==8192);
 s->receive_length=8192;s->peer=1;
 assert(stack_socket_receive(0,out,8192,10)==8192 && updates==2);
 s->receive_length=8192;s->peer=-1;s->state=TCP_CLOSE_WAIT;
 assert(stack_socket_receive(0,out,8192,10)==8192 && updates==2);
 assert(stack_socket_receive(256,out,8192,10)==-17);
 assert(stack_socket_receive(-1,out,8192,10)==-1);
 assert(stack_socket_receive(0,NULL,8192,10)==-1);
 puts("PASS: TCP drain reopens receive windows, data preserved, ACK outside buffer lock, small reads coalesced, loopback/EOF/UDP preserved");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'check.c').write_text(h)
 subprocess.run(['gcc','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
