static void reset(void) {
    for (unsigned i=0;i<FONT_CACHE_SLOTS;i++) { free(cache[i].data);cache[i].data=NULL; }
    memset(faces,0,sizeof(faces));face_count=0;
}
static uint32_t find_font(const char *name) {
    for (uint32_t i=0;i<bk_font_count();i++) {
        bk_font_info_t info;assert(bk_font_info(i,&info));
        if (!strcmp(info.name,name)) return i;
    }
    abort();
}
int main(int argc,char **argv) {
    assert(argc==3);font_dir=argv[1];
    _Static_assert(sizeof(bk_font_info_t)==60,"public metadata ABI");
    _Static_assert(sizeof(font_header_t)==64,"header ABI");
    _Static_assert(sizeof(font_strike_t)==16,"directory ABI");
    _Static_assert(sizeof(font_glyph_t)==20,"glyph ABI");
    assert(bk_font_count()==24);
    for (unsigned f=0;f<24;f++) {
        bk_font_info_t info;assert(bk_font_info(f,&info));assert(info.id==f);
        assert(info.min_size==8 && info.max_size==72);
        for (unsigned size=8;size<=72;size++) {
            assert(bk_font_line_height(f,size)>=size);
            assert(bk_font_text_width(f,size,"Hello",5)>0);
        }
    }
    uint32_t sans=find_font("DejaVu Sans"), mono=find_font("DejaVu Sans Mono");
    assert(!bk_font_info(24,NULL));assert(!bk_font_line_height(0,7));assert(!bk_font_line_height(0,73));
    assert(!bk_font_line_height(99,24));assert(!bk_font_text_width(sans,24,"x",4097));
    assert(bk_font_text_width(sans,24,"WWW",3)>bk_font_text_width(sans,24,"iii",3));
    assert(bk_font_text_width(mono,24,"WWW",3)==bk_font_text_width(mono,24,"iii",3));
    assert(bk_font_text_width(sans,24,"\xc3\xb1",2)==bk_font_text_width(sans,24,"n",1));
    assert(bk_font_text_width(sans,24,"\xf0\x9f\x98\x80",4)==bk_font_text_width(sans,24,"?",1));
    assert(bk_font_text_width(sans,24,"a\nb",3)==bk_font_text_width(sans,24,"b",1));
    assert(bk_font_text_width(sans,24,"\t",1)==4*bk_font_text_width(sans,24," ",1));
    assert(bk_font_text_width(sans,24,"\xc0",1)>0);
    unsigned before=reads;bk_font_text_width(sans,24,"Hello",5);assert(before==reads);
    /* Virtual screen larger than backing allocation; nonzero origin and pitch. */
    uint32_t pixels[12*12];for (unsigned i=0;i<144;i++) pixels[i]=0x112233;
    gui_surface_t s={pixels,1024,768,12,{103,203,4,4},100,200,10,10};
    assert(bk_font_draw_text(&s,sans,24,100,195,"MMMM",4,0xffffff,(gui_rect_t){INT_MIN,INT_MIN,INT_MAX,INT_MAX}));
    for (unsigned i=0;i<144;i++) assert(pixels[i]==0x112233);
    assert(bk_font_draw_text(&s,sans,24,100,195,"MMMM",4,0xffffff,(gui_rect_t){100,200,100,100}));
    bool changed=false;
    for (int y=0;y<12;y++) for (int x=0;x<12;x++) {
        if (x<3 || x>=7 || y<3 || y>=7) assert(pixels[y*12+x]==0x112233);
        else if (pixels[y*12+x]!=0x112233) changed=true;
    }
    assert(changed);
    /* Render the actual C engine to a standalone preview. */
    unsigned w=1000,h=510;uint32_t *buffer=malloc(w*h*4);assert(buffer);
    for (unsigned i=0;i<w*h;i++) buffer[i]=0xffffff;
    s=(gui_surface_t){buffer,w,h,w,{0,0,w,h},0,0,w,h};
    const char *text="BlesKernOS: \xc2\xa1Hola! \xc3\x91\xc3\xb1 \xc3\xa1\xc3\xa9\xc3\xad\xc3\xb3\xc3\xba \xe2\x82\xac 0123456789";
    unsigned sizes[]={12,24,48};int y=10;
    for (unsigned f=0;f<3;f++) {
        for (unsigned i=0;i<3;i++) {
            assert(bk_font_draw_text(&s,f,sizes[i],10,y,text,(uint32_t)strlen(text),0x172b40,s.clip));
            y+=bk_font_line_height(f,sizes[i])+3;
        }
        y+=10;
    }
    for (unsigned i=0;i<w*h;i++) assert(buffer[i]==0xffffff || buffer[i]==0x172b40);
    FILE *out=fopen(argv[2],"wb");assert(out);fprintf(out,"P6\n%u %u\n255\n",w,h);
    for (unsigned i=0;i<w*h;i++) { fputc(buffer[i]>>16,out);fputc(buffer[i]>>8,out);fputc(buffer[i],out); }
    fclose(out);free(buffer);
    uint32_t decoded_bytes=0;
    const uint8_t run[]={4,0,0,0,0x83,0x7f};
    uint8_t *decoded=unpack_strike(run,sizeof(run),&decoded_bytes);
    assert(decoded && decoded_bytes==4 && decoded[0]==0x7f && decoded[3]==0x7f);free(decoded);
    const uint8_t literal[]={4,0,0,0,3,1,2,3,4};
    decoded=unpack_strike(literal,sizeof(literal),&decoded_bytes);
    assert(decoded && decoded_bytes==4 && decoded[3]==4);free(decoded);
    assert(!unpack_strike(literal,sizeof(literal)-1,&decoded_bytes));
    assert(!unpack_strike(run,sizeof(run)-1,&decoded_bytes));
    const uint8_t overflow[]={4,0,0,0,0x84,0};
    const uint8_t oversized[]={0xff,0xff,0xff,0x7f};
    assert(!unpack_strike(overflow,sizeof(overflow),&decoded_bytes));
    assert(!unpack_strike(oversized,sizeof(oversized),&decoded_bytes));
    /* A complete selector pass stays in the small cache on its second paint. */
    for (unsigned f=0;f<24;f++) assert(bk_font_text_width(f,16,"Name",4));
    before=reads;
    for (unsigned f=0;f<24;f++) assert(bk_font_text_width(f,16,"Name",4));
    assert(reads==before);
    /* Validate rejects malformed glyphs, offsets, truncated packs and names. */
    char path[1024];snprintf(path,sizeof(path),"%s/DEJAVUSANS.BFN",font_dir);
    FILE *f=fopen(path,"r+b");assert(f);font_strike_t strike;
    fseek(f,64,SEEK_SET);assert(fread(&strike,1,sizeof(strike),f)==sizeof(strike));
    uint32_t invalid=UINT32_MAX;fseek(f,strike.offset+4+4,SEEK_SET);fwrite(&invalid,4,1,f);fclose(f);
    reset();assert(bk_font_count()==24);assert(!bk_font_line_height(find_font("DejaVu Sans"),8));
    f=fopen(path,"r+b");assert(f);fseek(f,64+8,SEEK_SET);fwrite(&invalid,4,1,f);fclose(f);
    reset();assert(bk_font_count()==23);
    reset();
    puts("PASS native font engine: 24 faces, 65 sizes, BFN1/BFN2 compression, UTF-8, cache, proportional/mono metrics, monochrome clipping and corrupt packs");
    return 0;
}
