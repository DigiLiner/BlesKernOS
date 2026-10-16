#!/usr/bin/env python3
"""Exercise the kernel OSS conversion/queue/ABI with a recording PCM driver."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <assert.h>
#define UNUSED __attribute__((unused))
#define BK_EINVAL 22
#define BK_EFAULT 14
#define BK_EAGAIN 11
#define BK_EBUSY 16
#define BK_ENOTTY 25
#define kmemset memset
#define kstrncpy strncpy
typedef struct {uint32_t refs,flags;unsigned kind;void *gfx;uint8_t *pcm;uint32_t pcm_rate,pcm_format,pcm_channels,pcm_read,pcm_count,pcm_inflight,pcm_serial;} linux_device;
static linux_device linux_devices[128];static int linux_io_lock;
static uint32_t kspin_lock_irqsave(int *p){(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;}
static bool copy_to_user(void *to,const void *from,uint32_t n){if(!to)return false;memcpy(to,from,n);return true;}
static bool copy_from_user(void *to,const void *from,uint32_t n){if(!from)return false;memcpy(to,from,n);return true;}
static bool sound_pcm_available(void){return true;}
static const char *sound_pcm_name(void){return "test-PCM";}
static void sound_poll(void){}
static bool sound_pcm_is_busy(void){return false;}
static uint32_t submitted;static uint8_t output[5000];
static bool sound_play_pcm_u8(const uint8_t *p,uint32_t n,uint16_t rate,uint8_t volume){assert(rate==44100&&volume==255);submitted=n;memcpy(output,p,n);return true;}
static jmp_buf stop;
static void task_sleep(unsigned ticks){(void)ticks;longjmp(stop,1);}
'''+(ROOT/'kernel/linux/i386_oss.inc').read_text()+r'''
int main(void){
 linux_device *d=&linux_devices[0];d->refs=1;d->kind=4;d->pcm=calloc(1,BK_OSS_QUEUE);d->pcm_rate=22050;d->pcm_format=BK_OSS_S16;d->pcm_channels=2;d->pcm_serial=1;
 const int16_t stereo[]={32767,32767,-32768,-32768,0,0};
 assert(linux_pcm_write(d,(const uint8_t *)stereo,sizeof(stereo))==sizeof(stereo));
 assert(d->pcm_count==6);assert(d->pcm[0]==255&&d->pcm[1]==255&&d->pcm[2]==0&&d->pcm[3]==0&&d->pcm[4]==128);
 assert(linux_pcm_write(d,(const uint8_t *)stereo,3)==-BK_EINVAL);
 assert(linux_oss_ioctl(d,0xc0045002,(uint32_t)&(int32_t){11025})==-BK_EBUSY);
 assert(linux_oss_ioctl(d,0x5000,0)==0);assert(d->pcm_count==0);
 assert(linux_oss_ioctl(d,0xc0045002,(uint32_t)&(int32_t){11025})==0);
 assert(linux_oss_ioctl(d,0xc0045005,(uint32_t)&(int32_t){8})==0);
 assert(linux_oss_ioctl(d,0xc0045006,(uint32_t)&(int32_t){1})==0);
 uint8_t samples[]={0,128,255};assert(linux_pcm_write(d,samples,3)==3);assert(d->pcm_count==12);
 if(!setjmp(stop))linux_pcm_worker(NULL);
 assert(submitted==12&&output[0]==0&&output[4]==128&&output[8]==255);
 assert(d->pcm_count==0&&d->pcm_inflight==12);
 uint32_t space[4];assert(linux_oss_ioctl(d,0x8010500c,(uint32_t)space)==0);assert(space[3]<space[1]*space[2]+space[2]);
 bk_oss_sysinfo info;assert(linux_oss_ioctl(d,0x84e05801,(uint32_t)&info)==0);assert(info.numaudios==1&&info.numaudioengines==1);
 bk_oss_audioinfo ai={.dev=-1};assert(linux_oss_ioctl(d,0xc49c580c,(uint32_t)&ai)==0);assert(ai.caps==0x20000&&!strcmp(ai.devnode,"/dev/dsp"));
 assert(linux_oss_ioctl(d,0x84e05801,0)==-BK_EFAULT);assert(linux_oss_ioctl(d,0x84e15801,(uint32_t)&info)==-BK_ENOTTY);
 d->pcm_count=BK_OSS_QUEUE;d->pcm_inflight=0;assert(linux_pcm_write(d,samples,3)==-BK_EAGAIN);
 d->pcm_count=d->pcm_inflight=d->pcm_read=0;
 assert(linux_oss_ioctl(d,0xc0045002,(uint32_t)&(int32_t){44100})==0);
 assert(linux_oss_ioctl(d,0xc0045005,(uint32_t)&(int32_t){BK_OSS_FLOAT})==0);
 assert(linux_oss_ioctl(d,0xc0045006,(uint32_t)&(int32_t){2})==0);
 const uint32_t floats[]={0x3f800000,0x3f800000,0xbf800000,0xbf800000,0x3f000000,0x3f000000,0x7fc00000,0x7fc00000,0x7f800000,0xff800000,0x40000000,0x40000000,1,1};
 assert(linux_pcm_write(d,(const uint8_t *)floats,sizeof(floats))==sizeof(floats));
 const uint8_t expected[]={255,0,192,128,128,255,128};
 assert(d->pcm_count==sizeof(expected) && !memcmp(d->pcm,expected,sizeof(expected)));
 assert(linux_pcm_write(d,(const uint8_t *)floats,4)==-BK_EINVAL);
 assert(ai.oformats & BK_OSS_FLOAT);
 free(d->pcm);puts("PASS OSS ABI, FLOAT/NaN/Inf/clipping, S16/stereo downmix, rate conversion, queue/reset, PCM submission, pointer and fullness errors");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-audio-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-Wall','-Wextra',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
