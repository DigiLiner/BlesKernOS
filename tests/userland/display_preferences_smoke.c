#include "../../kernel/include/api.h"

static bool failed;
static void check(bool ok, const char *name) {
    kprintf("[PREFTEST] %s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failed = true;
}
static void click(gui_window_t *window, gui_widget_t *widget, int x, int y) {
    gui_event_t e = {.type=GUI_EVENT_MOUSE_DOWN,.x=x,.y=y,.button=1,.buttons=1};
    (void)gui_widget_handle_event(window,widget,&e);
    e.type=GUI_EVENT_MOUSE_UP;e.buttons=0;
    (void)gui_widget_handle_event(window,widget,&e);
}
static bool choose(gui_window_t *window, gui_widget_t *widget, const char *value) {
    int index=-1;
    for(int i=0;i<16;i++)
        if(!bk_runtime_strcmp(gui_widget_dropdown_get_item_value(widget,i),value)) {index=i;break;}
    if(index<0)return false;
    gui_rect_t r=bk_gui_widget_screen_bounds(window,widget);
    gui_rect_t client=bk_gui_window_content_rect_raw(window);
    int count=*(const uint8_t *)widget->payload;
    int below=client.y+client.h-(r.y+r.h+1),above=r.y-client.y-1;
    bool upward=count*18+2>below && above>below;
    int visible=upward?above/18:below/18;
    if(visible>count)visible=count;
    bk_gui_widget_dropdown_set_selected(widget,0);
    click(window,widget,r.x+10,r.y+10);
    int popup_y=upward?r.y-(visible*18+2):r.y+r.h-1;
    click(window,widget,r.x+10,popup_y+1+index*18+8);
    bk_sys_sleep_ms(1600);
    return !bk_runtime_strcmp(bk_gui_widget_dropdown_get_selected_value(widget),value);
}
void bleskernos_program_main(gui_desktop_t *proxy) {
    bk_sys_sleep_ms(5000);
    check(bk_app_launch("/SYSTEM/CONTROL/DISPLAY.CPL",NULL),"launch Display CPL");
    gui_window_t *window=NULL;gui_widget_t *depth=NULL,*wallpaper=NULL;
    for(int tries=0;tries<300;tries++) {
        gui_desktop_t *real=bk_gui_get_desktop();
        for(gui_window_t *w=real->first_window;w;w=w->next)
            for(gui_widget_t *q=w->widgets;q;q=q->next)
                if(q->style==GUI_WIDGET_STYLE_DROPDOWN && q->bounds.x==308 && q->bounds.y==276) {
                    window=w;depth=q;
                }
        if(window && window->content_pid && !bk_runtime_strcmp(gui_widget_dropdown_get_item_value(depth,3),"4"))break;
        bk_sys_sleep_ms(200);
    }
    check(window && window->content_pid && depth,"Display initialized");
    if(window && depth) {
        const char *values[]={"8","16","4","32"};
        for(unsigned i=0;i<4;i++) {
            bool selected=choose(window,depth,values[i]);
            uint8_t wanted=i==0?8:i==1?16:i==2?4:32;
            check(selected && bk_gui_display_color_depth()==wanted,"dropdown applies color depth");
            kprintf("[PREFTEST:DEPTH] wanted=%u actual=%u\n",wanted,bk_gui_display_color_depth());
        }
        for(gui_widget_t *q=window->widgets;q;q=q->next)
            if(q->style==GUI_WIDGET_STYLE_DROPDOWN && q->bounds.x==308 && q->bounds.y==311)wallpaper=q;
        bk_desktop_set_background(0x205060);bk_sys_sleep_ms(1400);
        check(bk_desktop_get_background()==0x205060 && !bk_runtime_strcmp(bk_desktop_get_wallpaper(),""),"Vision applies solid background");
        bk_sys_sleep_ms(2500);
        if(wallpaper) {
            char path[VFS_MAX_PATH];
            bk_runtime_strncpy(path,gui_widget_dropdown_get_item_value(wallpaper,1),sizeof(path)-1);path[sizeof(path)-1]=0;
            wallpaper->visible=true;
            check(choose(window,wallpaper,path),"wallpaper dropdown selection");
            bk_sys_sleep_ms(2400);
            check(!bk_runtime_strcmp(bk_desktop_get_wallpaper(),path),"Vision publishes applied wallpaper");
        } else check(false,"wallpaper widget");
        /* Keep windows alive while inspecting their widget lists on SMP. */
    }
    check(bk_app_launch("/SYSTEM/CONTROL/MOUSE.CPL",NULL),"launch Mouse CPL");
    gui_widget_t *trail=NULL;window=NULL;
    for(int tries=0;tries<150;tries++) {
        gui_desktop_t *real=bk_gui_get_desktop();
        for(gui_window_t *w=real->first_window;w;w=w->next)
            for(gui_widget_t *q=w->widgets;q;q=q->next)
                if(q->style==GUI_WIDGET_STYLE_CHECKBOX && !bk_runtime_strcmp(q->text,"Mostrar estela")){window=w;trail=q;}
        if(trail && trail->callback_pid)break;
        bk_sys_sleep_ms(200);
    }
    if(trail) {
        bk_sys_sleep_ms(500);trail->visible=true;
        bk_gui_desktop_set_cursor_trail(proxy,false);
        bk_gui_choice_set_checked(trail,false);
        gui_rect_t r=bk_gui_widget_screen_bounds(window,trail);
        click(window,trail,r.x+10,r.y+10);bk_sys_sleep_ms(800);
        check(bk_gui_desktop_cursor_trail_enabled(proxy),"Mouse checkbox enables global trail");
        check(bk_gui_get_desktop()->cursor_trail_enabled,"real compositor has trail enabled");
        bk_gui_desktop_set_cursor_trail(proxy,false);
        check(!bk_gui_get_desktop()->cursor_trail_enabled,"global trail disable");
        /* Keep windows alive while inspecting their widget lists on SMP. */
    } else check(false,"trail checkbox");
    bk_memory_detail_t m;if(bk_memory_detail(&m))kprintf("[PREFTEST:RAM] free_kib=%u heap_free_kib=%u\n",m.free/1024,m.heap_free/1024);
    kprintf("[PREFTEST] DONE result=%s\n",failed?"FAIL":"PASS");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
