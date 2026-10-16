/* Device boundary and real GPU readback gate for the Wine renderer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "../../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
typedef struct {float x,y,z,rhw;uint32_t color;float u,v;} vertex;
static int command(int fd,bles_gfx_3d_t *a){return ioctl(fd,BLES_GFX_3D,a);}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    int fd=open("/dev/blesgfx",O_RDWR),other=open("/dev/blesgfx",O_RDWR);if(fd<0 || other<0)return 2;
    bles_gfx_3d_t a={BLES_3D_CAPS,{0}};if(command(fd,&a)<0)return 3;
    printf("[COMPAT:GFX3D] caps=%08x\n",a.p[0]);
    if(!a.p[0]){puts("[COMPAT:GFX3D] SKIP no-accelerated-device");return 77;}
    a=(bles_gfx_3d_t){BLES_3D_CREATE,{64,64,2,1}};if(command(fd,&a)<0)return 4;uint32_t h=a.p[0];
    a=(bles_gfx_3d_t){BLES_3D_DESTROY,{h}};
    if(command(other,&a)==0 || errno!=EINVAL){puts("[COMPAT:GFX3D] FAIL cross-device-handle");return 5;}
    a=(bles_gfx_3d_t){BLES_3D_CREATE,{0,4096,2,1}};
    if(command(fd,&a)==0 || errno!=EINVAL)return 6;
    a=(bles_gfx_3d_t){BLES_3D_DRAW,{h,0,1,3,0}};
    if(command(fd,&a)==0 || errno!=EFAULT)return 7;
    a=(bles_gfx_3d_t){BLES_3D_BEGIN,{h,0xff000000,0x3f800000,(1<<5)|(1<<6)}};if(command(fd,&a)<0)return 8;
    vertex v[3]={{4,4,.5f,1,0xff00ff00,0,0},{60,4,.5f,1,0xff00ff00,0,0},{32,60,.5f,1,0xff00ff00,0,0}};
    a=(bles_gfx_3d_t){BLES_3D_DRAW,{h,0,(uint32_t)(uintptr_t)v,3,0}};if(command(fd,&a)<0)return 9;
    a=(bles_gfx_3d_t){BLES_3D_END,{h}};if(command(fd,&a)<0)return 10;
    a=(bles_gfx_3d_t){BLES_3D_WAIT,{a.p[1]}};if(a.p[0] && command(fd,&a)<0)return 11;
    uint32_t pixels[64*64]={0};
    a=(bles_gfx_3d_t){BLES_3D_DOWNLOAD,{h,(uint32_t)(uintptr_t)pixels,64,0}};if(command(fd,&a)<0)return 12;
    uint32_t center=pixels[25*64+32]&0xffffff,corner=pixels[0]&0xffffff;
    printf("[COMPAT:GFX3D] %s triangle-readback center=%06x corner=%06x\n",center==0xff00 && !corner?"PASS":"FAIL",center,corner);
    close(fd);close(other); /* Exercise implicit destruction on last close. */
    for(unsigned i=0;i<140;i++) {
        fd=open("/dev/blesgfx",O_RDWR);if(fd<0)return 13;
        a=(bles_gfx_3d_t){BLES_3D_CREATE,{8,8,2,1}};
        if(command(fd,&a)<0){printf("[COMPAT:GFX3D] FAIL cleanup cycle=%u\n",i);return 14;}
        close(fd);
    }
    puts("[COMPAT:GFX3D] PASS ownership-invalid-input-exit-cleanup");
    return center==0xff00 && !corner?0:1;
}
