#!/usr/bin/env python3
"""Execute production NIC descriptors and audio conversion against mocked I/O."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def strip(text):return re.sub(r'^#include .*\n','',text,flags=re.M)
def read(path):return (ROOT/path).read_text()
prelude=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define BK_DRIVER_HOST_TEST
#define kmemcpy memcpy
#define kmemset memset
static uint8_t ports[65536];static uint32_t ticks;
static uint8_t legacy_in8(uint16_t p){return ports[p];}
static uint16_t legacy_in16(uint16_t p){uint16_t v;memcpy(&v,ports+p,2);return v;}
static uint32_t legacy_in32(uint16_t p){uint32_t v;memcpy(&v,ports+p,4);return v;}
static void legacy_out8(uint16_t p,uint8_t v){ports[p]=v;}
static void legacy_out16(uint16_t p,uint16_t v){memcpy(ports+p,&v,2);}
static void legacy_out32(uint16_t p,uint32_t v){memcpy(ports+p,&v,4);}
static uint32_t pit_get_ticks(void){return ticks;}
static uint32_t pit_get_frequency_hz(void){return 100;}
void *mm_alloc_guarded_stack(uint32_t,bool,void **,uint32_t *);
void mm_free_guarded_stack(void *,void *,uint32_t);
bool paging_kernel_mmio_uncached(uint32_t,uint32_t);
'''+strip(read('kernel/include/pci.h'))+strip(read('kernel/drivers/legacy_io.h'))
nettests=r'''
static unsigned received;static bool reply;
void netdev_receive(const void *data,uint16_t size){
 assert(size==60 && ((const uint8_t *)data)[0]==0x5a);received++;
 if(reply){uint8_t response[60]={0};assert(send_frame(response,60));}
}
static void packet(unsigned index,unsigned size,bool error){
 nic_desc_t *d=&nic.rx[index];memset((void *)d->word,0,sizeof(d->word));rx_buffer(index)[0]=0x5a;
 if(LEGACY_NET_KIND==NIC_E1000){d->word[2]=size;d->word[3]=3|(error?0x100:0);}
 else if(LEGACY_NET_KIND==NIC_PCNET){d->word[1]=0x03000000|(error?0x40000000:0);d->word[2]=size+4;}
 else if(LEGACY_NET_KIND==NIC_SIS900 || LEGACY_NET_KIND==NIC_NATSEMI)d->word[1]=OWN|0x08000000|(size+4)|(error?0x40000000:0);
 else if(LEGACY_NET_KIND==NIC_RTL8169)d->word[0]=0x30000000|(size+4)|(error?0x00200000:0);
 else if(LEGACY_NET_KIND==NIC_TULIP)d->word[0]=((size+4)<<16)|0x0300|(error?0x8000:0);
 else d->word[0]=((size+4)<<16)|0x8300|(error?1:0);
}
int main(void){
 nic.dma.data=calloc(1,DMA_SIZE);nic.mmio=calloc(1,0x6000);nic.running=true;nic.io=0x100;rings_prepare();
 uint8_t frame[1537];memset(frame,0xaa,sizeof(frame));
 assert(!send_frame(NULL,60));assert(!send_frame(frame,13));assert(!send_frame(frame,1537));
 if(LEGACY_NET_KIND==NIC_NE2K){nic.mmio=NULL;assert(send_frame(frame,14));assert(!memcmp(tx_buffer(0),frame,14));for(unsigned i=14;i<60;i++)assert(tx_buffer(0)[i]==0);assert(!send_frame(frame,60));nic.tx_lock=1;poll_device();assert(nic.tx_lock==1);nic.tx_lock=0;poll_device();assert(!nic.tx_lock);}
 else{
  assert(send_frame(frame,14));assert(!memcmp(tx_buffer(0),frame,14));for(unsigned i=14;i<60;i++)assert(tx_buffer(0)[i]==0);frame[0]=0;assert(tx_buffer(0)[0]==0xaa);
  for(unsigned i=1;i<RING;i++)assert(send_frame(frame,60));assert(!send_frame(frame,60));
  packet(0,60,false);packet(1,60,true);packet(2,1600,false);packet(3,1,false);poll_device();assert(received==1 && nic.rx_index==4);
  nic.tx_index=0;memset(nic.tx,0,RING*DESC_SIZE);rings_prepare();nic.rx_index=0;received=0;reply=true;
  packet(0,60,false);poll_device();assert(received==1); /* RX callbacks can transmit. */
  reply=false;rings_prepare();nic.rx_index=0;received=0;for(unsigned i=0;i<RING;i++)packet(i,60,false);poll_device();assert(received==RING && nic.rx_index==0);
 }
 uint8_t good[6]={2,1,2,3,4,5},bad[6]={1,2,3,4,5,6};assert(legacy_mac_valid(good));assert(!legacy_mac_valid(bad));memset(bad,0,6);assert(!legacy_mac_valid(bad));
 free(nic.dma.data);puts("PASS: NIC bounds, copied TX/padding, full ring, RX errors/size/budget, callback reentrancy");
}
'''
audiotests=r'''
int main(void){
 static pci_device_t dev;audio.pci=&dev;
 audio.dma.data=calloc(1,DMA_BYTES);audio.mmio=calloc(1,0x1000);audio.ready=true;
 uint8_t samples[3]={0,128,255};uint16_t rate=(uint16_t)output_rate();
 assert(!pcm(NULL,3,rate,255));assert(!pcm(samples,0,rate,255));assert(!pcm(samples,3,3999,255));assert(!pcm(samples,0xffffffff,4000,255));
 audio.lock=1;assert(!pcm(samples,3,rate,255));audio.lock=0;
 assert(pcm(samples,3,rate,255));int16_t *data=(int16_t *)(audio.dma.data+PCM_OFFSET);
 assert(data[0]==-32768 && data[1]==-32768 && data[2]==0 && data[3]==0 && data[4]==32512 && data[5]==32512);
 samples[0]=255;assert(data[0]==-32768); /* Copy, not retained caller storage. */
 assert(pcm(samples,3,rate,0));for(unsigned i=0;i<6;i++)assert(data[i]==0);
 ticks=audio.end_tick;poll();assert(!audio.busy);assert(!start_tone(1,100));assert(!start_tone(440,1001));assert(start_tone(440,100));stop();assert(!audio.busy);
 free(audio.dma.data);free((void *)audio.mmio);puts("PASS: PCM input limits, supervisor copy, stereo/signed conversion, gain, bounded tone, timed stop");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 for kind,name in enumerate(['e1000','pcnet','ne2k_pci','tulip','rhine','sis900','natsemi','rtl8169'],1):
  runtime=read('kernel/drivers/network/legacy_net_runtime.inc');runtime=runtime[:runtime.index('static void shutdown_device(')]
  src=strip(read('kernel/drivers/network/legacy_net.h'))+strip(read('kernel/drivers/network/'+name+'.c'))+runtime
  src='#define NIC_E1000 1\n#define NIC_PCNET 2\n#define NIC_NE2K 3\n#define NIC_TULIP 4\n#define NIC_RHINE 5\n#define NIC_SIS900 6\n#define NIC_NATSEMI 7\n#define NIC_RTL8169 8\n'+src
  code=prelude+strip(read('kernel/include/network.h'))+src+nettests
  c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
  subprocess.run(['gcc','-std=c11','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=undefined','-DLEGACY_NET_KIND='+str(kind),str(c),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
 for kind,name in enumerate(['es1370','cmipci','via686','cs4281'],1):
  runtime=read('kernel/drivers/audio/legacy_pcm_runtime.inc');runtime=runtime[:runtime.index('static void play(')]
  src=strip(read('kernel/drivers/audio/legacy_pcm.h'))+strip(read('kernel/drivers/audio/'+name+'.c'))+runtime
  c.write_text(prelude+src+audiotests)
  subprocess.run(['gcc','-std=c11','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=undefined','-DLEGACY_AUDIO_KIND='+str(kind),str(c),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
