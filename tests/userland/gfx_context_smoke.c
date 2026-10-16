#include "../../ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
/* Linux/i386: graphics contexts must be created and freed with their fd. */
static int call3(int n,int a,int b,int c) {
    int r;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory");
    return r;
}
void _start(void) {
    int status=0;
    for(unsigned i=0;i<256;i++) {
        int fd=call3(5,(int)"/dev/blesgfx",2,0);
        if(fd<0 || call3(6,fd,0,0)<0){status=1;break;}
    }

    /* Rootless open reserves dimensions without two full kernel images.
     * Present reads the clipped source directly and keeps only one scratch
     * surface. Legacy full-desktop mode still allocates its resident images. */
    static uint32_t pixels[160*90];
    for(unsigned i=0;i<160*90;i++)pixels[i]=0x002060a0U;
    for(unsigned mode=0;mode<2 && !status;mode++) {
        int fd=call3(5,(int)"/dev/blesgfx",2,0);
        bles_gfx_open_t open={160,90,mode?0:BLES_GFX_OPEN_ROOTLESS};
        bles_gfx_wine_memory_t before={0},after={0};
        if(fd<0 || call3(54,fd,BLES_GFX_WINE_MEMORY,(int)&before)<0 ||
           call3(54,fd,BLES_GFX_OPEN,(int)&open)<0 ||
           call3(54,fd,BLES_GFX_WINE_MEMORY,(int)&after)<0){status=2;break;}
        if(!mode && after.heap_resident_pages>before.heap_resident_pages+2){status=3;break;}
        if(!mode) {
            bles_gfx_surface_t surface={0};surface.instance_id=call3(20,0,0,0);
            surface.surface_id=1;surface.x=5;surface.y=7;surface.width=80;surface.height=50;
            surface.title[0]='G';
            if(call3(54,fd,BLES_GFX_SURFACE_UPSERT,(int)&surface)<0){status=4;break;}
        }
        bles_gfx_present_t present={(uint32_t)pixels,160,90};
        if(call3(54,fd,BLES_GFX_PRESENT,(int)&present)<0){status=5;break;}
        present.pixels=0xfffff000U;
        if(call3(54,fd,BLES_GFX_PRESENT,(int)&present)!=-14){status=6;break;}
        if(call3(6,fd,0,0)<0){status=7;break;}
    }
    call3(1,status,0,0);
    for(;;);
}
