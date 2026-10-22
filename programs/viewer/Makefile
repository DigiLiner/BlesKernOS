# Viewer client: decoding and rendering are provided by MEDIA.LBS.
ROOT ?= ../..
CC := gcc
LD := ld
OUT_DIR := build
CFLAGS := -m32 -march=i486 -mtune=pentium -mfpmath=387 -mno-sse -mno-sse2 -ffreestanding -fno-builtin -nostdlib -nostdinc -Os -Wall -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -I$(ROOT)/sdk/include -I.
.PHONY: all clean
all: $(OUT_DIR)/VIEWER.O
$(OUT_DIR):
	mkdir -p $@
$(OUT_DIR)/viewer.raw.o: viewer.c $(ROOT)/sdk/include/bleskernos_media_lbs.h $(ROOT)/sdk/include/bleskernos_widgets_lbs.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT_DIR)/VIEWER.O: $(OUT_DIR)/viewer.raw.o Makefile
	$(LD) -m elf_i386 -r $< -o $@
# Install build/lbs/apps/programs/viewer.o from the root build; this is the
# intermediate object, with unresolved LBS calls, never an installation payload.
clean:
	rm -rf $(OUT_DIR)
