/* SPDX-License-Identifier: MIT
 * Draw known GPU pixels before trusting a moving benchmark or its score. */
#include "include/bleskernos_gallium.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    bkg_context *c=bkg_create(640,480);
    if(!c){puts("[GALLIUM:PROBE] FAIL create");return 1;}
    const unsigned char *(*getstring)(unsigned)=bkg_get_proc("glGetString");
    void (*clearcolor)(float,float,float,float)=bkg_get_proc("glClearColor");
    void (*clear)(unsigned)=bkg_get_proc("glClear");
    void (*enable)(unsigned)=bkg_get_proc("glEnable");
    void (*disable)(unsigned)=bkg_get_proc("glDisable");
    void (*scissor)(int,int,int,int)=bkg_get_proc("glScissor");
    void (*readpixels)(int,int,int,int,unsigned,unsigned,void *)=bkg_get_proc("glReadPixels");
    unsigned (*geterror)(void)=bkg_get_proc("glGetError");
    if(!getstring || !clearcolor || !clear || !enable || !disable || !scissor || !readpixels || !geterror)return 2;
    printf("[GALLIUM:PROBE] renderer=%s version=%s\n",getstring(0x1f01),getstring(0x1f02));
    const char *renderer=(const char *)getstring(0x1f01);
    if(!renderer || !strstr(renderer,"virgl") || strstr(renderer,"llvmpipe") ||
       strstr(renderer,"softpipe") || strstr(renderer,"Software Rasterizer")) {
        puts("[GALLIUM:PROBE] FAIL hardware renderer");return 3;
    }
    clearcolor(0,0,1,1);clear(0x4000);
    enable(0xc11);scissor(0,240,640,240);clearcolor(1,0,0,1);clear(0x4000);disable(0xc11);
    if(!bkg_present(c)){puts("[GALLIUM:PROBE] FAIL native present");return 5;}
    puts("[GALLIUM:PROBE] GPU frame ready: TOP RED / BOTTOM BLUE");
    sleep(3);
    unsigned char pixel[4]={0};readpixels(16,400,1,1,0x1908,0x1401,pixel);
    unsigned error=geterror();
    printf("[GALLIUM:PROBE] pixel=%u,%u,%u,%u error=%x\n",pixel[0],pixel[1],pixel[2],pixel[3],error);
    if(pixel[0]!=255 || pixel[1] || pixel[2] || pixel[3]!=255 || error) {puts("[GALLIUM:PROBE] FAIL pixels");return 4;}
    readpixels(16,40,1,1,0x1908,0x1401,pixel);error=geterror();
    if(pixel[0] || pixel[1] || pixel[2]!=255 || pixel[3]!=255 || error) {puts("[GALLIUM:PROBE] FAIL lower pixels");return 4;}
    bkg_destroy(c);puts("[GALLIUM:PROBE] PASS");return 0;
}
