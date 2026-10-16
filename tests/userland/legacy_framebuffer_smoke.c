#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    gfx_info_t info;
    bool valid=bk_gfx_info(&info) && info.mode==GFX_MODE_VESA_LFB && info.width==800 && info.height==600 && info.bpp==8 && info.pitch>=info.width;
    bool pixels=false;
    if(valid) {
        bk_gfx_putpixel(1,1,0xff0000);
        uint32_t red=bk_gfx_getpixel(1,1);
        bk_gfx_putpixel(2,1,0x00ff00);
        uint32_t green=bk_gfx_getpixel(2,1);
        bk_gfx_putpixel(3,1,0x0000ff);
        uint32_t blue=bk_gfx_getpixel(3,1);
        pixels=red==0xff0000 && green==0x00ff00 && blue==0x0000ff;
        kprintf("[DRIVERTEST] firmware LFB=%ux%ux%u pitch=%u RGB332 red=%x green=%x blue=%x\n",info.width,info.height,info.bpp,info.pitch,red,green,blue);
    }
    kprintf("[DRIVERTEST] DONE result=%s\n",valid && pixels?"PASS":"FAIL");
}
