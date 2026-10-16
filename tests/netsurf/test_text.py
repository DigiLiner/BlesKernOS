#!/usr/bin/env python3
"""Exercise the browser bridge against the real BFN renderer, with sanitizers."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
renderer='\n'.join(line for line in (ROOT/'gui/real_font.c').read_text().splitlines() if not line.startswith('#include '))
bridge='\n'.join(line for line in (ROOT/'programs/netsurf/font.c').read_text().splitlines() if not line.startswith('#include '))
css=(ROOT/'programs/netsurf/css_engine.c').read_text()
paint=css[css.index('typedef struct {\n    uint32_t color;'):css.index('} paint_style_t;')+len('} paint_style_t;')]
text_functions=css[css.index('static uint32_t transformed_text('):css.index('static void emit_utf8_word(')]
layout_source='\n'.join(line for line in (ROOT/'programs/netsurf/layout.c').read_text().splitlines() if not line.startswith('#include '))
domains=(ROOT/'kernel/sched/kernel_domains.c').read_text()
domain_header=(ROOT/'kernel/include/kernel_domains.h').read_text()
domain_enum=domain_header[domain_header.index('typedef enum {'):domain_header.index('} kernel_domain_mask_t;')+len('} kernel_domain_mask_t;')]
domain_helpers=domains[domains.index('static bool text_starts('):domains.index('void kernel_domains_init(')]
domain_classifier=domains[domains.index('uint32_t kernel_domain_mask_for_api('):domains.index('void kernel_domains_enter(')]
layout_adapters=r'''
#include "layout.h"
#define CSS_TEXT_TRANSFORM_NONE 4
#define CSS_TEXT_TRANSFORM_INHERIT 0
#define CSS_TEXT_TRANSFORM_UPPERCASE 2
#define CSS_TEXT_TRANSFORM_LOWERCASE 3
#define CSS_TEXT_TRANSFORM_CAPITALIZE 1
static void *bk_sys_alloc(uint32_t n){return malloc(n);}
static void bk_sys_free(void *p){free(p);}
typedef struct {nsbk_layout_t *layout;int32_t cursor_x,cursor_y,line_right,line_start_x,line_height,active_link;} css_context_t;
static bool layout_should_abort(css_context_t *ctx){return ctx->layout->truncated;}
static void finish_line(css_context_t *ctx){ctx->cursor_x=ctx->line_start_x;ctx->cursor_y+=ctx->line_height;ctx->line_height=20;}
'''
checks=r'''
static void check_fit(uint16_t face,const char *text) {
    uint32_t length=strlen(text);
    for (int available=0;available<240;available++) {
        uint32_t expected=0;
        for (uint32_t n=nsbk_utf8_next(text,length,0);n;n=nsbk_utf8_next(text,length,n)) {
            if (nsbk_font_width(face,16,false,false,text,n)>available) break;
            expected=n;if(n==length)break;
        }
        assert(nsbk_font_fit(face,16,false,false,text,length,available,0,0)==expected);
    }
}
int main(int argc,char **argv) {
    assert(kernel_domain_mask_for_api("bk_font_text_advances")==0U);
    assert(kernel_domain_mask_for_api("bk_font_draw_text_styled")==0U);
    assert(kernel_domain_mask_for_api("bk_gui_surface_draw_text")==KDOMAIN_GFX);
    assert(kernel_domain_mask_for_api("unknown_api")==KDOMAIN_LEGACY);
    assert(argc==2);font_dir=argv[1];nsbk_font_init();
    uint16_t sans=nsbk_font_default(NSBK_FAMILY_SANS),serif=nsbk_font_default(NSBK_FAMILY_SERIF);
    assert(sans!=NSBK_FONT_NONE && serif!=sans);
    assert(nsbk_font_named("dEjAvU sAnS",11)==sans);
    assert(nsbk_font_named("missing face",12)==NSBK_FONT_NONE);
    assert(nsbk_font_default(NSBK_FAMILY_MONO)!=sans);
    const char *samples[]={"iii WWW", "áéíóú ñ € — …", "Primera\nSegunda línea", "\tA\rB", "sí 😀"};
    for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);i++)
        for(unsigned size=12;size<=24;size+=4) {
            uint32_t n=strlen(samples[i]);
            assert(nsbk_font_width(sans,size,false,false,samples[i],n)==raw_font_text_width(sans,size,samples[i],n));
        }
    uint32_t advances[5]={0xdeadbeef,0,0,0,0xdeadbeef};
    assert(raw_font_text_advances(sans,16,"á €",strlen("á €"),advances+1,3)==3);
    assert(advances[0]==0xdeadbeef && advances[4]==0xdeadbeef);
    assert((advances[1]+advances[2]+advances[3]+63)/64==raw_font_text_width(sans,16,"á €",strlen("á €")));
    assert(raw_font_text_advances(sans,16,"abc",3,advances+1,1)==0);
    assert(advances[0]==0xdeadbeef && advances[4]==0xdeadbeef);
    check_fit(sans,"abcde");check_fit(sans,"áéíóú ñ € — …");check_fit(NSBK_FONT_NONE,"áéíóú");
    assert(nsbk_font_spacing("áé €",strlen("áé €"),2,3)==9);
    char longword[401];memset(longword,'W',400);longword[400]=0;
    memset(widths,0,sizeof(widths));memset(glyph_advances,0,sizeof(glyph_advances));measure_calls=0;
    uint32_t fit=nsbk_font_fit(sans,16,false,false,longword,400,250,0,0);
    assert(fit>0 && fit<400 && measure_calls<15);
    unsigned measured=measure_calls;
    measure_calls=0;
    assert(nsbk_font_fit(sans,16,false,false,longword,400,10000,0,0)==400);
    assert(measure_calls<=1);
    nsbk_font_width(sans,16,false,false,"cached",6);
    unsigned cached=measure_calls;
    assert(nsbk_font_width(sans,16,false,false,"cached",6)>0 && measure_calls==cached);
    nsbk_layout_t layout;assert(nsbk_layout_init(&layout));
    css_context_t ctx={.layout=&layout,.cursor_x=8,.cursor_y=8,.line_right=200,.line_start_x=8,.line_height=20,.active_link=-1};
    paint_style_t style={.font_id=sans,.font_px=16,.line_height=20,.color=0x203040,.visible=true,.opacity=255,.text_transform=CSS_TEXT_TRANSFORM_NONE};
    add_text_piece(&ctx,"Hola",4,&style);add_text_piece(&ctx," ",1,&style);add_text_piece(&ctx,"ñ €",strlen("ñ €"),&style);
    assert(layout.item_count==1 && !strcmp(layout.text,"Hola ñ €"));
    assert(layout.items[0].width==nsbk_font_width(sans,16,false,false,layout.text,strlen(layout.text)));
    ctx.active_link=0;add_text_piece(&ctx,"link",4,&style);assert(layout.item_count==2);
    nsbk_layout_reset(&layout);ctx.cursor_x=8;ctx.cursor_y=8;ctx.line_right=40;
    const char *unicode="áéíóúñ€";
    emit_text_run(&ctx,unicode,strlen(unicode),&style);
    uint32_t joined=0;
    for(unsigned i=0;i<layout.item_count;i++) {
        nsbk_layout_item_t *item=&layout.items[i];
        assert(item->width<=32 && item->text_length>0);
        assert(!memcmp(layout.text+item->text_offset,unicode+joined,item->text_length));
        assert(((unsigned char)layout.text[item->text_offset]&0xc0U)!=0x80U);
        joined+=item->text_length;
    }
    assert(joined==strlen(unicode));
    nsbk_layout_reset(&layout);ctx.cursor_x=8;ctx.cursor_y=8;ctx.line_right=120;
    style.letter_spacing=2;style.word_spacing=3;
    add_text_piece(&ctx,"ñ",strlen("ñ"),&style);add_text_piece(&ctx," ",1,&style);add_text_piece(&ctx,"€",strlen("€"),&style);
    assert(layout.item_count==1);
    assert(layout.items[0].width==nsbk_font_width(sans,16,false,false,"ñ €",strlen("ñ €"))+nsbk_font_spacing("ñ €",strlen("ñ €"),2,3));
    nsbk_layout_destroy(&layout);
    uint32_t pixels[160*80];for(unsigned i=0;i<160*80;i++)pixels[i]=0x203040;
    gui_surface_t s={.pixels=pixels,.width=160,.height=80,.pitch=160,.clip={5,5,145,65}};
    nsbk_font_draw_spaced(&s,(gui_rect_t){10,10,130,50},sans,24,5,8,"ñ € —",strlen("ñ € —"),0xffffff,true,true,false,2,3);
    unsigned ink=0,gray=0;
    for(unsigned y=0;y<80;y++)for(unsigned x=0;x<160;x++) {
        uint32_t p=pixels[y*160+x];
        if(x<10||x>=140||y<10||y>=60) assert(p==0x203040);
        if(p!=0x203040){ink++;if(p!=0xffffff)gray++;}
    }
    assert(ink>100 && gray>20);
    printf("PASS: real OS families, UTF-8 boundaries, cached metrics, clipped bold/italic/AA; long word: %u metric calls (old: ~%u); fitting 400-character run: 1 call (old: 400)\n",measured,fit+1);
    for(unsigned i=0;i<FONT_CACHE_SLOTS;i++)free(cache[i].data);
}
'''
with tempfile.TemporaryDirectory(prefix='bk-browser-text-') as directory:
    tmp=Path(directory)
    # Reuse the existing host VFS adapters; render actual installed font packs.
    prefix=(ROOT/'tests/fonts/runtime_harness.c').read_text()
    prefix+='\n#define bk_font_text_width raw_font_text_width\n#define bk_font_text_advances raw_font_text_advances\n'
    middle=r'''
#undef bk_font_text_width
#undef bk_font_text_advances
static unsigned measure_calls;
static uint32_t bk_font_text_advances(uint32_t id,uint16_t size,const char *text,uint32_t length,uint32_t *advances,uint32_t capacity){measure_calls++;return raw_font_text_advances(id,size,text,length,advances,capacity);}
static uint16_t bk_font_text_width(uint32_t id,uint16_t size,const char *text,uint32_t length){measure_calls++;return raw_font_text_width(id,size,text,length);}
#define BLESKERNOS_APPLICATION_API_H
 typedef gui_surface_t bk_gui_surface_t;
 typedef gui_rect_t bk_gui_rect_t;
#include "font.h"
#define faces browser_faces
#define face_count browser_face_count
static uint16_t bk_gui_text_width_px(const char *text,uint32_t n,uint16_t size,bool mono,bool bold){(void)text;(void)mono;return n*(size/2+bold);}
static void bk_gui_surface_draw_text_px(bk_gui_surface_t *s,int x,int y,const char *t,uint32_t n,uint32_t c,uint16_t z,bool b,bool i,bool m,bk_gui_rect_t clip){(void)s;(void)x;(void)y;(void)t;(void)n;(void)c;(void)z;(void)b;(void)i;(void)m;(void)clip;}
'''
    (tmp/'test.c').write_text(prefix+renderer+middle+bridge+layout_adapters+paint+layout_source+text_functions+domain_enum+domain_helpers+domain_classifier+checks)
    subprocess.run(['gcc','-std=c11','-O2','-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-pie','-no-pie','-idirafter',str(ROOT/'sdk/include'),'-iquote',str(ROOT/'programs/netsurf'),str(tmp/'test.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test'),str(ROOT/'assets/fonts')],check=True)
