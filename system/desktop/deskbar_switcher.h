#ifndef BLES_DESKBAR_SWITCHER_H
#define BLES_DESKBAR_SWITCHER_H
#define SWITCHER_ROW_H 38
#define SWITCHER_SCROLL_H 18

typedef struct {
    gui_rect_t cell, top, menu;
    int count, rows, scroll_h;
} deskbar_switcher_layout_t;

static deskbar_switcher_layout_t deskbar_switcher_layout(const gui_desktop_t *desktop) {
    deskbar_switcher_layout_t l = {0};
    l.cell = deskbar_cde_launcher_rect(desktop, DESKBAR_LAUNCHER_COUNT - 1);
    l.top = deskbar_design_rect(desktop, 881, 4, 59, 17);
    for (gui_window_t *w = desktop->first_window; w; w = w->next)
        if (w->listed) l.count++;
    int max_rows = (l.top.y - 8) / SWITCHER_ROW_H;
    if (max_rows < 1) return l;
    if (l.count > max_rows) {
        l.scroll_h = SWITCHER_SCROLL_H;
        max_rows = (l.top.y - 8 - 2*l.scroll_h) / SWITCHER_ROW_H;
        if (max_rows < 1) max_rows = 1;
    }
    l.rows = l.count < max_rows ? l.count : max_rows;
    /* Open over the complete right-hand launcher group. */
    gui_rect_t group = deskbar_design_rect(desktop,646,4,294,17);
    int x = group.x;
    int width = group.w;
    int height = l.rows*SWITCHER_ROW_H + 2*l.scroll_h + 6;
    l.menu = (gui_rect_t){x, l.top.y-height-2, width, height};
    return l;
}

static gui_window_t *deskbar_switcher_next(const gui_desktop_t *desktop, uint32_t after) {
    gui_window_t *next = NULL;
    for (gui_window_t *w = desktop->first_window; w; w = w->next)
        if (w->listed && w->id > after && (!next || w->id < next->id)) next = w;
    return next;
}

static gui_window_t *deskbar_switcher_selected(gui_desktop_t *desktop, deskbar_state_t *state) {
    gui_window_t *selected = NULL, *latest = NULL;
    for (gui_window_t *w = desktop->first_window; w; w = w->next) if (w->listed) {
        if (w->id == state->switcher_selected) selected = w;
        if (!latest || w->id > latest->id) latest = w;
        if (w->focused && !w->minimized) { state->switcher_selected = w->id; return w; }
    }
    if (!selected) selected = latest;
    state->switcher_selected = selected ? selected->id : 0;
    return selected;
}

/* Some native applications leave a named worker alive after their entry
 * thread exits (Paint uses "paint"). Match executable stems as well as names. */
static bool deskbar_switcher_name_matches(const char *name,const char *file) {
    while(*name && *file) {
        char a=*name++,b=*file++;
        if(a>='A'&&a<='Z')a+='a'-'A';
        if(b>='A'&&b<='Z')b+='a'-'A';
        if(a!=b)return false;
    }
    return !*name && (!*file || *file=='.');
}
static const char *deskbar_switcher_resource(const char *name) {
    for (int i=0;i<DESKBAR_LAUNCHER_COUNT;i++) {
        const char *base=g_cde_launchers[i].program_path;
        for (const char *p=base;*p;p++) if (*p=='/') base=p+1;
        if (deskbar_switcher_name_matches(name,base)) return g_cde_launchers[i].icon_name;
    }
    static const struct {const char *file,*icon;} controls[] = {
        {"APPEAR.CPL","DisplaySettings"},{"DISPLAY.CPL","DisplaySettings"},
        {"DATETIME.CPL","Timedate"},{"SYSTEM.CPL","FileManager"},
        {"DEVMGR.CPL","HardwareDiag"},{"MODEM.CPL","ModemFax"},
        {"LANGUAGE.CPL","Intl101"},{"SOUND.CPL","Mmsys100"},
        {"MOUSE.CPL","Mouse"},{"KEYBOARD.CPL","Keyboard"},
        {"INTERNET.CPL","InternetSettings"},{"WINE.CPL","ExecutableWindow"},
        {"RUNBOX.BEX","Runonce106"},{"VIEWER.BEX","FileText"},
        {"processmgr","Taskman100"},{"CALENDAR.BEX","Timedate"}
    };
    for (unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);i++)
        if (deskbar_switcher_name_matches(name,controls[i].file)) return controls[i].icon;
    return NULL;
}

/* Native windows do not always publish a title-bar icon. Resolve the owning
 * executable once and cache its existing desktop artwork by process ID. */
static gui_image_t deskbar_switcher_icon(deskbar_state_t *state, const gui_window_t *w) {
    if (w->icon_pixels && w->icon_width && w->icon_height)
        return (gui_image_t){(uint32_t *)w->icon_pixels,w->icon_width,w->icon_height};
    uint32_t pid = w->controller_pid ? w->controller_pid : w->owner_pid;
    for (int i=0;i<state->switcher_icon_count;i++)
        if (state->switcher_icon_pids[i]==pid) return state->switcher_icons[i];
    const char *resource = NULL;
    bk_proc_info_t processes[128];
    uint32_t n = bk_proc_snapshot(processes,128);
    const char *name = NULL;
    for (uint32_t i=0;i<n && i<128;i++)
        if (processes[i].pid==pid) { name=processes[i].name; break; }
    if(name)resource=deskbar_switcher_resource(name);
    if(!resource)for(uint32_t i=0;i<n && i<128;i++)
        if(processes[i].process_id==pid) {
            const char *candidate=deskbar_switcher_resource(processes[i].name);
            if(candidate){resource=candidate;name=processes[i].name;break;}
        }
    if(!resource)resource="ExecutableWindow";
    kprintf("[DESKBAR:SELECTOR] pid=%u process=%s icon=%s\n",pid,name?name:"?",resource);
    int slot;
    if (state->switcher_icon_count<32) slot=state->switcher_icon_count++;
    else {slot=state->switcher_icon_next;state->switcher_icon_next=(slot+1)%32;}
    if (state->switcher_icons[slot].pixels) bk_gui_image_free(&state->switcher_icons[slot]);
    state->switcher_icons[slot]=(gui_image_t){0};
    state->switcher_icon_pids[slot]=pid;
    if(!bk_graphics_icon_load_size(resource,32,&state->switcher_icons[slot]))
        (void)bk_graphics_icon_load_size("ExecutableWindow",32,&state->switcher_icons[slot]);
    return state->switcher_icons[slot];
}

static gui_image_t deskbar_switcher_desktop_icon(deskbar_state_t *state) {
    if (!state->switcher_desktop_icon.pixels)
        (void)bk_graphics_icon_load_size("KDE1/base/kpanel/pics/window_list",32,&state->switcher_desktop_icon);
    return state->switcher_desktop_icon;
}

static int deskbar_switcher_offset(deskbar_switcher_layout_t l,int offset) {
    int maximum=l.count-l.rows;
    if (offset>maximum) offset=maximum;
    return offset<0 ? 0 : offset;
}
static gui_rect_t deskbar_switcher_item(deskbar_switcher_layout_t l,int row) {
    return (gui_rect_t){l.menu.x+3,l.menu.y+3+l.scroll_h+row*SWITCHER_ROW_H,l.menu.w-6,SWITCHER_ROW_H};
}
static void deskbar_switcher_arrow(gui_surface_t *surface,gui_rect_t rect,bool up,uint32_t color) {
    int x=rect.x+rect.w/2,y=rect.y+rect.h/2;
    for(int i=0;i<4;i++) gui_gfx_draw_line(surface,x-i,y+(up?i-2:2-i),x+i,y+(up?i-2:2-i),color);
}
static void deskbar_switcher_draw(gui_desktop_t *desktop,gui_surface_t *surface,deskbar_state_t *state) {
    deskbar_switcher_layout_t l=deskbar_switcher_layout(desktop);
    gui_window_t *selected=deskbar_switcher_selected(desktop,state);
    if (selected) {
        if(state->switcher_hover || (state->switcher_capture&&!state->switcher_pressed_top))
            deskbar_cde_draw_panel(surface,l.cell,CDE_FACE_HOVER,state->switcher_capture);
        gui_image_t icon=deskbar_switcher_icon(state,selected);
        if(icon.pixels)bk_gui_surface_draw_image(surface,
            (gui_rect_t){l.cell.x+(l.cell.w-32)/2,l.cell.y+(l.cell.h-32)/2,32,32},l.cell,&icon);
    }
    else {
        gui_image_t icon=deskbar_switcher_desktop_icon(state);
        if(icon.pixels)bk_gui_surface_draw_image(surface,
            (gui_rect_t){l.cell.x+(l.cell.w-32)/2,l.cell.y+(l.cell.h-32)/2,32,32},l.cell,&icon);
    }
    if(l.count>1) {
        if(state->switcher_top_hover || state->switcher_open)
            deskbar_cde_draw_panel(surface,l.top,CDE_FACE_HOVER,state->switcher_open);
        deskbar_switcher_arrow(surface,l.top,!state->switcher_open,CDE_PAPER);
    }
    if(!state->switcher_open || l.count<2)return;
    deskbar_draw_panel(surface,l.menu,0x00C0C0C0,false);
    int offset=deskbar_switcher_offset(l,state->switcher_scroll);
    uint32_t id=0;
    for(int i=0;i<l.count;i++) {
        gui_window_t *w=deskbar_switcher_next(desktop,id);if(!w)break;id=w->id;
        if(i<offset || i>=offset+l.rows)continue;
        gui_rect_t r=deskbar_switcher_item(l,i-offset);
        bool hover=state->switcher_hover_id==w->id;
        gui_gfx_fill_rect(surface,r,hover?0x00383878:0x00C0C0C0);
        gui_image_t icon=deskbar_switcher_icon(state,w);
        if(icon.pixels)bk_gui_surface_draw_image(surface,(gui_rect_t){r.x+4,r.y+3,32,32},r,&icon);
        gui_font_draw_string_clipped(surface,r.x+42,r.y+15,w->title,hover?0x00FFFFFF:0x00101010,
            (gui_rect_t){r.x+42,r.y+2,r.w-48,r.h-4});
        if(w->minimized)gui_gfx_draw_line(surface,r.x+5,r.y+r.h-3,r.x+12,r.y+r.h-3,0x004E4E4E);
    }
    if(l.scroll_h) {
        deskbar_switcher_arrow(surface,(gui_rect_t){l.menu.x,l.menu.y+3,l.menu.w,l.scroll_h},true,CDE_INK);
        deskbar_switcher_arrow(surface,(gui_rect_t){l.menu.x,l.menu.y+l.menu.h-l.scroll_h-3,l.menu.w,l.scroll_h},false,CDE_INK);
    }
}

static bool deskbar_switcher_event(gui_desktop_t *desktop,deskbar_state_t *state,const gui_event_t *event) {
    if(event->type!=GUI_EVENT_MOUSE_MOVE && event->type!=GUI_EVENT_MOUSE_DOWN &&
       event->type!=GUI_EVENT_MOUSE_UP && event->type!=GUI_EVENT_MOUSE_WHEEL && event->type!=GUI_EVENT_KEY)return false;
    deskbar_switcher_layout_t l=deskbar_switcher_layout(desktop);
    bool was_open=state->switcher_open;
    if(l.count<2)state->switcher_open=false;
    bool cell=gui_rect_contains(l.cell,event->x,event->y);
    bool top=l.count>1 && gui_rect_contains(l.top,event->x,event->y);
    bool menu=state->switcher_open && gui_rect_contains(l.menu,event->x,event->y);
    if(state->menu_open && gui_rect_contains(deskbar_menu_rect(desktop),event->x,event->y))cell=top=false;
    int offset=deskbar_switcher_offset(l,state->switcher_scroll);
    gui_window_t *selected=deskbar_switcher_selected(desktop,state),*hit=NULL;
    uint32_t id=0;
    if(menu)for(int i=0;i<l.count;i++) {
        gui_window_t *w=deskbar_switcher_next(desktop,id);if(!w)break;id=w->id;
        if(i>=offset && i<offset+l.rows && gui_rect_contains(deskbar_switcher_item(l,i-offset),event->x,event->y)){hit=w;break;}
    }
    bool changed=was_open!=state->switcher_open,handled=cell||top||menu;
    if(event->type==GUI_EVENT_KEY) {
        handled=state->switcher_open && event->key==27;
        if(handled){state->switcher_open=false;changed=true;}
    } else if(event->type==GUI_EVENT_MOUSE_MOVE) {
        uint32_t hover=hit?hit->id:0;
        changed|=state->switcher_hover!=cell || state->switcher_top_hover!=top || state->switcher_hover_id!=hover;
        state->switcher_hover=cell;state->switcher_top_hover=top;state->switcher_hover_id=hover;
    } else if(event->type==GUI_EVENT_MOUSE_WHEEL) {
        if(menu){offset+=event->dy>0?-1:event->dy<0?1:0;changed=true;}
    } else if(event->type==GUI_EVENT_MOUSE_DOWN && event->button==MOUSE_LEFT_BUTTON) {
        if(cell||top||menu) {
            if(state->menu_open){deskbar_invalidate(desktop,state);state->menu_open=false;deskbar_reset_menu_state(state);}
            state->switcher_capture=true;state->switcher_pressed_top=top;
            state->switcher_pressed_id=hit?hit->id:cell&&selected?selected->id:0;
            if(menu&&l.scroll_h&&!hit)offset+=event->y<l.menu.y+3+l.scroll_h?-1:1;
            changed=true;
        } else if(state->switcher_open){state->switcher_open=false;changed=true;}
    } else if(event->type==GUI_EVENT_MOUSE_UP && event->button==MOUSE_LEFT_BUTTON) {
        handled=state->switcher_capture;
        if(state->switcher_capture) {
            if(state->switcher_pressed_top && top)state->switcher_open=!state->switcher_open;
            else {
                gui_window_t *activate=menu?hit:cell?selected:NULL;
                if(activate && activate->id==state->switcher_pressed_id) {
                    state->switcher_selected=activate->id;
                    deskbar_focus_window(desktop,activate);state->switcher_open=false;
                }
            }
            changed=true;
        }
        state->switcher_capture=false;state->switcher_pressed_top=false;state->switcher_pressed_id=0;
    }
    state->switcher_scroll=deskbar_switcher_offset(l,offset);
    if(changed) {
        gui_desktop_invalidate_rect(desktop,l.cell);
        gui_desktop_invalidate_rect(desktop,l.top);
        if(was_open||state->switcher_open)gui_desktop_invalidate_rect(desktop,l.menu);
        gui_request_paint();
    }
    if(event->type==GUI_EVENT_MOUSE_MOVE && !menu)return false;
    return handled;
}
#endif
