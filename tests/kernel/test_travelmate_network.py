#!/usr/bin/env python3
"""Production DHCP loss/malformed-packet and RTL8139 DMA ring regressions."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/drivers/network/netstack.c').read_text()
def fn(source,signature):
 start=source.index(signature);end=source.index('\n}',start)+2
 return source[start:end]
packet=s[s.index('typedef struct {\n    uint8_t op,'):s.index('} PACKED dhcp_packet_t;')+len('} PACKED dhcp_packet_t;')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define PACKED __attribute__((packed))
#define kmemset memset
#define kmemcpy memcpy
#define kmemcmp memcmp
#define DHCP_MAGIC 0x63825363U
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
static uint8_t g_mac[6]={2,3,4,5,6,7},g_ip[4],g_mask[4],g_gateway[4],g_dns[4];
static bool g_configured,g_arp_valid;
static volatile uint8_t g_dhcp_type,g_dhcp_phase;
static volatile uint32_t g_dhcp_busy;
static uint32_t g_dhcp_xid,ticks,discovers,requests;
static uint8_t g_dhcp_offer[4],g_dhcp_server[4],g_dhcp_mask[4],g_dhcp_gateway[4],g_dhcp_dns[4];
static uint32_t pit_get_ticks(void){return ticks;}
static uint32_t timeout_ticks(uint32_t ms){return ms?ms:1;}
static bool refresh_device_mac(void){return true;}
static bool udp_send(const uint8_t *,uint16_t,uint16_t,const void *,uint16_t,bool);
static void wait_one_tick(void);
'''+packet+'\n'
for signature in ['static uint16_t be16(', 'static uint32_t be32(', 'static bool bytes_equal(', 'static bool ip_zero(', 'static uint8_t *dhcp_option(', 'static bool dhcp_send(', 'static void dhcp_receive(', 'static bool stack_configure(', 'static bool dhcp_wait_phase(', 'static bool stack_dhcp(']:
 # One-line helpers end on their own line, rather than the next function.
 start=s.index(signature);line=s[start:s.index('\n',start)]
 code+=(line if line.endswith('}') else fn(s,signature))+'\n'
code+=r'''
static void response(uint8_t type,bool params){
 dhcp_packet_t p={0};p.op=2;p.htype=1;p.hlen=6;p.xid=be32(g_dhcp_xid);
 p.magic=be32(DHCP_MAGIC);memcpy(p.chaddr,g_mac,6);
 const uint8_t ip[4]={192,168,1,45},srv[4]={192,168,1,1},mask[4]={255,255,255,0},dns[4]={192,168,1,9};
 memcpy(p.yiaddr,ip,4);uint8_t *o=p.options;
 o=dhcp_option(o,53,&type,1);o=dhcp_option(o,54,srv,4);
 if(params){o=dhcp_option(o,1,mask,4);o=dhcp_option(o,3,srv,4);o=dhcp_option(o,6,dns,4);}
 *o++=255;dhcp_receive((void *)&p,(uint16_t)(o-(uint8_t *)&p));
}
static bool udp_send(const uint8_t *to,uint16_t src,uint16_t dst,const void *data,uint16_t n,bool broadcast){
 const dhcp_packet_t *p=data;assert(to[0]==255 && src==68 && dst==67 && broadcast && n>=300 && n<=sizeof(*p));
 assert(p->flags==be16(0x8000) && p->xid==be32(g_dhcp_xid));
 assert(p->options[0]==53 && p->options[1]==1);
 if(p->options[2]==1)discovers++;else {assert(p->options[2]==3);requests++;}
 return true;
}
static void wait_one_tick(void){
 ticks++;
 if(g_dhcp_phase==1 && discovers>=2 && !g_dhcp_type)response(2,true);
 if(g_dhcp_phase==2 && requests>=2 && !g_dhcp_type)response(5,false);
}
int main(void){
 assert(stack_dhcp(6000));assert(discovers==2 && requests==2);
 assert(g_configured && !g_dhcp_phase && !g_dhcp_busy);
 assert(g_ip[3]==45 && g_mask[3]==0 && g_gateway[3]==1 && g_dns[3]==9);
 /* Late, malformed and foreign replies cannot erase negotiated metadata. */
 response(2,false);assert(g_dhcp_dns[3]==9);
 g_dhcp_phase=2;response(2,false);assert(g_dhcp_type==5 && g_dhcp_dns[3]==9);
 dhcp_packet_t bad={0};bad.op=2;bad.htype=1;bad.hlen=6;bad.xid=be32(g_dhcp_xid);
 bad.magic=be32(DHCP_MAGIC);memcpy(bad.chaddr,g_mac,6);memcpy(bad.yiaddr,g_ip,4);
 bad.options[0]=6;bad.options[1]=4;memset(bad.options+2,0,4);
 bad.options[6]=53;bad.options[7]=2; // truncated option
 dhcp_receive((void *)&bad,248);assert(g_dhcp_dns[3]==9);
 bad.options[0]=53;bad.options[1]=1;bad.options[2]=5;bad.options[3]=54;bad.options[4]=4;
 memset(bad.options+5,77,4);bad.options[9]=255;
 dhcp_receive((void *)&bad,250);assert(g_dhcp_dns[3]==9);
 g_dhcp_phase=0;g_dhcp_busy=1;assert(!stack_dhcp(100));g_dhcp_busy=0;
 puts("PASS production DHCP retransmits lost DISCOVER/REQUEST, inherits OFFER DNS/routes, rejects malformed/foreign/late replies and serializes callers");
}
'''
r=(root/'kernel/drivers/network/rtl8139.c').read_text()
rtl=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define kmemcpy memcpy
#define kmemset memset
#define NET_FRAME_MAX 1600
'''+ '\n'.join(line for line in r.splitlines() if line.startswith('#define RTL_'))+r'''
static struct {uint8_t *rx_ring;uint32_t rx_offset,rx_pending_tick,tx_lock;bool rx_pending,running;uint16_t io;uint8_t tx_slot,tx_threshold;uint8_t *tx_buffer[4];} g_rtl;
static bool empty;static unsigned received;static uint16_t capr;
static uint32_t ticks;
static uint32_t pit_get_ticks(void){return ticks;}
static uint32_t pit_get_frequency_hz(void){return 1000;}
static uint32_t transmit_status=RTL_TX_OWN,last_transmit;
static void outl(uint16_t p,uint32_t v){if(p>=RTL_TX_STATUS && p<RTL_TX_STATUS+16)last_transmit=v;}
static uint32_t inl(uint16_t p){(void)p;return transmit_status;}
static void task_yield(void){ticks++;}
static uint8_t inb(uint16_t p){(void)p;return empty?RTL_CMD_RX_EMPTY:0;}
static uint16_t in16(uint16_t p){(void)p;return 0;}
static void outb(uint16_t p,uint8_t v){(void)p;(void)v;}
static void out16(uint16_t p,uint16_t v){if(p==RTL_CAPR){capr=v;empty=true;}}
static void netdev_receive(const void *data,uint16_t length){
 const uint8_t *p=data;assert(length==64);for(unsigned i=0;i<64;i++)assert(p[i]==(uint8_t)(i+1));received++;
}
'''+fn(r,'static void rtl_copy_from_ring(')+'\n'+fn(r,'static void rtl_receive_restart(')+'\n'+fn(r,'static void rtl_receive_poll(')+'\n'+fn(r,'static bool rtl_send(')+r'''
int main(void){
 uint8_t ring[RTL_RX_ALLOC_SIZE];memset(ring,0xa5,sizeof ring);g_rtl.rx_ring=ring;
 unsigned start=RTL_RX_RING_SIZE-12;g_rtl.rx_offset=start;ring[start]=1;ring[start+1]=0;ring[start+2]=68;ring[start+3]=0;
 for(unsigned i=0;i<64;i++)ring[start+4+i]=(uint8_t)(i+1);
 rtl_receive_poll();assert(received==1 && g_rtl.rx_offset==60 && capr==44);
 /* In-flight DMA is not corruption and must not reset CAPR. */
 empty=false;g_rtl.rx_offset=0;ring[0]=1;ring[1]=0;ring[2]=0xf0;ring[3]=0xff;
 rtl_receive_poll();assert(received==1 && capr==44 && g_rtl.rx_offset==0);
 ticks=101;rtl_receive_poll();assert(capr==0xfff0 && !g_rtl.rx_pending);
 uint8_t tx[4][1536],frame[14];memset(frame,0x39,sizeof frame);memset(tx,0xff,sizeof tx);
 for(unsigned i=0;i<4;i++)g_rtl.tx_buffer[i]=tx[i];
 g_rtl.running=true;g_rtl.tx_threshold=8;
 assert(rtl_send(frame,sizeof frame) && last_transmit==0x0008003cU);
 assert(!memcmp(tx[0],frame,14));for(unsigned i=14;i<60;i++)assert(!tx[0][i]);
 transmit_status|=0x4000;assert(rtl_send(frame,sizeof frame) && last_transmit==0x000c003cU);
 assert(g_rtl.tx_slot==2);
 puts("PASS production RTL8139 RxNoWrap boundary copy, ring wrap, incomplete/stuck DMA, TX padding and correct/adaptive FIFO threshold bits");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-travelmate-net-') as directory:
 for name,source in [('dhcp',code),('rtl',rtl)]:
  path=Path(directory)/f'{name}.c';path.write_text(source);binary=path.with_suffix('')
  subprocess.run(['gcc','-m32','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-no-pie',str(path),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
