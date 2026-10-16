#include "../../kernel/include/api.h"
static uint8_t job[512];
static uint32_t size = 128, commands;
static void le16(uint8_t *p, unsigned value) {p[0]=value;p[1]=value>>8;}
static void le32(uint8_t *p, unsigned value) {for(unsigned i=0;i<4;i++)p[i]=value>>(i*8);}
static uint8_t *add(unsigned type, unsigned bytes) {
    uint8_t *p=job+size;le16(p,type);le32(p+4,bytes);size+=8+bytes;commands++;return p+8;
}
static uint32_t be(const uint8_t *p) {return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    job[0]='B';job[1]='P';job[2]='J';job[3]='1';le16(job+4,1);le16(job+6,128);
    const char *profile="ZJSTEST";for(unsigned i=0;profile[i];i++)job[88+i]=profile[i];
    uint8_t *p=add(1,12);le32(p,595);le32(p+4,842);le32(p+8,300);
    p=add(3,13);le32(p,10);le32(p+4,10);le32(p+8,1);p[12]='A';
    p=add(5,29);le32(p,72);le32(p+4,72);le32(p+8,1);le32(p+12,1);le32(p+16,1);le32(p+20,1);le16(p+24,300);p[28]=0x80;
    add(6,0);le32(job+8,size);le32(job+16,1);le32(job+20,commands);
    bool published=bk_file_write_all("/TEMP/SPOOL/ZJSTEST.BPJ",job,size),ok=false;
    for(unsigned attempt=0;published && attempt<100;attempt++) {
        void *raw=NULL;uint32_t bytes=0;
        if(bk_file_read_all("/TEMP/PRINT/ZJSTEST.ZJS",&raw,&bytes)) {
            uint8_t *data=raw;uint32_t at=0;
            while(at+4<bytes && !(data[at]=='J' && data[at+1]=='Z' && data[at+2]=='J' && data[at+3]=='Z'))at++;
            at+=4;unsigned pages=0,bih=0,bid=0;bool ended=false;
            while(at+16<=bytes) {
                uint32_t length=be(data+at),type=be(data+at+4);
                if(length<16 || length>bytes-at || data[at+14]!=0x5a || data[at+15]!=0x5a)break;
                if(type==2)pages++;if(type==4)bih++;if(type==5)bid++;if(type==1){ended=true;break;}at+=length;
            }
            ok=pages==1 && bih==1 && bid>0 && ended;
            kprintf("[DRIVERTEST] Ring 3 PrintSpool ZJS bytes=%u pages=%u BIH=%u BID=%u end=%u\n",bytes,pages,bih,bid,ended);
            bk_sys_free(raw);break;
        }
        bk_sys_sleep_ms(200);
    }
    kprintf("[DRIVERTEST] DONE result=%s\n",published && ok?"PASS":"FAIL");
}
