#define bleskernos_program_main imageviewer_application_main
#ifndef IMAGEVIEWER_SOURCE_PATH
#define IMAGEVIEWER_SOURCE_PATH "../../programs/imageviewer.c"
#endif
#include IMAGEVIEWER_SOURCE_PATH
#undef bleskernos_program_main

void bleskernos_program_main(gui_desktop_t *desktop) {
    viewer_state_t *st=bk_sys_alloc_zero(sizeof(*st));
    uint32_t *pixels=bk_sys_alloc_zero(800U*600U*4);
    if(!st || !pixels){kprintf("[IMAGEPROBE] FAIL allocation\n");return;}
    st->desktop=desktop;
    st->window=bk_gui_create_window(desktop,82,36,440,320,"Image probe");
    if(!st->window){kprintf("[IMAGEPROBE] FAIL window\n");return;}
    gui_rect_t client=bk_gui_window_content_rect_raw(st->window);
    gui_surface_t surface={.pixels=pixels,.width=800,.height=600,.pitch=client.w,
        .clip=client,.origin_x=client.x,.origin_y=client.y,
        .storage_width=client.w,.storage_height=client.h};
    const char *paths[]={"/TESTPHOTO.BMP","/TESTANIM.GIF"};
    for(unsigned media=0;media<2;media++) {
        bk_runtime_strncpy(st->path,paths[media],sizeof(st->path)-1);
        uint32_t start=bk_sys_uptime_ms();viewer_load_current(st);
        kprintf("[IMAGEPROBE] media=%u loaded=%u frames=%u load_ms=%u\n",media,st->loaded,
            st->animated?st->gif.frame_count:1,bk_sys_uptime_ms()-start);
        if(!st->loaded){kprintf("[IMAGEPROBE] FAIL load\n");break;}
        for(unsigned mode=0;mode<3;mode++) {
            if(mode==1)viewer_actual_size(st);
            if(mode==2){viewer_zoom_to(st,800);st->pan_x=-83;st->pan_y=61;}
            if(media && mode)st->frame=1;
            start=bk_sys_uptime_ms();viewer_draw(st->window,&surface,st);
            uint32_t elapsed=bk_sys_uptime_ms()-start,hash=2166136261U;
            for(unsigned i=0;i<(unsigned)client.w*client.h;i++)hash=(hash^pixels[i])*16777619U;
            kprintf("[IMAGEPROBE] media=%u mode=%u render_ms=%u hash=%u\n",media,mode,elapsed,hash);
        }
    }
    viewer_cleanup(st);bk_sys_free(pixels);
    kprintf("[IMAGEPROBE] DONE\n");
    bk_app_launch("/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX","/TESTPHOTO.BMP");
}
