#ifndef BLESKERNOS_APPLICATION_API_H
#define BLESKERNOS_APPLICATION_API_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct {uint16_t width,height; uint32_t *pixels;} bk_gui_image_t;
#define BK_FILE_READ_ONLY 1U
int bk_file_open(const char *, uint32_t);
int bk_file_read(int, void *,uint32_t);
int32_t bk_file_seek(int,int32_t,uint32_t);
int32_t bk_file_tell(int);
int32_t bk_file_size(int);
bool bk_file_close(int);
void *bk_sys_alloc(size_t);
void bk_sys_free(void *);
bool bk_gui_image_decode_jpeg(bk_gui_image_t *,const void *,uint32_t);
void bk_gui_image_free(bk_gui_image_t *);
#endif
