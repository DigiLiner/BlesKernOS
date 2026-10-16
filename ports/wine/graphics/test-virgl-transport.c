/* SPDX-License-Identifier: MIT
 * Run inside the guest: real VirGL texture upload/readback roundtrip. */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "blesvirgl.h"
static int fd;
static int call(bles_virgl_t *a,void *data,unsigned bytes){a->abi=1;a->buffer=(uintptr_t)data;a->bytes=bytes;int r=ioctl(fd,BLES_GFX_VIRGL,a);printf("[VIRGL-PROBE] op=%u rc=%d context=%u p0=%u\n",a->op,r,a->context,a->p[0]);if(r)perror("virgl");return r;}
int main(void){setbuf(stdout,NULL);puts("[VIRGL-PROBE] begin");fd=open("/dev/blesgfx",O_RDWR);if(fd<0){perror("open");return 1;}
bles_virgl_t a={.op=BLES_VIRGL_CAPS};if(call(&a,NULL,0))return 1;printf("[VIRGL-PROBE] capset=%u version=%u bytes=%u\n",a.p[1],a.p[2],a.p[3]);
a=(bles_virgl_t){.op=BLES_VIRGL_CONTEXT_CREATE};if(call(&a,NULL,0))return 1;
a=(bles_virgl_t){.op=BLES_VIRGL_RESOURCE_CREATE,.p={2,1,2,16,16,1,1,0,0,0,1024}};if(call(&a,NULL,0))return 1;unsigned id=a.p[0];
unsigned char buf[1024];for(unsigned i=0;i<sizeof(buf);i++)buf[i]=(i*7)%251;
a=(bles_virgl_t){.op=BLES_VIRGL_TRANSFER_WRITE,.p={id,0,64,1024,0,0,0,16,16,1,0,0}};if(call(&a,buf,sizeof(buf)))return 1;memset(buf,0,sizeof(buf));a.op=BLES_VIRGL_TRANSFER_READ;if(call(&a,buf,sizeof(buf)))return 1;
for(unsigned i=0;i<sizeof(buf);i++)if(buf[i]!=(i*7)%251){printf("[VIRGL-PROBE] FAIL at %u got=%u\n",i,buf[i]);return 2;}
a=(bles_virgl_t){.op=BLES_VIRGL_CONTEXT_DESTROY};if(call(&a,NULL,0))return 1;close(fd);puts("[VIRGL-PROBE] PASS host texture roundtrip");return 0;}
