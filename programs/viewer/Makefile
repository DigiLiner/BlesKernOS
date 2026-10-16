# Viewer 3.2 - default BlesKernOS multimedia player
# Build from the BlesKernOS repository root with:
#   make -C "programs/viewer" ROOT=../..

ROOT ?= ../..
CC   ?= gcc
LD   ?= ld
AR   ?= ar

SDK_INCLUDE := $(ROOT)/sdk/include
OUT_DIR := build
PLM_HEADER := third_party/pl_mpeg.h
PLM_COMMIT := c871f2be022ece7ef4f64230b4fb8e1fb9eb6023

CFLAGS := -m32 -march=i486 -mtune=pentium -mfpmath=387 -mno-sse -mno-sse2 \
          -ffreestanding -fno-builtin -nostdlib -nostdinc -Os -Wall \
          -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
          -DPLM_NO_STDIO=1 -I$(SDK_INCLUDE) -I.

RAW_OBJ := $(OUT_DIR)/viewer.raw.o
LIB_OBJS := \
	$(OUT_DIR)/video.o \
	$(OUT_DIR)/bkv.o \
	$(OUT_DIR)/avi.o \
	$(OUT_DIR)/gif.o \
	$(OUT_DIR)/mpeg.o \
	$(OUT_DIR)/pl_mpeg_bles.o \
	$(OUT_DIR)/render.o \
	$(OUT_DIR)/retro_ui.o
LIB := $(OUT_DIR)/libviewer.a
TARGET := $(OUT_DIR)/VIEWER.O

.PHONY: all clean info

all: $(TARGET)
	@echo "[OK] Viewer 3.2: $(TARGET)"
	@echo "     Formats: streaming MPEG-1 PS+MP2 and AVI MJPEG/DIB+PCM; BKV, GIF"
	@echo "     Copy VIEWER.O to the programs directory of your BlesKernOS image."

info:
	@echo "Viewer is a default application built by the root Makefile."

$(OUT_DIR):
	@mkdir -p $@

$(RAW_OBJ): viewer.c lib/video.h lib/render.h lib/retro_ui.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/video.o: lib/video.c lib/video.h lib/video_internal.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/bkv.o: lib/bkv.c lib/video.h lib/video_internal.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/avi.o: lib/avi.c lib/video.h lib/video_internal.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/gif.o: lib/gif.c lib/video.h lib/video_internal.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Only download if the pinned header is absent. An already-vendored copy
# must not be re-downloaded just because files were copied with new mtimes.
$(PLM_HEADER):
	@echo "[Viewer] Fetching pinned pl_mpeg $(PLM_COMMIT)"
	python3 tools/fetch_pl_mpeg.py

$(OUT_DIR)/mpeg.o: lib/mpeg.c lib/video.h lib/video_internal.h $(PLM_HEADER) $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -Ithird_party/compat -c $< -o $@

$(OUT_DIR)/pl_mpeg_bles.o: lib/pl_mpeg_bles.c $(PLM_HEADER) third_party/compat/stdint.h third_party/compat/string.h third_party/compat/stdlib.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -O2 -Ithird_party/compat -c $< -o $@

$(OUT_DIR)/render.o: lib/render.c lib/render.h lib/video.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/retro_ui.o: lib/retro_ui.c lib/retro_ui.h $(SDK_INCLUDE)/bleskernos_api.h $(SDK_INCLUDE)/bleskernos_theme.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(TARGET): $(RAW_OBJ) $(LIB)
	$(LD) -m elf_i386 -r $(RAW_OBJ) $(LIB) -o $@

clean:
	rm -rf $(OUT_DIR)
