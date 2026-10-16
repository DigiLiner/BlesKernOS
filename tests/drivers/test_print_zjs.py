#!/usr/bin/env python3
"""Execute production BPJ renderers, parse ZJS and decode real JBIG pixels."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'system/services/printspool.c').read_text()
s=s[:s.index('static void stem_from_name(')]
s=s.replace('#include <bleskernos_api.h>','').replace('#include <bleskernos_print.h>','').replace('static bk_file_entry_t *g_spool_scan_entries;','')
s=s.replace('#include "print_zjs.inc"',(ROOT/'system/services/print_zjs.inc').read_text().replace('../../',str(ROOT)+'/'))
s=s.replace('static bool profile_auto_detect(printer_profile_t *profile);','static bool profile_auto_detect(printer_profile_t *profile) { (void)profile; return false; }')
header=(ROOT/'sdk/include/bleskernos_print.h').read_text()
header=header[header.index('#define BK_PRINT_API_VERSION'):header.index('typedef struct bk_print_job')]
prelude=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define NORETURN __attribute__((noreturn))
static void bk_sys_log(const char *s){fputs(s,stderr);}
static void bk_proc_exit(void) __attribute__((noreturn));
static void bk_proc_exit(void){exit(1);}
#define BK_PATH_MAX 256
#define bk_sys_alloc malloc
#define bk_sys_free free
static bool bk_file_read_all(const char *p,void **data,uint32_t *size){
 FILE *f=fopen(p,"rb");if(!f)return false;fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);*data=malloc(*size);bool ok=fread(*data,1,*size,f)==*size;fclose(f);return ok;
}
'''
tests=r'''
static unsigned count,size=128;static uint8_t job[2048];
static void le16(uint8_t *p,unsigned x){p[0]=x;p[1]=x>>8;}
static void le32(uint8_t *p,unsigned x){for(unsigned i=0;i<4;i++)p[i]=x>>(8*i);}
static uint8_t *add(unsigned type,unsigned payload){uint8_t *p=job+size;le16(p,type);le32(p+4,payload);size+=8+payload;count++;return p+8;}
static void begin(void){memset(job,0,sizeof(job));size=128;count=0;memcpy(job,"BPJ1",4);le16(job+4,1);le16(job+6,128);uint8_t *p=add(1,12);le32(p,595);le32(p+4,842);le32(p+8,300);}
static void finish(void){add(6,0);le32(job+8,size);le32(job+16,1);le32(job+20,count);}
static uint32_t be(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void test_decode(output_buffer_t *out){
 unsigned start=0;while(start+4<=out->size && memcmp(out->data+start,"JZJZ",4))start++;assert(start+4<out->size);start+=4;
 output_buffer_t encoded={0};unsigned records=0,pages=0;
 for(unsigned at=start;at+16<=out->size;){
  uint8_t *p=out->data+at;uint32_t len=be(p),type=be(p+4),items=be(p+8);assert(len>=16 && len<=out->size-at);assert(p[14]==0x5a && p[15]==0x5a);
  if(type==0){assert(items==3 && len==52 && p[13]==0x24);}
  if(type==2){pages++;assert(items==13 && len==172 && p[13]==0x9c);for(unsigned i=0;i<items;i++){assert(be(p+16+12*i)==12);assert(p[22+12*i]==1);}}
  if(type==4 || type==5)assert(output_append(&encoded,p+16,len-16));
  records++;at+=len;if(type==1)break;
 }
 assert(records>=7 && pages==1 && encoded.size>=20);
 struct jbg_dec_state decoder;jbg_dec_init(&decoder);size_t used=0;
 int result=jbg_dec_in(&decoder,encoded.data,encoded.size,&used);assert(result==JBG_EOK);
 assert(jbg_dec_getwidth(&decoder)==4992 && jbg_dec_getheight(&decoder)==7016);
 uint8_t *bits=jbg_dec_getimage(&decoder,0);size_t stride=4992/8;
 /* A 1-bit image positioned at 72 points (600 pixels), enlarged from 300 dpi. */
 assert(bits[600*stride+600/8]&(0x80>>(600%8)));assert(bits[601*stride+601/8]&(0x80>>(601%8)));assert(!(bits[602*stride+602/8]&(0x80>>(602%8))));
 bool text=false;for(unsigned y=80;y<170;y++)for(unsigned x=80;x<180;x++)if(bits[y*stride+x/8]&(0x80>>(x%8)))text=true;assert(text);
 assert(bits[300*stride+300/8]&(0x80>>(300%8))); /* line at 36 points */
 jbg_dec_free(&decoder);output_free(&encoded);
}
int main(void){
 begin();uint8_t *p=add(2,8);le16(p,2);le16(p+2,12);
 p=add(3,13);le32(p,10);le32(p+4,10);le32(p+8,1);p[12]='A';
 p=add(4,20);le32(p,36);le32(p+4,36);le32(p+8,50);le32(p+12,36);le32(p+16,1);
 p=add(5,29);le32(p,72);le32(p+4,72);le32(p+8,1);le32(p+12,1);le32(p+16,1);le32(p+20,1);le16(p+24,300);p[28]=0x80;
 finish();output_buffer_t out={0};assert(render_zjs(job,size,&out));test_decode(&out);output_free(&out);
 printer_profile_t profile;profile_defaults(&profile,"TEST");profile_apply(&profile,"Language","ZJS");assert(profile.language==LANG_ZJS);
 job[8]=0;assert(!render_zjs(job,size,&out));output_free(&out);
 begin();p=add(3,13);le32(p+8,1);p[12]=0xc3;finish();assert(!render_zjs(job,size,&out));output_free(&out);
 begin();le32(job+136,999);finish();assert(!render_zjs(job,size,&out));output_free(&out);
 begin();p=add(5,29);le32(p+8,1);le32(p+12,1);le32(p+16,1);le32(p+20,0xffffffff);le16(p+24,300);finish();assert(!render_zjs(job,size,&out));output_free(&out);
 for(int language=LANG_TEXT;language<LANG_ZJS;language++){begin();finish();profile.language=language;assert(render_job(job,size,&profile,&out));output_free(&out);}
 puts("PASS: HP1020 ZJS/JBIG round-trip, text/line/bitmap raster, malformed BPJ, existing renderers");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';c.write_text(prelude+header+s+tests)
 exe=Path(tmp)/'test'
 subprocess.run(['gcc','-std=c11','-Dmemchr=bk_print_memchr','-Dabort=bk_print_jbig_abort','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DNDEBUG','-UNDEBUG',str(c),str(ROOT/'third_party/jbigkit/jbig.c'),str(ROOT/'third_party/jbigkit/jbig_ar.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
