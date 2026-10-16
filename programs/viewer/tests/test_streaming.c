#include "bleskernos_api.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "../lib/video.h"
static FILE *files[32];
int bk_file_open(const char *path,uint32_t flags){(void)flags;for(int i=0;i<32;i++)if(!files[i]){files[i]=fopen(path,"rb"); return files[i]?i:-1;}return -1;}
int bk_file_read(int fd,void *p,uint32_t n){if(fd<0||fd>=32||!files[fd])return -1;return (int)fread(p,1,n,files[fd]);}
int32_t bk_file_seek(int fd,int32_t pos,uint32_t whence){return fseek(files[fd],pos,(int)whence)==0?(int32_t)ftell(files[fd]):-1;}
int32_t bk_file_tell(int fd){return (int32_t)ftell(files[fd]);}
int32_t bk_file_size(int fd){long cur=ftell(files[fd]); fseek(files[fd],0,SEEK_END);long end=ftell(files[fd]);fseek(files[fd],cur,SEEK_SET);return (int32_t)end;}
bool bk_file_close(int fd){if(fd<0||fd>=32||!files[fd])return false; fclose(files[fd]);files[fd]=NULL;return true;}
void *bk_sys_alloc(size_t size){return malloc(size);}
void bk_sys_free(void *p){free(p);}
bool bk_gui_image_decode_jpeg(bk_gui_image_t *i,const void *b,uint32_t n){(void)i;(void)b;(void)n;return false;}
void bk_gui_image_free(bk_gui_image_t *i){if(i->pixels)free(i->pixels);memset(i,0,sizeof(*i));}
int main(int argc,char **argv){
  if(argc!=3)return 2;
  vv_video_t m,a;
  memset(&m,0,sizeof(m)); memset(&a,0,sizeof(a));
  int f=bk_file_open(argv[1],1); assert(f>=0);int n=bk_file_size(f);bk_file_close(f);
  printf("MPEG %d bytes\n",n);
  assert(vv_open_mpeg_stream(&m,argv[1],n));
  printf("MPEG %dx%d frames=%u rate=%u/%u audio=%d\n",m.width,m.height,m.frame_count,m.fps_num,m.fps_den,(int)vv_audio_available(&m));
  assert(m.frame_count>3);
  for(uint32_t j=0;j<3;j++){assert(vv_prepare_frame(&m,j));uint16_t color=vv_sample_rgb565(&m,0,0);printf("  frame %u color=%x\n",j,color);}
  assert(vv_prepare_frame(&m,0));
  uint8_t audio[1024];uint32_t c=0,o=0; vv_audio_seek_ms(&m,0,&c,&o);printf("  audio decoded %u\n",vv_audio_decode_u8_mono(&m,&c,&o,audio,1024));
  vv_close(&m);
  f=bk_file_open(argv[2],1);assert(f>=0);n=bk_file_size(f);bk_file_close(f);
  printf("AVI %d bytes\n",n);
  assert(vv_open_avi_stream(&a,argv[2],n));
  printf("AVI %dx%d frames=%u bpp=%u audio=%d\n",a.width,a.height,a.frame_count,a.avi.bpp,(int)vv_audio_available(&a));
  assert(a.frame_count>1);
  assert(vv_prepare_frame(&a,0));
  printf(" AVI first sample=%x\n",vv_sample_rgb565(&a,20,20));
  assert(vv_prepare_frame(&a,1));
  printf(" AVI next sample=%x\n",vv_sample_rgb565(&a,20,20));
  if (vv_audio_available(&a)) {
    uint32_t ac=0,ao=0;
    vv_audio_seek_ms(&a,0,&ac,&ao);
    uint32_t got=vv_audio_decode_u8_mono(&a,&ac,&ao,audio,sizeof(audio));
    printf(" AVI audio decoded %u\n",got);
    assert(got==sizeof(audio));
  }
  vv_close(&a);
  puts("PASS");return 0;
}
