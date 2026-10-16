#ifndef VIEWER_VIDEO_INTERNAL_H
#define VIEWER_VIDEO_INTERNAL_H

#include "video.h"

uint16_t vv_rd16(const uint8_t *p);
uint32_t vv_rd32(const uint8_t *p);
int32_t vv_rd32s(const uint8_t *p);
bool vv_range_ok(uint32_t offset, uint32_t length, uint32_t size);
uint16_t vv_xrgb_to_rgb565(uint32_t color);
uint32_t vv_rgb565_to_xrgb(uint16_t color);
uint32_t vv_muldiv_u32_sat(uint32_t a, uint32_t b, uint32_t divisor);
uint16_t vv_bgr_to_rgb565(uint8_t b, uint8_t g, uint8_t r);
void vv_memzero(void *pointer, uint32_t size);
void vv_memcpy(void *dst, const void *src, uint32_t size);

bool vv_bkv_open(vv_video_t *video);
void vv_bkv_close(vv_video_t *video);
bool vv_bkv_prepare(vv_video_t *video, uint32_t frame);
uint16_t vv_bkv_sample(const vv_video_t *video, uint32_t x, uint32_t y);

bool vv_avi_open(vv_video_t *video);
bool vv_avi_open_stream(vv_video_t *video, const char *path, uint32_t size);
void vv_avi_close(vv_video_t *video);
bool vv_avi_prepare(vv_video_t *video, uint32_t frame);
uint16_t vv_avi_sample(const vv_video_t *video, uint32_t x, uint32_t y);
void vv_avi_audio_seek_ms(const vv_video_t *video, uint32_t time_ms,
                          uint32_t *chunk_index, uint32_t *chunk_offset);
uint32_t vv_avi_audio_decode_u8_mono(vv_video_t *video,
                                     uint32_t *chunk_index,
                                     uint32_t *chunk_offset,
                                     uint8_t *output, uint32_t capacity);

bool vv_gif_open(vv_video_t *video);
void vv_gif_close(vv_video_t *video);
bool vv_gif_prepare(vv_video_t *video, uint32_t frame);
uint16_t vv_gif_sample(const vv_video_t *video, uint32_t x, uint32_t y);
uint32_t vv_gif_duration_ms(const vv_video_t *video, uint32_t frame);

bool vv_mpeg_open(vv_video_t *video);
bool vv_mpeg_open_stream(vv_video_t *video, const char *path);
void vv_mpeg_close(vv_video_t *video);
bool vv_mpeg_prepare(vv_video_t *video, uint32_t frame);
uint16_t vv_mpeg_sample(const vv_video_t *video, uint32_t x, uint32_t y);
const bk_gui_image_t *vv_mpeg_frame_image(vv_video_t *video);
bool vv_mpeg_audio_available(const vv_video_t *video);
void vv_mpeg_audio_seek_ms(vv_video_t *video, uint32_t time_ms);
uint32_t vv_mpeg_audio_decode_u8_mono(vv_video_t *video,
                                      uint8_t *output, uint32_t capacity);

#endif
