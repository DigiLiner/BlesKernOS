#include "../kernel/include/api.h"

#define FACE_COLOR 0x00c0c0c0U
#define INK_COLOR 0x00000000U
#define SELECT_COLOR 0x00000080U
#define ROW_HEIGHT 26
#define NAME_SIZE 16
#define CTRL_FONT 0
#define CTRL_SIZE 1
#define CTRL_PREVIEW 2

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    bk_font_info_t fonts[BK_FONT_MAX_FACES];
    uint32_t count, selected, hover, first;
    uint16_t size;
    uint8_t focus;
    bool expanded, editing;
    int pressed;
    char size_text[4];
    uint32_t preview_offset, preview_total;
    uint32_t layout_font, layout_size, layout_width, line_count, line_height;
    bool layout_valid;
    struct { uint16_t start, length; uint8_t sample; uint32_t y; } lines[96];
    gui_scrollbar_drag_t list_drag, preview_drag;
} font_state_t;

typedef struct {
    gui_rect_t client, font, size, up, down, popup, list, preview, paper;
    gui_scrollbar_t list_bar, preview_bar;
    uint32_t rows;
} font_layout_t;

static void number(char *out, uint32_t value) {
    char tmp[12]; int n=0;
    do { tmp[n++]=(char)('0'+value%10); value/=10; } while (value);
    for (int i=0;i<n;i++) out[i]=tmp[n-1-i];
    out[n]=0;
}

static bool inside(gui_rect_t r, const gui_event_t *e) {
    return e->x>=r.x && e->y>=r.y && e->x<r.x+r.w && e->y<r.y+r.h;
}

static font_layout_t layout(font_state_t *s) {
    font_layout_t l={0};
    l.client=bk_gui_window_content_rect_raw(s->window);
    gui_rect_t c=l.client;
    l.font=(gui_rect_t){c.x+24,c.y+44,c.w-174,32};
    l.size=(gui_rect_t){c.x+c.w-130,c.y+44,104,32};
    l.up=(gui_rect_t){l.size.x+l.size.w-19,l.size.y+2,17,14};
    l.down=(gui_rect_t){l.up.x,l.up.y+14,17,14};
    l.preview=(gui_rect_t){c.x+12,c.y+152,c.w-24,c.h-180};
    l.paper=(gui_rect_t){l.preview.x+12,l.preview.y+10,l.preview.w-42,l.preview.h-20};
    int rows=(c.y+c.h-30-(l.font.y+l.font.h))/ROW_HEIGHT;
    if (rows<1) rows=1;
    if (rows>10) rows=10;
    if ((uint32_t)rows>s->count) rows=(int)s->count;
    l.rows=(uint32_t)rows;
    l.popup=(gui_rect_t){l.font.x,l.font.y+l.font.h-1,l.font.w,rows*ROW_HEIGHT+4};
    l.list=(gui_rect_t){l.popup.x+2,l.popup.y+2,l.popup.w-20,rows*ROW_HEIGHT};
    bk_gui_scrollbar_init_vertical(&l.list_bar,
        (gui_rect_t){l.popup.x+l.popup.w-18,l.popup.y+2,16,l.popup.h-4},
        s->first,l.rows,s->count);
    s->first=l.list_bar.value;
    bk_gui_scrollbar_init_vertical(&l.preview_bar,
        (gui_rect_t){l.preview.x+l.preview.w-18,l.preview.y+2,16,l.preview.h-4},
        s->preview_offset,(uint32_t)l.paper.h,s->preview_total);
    return l;
}

static void label(gui_surface_t *surface, gui_rect_t clip, int x, int y, const char *text) {
    bk_gui_font_draw_string_clipped(surface,x,y,text,INK_COLOR,clip);
}

static void bevel(gui_surface_t *surface, gui_rect_t r, bool sunken, uint32_t fill) {
    bk_gui_gfx_fill_rect(surface,r,fill);
    const uint32_t light=sunken ? 0x00808080 : 0x00ffffff;
    const uint32_t dark=sunken ? 0x00ffffff : 0x00404040;
    bk_gui_gfx_draw_line(surface,r.x,r.y,r.x+r.w-1,r.y,light);
    bk_gui_gfx_draw_line(surface,r.x,r.y,r.x,r.y+r.h-1,light);
    bk_gui_gfx_draw_line(surface,r.x+r.w-1,r.y,r.x+r.w-1,r.y+r.h-1,dark);
    bk_gui_gfx_draw_line(surface,r.x,r.y+r.h-1,r.x+r.w-1,r.y+r.h-1,dark);
    if (sunken) {
        bk_gui_gfx_draw_line(surface,r.x+1,r.y+1,r.x+r.w-2,r.y+1,0x00404040);
        bk_gui_gfx_draw_line(surface,r.x+1,r.y+1,r.x+1,r.y+r.h-2,0x00404040);
    }
}

static void focus_rect(gui_surface_t *surface, gui_rect_t r) {
    for (int x=r.x;x<r.x+r.w;x+=2) {
        bk_gui_gfx_putpixel(surface,x,r.y,0);
        bk_gui_gfx_putpixel(surface,x,r.y+r.h-1,0);
    }
    for (int y=r.y;y<r.y+r.h;y+=2) {
        bk_gui_gfx_putpixel(surface,r.x,y,0);
        bk_gui_gfx_putpixel(surface,r.x+r.w-1,y,0);
    }
}

static void arrow(gui_surface_t *surface, gui_rect_t r, bool up, bool down) {
    bevel(surface,r,down,FACE_COLOR);
    int x=r.x+r.w/2+(down ? 1 : 0), y=r.y+r.h/2+(down ? 1 : 0);
    for (int row=0;row<4;row++) {
        int n=up ? row : 3-row;
        bk_gui_gfx_draw_line(surface,x-n,y-2+row,x+n,y-2+row,INK_COLOR);
    }
}

static void size_set(font_state_t *s, int value) {
    if (value<(int)BK_FONT_MIN_SIZE) value=BK_FONT_MIN_SIZE;
    if (value>(int)BK_FONT_MAX_SIZE) value=BK_FONT_MAX_SIZE;
    s->size=(uint16_t)value;s->editing=false;
    number(s->size_text,s->size);s->preview_offset=0;
}

static void size_commit(font_state_t *s) {
    if (!s->editing) return;
    int value=0;
    for (uint32_t i=0;s->size_text[i];i++) value=value*10+s->size_text[i]-'0';
    size_set(s,value ? value : s->size);
}

static void select_font(font_state_t *s, uint32_t index) {
    if (index>=s->count) return;
    s->selected=s->hover=index;s->expanded=false;s->preview_offset=0;
    s->list_drag.active=false;
}

static void reveal(font_state_t *s, uint32_t rows) {
    if (!rows) return;
    if (s->hover<s->first) s->first=s->hover;
    if (s->hover>=s->first+rows) s->first=s->hover-rows+1;
    if (s->count<=rows) s->first=0;
    else if (s->first>s->count-rows) s->first=s->count-rows;
}

static void toggle(font_state_t *s, uint32_t rows) {
    size_commit(s);s->focus=CTRL_FONT;s->expanded=!s->expanded;
    s->hover=s->selected;s->pressed=0;reveal(s,rows);
}

static const char *preview_samples[]={"BlesKernOS: ¡Hola, mundo!",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz",
    "0123456789  !? @#$% &() +-=/", "ÁÉÍÓÚ áéíóú Ññ Üü Çç € — …",
    "El veloz murciélago hindú comía feliz cardillo y kiwi."};

/* Cache wrapping by family, size and width. Hovering list rows only repaints;
 * it must not remeasure hundreds of characters through the API every frame. */
static int preview_lines(font_state_t *s, gui_surface_t *surface, gui_rect_t paper, bool paint) {
    uint32_t id=s->fonts[s->selected].id;
    if (!s->layout_valid || s->layout_font!=id || s->layout_size!=s->size ||
        s->layout_width!=(uint32_t)paper.w) {
        int line=bk_font_line_height(id,s->size)+6, y=0;
        s->line_count=0;s->layout_valid=false;
        if (line<=6) return 0;
        s->line_height=(uint32_t)line;
        for (uint32_t row=0;row<sizeof(preview_samples)/sizeof(preview_samples[0]);row++) {
            const char *text=preview_samples[row];uint32_t start=0,end=0;
            while (text[start] && s->line_count<96) {
                end=start;uint32_t word_break=start;
                while (text[end]) {
                    uint8_t c=(uint8_t)text[end];
                    uint32_t n=c<0x80 ? 1 : (c<0xe0 ? 2 : (c<0xf0 ? 3 : 4));
                    uint32_t next=end+n;
                    if (bk_font_text_width(id,s->size,text+start,next-start)>(uint32_t)paper.w && end>start) {
                        if (word_break>start) end=word_break;
                        break;
                    }
                    if (c==' ') word_break=next;
                    end=next;
                }
                uint32_t index=s->line_count++;
                s->lines[index].sample=(uint8_t)row;
                s->lines[index].start=(uint16_t)start;s->lines[index].length=(uint16_t)(end-start);
                s->lines[index].y=(uint32_t)y;
                y+=line;start=end;
                while (text[start]==' ') start++;
            }
            y+=4;
        }
        s->preview_total=(uint32_t)y;s->layout_font=id;s->layout_size=s->size;
        s->layout_width=(uint32_t)paper.w;s->layout_valid=true;
    }
    if (paint) for (uint32_t i=0;i<s->line_count;i++) {
        int y=(int)s->lines[i].y-(int)s->preview_offset;
        if (y+(int)s->line_height<=0 || y>=paper.h) continue;
        const char *text=preview_samples[s->lines[i].sample]+s->lines[i].start;
        bk_font_draw_text(surface,id,s->size,paper.x,paper.y+y,text,s->lines[i].length,INK_COLOR,paper);
    }
    return (int)s->preview_total;
}

static void paint_list(font_state_t *s, gui_surface_t *surface, font_layout_t *l) {
    /* Owner-drawn combo: each label uses the typeface it selects. */
    bk_gui_gfx_fill_rect(surface,(gui_rect_t){l->popup.x+3,l->popup.y+3,l->popup.w,l->popup.h},0x00606060);
    bevel(surface,l->popup,true,0x00ffffff);
    for (uint32_t row=0;row<l->rows;row++) {
        uint32_t index=s->first+row;
        if (index>=s->count) break;
        gui_rect_t cell={l->list.x,l->list.y+(int)row*ROW_HEIGHT,l->list.w,ROW_HEIGHT};
        uint32_t color=INK_COLOR;
        if (index==s->hover) {
            bk_gui_gfx_fill_rect(surface,cell,SELECT_COLOR);color=0x00ffffff;
        }
        bk_font_info_t *f=&s->fonts[index];
        if (!bk_font_draw_text(surface,f->id,NAME_SIZE,cell.x+6,cell.y+2,f->name,
                               bk_runtime_strlen(f->name),color,cell))
            label(surface,cell,cell.x+6,cell.y+8,f->name);
    }
    bk_gui_scrollbar_paint_vertical(surface,&l->list_bar);
}

static void content(gui_window_t *w UNUSED, gui_surface_t *surface, void *context) {
    font_state_t *s=context;font_layout_t l=layout(s);gui_rect_t c=l.client;
    bk_gui_gfx_fill_rect(surface,c,FACE_COLOR);
    /* Etched configuration group and a double rule separate it from the preview. */
    gui_rect_t group={c.x+12,c.y+12,c.w-24,104};
    bk_gui_gfx_draw_rect(surface,group,0x00808080);
    bk_gui_gfx_draw_rect(surface,(gui_rect_t){group.x+1,group.y+1,group.w,group.h},0x00ffffff);
    bk_gui_gfx_fill_rect(surface,(gui_rect_t){c.x+22,c.y+8,104,12},FACE_COLOR);
    label(surface,c,c.x+26,c.y+9,"Configuracion");
    label(surface,c,l.font.x,c.y+29,"Fuente:");
    label(surface,c,l.size.x,c.y+29,"Tama\xf1o (px):");
    bevel(surface,l.font,true,0x00ffffff);
    gui_rect_t name_clip={l.font.x+5,l.font.y+3,l.font.w-29,l.font.h-6};
    gui_rect_t drop={l.font.x+l.font.w-22,l.font.y+2,20,l.font.h-4};
    arrow(surface,drop,false,s->expanded || s->pressed==1);
    if (s->count) {
        bk_font_info_t *f=&s->fonts[s->selected];
        bk_font_draw_text(surface,f->id,NAME_SIZE,name_clip.x+2,name_clip.y+3,f->name,
                          bk_runtime_strlen(f->name),INK_COLOR,name_clip);
        if (s->focus==CTRL_FONT && s->window->focused) focus_rect(surface,name_clip);
        char detail[96],digits[12];number(digits,s->count);
        bk_runtime_strcpy(detail,digits);bk_runtime_strcat(detail," fuentes instaladas    |    ");
        bk_runtime_strcat(detail,(f->flags&BK_FONT_MONOSPACE) ? "Monoespaciada" : "Proporcional");
        bk_runtime_strcat(detail,"    |    Latin-1");label(surface,c,l.font.x,c.y+92,detail);
    } else label(surface,c,l.font.x,l.font.y+10,"No hay fuentes instaladas");
    bevel(surface,l.size,true,0x00ffffff);
    gui_rect_t size_inner={l.size.x+5,l.size.y+4,l.size.w-27,l.size.h-8};
    bool size_focused=s->focus==CTRL_SIZE && s->window->focused;
    if (size_focused && !s->editing) bk_gui_gfx_fill_rect(surface,size_inner,SELECT_COLOR);
    bk_gui_font_draw_string_clipped(surface,size_inner.x+7,size_inner.y+8,s->size_text,
        size_focused && !s->editing ? 0x00ffffff : INK_COLOR,size_inner);
    if (size_focused && s->editing) {
        int caret=size_inner.x+7+(int)bk_runtime_strlen(s->size_text)*7;
        bk_gui_gfx_draw_line(surface,caret,size_inner.y+5,caret,size_inner.y+17,INK_COLOR);
    }
    arrow(surface,l.up,true,s->pressed==3);arrow(surface,l.down,false,s->pressed==4);
    bk_gui_gfx_draw_line(surface,c.x+12,c.y+128,c.x+c.w-13,c.y+128,0x00808080);
    bk_gui_gfx_draw_line(surface,c.x+12,c.y+129,c.x+c.w-13,c.y+129,0x00ffffff);
    label(surface,c,c.x+16,c.y+139,"Vista previa");
    bevel(surface,l.preview,true,0x00ffffff);
    if (s->count) {
        s->preview_total=(uint32_t)preview_lines(s,surface,l.paper,false);
        uint32_t max=s->preview_total>(uint32_t)l.paper.h ? s->preview_total-l.paper.h : 0;
        if (s->preview_offset>max) s->preview_offset=max;
        if (!s->preview_total) label(surface,l.paper,l.paper.x,l.paper.y,"No se pudo leer la fuente.");
        else preview_lines(s,surface,l.paper,true);
        l=layout(s);
        bk_gui_scrollbar_paint_vertical(surface,&l.preview_bar);
    }
    gui_rect_t status={c.x+12,c.y+c.h-21,c.w-24,18};
    bevel(surface,status,true,FACE_COLOR);
    label(surface,status,status.x+5,status.y+5,"Tab: cambiar control   F4: lista   + / -: tama\xf1o   Rueda: desplazar");
    if (s->expanded && s->count) paint_list(s,surface,&l);
}

static bool key_event(font_state_t *s, const gui_event_t *e, font_layout_t *l) {
    uint8_t key=(uint8_t)e->key;
    if (key==KEY_F4 || (e->alt && key==KEY_DOWN) ||
        (s->focus==CTRL_FONT && key==' ')) { toggle(s,l->rows);return true; }
    if (key==KEY_TAB) {
        size_commit(s);s->expanded=false;s->focus=(uint8_t)((s->focus+(e->shift ? 2 : 1))%3);return true;
    }
    if (s->expanded) {
        if (key==KEY_ESCAPE) { s->expanded=false;s->hover=s->selected;return true; }
        if (key==KEY_ENTER || key=='\n') { select_font(s,s->hover);return true; }
        int next=(int)s->hover;
        if (key==KEY_UP) next--;
        else if (key==KEY_DOWN) next++;
        else if (key==KEY_PGUP) next-=(int)l->rows;
        else if (key==KEY_PGDN) next+=(int)l->rows;
        else if (key==KEY_HOME) next=0;
        else if (key==KEY_END) next=(int)s->count-1;
        else if ((key>='A' && key<='Z') || (key>='a' && key<='z')) {
            char lower=(char)(key|32);
            for (uint32_t n=1;n<=s->count;n++) {
                uint32_t index=(s->hover+n)%s->count;
                if ((s->fonts[index].name[0]|32)==lower) { next=(int)index;break; }
            }
        } else return false;
        if (next<0) next=0;
        if (next>=(int)s->count) next=(int)s->count-1;
        s->hover=(uint32_t)next;reveal(s,l->rows);return true;
    }
    if (s->focus==CTRL_SIZE) {
        if (key>='0' && key<='9') {
            if (!s->editing) { s->size_text[0]=0;s->editing=true; }
            uint32_t n=bk_runtime_strlen(s->size_text);
            if (n<2) { s->size_text[n]=(char)key;s->size_text[n+1]=0; }return true;
        }
        if (key==KEY_BACKSPACE) {
            uint32_t n=bk_runtime_strlen(s->size_text);s->editing=true;
            if (n) s->size_text[n-1]=0;
            return true;
        }
        if (key==KEY_ENTER || key=='\n') { size_commit(s);return true; }
        if (key==KEY_ESCAPE) { size_set(s,s->size);return true; }
    }
    if (key=='+' || key=='=' || (s->focus==CTRL_SIZE && key==KEY_UP)) { size_set(s,s->size+1);return true; }
    if (key=='-' || (s->focus==CTRL_SIZE && key==KEY_DOWN)) { size_set(s,s->size-1);return true; }
    if (s->count && (key=='[' || key==']' || (s->focus==CTRL_FONT && (key==KEY_UP || key==KEY_DOWN)))) {
        bool previous=key=='[' || key==KEY_UP;
        select_font(s,(s->selected+(previous ? s->count-1 : 1))%s->count);return true;
    }
    if (key==KEY_PGUP || key==KEY_PGDN || key==KEY_HOME || key==KEY_END) {
        int offset=(int)s->preview_offset;
        if (key==KEY_PGUP) offset-=l->paper.h;
        if (key==KEY_PGDN) offset+=l->paper.h;
        if (key==KEY_HOME) offset=0;
        if (key==KEY_END) offset=(int)s->preview_total;
        s->preview_offset=offset<0 ? 0 : (uint32_t)offset;return true;
    }
    return false;
}

static bool event(gui_window_t *w UNUSED, const gui_event_t *e, void *context) {
    font_state_t *s=context;font_layout_t l=layout(s);bool handled=false;
    if (e->type==GUI_EVENT_KEY) handled=key_event(s,e,&l);
    else if (e->type==GUI_EVENT_FOCUS_LOST || e->type==GUI_EVENT_RESIZE) {
        size_commit(s);s->expanded=false;s->pressed=0;
        s->list_drag.active=s->preview_drag.active=false;handled=true;
    } else if (s->expanded && s->count) {
        uint32_t value=s->first;
        if (bk_gui_scrollbar_handle_event_vertical(&l.list_bar,&s->list_drag,e,3,&value)) {
            s->first=value;handled=true;
        } else if (e->type==GUI_EVENT_MOUSE_MOVE && inside(l.list,e)) {
            uint32_t index=s->first+(uint32_t)(e->y-l.list.y)/ROW_HEIGHT;
            if (index<s->count && index!=s->hover) { s->hover=index;handled=true; }
        } else if (e->type==GUI_EVENT_MOUSE_DOWN && e->button==1) {
            if (inside(l.list,e)) s->pressed=100+(int)(s->first+(uint32_t)(e->y-l.list.y)/ROW_HEIGHT);
            else { s->expanded=false;s->pressed=0; }
            handled=true;
        } else if (e->type==GUI_EVENT_MOUSE_UP && e->button==1) {
            if (inside(l.list,e)) {
                uint32_t index=s->first+(uint32_t)(e->y-l.list.y)/ROW_HEIGHT;
                if (s->pressed==100+(int)index) select_font(s,index);
            }
            s->pressed=0;handled=true;
        }
    } else {
        uint32_t value=s->preview_offset;
        bool bar_event=s->preview_drag.active || inside(l.preview,e);
        if (bar_event && bk_gui_scrollbar_handle_event_vertical(&l.preview_bar,&s->preview_drag,e,36,&value)) {
            s->preview_offset=value;handled=true;
        } else if (e->type==GUI_EVENT_MOUSE_DOWN && e->button==1) {
            if (inside(l.font,e) && s->count) { size_commit(s);s->focus=CTRL_FONT;s->pressed=1; }
            else if (inside(l.up,e)) { size_commit(s);s->focus=CTRL_SIZE;s->pressed=3; }
            else if (inside(l.down,e)) { size_commit(s);s->focus=CTRL_SIZE;s->pressed=4; }
            else if (inside(l.size,e)) { s->focus=CTRL_SIZE;s->pressed=2; }
            else { size_commit(s);s->focus=CTRL_PREVIEW;s->pressed=0; }
            handled=true;
        } else if (e->type==GUI_EVENT_MOUSE_UP && e->button==1) {
            if (s->pressed==1 && inside(l.font,e)) toggle(s,l.rows);
            if (s->pressed==3 && inside(l.up,e)) size_set(s,s->size+1);
            if (s->pressed==4 && inside(l.down,e)) size_set(s,s->size-1);
            handled=s->pressed!=0;s->pressed=0;
        }
    }
    if (handled) s->window->dirty=true;
    return handled;
}

static void font_main(void *argument) {
    font_state_t *s=argument;
    s->window=bk_gui_create_window(s->desktop,100,40,740,570,"Font - Fuentes");
    if (s->window) {
        s->window->owner_pid=bk_sys_getpid();bk_proc_bind_window(s->window);
        bk_gui_set_window_content(s->window,content,s);
        bk_gui_set_window_event_handler(s->window,event,s);
        bk_gui_window_set_extended_events(s->window,true);
        bk_gui_set_window_min_size(s->window,620,450);
    }
    while (!bk_proc_exit_requested() && s->window && s->window->listed) bk_sys_sleep_ticks(4);
    if (s->window) {
        bk_gui_desktop_remove_window(s->desktop,s->window);
        bk_gui_window_destroy_raw(s->window);bk_proc_bind_window(NULL);
    }
    bk_sys_free(s);bk_proc_exit();
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    if (!desktop) return;
    font_state_t *s=bk_sys_alloc_zero(sizeof(*s));if (!s) return;
    s->desktop=desktop;size_set(s,24);
    uint32_t count=bk_font_count();
    for (uint32_t i=0;i<count && s->count<BK_FONT_MAX_FACES;i++)
        if (bk_font_info(i,&s->fonts[s->count])) s->count++;
    for (uint32_t i=0;i<s->count;i++) for (uint32_t j=i+1;j<s->count;j++)
        if (bk_runtime_strcmp(s->fonts[i].name,s->fonts[j].name)>0) {
            bk_font_info_t temp=s->fonts[i];s->fonts[i]=s->fonts[j];s->fonts[j]=temp;
        }
    for (uint32_t i=0;i<s->count;i++)
        if (!bk_runtime_strcmp(s->fonts[i].name,"DejaVu Sans")) s->selected=i;
    bk_proc_set_memory_hint(sizeof(*s));
    if (bk_proc_spawn_thread("font",font_main,s)<0) bk_sys_free(s);
}
