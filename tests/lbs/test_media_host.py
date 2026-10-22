#!/usr/bin/env python3
"""Run the canonical codecs with real file callbacks before the Ring 3 tests."""
import subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)
 (p/'bleskernos_api.h').write_text('''#ifndef MEDIA_HOST_API
#define MEDIA_HOST_API
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define BK_FILE_READ_ONLY 1
#define BK_PATH_MAX 260
typedef struct {uint32_t *pixels;uint16_t width,height;} bk_gui_image_t;
void *bk_sys_alloc(size_t);void bk_sys_free(void*);
int bk_file_open(const char*,uint32_t);bool bk_file_close(int);
int bk_file_read(int,void*,uint32_t);int32_t bk_file_seek(int,int32_t,uint32_t);int32_t bk_file_tell(int);
bool bk_gui_image_decode_jpeg(bk_gui_image_t*,const void*,uint32_t);void bk_gui_image_free(bk_gui_image_t*);
#endif
''')
 (p/'test.c').write_text('''#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include "video.h"
void *bk_sys_alloc(size_t n){return malloc(n);}void bk_sys_free(void*p){free(p);}
int bk_file_open(const char*p,uint32_t f){(void)f;return open(p,O_RDONLY);}
bool bk_file_close(int fd){return close(fd)==0;}
int bk_file_read(int fd,void*p,uint32_t n){return read(fd,p,n);}
int32_t bk_file_seek(int fd,int32_t n,uint32_t whence){return lseek(fd,n,whence);}
int32_t bk_file_tell(int fd){return lseek(fd,0,SEEK_CUR);}
bool bk_gui_image_decode_jpeg(bk_gui_image_t*i,const void*p,uint32_t n){(void)i;(void)p;(void)n;return false;}
void bk_gui_image_free(bk_gui_image_t*i){free(i->pixels);i->pixels=0;}
int main(int argc,char**argv){
 assert(argc==3);
 for(int i=1;i<argc;i++){
  struct stat s;assert(!stat(argv[i],&s));vv_video_t v={0};
  bool ok=i==1?vv_open_avi_stream(&v,argv[i],s.st_size):vv_open_mpeg_stream(&v,argv[i],s.st_size);
  fprintf(stderr,"stream=%d open=%d frames=%u size=%u\\n",i,ok,v.frame_count,v.size);
  assert(ok);assert(vv_prepare_frame(&v,0));assert(vv_prepare_frame(&v,1));assert(vv_prepare_frame(&v,0));
  if(i==2){uint32_t c=0,o=0;uint8_t pcm[256];vv_audio_seek_ms(&v,0,&c,&o);assert(vv_audio_available(&v));assert(vv_audio_decode_u8_mono(&v,&c,&o,pcm,sizeof(pcm))>0);}
  vv_close(&v);
 }
 puts("PASS: canonical AVI/MPEG streaming, frame seek and decoded MP2 audio");
}
''')

 sources=[ROOT/'libs/native/media'/n for n in ['video.c','avi.c','bkv.c','gif.c','mpeg.c','pl_mpeg_bles.c']]
 subprocess.run(['gcc','-m32','-O2','-g','-DPLM_NO_STDIO=1','-I'+str(p),'-I'+str(ROOT/'libs/native/media'),*map(str,sources),str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
 fixtures=p
 subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i','color=c=blue:s=32x32:r=2','-t','1','-c:v','rawvideo','-pix_fmt','bgr24','-threads','1',str(p/'STREAM.AVI')],check=True)
 subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i','color=c=red:s=32x32:r=25','-f','lavfi','-i','sine=frequency=440:sample_rate=44100','-t','0.4','-c:v','mpeg1video','-c:a','mp2','-threads','1','-f','mpeg',str(p/'STREAM.MPG')],check=True)
 subprocess.run([str(p/'test'),str(fixtures/'STREAM.AVI'),str(fixtures/'STREAM.MPG')],check=True)
