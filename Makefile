# =============================================================================
# BleskernOS Makefile
# =============================================================================

EDITION ?= user
VALID_EDITIONS := user developer
ifeq ($(filter $(EDITION),$(VALID_EDITIONS)),)
$(error EDITION debe ser user o developer)
endif

# Herramientas
NASM    := nasm
CC      := gcc
LD      := ld
AR      := ar
PYTHON  ?= python3
QEMU    ?= qemu-system-i386
QEMU_IMG ?= qemu-img
SELFHOST_DIR := build/selfhost/toolchain
SELFHOST_STAMP := build/selfhost/.stamp

# Flags
KERNEL_SIZE_OPT ?= -Oz
NASM_FLAGS := -f bin
CC_FLAGS   := -march=pentium3 -mtune=pentium3 -m32 -mfpmath=387 -mno-sse -mno-sse2 -ffreestanding -fno-builtin -nostdlib -nostdinc -Os -Wall -MMD -MP -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
LD_FLAGS   := -m elf_i386 -T kernel/arch/x86/linker.ld --gc-sections
# Conservar las APIs alcanzables desde las tablas de exportacion y eliminar
# funciones sin referencias. El limite fisico de 1 MiB sigue siendo estricto.
CC_FLAGS   += -ffunction-sections -fdata-sections
CC_FLAGS   += $(KERNEL_SIZE_OPT)
INCLUDE_FLAGS := -I. -Ikernel/libc/include -Ikernel -Iprograms -Ilibs/tinygl -Ilibs/src

# Archivos
BOOT1_SRC  := boot/boot.asm
BOOT1_CD_SRC := boot/boot.asm
BOOT1_FAT32_SRC := boot/boot_fat32.asm
MBR_SRC := boot/mbr.asm
BOOT2_SRC  := boot/stage2.asm
KERNEL_ENTRY_SRC := kernel/arch/x86/entry.asm
LIBC_SOURCES := \
	kernel/libc/string.c \
	kernel/libc/ctype.c \
	kernel/libc/stdlib.c \
	kernel/libc/math.c \
	kernel/libc/stdio.c \
	kernel/libc/gcc_runtime.c \
	kernel/libc/inflate.c
TINYGL_SOURCES := \
	libs/src/bleskernos_compat.c \
	libs/src/bleskernos_gpu.c \
	libs/src/api.c \
	libs/src/arrays.c \
	libs/src/clear.c \
	libs/src/clip.c \
	libs/src/get.c \
	libs/src/image_util.c \
	libs/src/init.c \
	libs/src/light.c \
	libs/src/list.c \
	libs/src/matrix.c \
	libs/src/memory.c \
	libs/src/misc.c \
	libs/src/msghandling.c \
	libs/src/select.c \
	libs/src/specbuf.c \
	libs/src/texture.c \
	libs/src/vertex.c \
	libs/src/zbuffer.c \
	libs/src/zline.c \
	libs/src/zmath.c \
	libs/src/zpostprocess.c \
	libs/src/zraster.c \
	libs/src/ztext.c \
	libs/src/ztriangle.c

# GNU Make sobre el FAT de BlesKernOS recibe de wildcard() el nombre tal como
# está almacenado en disco (mtools suele devolver los 8.3 en mayúsculas).  Las
# funciones de texto de Make siguen siendo case-sensitive aunque el filesystem
# sea case-insensitive, por lo que un SHELL32.C no coincide con %.c y terminaría
# entrando como fuente en lugar de convertirse a build/.../SHELL32.o.
# Normalizamos sólo el sufijo C devuelto por wildcard; las rutas explícitas ya
# funcionan mediante el lookup FAT case-insensitive.
c_wildcard = $(patsubst %.C,%.c,$(wildcard $(1)))

# Mesa 3.5 freestanding con backend híbrido OSMesa/GFX3D.  El core y TNL
# conservan el fallback SWRAST; las primitivas compatibles se envían al driver
# 3D activo. X11/GLX, SVGAlib, Glide y threads del host quedan desactivados.
MESA35_ROOT := libs/Mesa-3.5
MESA35_X86_SOURCES := \
	$(MESA35_ROOT)/src/X86/x86.c \
	$(MESA35_ROOT)/src/X86/common_x86.c \
	$(MESA35_ROOT)/src/X86/sse.c
MESA35_X86_ASM_SOURCES := \
	$(MESA35_ROOT)/src/X86/common_x86_asm.S \
	$(MESA35_ROOT)/src/X86/x86_vertex.S \
	$(MESA35_ROOT)/src/X86/x86_cliptest.S \
	$(MESA35_ROOT)/src/X86/x86_xform2.S \
	$(MESA35_ROOT)/src/X86/x86_xform3.S \
	$(MESA35_ROOT)/src/X86/x86_xform4.S \
	$(MESA35_ROOT)/src/X86/sse_vertex.S \
	$(MESA35_ROOT)/src/X86/sse_normal.S \
	$(MESA35_ROOT)/src/X86/sse_xform1.S \
	$(MESA35_ROOT)/src/X86/sse_xform2.S \
	$(MESA35_ROOT)/src/X86/sse_xform3.S \
	$(MESA35_ROOT)/src/X86/sse_xform4.S \
	$(MESA35_ROOT)/src/X86/mmx_blend.S

MESA35_SOURCES := \
	$(filter-out $(MESA35_ROOT)/src/config.c $(MESA35_ROOT)/src/CONFIG.c,$(call c_wildcard,$(MESA35_ROOT)/src/*.c)) \
	$(filter-out $(MESA35_ROOT)/src/math/m_debug_%.c $(MESA35_ROOT)/src/math/M_DEBUG_%.c,$(call c_wildcard,$(MESA35_ROOT)/src/math/*.c)) \
	$(call c_wildcard,$(MESA35_ROOT)/src/swrast/*.c) \
	$(call c_wildcard,$(MESA35_ROOT)/src/swrast_setup/*.c) \
	$(call c_wildcard,$(MESA35_ROOT)/src/tnl/*.c) \
	$(call c_wildcard,$(MESA35_ROOT)/src/array_cache/*.c) \
	$(call c_wildcard,$(MESA35_ROOT)/src/BlesKernOS/*.c) \
	$(MESA35_ROOT)/src/OSmesa/osmesa.c \
	$(MESA35_ROOT)/bleskernos/config_stub.c \
	$(MESA35_ROOT)/bleskernos/runtime.c \
	$(MESA35_X86_SOURCES)

KERNEL_SOURCES := \
	kernel/core/kernel.c \
	kernel/core/admin.c \
	kernel/setup/installer.c \
	kernel/setup/floppy_installer.c \
	kernel/setup/bkl_setup.c \
	kernel/setup/setup_boot.c \
	kernel/console/vga.c \
	kernel/setup/recovery_console.c \
	kernel/mm/paging.c \
	kernel/mm/swap.c \
	kernel/mm/pager.c \
	kernel/mm/usercopy.c \
	kernel/mm/memory.c \
	kernel/core/compat_mode.c \
	kernel/arch/x86/gdt.c \
	kernel/sched/task.c \
	kernel/sched/scheduler_smp.c \
	kernel/arch/x86/smp.c \
	kernel/arch/x86/acpi_pm.c \
	kernel/sched/smp_watchdog.c \
	kernel/sched/klock.c \
	kernel/sched/kernel_domains.c \
	kernel/sched/perfmon.c \
	kernel/api/syscall.c \
	kernel/api/native_memory.c \
	kernel/api/api.c \
	kernel/api/vision_ipc.c \
	kernel/api/vision_resources.c \
	kernel/ui/graphics_resources.c \
	kernel/ui/language.c \
	kernel/api/public_api.c \
	kernel/ui/clipboard.c \
	kernel/ui/file_dialog.c \
	kernel/arch/x86/usermode.c \
	kernel/arch/x86/pic.c \
	kernel/arch/x86/idt.c \
	kernel/core/panic.c \
	kernel/exec/elf_loader.c \
	kernel/exec/bex_loader.c \
	kernel/ui/about_dialog.c \
	kernel/ui/input_preferences.c \
	kernel/audio/sound_file.c \
	kernel/audio/system_sounds.c \
	kernel/audio/startup_sound.c \
	kernel/exec/driver_loader.c \
	kernel/net/network.c \
	kernel/ui/bootsplash.c \
	gui/gfx.c \
	gui/font.c \
	gui/real_font.c \
	gui/windows.c \
	gui/widget.c \
	gui/desktop.c \
	gui/compositor.c \
	gui/gpu_compositor.c \
	gui/event.c \
	gui/gui.c \
	gui/image.c \
	gui/svg.c \
	gui/jpeg.c \
	programs/shell.c \
	system/desktop/vision_bridge.c \
	system/services/screensaverd.c \
	programs/launcher.c \
	kernel/drivers/core/block.c \
	kernel/drivers/platform/lpt.c \
	kernel/drivers/storage/floppy.c \
	kernel/drivers/storage/usb_storage.c \
	kernel/drivers/usb/usb_uhci.c \
	kernel/drivers/usb/usb_ohci.c \
	kernel/drivers/core/vfs.c \
	kernel/drivers/core/pci.c \
	kernel/drivers/core/gfx.c \
	kernel/drivers/core/gfx3d.c \
	kernel/drivers/core/sdr.c \
	kernel/drivers/core/rage128_engine.c \
	kernel/drivers/graphics/svga_transport.c \
	kernel/drivers/graphics/vga.c \
	kernel/drivers/graphics/vesa_core.c \
	kernel/drivers/input/mouse_core.c \
	kernel/drivers/storage/ata.c \
	kernel/drivers/storage/ahci.c \
	kernel/drivers/core/pit.c \
	kernel/drivers/audio/sound_core.c \
	kernel/drivers/platform/rtc_core.c \
	kernel/drivers/input/keyboard.c \
	kernel/drivers/storage/iso9660_core.c \
	kernel/drivers/storage/iso9660.c \
	kernel/drivers/storage/fat.c \
	kernel/drivers/storage/bfs.c \
	kernel/drivers/storage/bfs4.c
KERNEL_ASM_SOURCES := kernel/arch/x86/isr_stubs.asm kernel/arch/x86/api_call.asm

BOOT1_BIN  := build/boot.bin
BOOT1_CD_BIN := build/boot_cd.bin
BOOT1_FAT32_BIN := build/boot_fat32.bin
MBR_BIN := build/mbr.bin
BOOT2_BIN  := build/stage2.bin
KERNEL_ENTRY_OBJ := build/entry.o
KERNEL_OBJS := $(patsubst %.c,build/%.o,$(KERNEL_SOURCES))
VISION_IMAGE_COMPAT_OBJ := build/system/desktop/vision_image_compat.o
KERNEL_OBJS += $(VISION_IMAGE_COMPAT_OBJ)
KERNEL_ASM_OBJS := build/isr_stubs.o build/api_call.o
KERNEL_GAS_OBJS := build/kernel/arch/x86/ap_trampoline.o
KERNEL_ELF := build/kernel.elf
KERNEL_BIN := build/kernel.bin
LIBC_OBJS := $(patsubst %.c,build/%.o,$(LIBC_SOURCES))
TINYGL_OBJS := $(patsubst %.c,build/%.o,$(TINYGL_SOURCES))
MESA35_C_OBJS := $(patsubst %.c,build/%.o,$(MESA35_SOURCES))
MESA35_ASM_OBJS := $(patsubst %.S,build/%.o,$(MESA35_X86_ASM_SOURCES))
MESA35_OBJS := $(MESA35_C_OBJS) $(MESA35_ASM_OBJS)
LIBC_A := build/system/libs/libc/libc.a
TINYGL_A := build/system/libs/tinygl/tinygl.a
MESA35_A := build/system/libs/mesa35/mesa35.a
SDK_CFLAGS := -m32 -march=pentium3 -mtune=pentium3 -mfpmath=387 -mno-sse -mno-sse2 -ffreestanding -fno-builtin -nostdlib -nostdinc -Os -Wall -MMD -MP -fno-pic -fno-pie -fno-stack-protector -Isdk/include
MESA35_CPU_FLAGS := -march=pentium3 -mtune=pentium3 -msse -mno-sse2 -mfpmath=sse,387
MESA35_CFLAGS := $(filter-out -Os -march=pentium3 -mtune=pentium3 -mfpmath=387 -mno-sse -mno-sse2,$(CC_FLAGS)) \
	$(MESA35_CPU_FLAGS) -O2 -fomit-frame-pointer -fno-strict-aliasing \
	-I$(MESA35_ROOT)/bleskernos/include \
	-I$(MESA35_ROOT) -I$(MESA35_ROOT)/include -I$(MESA35_ROOT)/src \
	-I$(MESA35_ROOT)/src/math -I$(MESA35_ROOT)/src/swrast \
	-I$(MESA35_ROOT)/src/swrast_setup -I$(MESA35_ROOT)/src/tnl \
	-I$(MESA35_ROOT)/src/array_cache -I$(MESA35_ROOT)/src/BlesKernOS \
	-DHAVE_CONFIG_H -DMESA_BLESKERNOS=1 -DUSE_SSE_ASM -DUSE_MMX_ASM -U__linux__ -Ulinux
SCREENSAVER_MESA_INCLUDES := -I$(MESA35_ROOT)/include -I$(MESA35_ROOT)/bleskernos/include
SDK_SYSCALL_OBJ := build/sdk/syscalls.o
SDK_THREAD_OBJ := build/sdk/thread.o
SDK_PRINT_OBJ := build/sdk/print.o
SDK_POSIX_OBJ := build/sdk/posix.o
SDK_POSIX_STRING_OBJ := build/sdk/posix_string.o
SDK_POSIX_STDLIB_OBJ := build/sdk/posix_stdlib.o
SDK_POSIX_STDIO_OBJ := build/sdk/posix_stdio.o
SDK_POSIX_MMAN_OBJ := build/sdk/posix_mman.o
SDK_POSIX_PTHREAD_OBJ := build/sdk/posix_pthread.o
SDK_POSIX_NET_OBJ := build/sdk/posix_net.o
SDK_POSIX_CTYPE_OBJ := build/sdk/posix_ctype.o
SDK_POSIX_IPC_OBJ := build/sdk/posix_ipc.o
SDK_POSIX_COMPAT_OBJ := build/sdk/posix_compat.o
SDK_POSIX_SPAWN_OBJ := build/sdk/posix_spawn.o
SDK_POSIX_SIGNAL_OBJ := build/sdk/posix_signal.o
SDK_POSIX_MAIN_OBJ := build/sdk/posix_main.o
SDK_XLIB_OBJ := build/sdk/xlib.o
SDK_POSIX_DLFCN_OBJ := build/sdk/posix_dlfcn.o
SDK_POSIX_SETJMP_OBJ := build/sdk/posix_setjmp.o
SDK_LINUX_MAIN_OBJ := build/sdk/linux_main.o
SDK_LINUX_RUNTIME_OBJ := build/sdk/linux_runtime.o
SDK_POSIX_MATH_OBJ := build/sdk/posix_math.o
SDK_TINYGL_OBJ := build/sdk/tinygl.o
SDK_MESA_OBJ := build/sdk/mesa.o
SDK_SDL2_OBJ := build/sdk/sdl2.o
SDK_SDL_MAIN_OBJ := build/sdk/sdl_main.o
SDK_SDL_EXTRA_OBJ := build/sdk/sdl_extra.o
SDK_A := build/sdk/libblesk.a
SDK_POSIX_A := build/sdk/libblesk_posix.a
SDK_LINUX_A := build/sdk/libblesk_linux.a
SDK_TINYGL_A := build/sdk/libblesk_tinygl.a
SDK_MESA_A := build/sdk/libblesk_mesa.a
SDK_SDL2_A := build/sdk/libblesk_sdl2.a
SDK_MATH_A := build/sdk/libm.a

# Tux Racer 0.61: port Ring 3 SDL/POSIX + Mesa/GLU + Jim Tcl. Los objetos se
# aplanan bajo build/ para mantener nombres de objetos estables.
TUXRACER_ROOT := extras/TuxRacer-BlesKernOS-Standalone
TUXRACER_ROOT_ESC := $(TUXRACER_ROOT)
TUXRACER_C_NAMES := alglib audio audio_data button course_load course_mgr \
	course_render credits debug error_util event_select file_util fog fonts fps \
	game_config game_logic_util game_over game_type_select gauss gl_util hash hier \
	hier_cb hier_util hud image intro joystick keyboard keyboard_util keyframe \
	lights list listbox loading loop main mirror_course multiplayer nmrcl os_util \
	part_sys paused phys_sim pic preview race_select racing render_util reset save \
	screenshot splash_screen ssbutton string_util tcl_util tex_font_metrics \
	textarea textures track_marks tux tux_shadow ui_mgr ui_snow ui_theme view \
	viewfrustum
TUXRACER_CXX_NAMES := course_quad quadgeom quadtree
TUXRACER_JIM_NAMES := _load-static-exts jim-subcmd jim-format jim utf8 jimregexp \
	jimiocompat jim-nosignal jim-file jim-package jim-readdir jim-regexp _glob \
	_stdlib _tclcompat
TUXRACER_C_OBJS := $(addprefix build/programs/tuxracer/src/,$(addsuffix .o,$(TUXRACER_C_NAMES)))
TUXRACER_CXX_OBJS := $(addprefix build/programs/tuxracer/src/,$(addsuffix .o,$(TUXRACER_CXX_NAMES)))
TUXRACER_PLATFORM_OBJS := build/programs/tuxracer/platform/blesk_app.o \
	build/programs/tuxracer/platform/blesk_glu.o \
	build/programs/tuxracer/platform/blesk_winsys.o \
	build/programs/tuxracer/platform/cxx_runtime.o \
	build/programs/tuxracer/platform/tcl_compat.o
TUXRACER_JIM_OBJS := $(addprefix build/programs/tuxracer/jim/,$(addsuffix .o,$(TUXRACER_JIM_NAMES)))
TUXRACER_JIM_A := build/programs/tuxracer/libjim.a
TUXRACER_OBJ := build/programs/tuxracer.o
TUXRACER_PORT_CFLAGS := -m32 -march=pentium3 -mtune=pentium3 -mfpmath=sse,387 \
	-msse -mno-sse2 -ffreestanding -fno-builtin -nostdlib -nostdinc -O2 \
	-fomit-frame-pointer -fno-strict-aliasing -Wall -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -DHAVE_CONFIG_H \
	-D__BLESKERNOS__=1 -I"$(TUXRACER_ROOT)/blesk/include" \
	-I"$(TUXRACER_ROOT)/dc/include" -I"$(TUXRACER_ROOT)/src" \
	-I"$(TUXRACER_ROOT)/blesk/third_party" \
	-I"$(TUXRACER_ROOT)/blesk/third_party/jimtcl" -Isdk/include \
	-Ilibs/Mesa-3.5/include -Ilibs/Mesa-3.5/si-glu/include
BEARSSL_SOURCES := $(call c_wildcard,libs/bearssl/src/*.c libs/bearssl/src/*/*.c)
BEARSSL_OBJS := $(patsubst %.c,build/%.o,$(BEARSSL_SOURCES))
BEARSSL_A := build/system/libs/bearssl/libbearssl.a

# Conservado sólo para que reglas antiguas puedan limpiarlo; `all` ya no lo
# construye ni inicializa el controlador de disquete.
DISK_IMG   := build/bleskernos-$(EDITION).img
ATA_IMG    := build/bleskernos-ata-$(EDITION).img
USB_IMG    := build/bleskernos-usb-$(EDITION).img
VMWARE_IMG := build/bleskernos-vmware-$(EDITION).vmdk
VMWARE_VMX := build/bleskernos-vmware-$(EDITION).vmx
# Reserva máxima para el kernel crudo desde LBA 9 y para la FAT32.
# reservado al inicio de la imagen FAT32 para el kernel crudo.
# Reserva máxima de 1280 KiB; Stage 2 carga solamente los sectores reales.
KERNEL_STAGE2_SECTORS := 2560
KERNEL_LOAD_SECTORS = 1280
# Evita depender de `expr`/$(shell) al analizar este archivo dentro del OS.
FAT_RESERVED_SECTORS := 2569
QEMU_AUDIO_ARGS ?= -device sb16
EXTRA_PROGRAMS_IMG := tests/fixtures/disk-images/programas-extra.img
EXTRA_CODEX_OBJ := extras/Bles-Codex32-standalone/build/CODEX.O
EXTRA_HYPERZIP_OBJ := extras/HyperZip-BlesKernOS-Standalone/build/HYPERZIP.O
VIEWER_OBJ := build/programs/viewer.o
EXTRA_LEXINET_OBJ := extras/LexiNet/build/LEXINET.O
EXTRA_CIV2_OBJ := extras/Civ2-BlesKernOS-Standalone/build/CIV2.O

ifdef CDROM_IMG
QEMU_CDROM_ARGS := -drive file=$(CDROM_IMG),media=cdrom,if=ide,readonly=on
else
QEMU_CDROM_ARGS :=
endif

# Layout del floppy FAT12:
# Sector 0       = Stage 1 + BPB FAT12
# Sectores 1-4   = Stage 2
# Sectores 9..(8 + KERNEL_STAGE2_SECTORS) = Kernel crudo reservado para stage2
# 1152 sectores cargan desde 0x10000 hasta 0xA0000 (limite antes de VGA).
# Sectores siguientes                    = FAT12 + root dir + data
#
# Estructura de la imagen principal FAT32:
#   /SYSTEM/PROGRAMS/*.O     ← aplicaciones nativas
#   /SYSTEM/CORE/*.O         ← componentes centrales del escritorio
#   /OPT/WINE               ← runtime Windows de Wine

.PHONY: all user developer developer-fast editions clean run run-svga run-net run-ac97 run-usb run-uhci build sdk selfhost source mesa35 netsurf extra-programs-image graphics-pak vmware-image FORCE

# Los proyectos de "extras" son repositorios independientes. No
# deben bloquear la compilación normal del SO; su imagen se genera sólo con
# `make extra-programs-image` cuando se necesita para una prueba concreta.
all: $(ATA_IMG) $(USB_IMG) $(VMWARE_IMG)

user:
	$(MAKE) EDITION=user all

developer:
	$(MAKE) EDITION=developer all

# Iteracion rapida para cambios de kernel durante desarrollo.  La imagen
# developer completa contiene el toolchain self-host y reconstruirla desde
# cero puede disparar una compilacion de Binutils innecesaria.  Este target
# conserva el filesystem existente y reemplaza solamente Stage 2 + el kernel
# crudo en sus sectores reservados.  Stage 2 se vuelve a ensamblar porque
# contiene el numero real de sectores y checksum del kernel.
developer-fast:
	@if [ ! -f build/bleskernos-ata-developer.img ]; then \
		if [ -f build/bleskernos-ata-user.img ]; then \
			echo "[FAST] No existe imagen developer; usando la ATA user existente como base de prueba..."; \
			cp -f build/bleskernos-ata-user.img build/bleskernos-ata-developer.img; \
		else \
			echo "[ERROR] No existe ni build/bleskernos-ata-developer.img ni build/bleskernos-ata-user.img."; \
			echo "[ERROR] developer-fast necesita una imagen ATA base ya creada."; \
			exit 2; \
		fi; \
	fi
	$(MAKE) EDITION=developer $(BOOT2_BIN)
	@KERNEL_SECTORS=$$(( ($$(wc -c < $(KERNEL_BIN)) + 511) / 512 )); \
	if [ $$KERNEL_SECTORS -gt $(KERNEL_STAGE2_SECTORS) ]; then \
		echo "[ERROR] Kernel ocupa $$KERNEL_SECTORS sectores, maximo $(KERNEL_STAGE2_SECTORS)."; \
		exit 1; \
	fi; \
	echo "[FAST] Actualizando Stage 2 + kernel ($$KERNEL_SECTORS sectores)..."
	@dd if=/dev/zero of=build/bleskernos-ata-developer.img bs=512 seek=1 count=8 conv=notrunc status=none
	@dd if=$(BOOT2_BIN) of=build/bleskernos-ata-developer.img bs=512 seek=1 conv=notrunc status=none
	@dd if=/dev/zero of=build/bleskernos-ata-developer.img bs=512 seek=9 count=$(KERNEL_STAGE2_SECTORS) conv=notrunc status=none
	@dd if=$(KERNEL_BIN) of=build/bleskernos-ata-developer.img bs=512 seek=9 conv=notrunc status=none
	@sync
	@echo "[OK]   Developer ATA actualizada sin reconstruir self-host: build/bleskernos-ata-developer.img"

selfhost: $(SELFHOST_STAMP)

SOURCE_IMAGE := build/source/bleskernos-source.img

source: $(SOURCE_IMAGE)
	@echo "[OK]   Source image: $(SOURCE_IMAGE)"

# FORCE permite que `make source` refleje siempre los cambios recientes del
# arbol, aunque el nombre de la imagen ya exista.
$(SOURCE_IMAGE): FORCE $(BOOT1_FAT32_BIN) $(BOOT2_BIN) $(KERNEL_BIN) \
	tools/build_source_image.py tools/build_fat32_ata.py
	@$(PYTHON) tools/build_source_image.py $@

$(SELFHOST_STAMP): tools/build_selfhost_toolchain.py tools/selfhost/fatpack.c
	$(PYTHON) tools/build_selfhost_toolchain.py --output $(SELFHOST_DIR) --stamp $@

editions:
	$(MAKE) EDITION=user all
	$(MAKE) EDITION=developer all
	@echo ""
	@echo "============================================"
	@echo "  BlesKernOS User + Developer listos"
	@echo "  User ATA:      build/bleskernos-ata-user.img"
	@echo "  User USB:      build/bleskernos-usb-user.img"
	@echo "  User VMware:   build/bleskernos-vmware-user.vmdk"
	@echo "  Developer ATA: build/bleskernos-ata-developer.img"
	@echo "  Developer USB: build/bleskernos-usb-developer.img"
	@echo "  Developer VMware: build/bleskernos-vmware-developer.vmdk"
	@echo "============================================"

build:
	@mkdir -p build

# Standalone programs are kept out of the OS image, but bundled in a separate
# FAT fixture so they can be attached or copied into a test image as needed.
extra-programs-image: $(EXTRA_PROGRAMS_IMG)

.PHONY: cursors
cursors: assets/cursors/INDEX.JSON
assets/cursors/INDEX.JSON: tools/build_retrosmart_cursors.py $(shell find third_party/retrosmart -type f)
	$(PYTHON) tools/build_retrosmart_cursors.py

graphics-pak:
	$(PYTHON) tools/build_graphics_pak.py

assets/graphics/GRAPHICS.PAK assets/graphics/GRAPHICS.CSV assets/graphics/KDE1SRC.TGZ &: tools/build_graphics_pak.py tools/kde1_graphics.py tools/import_kde1_icons.py $(shell find third_party/kde1 -type f)
	$(PYTHON) tools/build_graphics_pak.py

$(EXTRA_PROGRAMS_IMG): FORCE tools/build_extra_programs_image.py
	@$(MAKE) -C "extras/Bles-Codex32-standalone" BLESKERNOS_SDK=../../sdk
	@$(MAKE) -C "extras/HyperZip-BlesKernOS-Standalone" SDK_INCLUDE=../../sdk/include
	@$(MAKE) -C "extras/LexiNet" ROOT=../..
	@$(MAKE) -C "extras/Civ2-BlesKernOS-Standalone" ROOT=../..
	@echo "[IMG]  Empaquetando programas extra en $@..."
	@$(PYTHON) tools/build_extra_programs_image.py "$@" "$(EXTRA_CODEX_OBJ)" "$(EXTRA_HYPERZIP_OBJ)" "$(EXTRA_LEXINET_OBJ)" "$(EXTRA_CIV2_OBJ)"
	@echo "[OK]   Imagen de programas extra: $@"

FORCE:

# Stage 1: Bootloader MBR (512 bytes exactos)
$(BOOT1_BIN): $(BOOT1_SRC) | build
	@echo "[NASM] Ensamblando Stage 1..."
	$(NASM) $(NASM_FLAGS) -DRESERVED_SECTORS=$(FAT_RESERVED_SECTORS) $< -o $@
	@SIZE=$$(wc -c < $@); \
	if [ $$SIZE -ne 512 ]; then \
		echo "[ERROR] Stage 1 debe ser 512 bytes, es $$SIZE bytes!"; \
		exit 1; \
	fi
	@echo "[OK]   Stage 1: $$(wc -c < $@) bytes"

$(BOOT1_CD_BIN): $(BOOT1_CD_SRC) | build
	@echo "[NASM] Ensamblando Stage 1 especial de instalacion CD..."
	$(NASM) $(NASM_FLAGS) -DRESERVED_SECTORS=$(FAT_RESERVED_SECTORS) -DBOOT_MODE_MAGIC=0x54534E49 $< -o $@
	@SIZE=$$(wc -c < $@); \
	if [ $$SIZE -ne 512 ]; then \
		echo "[ERROR] Stage 1 CD debe ser 512 bytes, es $$SIZE bytes!"; \
		exit 1; \
	fi
	@echo "[OK]   Stage 1 CD: $$(wc -c < $@) bytes"

$(BOOT1_FAT32_BIN): $(BOOT1_FAT32_SRC) | build
	@echo "[NASM] Ensamblando Stage 1 FAT32 ATA..."
	$(NASM) $(NASM_FLAGS) $< -o $@
	@SIZE=$$(wc -c < $@); \
	if [ $$SIZE -ne 512 ]; then \
		echo "[ERROR] Stage 1 FAT32 debe ser 512 bytes, es $$SIZE bytes!"; \
		exit 1; \
	fi
	@echo "[OK]   Stage 1 FAT32: $$(wc -c < $@) bytes"

$(MBR_BIN): $(MBR_SRC) | build
	@echo "[NASM] Ensamblando MBR USB-HDD..."
	$(NASM) $(NASM_FLAGS) $< -o $@
	@SIZE=$$(wc -c < $@); \
	if [ $$SIZE -ne 512 ]; then \
		echo "[ERROR] MBR debe ser 512 bytes, es $$SIZE bytes!"; \
		exit 1; \
	fi
	@echo "[OK]   MBR: $$(wc -c < $@) bytes"

# Stage 2: Second stage bootloader
$(BOOT2_BIN): $(BOOT2_SRC) $(KERNEL_BIN) | build
	@echo "[NASM] Ensamblando Stage 2..."
	@SECTORS=$$(( ($$(wc -c < $(KERNEL_BIN)) + 511) / 512 )); \
	 CHECKSUM=$$($(PYTHON) -c 'import sys; print(sum(open(sys.argv[1], "rb").read()) & 0xffffffff)' $(KERNEL_BIN)); \
	 echo "[NASM] Kernel real: $$SECTORS sectores, checksum $$CHECKSUM"; \
	 $(NASM) $(NASM_FLAGS) -DKERNEL_SECTORS=$$SECTORS \
	     -DKERNEL_CHECKSUM=$$CHECKSUM $< -o $@
	@echo "[OK]   Stage 2: $$(wc -c < $@) bytes"

# Kernel entry stub: ensamblar
$(KERNEL_ENTRY_OBJ): $(KERNEL_ENTRY_SRC) | build
	@echo "[NASM] Ensamblando entry stub..."
	$(NASM) -f elf32 $< -o $@
	@echo "[OK]   Kernel entry: $@"

# Kernel assembly stubs
build/%.o: kernel/arch/x86/%.asm | build
	@echo "[NASM] Ensamblando ISR/IRQ stubs..."
	$(NASM) -f elf32 $< -o $@
	@echo "[OK]   Kernel stubs: $@"

# Trampoline SMP escrito en GNU assembler para no depender de relocalizaciones
# al copiarlo a memoria baja.
build/%.o: %.S | build
	@mkdir -p $(dir $@)
	@echo "[GAS]  Compilando $<..."
	$(CC) -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -c $< -o $@

# Mesa 3.5: compilar el backend híbrido GFX3D/SWRAST con headers freestanding.
$(MESA35_C_OBJS): build/%.o: %.c | build
	@mkdir -p $(dir $@)
	@echo "[MESA] Compilando $<..."
	$(CC) $(MESA35_CFLAGS) -c $< -o $@

# Mesa 3.5 todavía conserva sus transformadores P6/SSE/MMX escritos en GAS.
# Activarlos evita que el TNL caiga siempre en los loops C escalares.
$(MESA35_ASM_OBJS): build/%.o: %.S | build
	@mkdir -p $(dir $@)
	@echo "[MESA:SSE] Compilando $<..."
	$(CC) -m32 $(MESA35_CPU_FLAGS) -I$(MESA35_ROOT)/src/X86 \
		-I$(MESA35_ROOT)/src -I$(MESA35_ROOT)/include \
		-DUSE_SSE_ASM -DUSE_MMX_ASM -c $< -o $@

# Rutas calientes del escritorio. El baseline 0.9 pasa a Pentium III (i686),
# pero el kernel conserva x87: sólo Mesa/Tux usan SSE después de que el
# scheduler habilita y preserva XMM mediante FXSAVE/FXRSTOR. El controlador
# desktop.c usa optimizacion de tamano; rasterizado y composicion conservan O2.
# memory.c queda fuera para conservar bajo el pressure de registros de su asm.
HOT_GUI_OBJS := \
	build/gui/gfx.o \
	build/gui/compositor.o \
	build/gui/windows.o \
	build/gui/font.o \
	build/gui/image.o

$(HOT_GUI_OBJS): build/%.o: %.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC:HOT] Compilando $<..."
	$(CC) $(CC_FLAGS) -O2 -mtune=pentium3 -fno-align-functions -fno-align-jumps -fno-align-labels $(INCLUDE_FLAGS) -c $< -o $@

# Kernel: compilar C a objeto
build/%.o: %.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando $<..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

# Kernel: linkear
$(LIBC_A): $(LIBC_OBJS) | build
	@mkdir -p $(dir $@)
	@echo "[AR]   Creando libc externa..."
	$(AR) rcs $@ $^

$(TINYGL_A): $(TINYGL_OBJS) | build
	@mkdir -p $(dir $@)
	@echo "[AR]   Creando TinyGL externa..."
	$(AR) rcs $@ $^

$(MESA35_A): $(MESA35_OBJS) | build
	@mkdir -p $(dir $@)
	@echo "[AR]   Creando Mesa 3.5 GFX3D/OSMesa..."
	$(AR) rcs $@ $^
	@test -s $@ || { echo "[ERROR] Mesa 3.5 vacia"; rm -f $@; exit 1; }

mesa35: $(MESA35_A)
	@echo "[OK]   Mesa 3.5: $(MESA35_A)"

$(SDK_SYSCALL_OBJ): sdk/lib/syscalls.c sdk/include/bleskernos.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando ABI de user-space..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_THREAD_OBJ): sdk/lib/thread.c sdk/include/bleskernos.h \
		sdk/include/bleskernos_thread.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando hilos y job pool del SDK..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_PRINT_OBJ): sdk/lib/print.c sdk/include/bleskernos_print.h \
		sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando API de impresion..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_A): $(SDK_SYSCALL_OBJ) $(SDK_THREAD_OBJ) $(SDK_PRINT_OBJ) | build
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SDK_POSIX_OBJ): sdk/lib/posix.c sdk/include/bleskernos.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando capa POSIX de user-space..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_STRING_OBJ): sdk/lib/posix_string.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_STDLIB_OBJ): sdk/lib/posix_stdlib.c sdk/include/stdlib.h sdk/include/inttypes.h sdk/include/stdint.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_STDIO_OBJ): sdk/lib/posix_stdio.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_MMAN_OBJ): sdk/lib/posix_mman.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_PTHREAD_OBJ): sdk/lib/posix_pthread.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_NET_OBJ): sdk/lib/posix_net.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_CTYPE_OBJ): sdk/lib/posix_ctype.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_IPC_OBJ): sdk/lib/posix_ipc.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_COMPAT_OBJ): sdk/lib/posix_compat.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_SPAWN_OBJ): sdk/lib/posix_spawn.c sdk/include/spawn.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_SIGNAL_OBJ): sdk/lib/posix_signal.c sdk/include/signal.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_MAIN_OBJ): sdk/lib/posix_main.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_XLIB_OBJ): sdk/lib/xlib.c sdk/include/X11/Xlib.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_DLFCN_OBJ): sdk/lib/posix_dlfcn.c sdk/include/dlfcn.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_POSIX_SETJMP_OBJ): sdk/lib/posix_setjmp.c sdk/include/setjmp.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_LINUX_MAIN_OBJ): sdk/lib/linux_main.c sdk/include/bleskernos.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_LINUX_RUNTIME_OBJ): sdk/lib/linux_runtime.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -fomit-frame-pointer -c $< -o $@

$(SDK_LINUX_A): $(SDK_LINUX_MAIN_OBJ) $(SDK_LINUX_RUNTIME_OBJ) \
		$(SDK_POSIX_STRING_OBJ) | build
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SDK_POSIX_MATH_OBJ): sdk/lib/posix_math.c sdk/include/math.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_MATH_A): $(SDK_POSIX_MATH_OBJ) | build
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SDK_POSIX_A): $(SDK_POSIX_OBJ) $(SDK_POSIX_STRING_OBJ) $(SDK_POSIX_STDLIB_OBJ) $(SDK_POSIX_STDIO_OBJ) \
		$(SDK_POSIX_MMAN_OBJ) $(SDK_POSIX_PTHREAD_OBJ) $(SDK_POSIX_NET_OBJ) \
		$(SDK_POSIX_CTYPE_OBJ) $(SDK_POSIX_IPC_OBJ) $(SDK_POSIX_COMPAT_OBJ) \
		$(SDK_POSIX_SPAWN_OBJ) $(SDK_POSIX_SIGNAL_OBJ) $(SDK_POSIX_MAIN_OBJ) \
		$(SDK_XLIB_OBJ) $(SDK_POSIX_DLFCN_OBJ) $(SDK_POSIX_SETJMP_OBJ) | build
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SDK_SDL2_OBJ): sdk/lib/sdl2.c sdk/include/SDL2/SDL.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando compatibilidad SDL2 Ring 3..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_SDL_MAIN_OBJ): sdk/lib/sdl_main.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_SDL_EXTRA_OBJ): sdk/lib/sdl_extra.c sdk/include/SDL2/SDL.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_SDL2_A): $(SDK_SDL2_OBJ) $(SDK_SDL_MAIN_OBJ) $(SDK_SDL_EXTRA_OBJ) | build
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SDK_TINYGL_OBJ): sdk/lib/tinygl.c sdk/include/bleskernos_tinygl.h \
	sdk/include/TGL/gl.h sdk/include/tinygl/zbuffer.h sdk/include/tinygl/zfeatures.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando API TinyGL del SDK..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_TINYGL_A): $(SDK_TINYGL_OBJ) $(TINYGL_A) | build
	@mkdir -p $(dir $@)
	@rm -f $@
	@mkdir -p build/sdk/tinygl-merge
	@rm -f build/sdk/tinygl-merge/*.o
	@cd build/sdk/tinygl-merge && $(AR) x ../../../$(TINYGL_A)
	@cp $(SDK_TINYGL_OBJ) build/sdk/tinygl-merge/bleskernos_tinygl.o
	$(AR) rcs $@ build/sdk/tinygl-merge/*.o

$(SDK_MESA_OBJ): sdk/lib/mesa.c sdk/include/bleskernos_mesa.h \
		sdk/include/GL/gl.h sdk/include/GL/glext.h sdk/include/GL/osmesa.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando API Mesa 3.5 del SDK..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDK_MESA_A): $(SDK_MESA_OBJ) $(MESA35_A) | build
	@mkdir -p $(dir $@)
	@rm -f $@
	@mkdir -p build/sdk/mesa35-merge
	@rm -f build/sdk/mesa35-merge/*.o
	@cd build/sdk/mesa35-merge && $(AR) x ../../../$(MESA35_A)
	@cp $(SDK_MESA_OBJ) build/sdk/mesa35-merge/bleskernos_mesa.o
	$(AR) rcs $@ build/sdk/mesa35-merge/*.o

sdk: $(SDK_A) $(SDK_POSIX_A) $(SDK_LINUX_A) $(SDK_SDL2_A) $(SDK_MATH_A) $(SDK_TINYGL_A) $(SDK_MESA_A)
	@echo "[OK]   SDK nativo: $(SDK_A) + $(SDK_POSIX_A)"
	@echo "[OK]   ABI Linux i386 inicial: $(SDK_LINUX_A)"
	@echo "[OK]   SDL2 compatible: $(SDK_SDL2_A)"
	@echo "[OK]   libm POSIX: $(SDK_MATH_A)"
	@echo "[OK]   SDK TinyGL: $(SDK_TINYGL_A)"
	@echo "[OK]   SDK Mesa 3.5: $(SDK_MESA_A)"

$(KERNEL_ELF): $(KERNEL_ENTRY_OBJ) $(KERNEL_ASM_OBJS) $(KERNEL_GAS_OBJS) $(KERNEL_OBJS) $(LIBC_A) kernel/arch/x86/linker.ld | build
	@echo "[LD]   Linkeando kernel..."
	$(LD) $(LD_FLAGS) $(KERNEL_ENTRY_OBJ) $(KERNEL_ASM_OBJS) $(KERNEL_GAS_OBJS) $(KERNEL_OBJS) $(LIBC_A) -o $@

# Kernel: extraer binario plano del ELF
$(KERNEL_BIN): $(KERNEL_ELF)
	@echo "[OBJCOPY] Extrayendo binario..."
	objcopy -O binary $< $@
	@SIZE=$$(wc -c < $@); \
	if [ $$SIZE -gt 1310720 ]; then \
		echo "[ERROR] kernel binario supera 1280 KiB: $$SIZE bytes"; \
		rm -f $@; \
		exit 1; \
	fi
	@echo "[OK]   Kernel: $$(wc -c < $@) bytes"

# ── Programas externos de /SYSTEM/PROGRAMS ─────────────────────

# shell.o — compilado como objeto independiente para el disquete
SHELL_OBJ := build/programs/shelllauncher.o

$(SHELL_OBJ): programs/shelllauncher.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando shell como programa separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   Shell object: $@"

# filebrowser.o — compilado como objeto independiente para el disquete
FILEBROWSER_OBJ := build/programs/filebrowser.o

$(FILEBROWSER_OBJ): programs/filebrowser.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando filebrowser como programa separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   Filebrowser object: $@"

# processmanager.o - compilado como objeto independiente para el disquete
PROCESSMANAGER_OBJ := build/programs/processmanager.o

$(PROCESSMANAGER_OBJ): programs/processmanager.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando processmanager como programa separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   Processmanager object: $@"

# calendar.o - compilado como objeto independiente para el disquete
CALENDAR_OBJ := build/programs/calendar.o
ABOUT_OBJ := build/programs/about.o
RUNBOX_OBJ := build/programs/runbox.o
TEXTEDITOR_OBJ := build/programs/texteditor.o
FONT_OBJ := build/programs/font.o
FONT_FILES := $(wildcard assets/fonts/*.BFN) assets/fonts/LICENSE.txt
CALCULATOR_OBJ := build/programs/calculator.o
MIDAMP_OBJ := build/programs/midamp.o
IMAGEVIEWER_OBJ := build/programs/imageviewer.o
PAINT_OBJ := build/programs/paint.o
APITEST_OBJ := build/programs/apitest.o
SCANDISK_OBJ := build/programs/scandisk.o
WINE_OBJ := build/programs/wine.o
WINE_DESKTOP_LAUNCHER := build/wine/wine-launch
WINE_READY_CLIENT := build/wine/wine-ready.exe
WINE_TOOL_LAUNCHER := build/wine/wine-tool
WINE_GL_LIB := ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/lib/libGL.so.1
WINE_MESA_LIB := ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/lib/libbkmesa.so.1
WINE_PACKAGE := ports/wine/package/wine-11.0-i386
WINE_TINYX_BINARY := $(WINE_PACKAGE)/rootfs/WINEGATE/bin/XFBDEV
WINE_TINYX_SOURCES := $(shell find ports/tinyx/upstream -type f 2>/dev/null)
WINE_GALLIUM_LIB := $(WINE_PACKAGE)/rootfs/WINEGATE/lib/libbkvirgl.so.8
WINE_GALLIUM_FILES := $(WINE_GALLIUM_LIB) \
	$(addprefix $(WINE_PACKAGE)/rootfs/WINEGATE/lib/,libstdc++.so.6 libgcc_s.so.1 libexpat.so.1 libz.so.1) \
	$(addprefix $(WINE_PACKAGE)/licenses/,MESA-GALLIUM-LICENSE.rst libstdc++6-COPYRIGHT libgcc-s1-COPYRIGHT libexpat1-COPYRIGHT zlib1g-COPYRIGHT GALLIUM-RUNTIME-GPL-3 GALLIUM-RUNTIME-LGPL-3 GALLIUM-RUNTIME-LGPL-2.1 GALLIUM-RUNTIME-GPL-2)
WINE_RETRO_ICONS := build/wine/BLESICONS.DLL

# Wine: comprobar dependencias antes de evaluar las reglas.
# Evitar descargas en tareas de limpieza e inspección.
WINE_SKIP_GOALS := clean distclean help
WINE_ACTIVE_GOALS := $(if $(MAKECMDGOALS),$(filter-out $(WINE_SKIP_GOALS),$(MAKECMDGOALS)),all)

ifneq ($(strip $(WINE_ACTIVE_GOALS)),)
ifeq ($(wildcard ports/wine/package/wine-11.0-i386/rootfs/opt/wine/bin/wine),)
WINE_FETCH_STATUS := $(shell $(PYTHON) ports/wine/scripts/fetch-release.py >&2; echo $$?)
ifneq ($(WINE_FETCH_STATUS),0)
$(error No se pudo preparar Wine. Revisá BLES_WINE_URL o el paquete local)
endif
endif
endif

WINE_RUNTIME_FILES := $(shell find ports/wine/package/wine-11.0-i386/rootfs/opt/wine ports/wine/package/wine-11.0-i386/rootfs/WINEGATE -path '*/WINEGATE/prefix' -prune -o -type f -print 2>/dev/null)
WINE_PREFIX_MANIFEST := ports/wine/package/wine-11.0-i386/prefix-template.json
WINE_LICENSE_FILES := $(wildcard ports/wine/package/wine-11.0-i386/licenses/*) ports/tinyx/upstream/tinycorelinux-tinyx/COPYING
NETSURF_OBJ := build/programs/netsurf.o
SETUP_OBJ := build/programs/setup.o
SYSTEM_TOOL_NAMES := help_center find_files archive_manager disk_tools network_status clipboard_viewer performance_monitor event_viewer defrag
SYSTEM_TOOL_OBJS := $(addprefix build/programs/,$(addsuffix .o,$(SYSTEM_TOOL_NAMES)))
NETSURF_VENDOR_SOURCES := \
	programs/netsurf/vendor/libparserutils/src/charset/aliases.c \
	programs/netsurf/vendor/libparserutils/src/charset/codec.c \
	programs/netsurf/vendor/libparserutils/src/charset/codecs/codec_8859.c \
	programs/netsurf/vendor/libparserutils/src/charset/codecs/codec_ascii.c \
	programs/netsurf/vendor/libparserutils/src/charset/codecs/codec_ext8.c \
	programs/netsurf/vendor/libparserutils/src/charset/codecs/codec_utf16.c \
	programs/netsurf/vendor/libparserutils/src/charset/codecs/codec_utf8.c \
	programs/netsurf/vendor/libparserutils/src/charset/encodings/utf16.c \
	programs/netsurf/vendor/libparserutils/src/charset/encodings/utf8.c \
	programs/netsurf/vendor/libparserutils/src/input/filter.c \
	programs/netsurf/vendor/libparserutils/src/input/inputstream.c \
	programs/netsurf/vendor/libparserutils/src/utils/buffer.c \
	programs/netsurf/vendor/libparserutils/src/utils/errors.c \
	programs/netsurf/vendor/libparserutils/src/utils/stack.c \
	programs/netsurf/vendor/libparserutils/src/utils/vector.c \
	programs/netsurf/vendor/libwapcaplet/src/libwapcaplet.c \
	programs/netsurf/vendor/libhubbub/src/charset/detect.c \
	programs/netsurf/vendor/libhubbub/src/parser.c \
	programs/netsurf/vendor/libhubbub/src/tokeniser/entities.c \
	programs/netsurf/vendor/libhubbub/src/tokeniser/tokeniser.c \
	$(filter-out programs/netsurf/vendor/libhubbub/src/treebuilder/autogenerated-element-type.c programs/netsurf/vendor/libhubbub/src/treebuilder/AUTOGENERATED-ELEMENT-TYPE.c,$(call c_wildcard,programs/netsurf/vendor/libhubbub/src/treebuilder/*.c)) \
	programs/netsurf/vendor/libhubbub/src/utils/errors.c \
	programs/netsurf/vendor/libhubbub/src/utils/string.c \
	$(call c_wildcard,programs/netsurf/vendor/libcss/src/*.c) \
	$(call c_wildcard,programs/netsurf/vendor/libcss/src/charset/*.c) \
	$(call c_wildcard,programs/netsurf/vendor/libcss/src/lex/*.c) \
	$(call c_wildcard,programs/netsurf/vendor/libcss/src/parse/*.c) \
	$(filter-out programs/netsurf/vendor/libcss/src/parse/properties/css_property_parser_gen.c programs/netsurf/vendor/libcss/src/parse/properties/CSS_PROPERTY_PARSER_GEN.c,$(call c_wildcard,programs/netsurf/vendor/libcss/src/parse/properties/*.c)) \
	$(call c_wildcard,programs/netsurf/vendor/libcss/src/select/*.c) \
	$(call c_wildcard,programs/netsurf/vendor/libcss/src/select/properties/*.c) \
	$(call c_wildcard,programs/netsurf/vendor/libcss/src/utils/*.c)
NETSURF_VENDOR_OBJS := $(patsubst programs/netsurf/%.c,build/programs/netsurf/%.o,$(NETSURF_VENDOR_SOURCES))
NETSURF_RAW_OBJS := \
	build/programs/netsurf/main.o \
	build/programs/netsurf/platform.o \
	build/programs/netsurf/html_parser.o \
	build/programs/netsurf/dom_tree.o \
	build/programs/netsurf/css_engine.o \
	build/programs/netsurf/css_compat.o \
	build/programs/netsurf/layout.o \
	build/programs/netsurf/plotters.o \
	build/programs/netsurf/font.o \
	build/programs/netsurf/compat.o \
	build/programs/netsurf/string.o \
	$(NETSURF_VENDOR_OBJS)
NETSURF_CFLAGS := -Iprograms/netsurf/compat/include $(SDK_CFLAGS) -DNDEBUG -DWITHOUT_ICONV_FILTER -D_ALIGNED="__attribute__((aligned))" -DSTMTEXPR=1 \
	-fno-strict-aliasing -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Iprograms/netsurf \
	-Iprograms/netsurf/vendor/libparserutils/include \
	-Iprograms/netsurf/vendor/libparserutils/src \
	-Iprograms/netsurf/vendor/libhubbub/include \
	-Iprograms/netsurf/vendor/libhubbub/src \
	-Iprograms/netsurf/vendor/libwapcaplet/include \
	-Iprograms/netsurf/vendor/libcss/include
PHASE2_RAW_OBJ := build/tests/userland/phase2.raw.o
PHASE2_OBJ := build/programs/phase2.o
POSIX_TEST_RAW_OBJ := build/tests/userland/posix_smoke.raw.o
POSIX_TEST_OBJ := build/programs/posix_test.o
LINUX_ABI_TEST_RAW_OBJ := build/tests/userland/linux_abi_smoke.raw.o
LINUX_ABI_TEST_OBJ := build/programs/linux_abi_test.o
DL_TEST_RAW_OBJ := build/tests/userland/dl_module.raw.o
DL_TEST_SO := build/tests/userland/DLMOD.SO
DL_DEP_RAW_OBJ := build/tests/userland/dl_dependency.raw.o
DL_DEP_SO := build/tests/userland/DLDEP.SO
LINUX_TLS_DEP_OBJ := build/tests/userland/linux_tls_dependency.o
LINUX_TLS_DEP_SO := build/tests/userland/TLSDEP.SO
LINUX_PIE_TEST_OBJ := build/tests/userland/linux_pie_smoke.o
LINUX_PIE_TEST := build/tests/userland/LINUXPIE.ELF
LD_BLESK_OBJ := build/tests/userland/ld_blesk.o
LD_BLESK_SO := build/system/libs/posix/ld-blesk.so
LINUX_INTERP_TEST := build/tests/userland/INTERPPIE.ELF
LINUX_DYNAMIC_TEST_OBJ := build/tests/userland/linux_dynamic_smoke.o
LINUX_DYNAMIC_TEST := build/tests/userland/DYNAMIC.ELF
LINUX_EXEC_TEST_OBJ := build/tests/userland/linux_exec_smoke.o
LINUX_EXEC_TEST := build/tests/userland/EXECFIX.ELF
LINUX_LIBC_OBJ := build/system/libs/posix/libc_linux.o
LINUX_LIBC_SO := build/system/libs/posix/libc.so.6
LINUX_GLIBC_TEST := build/tests/userland/GLIBCHEL.ELF
SDL_DEMO_RAW_OBJ := build/tests/userland/sdl_demo.raw.o
SDL_DEMO_OBJ := build/programs/sdl_demo.o
RING3PROXY_OBJ := build/programs/ring3proxy.o
COMMAND_NAMES := help echo sh about uname hostname uptime date time shutdown reboot sleep bosrecovery \
	fdisk format mount unmount label checkdisk fsinfo backup dir ls copy cp move delete rm mkdir \
	rmdir rename touch tree find attrib chmod type more cat diff ps kill tasklist \
	taskkill top nice pci usb lspci lsusb cpuinfo mem soundtest ipconfig ping \
	netstat ftp wget curl compile link gcc cc ld as ar ranlib build nasm ndisasm make fatpack addr2line cxxfilt elfedit objcopy objdump readelf nm size strip hexdump strings calc \
	hexedit compress extract checksum benchmark start run head tail wc grep stat df du cmp which sync true false diskpart
COMMAND_COMMON_OBJ := build/system/commands/common.o
COMMAND_RAW_OBJS := $(addprefix build/system/commands/raw/,$(addsuffix .o,$(COMMAND_NAMES)))
COMMAND_OBJS := $(addprefix build/system/commands/,$(addsuffix .o,$(COMMAND_NAMES)))
CONTROL_PANEL_OBJ := build/system/control/control_panel.o
APPEAR_CPL_RAW_OBJ := build/system/control/appearance.raw.o
APPEAR_CPL_OBJ := build/system/control/appearance.o
DISPLAY_CPL_RAW_OBJ := build/system/control/display.raw.o
DISPLAY_CPL_OBJ := build/system/control/display.o
SOUND_CPL_OBJ := build/system/control/sound.o
DATETIME_CPL_OBJ := build/system/control/datetime.o
MOUSE_CPL_OBJ := build/system/control/mouse.o
KEYBOARD_CPL_OBJ := build/system/control/keyboard.o
SYSTEM_CPL_OBJ := build/system/control/system.o
DEVMGR_CPL_OBJ := build/system/control/device_manager.o
INTERNET_CPL_OBJ := build/system/control/internet.o
MODEM_CPL_OBJ := build/system/control/modem.o
LANGUAGE_CPL_OBJ := build/system/control/language.o
WINE_CPL_OBJ := build/system/control/wine.o
CONTROL_OBJS := \
	$(CONTROL_PANEL_OBJ) \
	$(APPEAR_CPL_OBJ) \
	$(DISPLAY_CPL_OBJ) \
	$(SOUND_CPL_OBJ) \
	$(DATETIME_CPL_OBJ) \
	$(MOUSE_CPL_OBJ) \
	$(KEYBOARD_CPL_OBJ) \
	$(SYSTEM_CPL_OBJ) \
	$(DEVMGR_CPL_OBJ) \
	$(INTERNET_CPL_OBJ) \
	$(MODEM_CPL_OBJ) \
	$(LANGUAGE_CPL_OBJ) \
	$(WINE_CPL_OBJ)
GEARS_OBJ := build/programs/gears.o
GEARS_RAW_OBJ := build/programs/gears.raw.o
SCREENSAVERD_OBJ := build/system/services/screensaverd.o
VISION_OBJ := build/system/services/vision.o
VISION_MAIN_OBJ := build/system/services/vision_main.o
VISION_DESKMANAGER_OBJ := build/system/services/vision_deskmanager.o
VISION_DESKBAR_OBJ := build/system/services/vision_deskbar.o
VISION_SCREENSAVER_OBJ := build/system/services/vision_screensaver.o
VISION_ENGINE_OBJS := $(patsubst gui/%.c,build/system/services/engine_%.o,gui/desktop.c gui/compositor.c gui/windows.c gui/widget.c gui/font.c gui/real_font.c gui/event.c gui/gpu_compositor.c gui/raster.c)
VISION_ENGINE_OBJS += build/system/services/vision_file_dialog.o build/system/services/vision_about_dialog.o build/system/services/vision_engine.o build/system/services/vision_memory.o build/system/services/vision_string.o build/system/services/vision_windows.o build/system/services/vision_raster_api.o
VISION_COMPONENT_OBJS := $(VISION_ENGINE_OBJS) $(VISION_MAIN_OBJ) $(VISION_DESKMANAGER_OBJ) $(VISION_DESKBAR_OBJ) $(VISION_SCREENSAVER_OBJ)
VISION_DESKTOP_RENAMES := \
	-Ddeskmanager_install=vision_deskmanager_install \
	-Ddeskmanager_install_setup=vision_deskmanager_install_setup \
	-Ddeskmanager_set_background=vision_deskmanager_set_background \
	-Ddeskmanager_get_background=vision_deskmanager_get_background \
	-Ddeskmanager_get_wallpaper_path=vision_deskmanager_get_wallpaper_path \
	-Ddeskmanager_set_wallpaper=vision_deskmanager_set_wallpaper \
	-Ddeskmanager_set_wallpaper_mode=vision_deskmanager_set_wallpaper_mode \
	-Ddeskmanager_get_wallpaper_mode=vision_deskmanager_get_wallpaper_mode \
	-Ddeskmanager_refresh_layout=vision_deskmanager_refresh_layout
PRINTSPOOL_OBJ := build/system/services/printspool.o
PRINT_TEST_RAW_OBJ := build/programs/print_test.raw.o
PRINT_TEST_OBJ := build/programs/print_test.o
SB16_DVR := build/system/drivers/SB16.DVR
AC97_DVR := build/system/drivers/AC97.DVR
MAESTRO3_DVR := build/system/drivers/MAESTRO3.DVR
RTC_DVR := build/system/drivers/CMOSRTC.DVR
ISO9660_DVR := build/system/drivers/ISO9660.DVR
PS2MOUSE_DVR := build/system/drivers/PS2MOUSE.DVR
USBCLASS_DVR := build/system/drivers/USBCLASS.DVR
VESA_DVR := build/system/drivers/VESA.DVR
VMWARESVGA_DVR := build/system/drivers/VMWARESVGA.DVR
ATIR128_DVR := build/system/drivers/ATIR128.DVR
ATIR1283D_DVR := build/system/drivers/ATIR1283D.DVR
VMWARESVGA3D_DVR := build/system/drivers/VMWARESVGA3D.DVR
VIRTIOGPU_DVR := build/system/drivers/VIRTIOGPU.DVR
INTELGMA_DVR := build/system/drivers/INTELGMA.DVR
THREEC90X_DVR := build/system/drivers/3C90X.DVR
NETSTACK_DVR := build/system/drivers/NETSTACK.DVR
RTL8139_DVR := build/system/drivers/RTL8139.DVR
TLS_DVR := build/system/drivers/TLS.DVR
TLS_RAW_OBJ := build/system/drivers/tls.raw.o
LEGACY_AUDIO_DRIVERS := ES1370 CMIPCI VIA686 CS4281
LEGACY_AUDIO_OBJS := $(addprefix build/system/drivers/,$(addsuffix .DVR,$(LEGACY_AUDIO_DRIVERS)))
LEGACY_NET_DRIVERS := CDC_ECM E100 E1000 PCNET NE2KPCI TULIP RHINE SIS900 NATSEMI RTL8169
LEGACY_NET_OBJS := $(addprefix build/system/drivers/,$(addsuffix .DVR,$(LEGACY_NET_DRIVERS)))

DRIVER_OBJS := build/system/drivers/LEGACYFB.DVR $(AC97_DVR) $(MAESTRO3_DVR) $(SB16_DVR) $(RTC_DVR) $(ISO9660_DVR) \
	$(PS2MOUSE_DVR) $(USBCLASS_DVR) $(VESA_DVR) $(VMWARESVGA_DVR) $(VMWARESVGA3D_DVR) $(VIRTIOGPU_DVR) $(INTELGMA_DVR) $(THREEC90X_DVR) $(NETSTACK_DVR) $(RTL8139_DVR) \
	$(TLS_DVR) $(LEGACY_NET_OBJS) $(LEGACY_AUDIO_OBJS)

SSLOGO_OBJ := build/system/screensavers/ss_logo.o
SSPIPES_OBJ := build/system/screensavers/ss_pipes.o
SSPIPES_RAW_OBJ := build/system/screensavers/ss_pipes.raw.o
SSBALLS_OBJ := build/system/screensavers/ss_balls.o
SSBALLS_RAW_OBJ := build/system/screensavers/ss_balls.raw.o

USER_EXTERNAL_APP_OBJS := \
	$(SHELL_OBJ) \
	$(FILEBROWSER_OBJ) \
	$(TEXTEDITOR_OBJ) \
	$(CALCULATOR_OBJ) \
	$(FONT_OBJ) \
	$(MIDAMP_OBJ) \
	$(PROCESSMANAGER_OBJ) \
	$(CALENDAR_OBJ) \
	$(ABOUT_OBJ) \
	$(RUNBOX_OBJ) \
	$(IMAGEVIEWER_OBJ) \
	$(VIEWER_OBJ) \
	$(PAINT_OBJ) \
	$(SCANDISK_OBJ) \
	$(WINE_OBJ) \
	$(NETSURF_OBJ) \
	$(SYSTEM_TOOL_OBJS) \
	$(COMMAND_OBJS) \
	$(CONTROL_OBJS) \
	$(SCREENSAVERD_OBJ) \
	$(VISION_OBJ) \
	$(PRINTSPOOL_OBJ) \
	$(SSLOGO_OBJ) \
	$(SSPIPES_OBJ) \
	$(SSBALLS_OBJ)

DEVELOPER_EXTERNAL_APP_OBJS := \
	$(APITEST_OBJ) \
	$(PHASE2_OBJ) \
	$(POSIX_TEST_OBJ) \
	$(LINUX_ABI_TEST_OBJ) \
	$(SDL_DEMO_OBJ) \
	$(RING3PROXY_OBJ) \
	$(GEARS_OBJ) \
	$(PRINT_TEST_OBJ)

ifeq ($(EDITION),developer)
EXTERNAL_APP_OBJS := $(USER_EXTERNAL_APP_OBJS) $(DEVELOPER_EXTERNAL_APP_OBJS)
EDITION_EXTRA_DEPS := $(DL_DEP_SO) $(DL_TEST_SO) $(LINUX_TLS_DEP_SO) $(LD_BLESK_SO) \
	$(LINUX_LIBC_SO) $(LINUX_PIE_TEST) $(LINUX_INTERP_TEST) \
	$(LINUX_DYNAMIC_TEST) $(LINUX_EXEC_TEST) $(LINUX_GLIBC_TEST)
else
EXTERNAL_APP_OBJS := $(USER_EXTERNAL_APP_OBJS)
EDITION_EXTRA_DEPS :=
endif



$(SYSTEM_TOOL_OBJS): build/programs/%.o: programs/%.c programs/system_tools_common.h sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando utilidad $*..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@
	@echo "[OK]   Utilidad: $@"

$(PHASE2_RAW_OBJ): tests/userland/phase2.c sdk/include/bleskernos.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(PHASE2_OBJ): $(PHASE2_RAW_OBJ) $(SDK_A)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r $< $(SDK_A) -o $@
	@echo "[OK]   Prueba Ring 3/API: $@"

$(POSIX_TEST_RAW_OBJ): sdk/examples/posix_smoke.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(POSIX_TEST_OBJ): $(POSIX_TEST_RAW_OBJ) $(SDK_POSIX_A) $(SDK_MATH_A) $(SDK_A)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r $< $(SDK_POSIX_A) $(SDK_MATH_A) $(SDK_A) -o $@
	@echo "[OK]   Prueba POSIX Ring 3: $@"

$(LINUX_ABI_TEST_RAW_OBJ): sdk/examples/linux_abi_smoke.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(LINUX_ABI_TEST_OBJ): $(LINUX_ABI_TEST_RAW_OBJ) $(SDK_LINUX_A) $(SDK_A)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r -u bleskernos_program_main $< \
		$(SDK_LINUX_A) $(SDK_A) -o $@
	@echo "[OK]   Prueba ABI Linux i386: $@"

$(DL_TEST_RAW_OBJ): sdk/examples/dl_module.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -fPIC -c $< -o $@

$(DL_DEP_RAW_OBJ): sdk/examples/dl_dependency.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -fPIC -c $< -o $@

$(DL_DEP_SO): $(DL_DEP_RAW_OBJ)
	$(LD) -m elf_i386 -shared --hash-style=sysv -soname DLDEP.SO $< -o $@

$(DL_TEST_SO): $(DL_TEST_RAW_OBJ) $(DL_DEP_SO)
	$(LD) -m elf_i386 -shared --hash-style=sysv -soname DLMOD.SO \
		$(DL_TEST_RAW_OBJ) $(DL_DEP_SO) -o $@

$(LINUX_TLS_DEP_OBJ): tests/userland/linux_tls_dependency.c | build
	@mkdir -p $(dir $@)
	$(CC) -m32 -fPIC -Os -ffreestanding -fno-stack-protector \
		-c $< -o $@

$(LINUX_TLS_DEP_SO): $(LINUX_TLS_DEP_OBJ)
	$(LD) -m elf_i386 -shared --hash-style=sysv -soname TLSDEP.SO $< -o $@

$(LINUX_PIE_TEST_OBJ): tests/userland/linux_pie_smoke.S | build
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(LINUX_PIE_TEST): $(LINUX_PIE_TEST_OBJ)
	$(LD) -m elf_i386 -pie --no-dynamic-linker -e _start \
		--hash-style=sysv $< -o $@

$(LD_BLESK_OBJ): tests/userland/ld_blesk.S | build
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(LD_BLESK_SO): $(LD_BLESK_OBJ)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -shared -e _start --hash-style=sysv $< -o $@

$(LINUX_INTERP_TEST): $(LINUX_PIE_TEST_OBJ) $(LD_BLESK_SO)
	$(LD) -m elf_i386 -pie --dynamic-linker /lib/ld-linux.so.2 \
		-e _start --hash-style=sysv $(LINUX_PIE_TEST_OBJ) -o $@

$(LINUX_DYNAMIC_TEST_OBJ): tests/userland/linux_dynamic_smoke.S | build
	@mkdir -p $(dir $@)
	$(CC) -m32 -fPIC -c $< -o $@

$(LINUX_DYNAMIC_TEST): $(LINUX_DYNAMIC_TEST_OBJ) $(DL_DEP_SO) $(LINUX_TLS_DEP_SO) $(LD_BLESK_SO)
	$(LD) -m elf_i386 -pie --allow-shlib-undefined --dynamic-linker /lib/ld-linux.so.2 \
		-e _start --hash-style=sysv --enable-new-dtags -rpath '$$ORIGIN/LIB' \
		$(LINUX_DYNAMIC_TEST_OBJ) \
		$(DL_DEP_SO) $(LINUX_TLS_DEP_SO) -o $@

$(LINUX_EXEC_TEST_OBJ): tests/userland/linux_exec_smoke.S | build
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(LINUX_EXEC_TEST): $(LINUX_EXEC_TEST_OBJ)
	$(LD) -m elf_i386 -e _start -Ttext=0x08048000 --hash-style=sysv $< -o $@

$(LINUX_LIBC_OBJ): tests/userland/libc_linux.S | build
	@mkdir -p $(dir $@)
	$(CC) -m32 -fPIC -c $< -o $@

$(LINUX_LIBC_SO): $(LINUX_LIBC_OBJ)
	$(LD) -m elf_i386 -shared --hash-style=both -soname libc.so.6 $< -o $@

$(LINUX_GLIBC_TEST): tests/userland/linux_glibc_hello.c $(LINUX_LIBC_SO) $(LD_BLESK_SO) | build
	@mkdir -p $(dir $@)
	$(CC) -m32 -no-pie -O2 -fno-stack-protector $< -o $@

$(SDL_DEMO_RAW_OBJ): sdk/examples/sdl_port_demo.c | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(SDL_DEMO_OBJ): $(SDL_DEMO_RAW_OBJ) $(SDK_SDL2_A) $(SDK_POSIX_A) $(SDK_A)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r -u bleskernos_program_main $< \
		$(SDK_SDL2_A) $(SDK_POSIX_A) $(SDK_A) -o $@
	@echo "[OK]   Demo SDL2/POSIX Ring 3: $@"

# The historical engine owns main(); the Bles loader enters through
# bleskernos_program_main(), which starts the game on its large-stack worker.
build/programs/tuxracer/src/main.o: TUXRACER_MAIN_CFLAG := -Dmain=tuxracer_main

$(TUXRACER_C_OBJS): build/programs/tuxracer/src/%.o: $(TUXRACER_ROOT_ESC)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TUXRACER_PORT_CFLAGS) $(TUXRACER_MAIN_CFLAG) -std=gnu99 -c "$<" -o $@

$(TUXRACER_CXX_OBJS): build/programs/tuxracer/src/%.o: $(TUXRACER_ROOT_ESC)/src/%.cpp
	@mkdir -p $(dir $@)
	g++ $(TUXRACER_PORT_CFLAGS) -fno-exceptions -fno-rtti \
		-fno-threadsafe-statics -fno-use-cxa-atexit -c "$<" -o $@

build/programs/tuxracer/platform/blesk_app.o: $(TUXRACER_ROOT_ESC)/blesk/src/blesk_app.c
	@mkdir -p $(dir $@)
	$(CC) $(TUXRACER_PORT_CFLAGS) -std=gnu99 -c "$<" -o $@

build/programs/tuxracer/platform/blesk_glu.o: $(TUXRACER_ROOT_ESC)/blesk/src/blesk_glu.c
	@mkdir -p $(dir $@)
	$(CC) $(TUXRACER_PORT_CFLAGS) -std=gnu99 -c "$<" -o $@

build/programs/tuxracer/platform/blesk_winsys.o: $(TUXRACER_ROOT_ESC)/blesk/src/blesk_winsys.c
	@mkdir -p $(dir $@)
	$(CC) $(TUXRACER_PORT_CFLAGS) -std=gnu99 -c "$<" -o $@

build/programs/tuxracer/platform/cxx_runtime.o: $(TUXRACER_ROOT_ESC)/blesk/src/cxx_runtime.cpp
	@mkdir -p $(dir $@)
	g++ $(TUXRACER_PORT_CFLAGS) -fno-exceptions -fno-rtti \
		-fno-threadsafe-statics -fno-use-cxa-atexit -c "$<" -o $@

build/programs/tuxracer/platform/tcl_compat.o: $(TUXRACER_ROOT_ESC)/dc/src/tcl_compat.c
	@mkdir -p $(dir $@)
	$(CC) $(TUXRACER_PORT_CFLAGS) -std=gnu99 -c "$<" -o $@

$(TUXRACER_JIM_OBJS): build/programs/tuxracer/jim/%.o: $(TUXRACER_ROOT_ESC)/blesk/third_party/jimtcl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TUXRACER_PORT_CFLAGS) -std=gnu99 -c "$<" -o $@

$(TUXRACER_JIM_A): $(TUXRACER_JIM_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(TUXRACER_OBJ): $(TUXRACER_C_OBJS) $(TUXRACER_CXX_OBJS) \
		$(TUXRACER_PLATFORM_OBJS) $(TUXRACER_JIM_A) \
		$(SDK_MESA_A) $(SDK_MATH_A) $(SDK_POSIX_A) $(SDK_A) $(LIBC_A)
	$(LD) -m elf_i386 -r -d --allow-multiple-definition \
		-u bleskernos_program_main -o $@ \
		$(TUXRACER_C_OBJS) $(TUXRACER_CXX_OBJS) $(TUXRACER_PLATFORM_OBJS) \
		--whole-archive $(TUXRACER_JIM_A) \
		--no-whole-archive $(SDK_MESA_A) $(SDK_MATH_A) $(SDK_POSIX_A) \
		$(SDK_A) $(LIBC_A)
	@echo "[OK]   Tux Racer 0.61 Ring 3: $@"

build/programs/admin_smoke.o: tests/userland/admin_smoke.c kernel/include/api.h sdk/include/bleskernos_admin.h | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(RING3PROXY_OBJ): tests/userland/ring3proxy.c kernel/include/api.h sdk/include/bleskernos_admin.h | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   Prueba proxy API Ring 3: $@"

$(COMMAND_COMMON_OBJ): system/commands/common.c system/commands/common.h \
		sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando biblioteca comun de comandos Ring 3..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(COMMAND_RAW_OBJS): build/system/commands/raw/%.o: system/commands/%.c \
		system/commands/common.h sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando comando $*..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(COMMAND_OBJS): build/system/commands/%.o: build/system/commands/raw/%.o $(COMMAND_COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r $< $(COMMAND_COMMON_OBJ) -o $@
	@echo "[OK]   Comando independiente: $@"

$(SB16_DVR): kernel/drivers/audio/sound.c kernel/include/driver.h kernel/include/sound.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver de sonido separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(AC97_DVR): kernel/drivers/audio/ac97.c kernel/include/driver.h kernel/include/sound.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver AC97 ICH-compatible separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(MAESTRO3_DVR): kernel/drivers/audio/maestro3.c kernel/drivers/audio/maestro3_regs.h \
		kernel/drivers/audio/maestro3_firmware.h kernel/include/driver.h \
		kernel/include/pci.h kernel/include/sound.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver externo ESS Maestro3..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(RTC_DVR): kernel/drivers/platform/rtc.c kernel/include/driver.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver CMOS/RTC separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(ISO9660_DVR): kernel/drivers/storage/iso9660.c kernel/include/iso9660.h kernel/include/driver.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver ISO9660 separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(PS2MOUSE_DVR): kernel/drivers/input/mouse.c kernel/include/driver.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver de mouse PS/2 separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(USBCLASS_DVR): kernel/drivers/usb/usbclass.c kernel/include/driver.h \
		kernel/include/usb_core.h kernel/include/keyboard.h kernel/include/mouse.h \
		kernel/include/lpt.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando USBCLASS.DVR (HID, hubs e impresoras USB)..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(VESA_DVR): kernel/drivers/graphics/vesa.c kernel/include/driver.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver VESA separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(VMWARESVGA_DVR): kernel/drivers/graphics/vmware_svga_dvr.c kernel/include/driver.h \
		kernel/include/gfx.h kernel/include/gfx_driver.h kernel/include/pci.h \
		kernel/include/svga_transport.h kernel/include/svga3d_protocol.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando VMWARESVGA.DVR (VMware SVGA-II 2D)..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
$(ATIR128_DVR): kernel/drivers/graphics/ati_rage128_dvr.c kernel/include/driver.h \
		kernel/include/gfx.h kernel/include/gfx_driver.h kernel/include/pci.h \
		kernel/include/vesa.h kernel/include/rage128_engine.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando ATIR128.DVR (Rage Mobility M3 2D/cursor/overlay)..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(ATIR1283D_DVR): kernel/drivers/graphics/ati_rage128_3d_dvr.c \
		kernel/drivers/graphics/ati_rage128_cce_ucode.h kernel/include/driver.h \
		kernel/include/gfx.h kernel/include/gfx3d.h \
		kernel/include/gfx3d_driver.h kernel/include/pci.h \
		kernel/include/rage128_engine.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando ATIR1283D.DVR (CCE/PM4 fixed-function)..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(VMWARESVGA3D_DVR): kernel/drivers/graphics/vmware_svga3d.c kernel/include/driver.h \
		kernel/include/gfx3d.h kernel/include/gfx3d_driver.h \
		kernel/include/svga3d_protocol.h kernel/include/svga_transport.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando VMWARESVGA3D.DVR (extension 3D)..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(VIRTIOGPU_DVR): kernel/drivers/graphics/virtio_gpu_dvr.c kernel/drivers/graphics/virtio_gpu_gallium.inc \
        kernel/include/gfx3d.h kernel/include/gfx3d_driver.h \
        ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h kernel/include/driver.h \
		kernel/include/gfx.h kernel/include/gfx_driver.h kernel/include/pci.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando VIRTIOGPU.DVR (VirtIO GPU moderno)..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(INTELGMA_DVR): kernel/drivers/graphics/intel_gma9xx_dvr.c \
		kernel/drivers/graphics/intel_i830_3d.inc \
		kernel/drivers/graphics/intel_i830_regs.h kernel/drivers/graphics/intel_i830_common.h \
		kernel/include/driver.h kernel/include/gfx.h kernel/include/gfx_driver.h \
		kernel/include/pci.h kernel/include/vesa.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando INTELGMA.DVR (GMA 9xx)..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(THREEC90X_DVR): kernel/drivers/network/3c90x.c kernel/include/driver.h \
		kernel/include/network.h kernel/include/pci.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver Ethernet externo 3c90x..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(NETSTACK_DVR): kernel/drivers/network/netstack.c kernel/include/driver.h \
		kernel/include/network.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando pila IPv4 externa..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

$(RTL8139_DVR): kernel/drivers/network/rtl8139.c kernel/include/driver.h \
		kernel/include/network.h kernel/include/pci.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando driver Ethernet externo RTL8139..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

build/libs/bearssl/%.o: libs/bearssl/%.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) -Ikernel/libc/include -Ikernel -Ilibs/bearssl/inc -Ilibs/bearssl/src \
		-DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 \
		-DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 \
		-DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 -c $< -o $@

$(BEARSSL_A): $(BEARSSL_OBJS) | build
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TLS_RAW_OBJ): kernel/drivers/network/tls.c kernel/drivers/network/tls_trust_anchors.c \
		kernel/include/driver.h kernel/include/network.h libs/bearssl/inc/bearssl.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando proveedor TLS externo BearSSL..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -Ilibs/bearssl/inc \
		-DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 \
		-DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 \
		-DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 -c $< -o $@

$(TLS_DVR): $(TLS_RAW_OBJ) $(BEARSSL_A) $(LIBC_A)
	$(LD) -m elf_i386 -r $(TLS_RAW_OBJ) $(BEARSSL_A) $(LIBC_A) -o $@
	@echo "[OK]   Proveedor TLS externo: $@"



$(SETUP_OBJ): programs/setup.c programs/system_tools_common.h sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando asistente grafico SETUP.BEX..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@
	@echo "[OK]   Setup object: $@"

$(CALENDAR_OBJ): programs/calendar.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando calendar como programa separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   Calendar object: $@"

$(APITEST_OBJ): programs/apitest.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando apitest como programa separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   APITest object: $@"

$(SCANDISK_OBJ): programs/scandisk.c sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando ScanDisk como programa Ring 3..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@
	@echo "[OK]   ScanDisk object: $@"

$(WINE_OBJ): programs/wine.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando lanzador PE/Win32..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   Wine/PE launcher object: $@"

$(WINE_GL_LIB): ports/wine/graphics/glx.c ports/wine/graphics/blesmesa.h libs/Mesa-3.5/include/GL/osmesa.h ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h
	@mkdir -p $(dir $@)
	gcc -m32 -O2 -fPIC -shared -Wall -Wextra $< -Wl,-soname,libGL.so.1 -Wl,-z,defs -l:libX11.so.6 -ldl -lpthread -o $@

.PHONY: wine-gallium
wine-gallium: $(WINE_GL_LIB) $(WINE_GALLIUM_FILES)

$(WINE_TINYX_BINARY): ports/tinyx/scripts/build-i386.sh $(WINE_TINYX_SOURCES)
	TINYX_OUTPUT="$(abspath $@)" bash ports/tinyx/scripts/build-i386.sh

# One build produces the backend, dependency closure and redistribution notices.
$(WINE_GALLIUM_FILES) &: ports/wine/graphics/build-gallium.py ports/wine/graphics/virgl-winsys.c ports/wine/graphics/mesa-blesk-frontend.patch ports/wine/graphics/mesa-blesk-staging.patch ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h
	$(PYTHON) ports/wine/graphics/build-gallium.py --package $(if $(WINE_GALLIUM_CACHE),--cache "$(WINE_GALLIUM_CACHE)",)

$(WINE_MESA_LIB): ports/wine/graphics/build-mesa.py ports/wine/graphics/gfx3d-linux.c $(MESA35_SOURCES) ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h
	python3 ports/wine/graphics/build-mesa.py
	@mkdir -p $(dir $@)
	cp build/wine/graphics/libbkmesa.so.1 $@

$(WINE_READY_CLIENT): ports/wine/tools/wine-ready.c
	@mkdir -p $(dir $@)
	i686-w64-mingw32-gcc -Os -Wall -Wextra -Werror -nostdlib -mwindows -s -Wl,--entry,_wine_ready_entry@0 $< -o $@ -luser32 -lgdi32 -lkernel32

$(WINE_DESKTOP_LAUNCHER): ports/tinyx/wine-launch.c ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h $(WINE_READY_CLIENT)
	@mkdir -p $(dir $@)
	gcc -m32 -O2 -Wall -Wextra -no-pie -Wl,--disable-new-dtags -Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2 -Wl,-rpath,/WINEGATE/lib $< -o $@ -L/usr/lib/i386-linux-gnu -l:libX11.so.6 -l:libXfixes.so.3

$(WINE_TOOL_LAUNCHER): ports/wine/tools/wine-tool.c
	@mkdir -p $(dir $@)
	gcc -m32 -O2 -Wall -Wextra -no-pie -Wl,--disable-new-dtags -Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2 -Wl,-rpath,/WINEGATE/lib $< -o $@

$(WINE_RETRO_ICONS): tools/build_wine_retro_icons.py assets/graphics/GRAPHICS.PAK
	$(PYTHON) $< $@

build/programs/netsurf/vendor/libcss/%.o: programs/netsurf/vendor/libcss/%.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando libcss: $<..."
	$(CC) -Iprograms/netsurf/vendor/libcss/src $(NETSURF_CFLAGS) -c $< -o $@

build/programs/netsurf/%.o: programs/netsurf/%.c programs/netsurf/platform.h \
		programs/netsurf/html_parser.h programs/netsurf/dom_tree.h \
		programs/netsurf/layout.h programs/netsurf/css_engine.h \
		sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando frontend NetSurf/Hubbub: $<..."
	$(CC) $(NETSURF_CFLAGS) -c $< -o $@

$(NETSURF_OBJ): $(NETSURF_RAW_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r $^ -o $@
	@echo "[OK]   NetSurf frontend + Hubbub DOM + libcss layout: $@"

netsurf: $(NETSURF_OBJ)

$(APPEAR_CPL_RAW_OBJ): system/control/appearance.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando appearance..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(SCREENSAVER_MESA_INCLUDES) -c $< -o $@

$(APPEAR_CPL_OBJ): $(APPEAR_CPL_RAW_OBJ) $(MESA35_A)
	@echo "[LD]   Asociando appearance con Mesa 3.5..."
	$(LD) -m elf_i386 -r $< $(MESA35_A) -o $@
	@echo "[OK]   Appearance CPL object: $@"

$(DISPLAY_CPL_RAW_OBJ): system/control/display.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando display..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(SCREENSAVER_MESA_INCLUDES) -c $< -o $@

$(DISPLAY_CPL_OBJ): $(DISPLAY_CPL_RAW_OBJ) $(MESA35_A)
	@echo "[LD]   Asociando display con Mesa 3.5..."
	$(LD) -m elf_i386 -r $< $(MESA35_A) -o $@
	@echo "[OK]   Display CPL object: $@"

$(GEARS_RAW_OBJ): programs/gears.c sdk/include/bleskernos_tinygl.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando gears..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -Isdk/include -c $< -o $@

$(GEARS_OBJ): $(GEARS_RAW_OBJ) $(SDK_TINYGL_A)
	@echo "[LD]   Asociando gears con TinyGL..."
	$(LD) -m elf_i386 -r $< $(SDK_TINYGL_A) -o $@
	@echo "[OK]   Gears object: $@"

$(SCREENSAVERD_OBJ): system/services/screensaverd.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando screensaverd como programa separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   ScreenSaver daemon object: $@"

$(VISION_IMAGE_COMPAT_OBJ): system/desktop/deskmanager.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando compatibilidad grafica legacy de Vision..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(VISION_DESKTOP_RENAMES) -c $< -o $@

$(VISION_MAIN_OBJ): system/services/vision.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando nucleo de Vision Ring 3..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(VISION_DESKTOP_RENAMES) -c $< -o $@

$(VISION_DESKMANAGER_OBJ): system/desktop/deskmanager.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Moviendo DeskManager a Vision..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(VISION_DESKTOP_RENAMES) -DBLES_VISION_SERVICE=1 -c $< -o $@

$(VISION_DESKBAR_OBJ): system/desktop/deskbar.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Moviendo Deskbar a Vision..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(VISION_DESKTOP_RENAMES) -DBLES_VISION_SERVICE=1 -c $< -o $@

$(VISION_OBJ): $(VISION_COMPONENT_OBJS)
	@mkdir -p $(dir $@)
	@echo "[LD]   Asociando VISION.BEX (servicio + escritorio + Deskbar)..."
	$(LD) -m elf_i386 -r --gc-sections -u bleskernos_program_main $(VISION_COMPONENT_OBJS) -o $@
	@echo "[OK]   Vision service object: $@"

build/system/services/printspool_raw.o: system/services/printspool.c system/services/print_zjs.inc sdk/include/bleskernos_api.h sdk/include/bleskernos_print.h third_party/jbigkit/jbig.h libs/src/font8x8_basic.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/system/services/jbig.o build/system/services/jbig_ar.o: build/system/services/%.o: third_party/jbigkit/%.c third_party/jbigkit/jbig.h third_party/jbigkit/jbig_ar.h | build
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -Dmemchr=bk_print_memchr -Dabort=bk_print_jbig_abort -ffunction-sections -fdata-sections -c $< -o $@
$(PRINTSPOOL_OBJ): build/system/services/printspool_raw.o build/system/services/jbig.o build/system/services/jbig_ar.o
	$(LD) -m elf_i386 -r --gc-sections -u bleskernos_program_main $(filter %.o,$^) -o $@

$(PRINT_TEST_RAW_OBJ): programs/print_test.c \
		sdk/include/bleskernos_api.h sdk/include/bleskernos_print.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando prueba de impresion..."
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(PRINT_TEST_OBJ): $(PRINT_TEST_RAW_OBJ) $(SDK_A)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r $< $(SDK_A) -o $@
	@echo "[OK]   Prueba de impresion: $@"

$(SSLOGO_OBJ): system/screensavers/ss_logo.c | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando ss_logo como programa separado..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
	@echo "[OK]   SSLogo object: $@"

$(SSPIPES_RAW_OBJ): system/screensavers/ss_pipes.c system/screensavers/ss_mesa.h system/screensavers/ss_scale.h kernel/include/api.h gui/gui.h sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando ss_pipes..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(SCREENSAVER_MESA_INCLUDES) -DSS_PIPES_EXTERNAL_ENTRY -c $< -o $@

$(SSPIPES_OBJ): $(SSPIPES_RAW_OBJ) $(MESA35_A)
	@echo "[LD]   Asociando ss_pipes con Mesa 3.5..."
	$(LD) -m elf_i386 -r $< $(MESA35_A) -o $@
	@echo "[OK]   SSPipes object: $@"

$(SSBALLS_RAW_OBJ): system/screensavers/ss_balls.c system/screensavers/ss_mesa.h system/screensavers/ss_scale.h kernel/include/api.h gui/gui.h sdk/include/bleskernos_api.h | build
	@mkdir -p $(dir $@)
	@echo "[GCC]  Compilando ss_balls..."
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(SCREENSAVER_MESA_INCLUDES) -DSS_BALLS_EXTERNAL_ENTRY -c $< -o $@

$(SSBALLS_OBJ): $(SSBALLS_RAW_OBJ) $(MESA35_A)
	@echo "[LD]   Asociando ss_balls con Mesa 3.5..."
	$(LD) -m elf_i386 -r $< $(MESA35_A) -o $@
	@echo "[OK]   SSBalls object: $@"


# ── Imagen de disco ───────────────────────────────────────────────────────────
$(DISK_IMG):
$(ATA_IMG): $(DRIVER_OBJS) $(SELFHOST_STAMP) \
	mk/selfhost.mk kernel/arch/x86/linker_selfhost.ld docs/build/self-hosting.md \
	$(wildcard sdk/examples/selfhost/*)
$(ATA_IMG): $(WINE_GALLIUM_FILES) tools/wine_theme_defaults.py tools/wine_app_defaults.py

$(DISK_IMG): $(BOOT1_BIN) $(BOOT2_BIN) $(KERNEL_BIN) $(EXTERNAL_APP_OBJS) $(TINYGL_A) $(LIBC_A) config/desktop/Desktop.INI config/desktop/Associations.INI tools/build_fat_floppy.py $(wildcard assets/icons/*.BMP) $(wildcard assets/icons/*.bmp) $(wildcard assets/wallpapers/*) assets/gif/abount.gif
	@echo "[IMG]  Creando imagen de disco..."
	@KERNEL_BIN_SECTORS=$$(( ($$(wc -c < $(KERNEL_BIN)) + 511) / 512 )); \
	if [ $$KERNEL_BIN_SECTORS -gt $(KERNEL_STAGE2_SECTORS) ]; then \
		echo "[ERROR] Kernel ocupa $$KERNEL_BIN_SECTORS sectores, pero stage2 carga $(KERNEL_STAGE2_SECTORS)."; \
		exit 1; \
	fi; \
	dd if=/dev/zero of=$@ bs=512 count=$(FLOPPY_TOTAL_SECTORS) 2>/dev/null
	dd if=$(BOOT1_BIN) of=$@ bs=512 seek=0 conv=notrunc 2>/dev/null
	dd if=$(BOOT2_BIN) of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=9 conv=notrunc 2>/dev/null
	BLES_EDITION=$(EDITION) $(PYTHON) tools/build_fat_floppy.py $@ $(SHELL_OBJ) $(FILEBROWSER_OBJ) $(TEXTEDITOR_OBJ) $(CALCULATOR_OBJ) $(MIDAMP_OBJ) $(PROCESSMANAGER_OBJ) $(CALENDAR_OBJ) config/desktop/Desktop.INI assets/icons assets/gif/abount.gif config/desktop/Associations.INI $(FAT_RESERVED_SECTORS) $(SCREENSAVERD_OBJ) $(SSLOGO_OBJ) $(SSPIPES_OBJ) "" $(TINYGL_A) $(LIBC_A) $(CONTROL_OBJS) $(SSBALLS_OBJ)
	@echo "[OK]   Imagen: $$(wc -c < $@) bytes"

$(ATA_IMG): $(FONT_FILES) $(BOOT1_FAT32_BIN) $(BOOT2_BIN) $(KERNEL_BIN) $(EXTERNAL_APP_OBJS) $(DRIVER_OBJS) $(EDITION_EXTRA_DEPS) $(TINYGL_A) $(MESA35_A) $(LIBC_A) $(SDK_A) $(SDK_POSIX_A) $(SDK_LINUX_A) $(SDK_SDL2_A) $(SDK_MATH_A) config/desktop/Desktop.INI config/desktop/Associations.INI tools/build_fat32_ata.py tools/wine_display_defaults.py tools/bfs4.py tools/build_fat_floppy.py tools/build_icons_pak.py tools/build_bvi_pak.py assets/graphics/GRAPHICS.PAK assets/graphics/GRAPHICS.CSV assets/graphics/KDE1SRC.TGZ third_party/kde1/README.md $(wildcard assets/icons/*.BMP) $(wildcard assets/icons/*.bmp) $(wildcard assets/icons-bvi/*.bvi) $(wildcard assets/wallpapers/*) assets/cursors/INDEX.JSON $(wildcard assets/cursors/*) assets/gif/abount.gif assets/sounds/startup.wav system/leng/ESPANOL.LNG system/leng/ENGLISH.LNG system/leng/ITALIANO.LNG $(wildcard system/printers/*.BPD) docs/platform/printing.md docs/platform/usbclass.md $(WINE_DESKTOP_LAUNCHER) $(WINE_TOOL_LAUNCHER) $(WINE_GL_LIB) $(WINE_MESA_LIB) $(WINE_RETRO_ICONS) $(WINE_RUNTIME_FILES) $(WINE_PREFIX_MANIFEST) $(WINE_LICENSE_FILES)
	@echo "[ATA]  Creando imagen BFS4 ATA..."
	BLES_FS=bfs4 BLES_EDITION=$(EDITION) $(PYTHON) tools/build_fat32_ata.py $@ $(SHELL_OBJ) $(FILEBROWSER_OBJ) $(TEXTEDITOR_OBJ) $(CALCULATOR_OBJ) $(MIDAMP_OBJ) $(PROCESSMANAGER_OBJ) $(CALENDAR_OBJ) config/desktop/Desktop.INI assets/icons assets/gif/abount.gif config/desktop/Associations.INI $(FAT_RESERVED_SECTORS) $(SCREENSAVERD_OBJ) $(SSLOGO_OBJ) $(SSPIPES_OBJ) "" $(BOOT1_FAT32_BIN) $(BOOT2_BIN) $(KERNEL_BIN) $(KERNEL_STAGE2_SECTORS) $(TINYGL_A) $(LIBC_A) $(CONTROL_OBJS) $(SSBALLS_OBJ) $(MESA35_A)
	@echo "[OK]   ATA: $$(wc -c < $@) bytes"

$(USB_IMG): $(ATA_IMG) $(MBR_BIN) tools/build_usb_image.py
	@echo "[USB]  Creando imagen USB-HDD con MBR..."
	$(PYTHON) tools/build_usb_image.py $@ $(ATA_IMG) $(MBR_BIN)
	@echo "[OK]   USB: $$(wc -c < $@) bytes"

# Disco VMware con MBR/particion activa. monolithicSparse evita inflar el
# build a 65 MiB y compat6 funciona tanto en VMware Player como Workstation.
$(VMWARE_IMG): $(USB_IMG)
	@command -v $(QEMU_IMG) >/dev/null 2>&1 || { \
		echo "[ERROR] qemu-img es necesario para crear el disco VMware"; exit 1; \
	}
	@echo "[VMDK] Creando disco VMware IDE..."
	$(QEMU_IMG) convert -f raw -O vmdk \
		-o subformat=monolithicSparse,compat6=on,adapter_type=ide \
		$(USB_IMG) $@
	@echo "[OK]   VMware: $@ ($$(wc -c < $@) bytes)"

$(VMWARE_VMX): tools/build_vmware_config.py | $(VMWARE_IMG)
	$(PYTHON) tools/build_vmware_config.py $(VMWARE_IMG) $@

vmware-image: $(VMWARE_IMG) $(VMWARE_VMX)

# Ejecutar en QEMU (requiere qemu-system-i386)
run: $(ATA_IMG)
	@echo "[QEMU] Iniciando BleskernOS..."
	$(QEMU) \
		-cpu qemu32,+rdrand \
		-drive file=$(ATA_IMG),format=raw,if=ide \
		-device rtl8139,netdev=bknet0 \
		-netdev user,id=bknet0 \
		$(QEMU_CDROM_ARGS) \
		$(QEMU_AUDIO_ARGS) \
		-boot c \
		-m 128M \
		-no-reboot \
		-no-shutdown

# VMware SVGA-II: framebuffer 32 bpp + FIFO 2D (UPDATE/FILL/COPY).
run-svga: $(ATA_IMG)
	@echo "[QEMU] Iniciando prueba VMware SVGA-II..."
	$(QEMU) \
		-cpu qemu32,+rdrand \
		-drive file=$(ATA_IMG),format=raw,if=ide \
		-vga vmware \
		-boot c \
		-m 128M \
		-serial stdio \
		-no-reboot \
		-no-shutdown

# VirtIO GPU 2D moderno. La variante virtio-vga conserva compatibilidad VGA
# para el bootsplash y luego VIRTIOGPU.DVR cambia al scanout nativo VirtIO.
run-virtio-gpu: $(ATA_IMG)
	$(PYTHON) tools/run_qemu_virtio.py --qemu "$(QEMU)" --image "$(ATA_IMG)" $(if $(CDROM_IMG),--cdrom "$(CDROM_IMG)",)

# VirGL requiere OpenGL en el host y virglrenderer en la compilacion de QEMU.
# SDL/OpenGL por defecto; incluye GFX3D nativo y Gallium programable para Wine.
run-virtio-gpu-gl: $(ATA_IMG)
	$(PYTHON) tools/run_qemu_virtio.py --qemu "$(QEMU)" --image "$(ATA_IMG)" --gl $(if $(CDROM_IMG),--cdrom "$(CDROM_IMG)",)

# Entorno reproducible para verificar RTL8139, TCP/IP y TLS. RDRAND es la
# fuente de entropia exigida por TLS.DVR; SLiRP entrega DHCP/DNS/Internet.
run-net: $(ATA_IMG)
	@echo "[QEMU] Iniciando prueba completa RTL8139 + red en SMP..."
	$(QEMU) \
		-cpu qemu32,+rdrand \
		-smp 4,sockets=1,cores=4,threads=1 \
		-drive file=$(ATA_IMG),format=raw,if=ide \
		-device rtl8139,netdev=bknet0 \
		-netdev user,id=bknet0 \
		-boot c \
		-m 256M \
		-serial stdio \
		-no-reboot \
		-no-shutdown

run-ac97: $(ATA_IMG)
	@echo "[QEMU] Iniciando prueba AC97 ICH-compatible..."
	$(QEMU) \
		-drive file=$(ATA_IMG),format=raw,if=ide \
		-device AC97 \
		-boot c \
		-m 128M \
		-serial stdio \
		-no-reboot \
		-no-shutdown

# Ejecutar con salida serial (útil para debugging)
run-usb: $(USB_IMG)
	@echo "[QEMU] Iniciando BleskernOS desde imagen USB-HDD..."
	$(QEMU) \
		-device usb-ehci,id=ehci \
		-drive id=usbdisk,file=$(USB_IMG),format=raw,if=none \
		-device usb-storage,drive=usbdisk,bootindex=1 \
		$(QEMU_CDROM_ARGS) \
		$(QEMU_AUDIO_ARGS) \
		-boot c \
		-m 128M \
		-no-reboot \
		-no-shutdown

# Ejecutar el HCD UHCI contra un PIIX4 (8086:7112) emulado. El sistema
# arranca por ATA y la imagen USB queda como dispositivo secundario.
run-uhci: $(ATA_IMG) $(USB_IMG)
	@echo "[QEMU] Iniciando prueba PIIX4 UHCI..."
	$(QEMU) \
		-M pc,usb=off \
		-device piix4-usb-uhci,id=uhci \
		-drive file=$(ATA_IMG),format=raw,if=ide \
		-drive id=usbdisk,file=$(USB_IMG),format=raw,if=none \
		-device usb-storage,bus=uhci.0,drive=usbdisk \
		-boot c \
		-m 128M \
		-snapshot \
		-no-reboot \
		-no-shutdown

run-debug: $(ATA_IMG)
	$(QEMU) \
		-drive file=$(ATA_IMG),format=raw,if=ide \
		$(QEMU_CDROM_ARGS) \
		$(QEMU_AUDIO_ARGS) \
		-boot c \
		-m 128M \
		-serial stdio \
		-no-reboot

clean:
	@echo "[CLEAN] Limpiando build..."
	rm -rf build/
	@echo "[OK]"

# Rebuild C objects when one of their kernel/program headers changes.
ALL_C_DEPS_EXTRA := $(SSPIPES_RAW_OBJ:.o=.d) $(SSBALLS_RAW_OBJ:.o=.d)
ALL_C_DEPS := $(KERNEL_OBJS:.o=.d) $(LIBC_OBJS:.o=.d) $(TINYGL_OBJS:.o=.d) $(MESA35_OBJS:.o=.d) $(EXTERNAL_APP_OBJS:.o=.d) $(VISION_COMPONENT_OBJS:.o=.d) $(SETUP_OBJ:.o=.d) $(COMMAND_RAW_OBJS:.o=.d) $(COMMAND_COMMON_OBJ:.o=.d) $(APPEAR_CPL_RAW_OBJ:.o=.d) $(DISPLAY_CPL_RAW_OBJ:.o=.d) $(SDK_SYSCALL_OBJ:.o=.d) $(SDK_THREAD_OBJ:.o=.d) $(SDK_TINYGL_OBJ:.o=.d) $(SDK_MESA_OBJ:.o=.d) $(PHASE2_RAW_OBJ:.o=.d)
ifndef SELFHOST_NATIVE
-include $(ALL_C_DEPS) $(ALL_C_DEPS_EXTRA)
endif


# =============================================================================
# Installer CD ISO9660 (El Torito)
# =============================================================================
ISO_IMAGE    := build/bleskernos-cd-user.iso
ISO_ROOT     := build/cdroot-user
ISO_BOOT_IMG := build/cdboot-user.img
ISO_BOOT_SECTORS := 2880
INSTALL_TARGET ?= build/installer-target.img
INSTALL_TARGET_MB ?= 1536

.PHONY: iso iso-user run-iso run-installed installer-target reset-installer-target clean-iso

# La imagen El Torito contiene Stage 1 especial, Stage 2 y el kernel.
# El magic de Stage 1 fuerza el instalador en modo texto; discos normales no lo hacen.
$(ISO_BOOT_IMG): $(BOOT1_CD_BIN) $(BOOT2_BIN) $(KERNEL_BIN) | build
	@echo "[CD]   Creando imagen de arranque El Torito..."
	@SECTORS=$$(( ($$(wc -c < $(KERNEL_BIN)) + 511) / 512 )); \
	if [ $$((9 + SECTORS)) -gt $(ISO_BOOT_SECTORS) ]; then \
		echo "[ERROR] Kernel demasiado grande para cdboot.img: $$SECTORS sectores."; \
		exit 1; \
	fi
	@dd if=/dev/zero of=$@ bs=512 count=$(ISO_BOOT_SECTORS) status=none
	@dd if=$(BOOT1_CD_BIN) of=$@ bs=512 seek=0 conv=notrunc status=none
	@dd if=$(BOOT2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=9 conv=notrunc status=none
	@echo "[OK]   Boot image: $@"

$(ISO_IMAGE): $(ATA_IMG) $(ISO_BOOT_IMG) $(SETUP_OBJ) $(MBR_BIN) tools/build_cd_bkl_installer.py
	@command -v mcopy >/dev/null 2>&1 || { \
		echo "[ERROR] Falta mcopy (paquete mtools)."; \
		echo "        sudo apt install mtools xorriso"; exit 1; }
	@command -v xorriso >/dev/null 2>&1 || { \
		echo "[ERROR] Falta xorriso."; \
		echo "        sudo apt install mtools xorriso"; exit 1; }
	@echo "[CD]   Preparando instalador BKL3 por componentes..."
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/.full-root $(ISO_ROOT)/INSTALL
	@$(PYTHON) tools/bfs4.py extract $(ATA_IMG) $(ISO_ROOT)/.full-root/
	@$(PYTHON) tools/build_cd_bkl_installer.py \
		--source-root $(ISO_ROOT)/.full-root \
		--output $(ISO_ROOT)/INSTALL/BOOTSTRAP \
		--setup-exe $(SETUP_OBJ)
	@rm -rf $(ISO_ROOT)/.full-root
	@cp $(ISO_BOOT_IMG) $(ISO_ROOT)/BOOT.IMG
	@cp $(BOOT1_FAT32_BIN) $(ISO_ROOT)/INSTALL/BOOTF32.BIN
	@cp $(MBR_BIN) $(ISO_ROOT)/INSTALL/MBR.BIN
	@cp $(BOOT2_BIN) $(ISO_ROOT)/INSTALL/STAGE2.BIN
	@cp $(KERNEL_BIN) $(ISO_ROOT)/INSTALL/KERNEL.BIN
	@printf 'BlesKernOS 1.0 User Installer CD\nText bootstrap + graphical first boot Setup using BKL3 packages.\n' > $(ISO_ROOT)/CDINFO.TXT
	@echo "[ISO]  Generando $(ISO_IMAGE)..."
	@xorriso -as mkisofs -quiet \
		-V "BLES_USER" \
		-iso-level 3 -J -R \
		-b BOOT.IMG -c BOOT.CAT \
		$(ISO_ROOT) -o $(ISO_IMAGE)
	@echo "[OK]   Installer CD BKL3: $(ISO_IMAGE)"

iso: $(ISO_IMAGE)

iso-user:
	@$(MAKE) iso

installer-target: $(INSTALL_TARGET)

$(INSTALL_TARGET): | build
	@echo "[IMG]  Creando disco vacio de $(INSTALL_TARGET_MB) MiB para Setup..."
	@truncate -s $(INSTALL_TARGET_MB)M $@
	@echo "[OK]   Target: $@"

reset-installer-target:
	@rm -f $(INSTALL_TARGET)
	@$(MAKE) $(INSTALL_TARGET) INSTALL_TARGET=$(INSTALL_TARGET) INSTALL_TARGET_MB=$(INSTALL_TARGET_MB)

run-iso: $(ISO_IMAGE) $(INSTALL_TARGET)
	@echo "[QEMU] Arrancando instalador CD User..."
	@echo "[QEMU] Disco destino: $(INSTALL_TARGET)"
	$(QEMU) \
		-cpu qemu32,+rdrand \
		-drive file=$(INSTALL_TARGET),format=raw,if=ide \
		-drive file=$(ISO_IMAGE),media=cdrom,if=ide,readonly=on \
		-device rtl8139,netdev=bknet0 \
		-netdev user,id=bknet0 \
		$(QEMU_AUDIO_ARGS) \
		-boot order=dc,once=d -m 256M -serial stdio -no-shutdown

run-installed: $(INSTALL_TARGET)
	@echo "[QEMU] Arrancando instalacion desde $(INSTALL_TARGET)..."
	$(QEMU) \
		-cpu qemu32,+rdrand \
		-drive file=$(INSTALL_TARGET),format=raw,if=ide \
		-device rtl8139,netdev=bknet0 \
		-netdev user,id=bknet0 \
		$(QEMU_AUDIO_ARGS) \
		-boot c -m 256M -serial stdio -no-reboot -no-shutdown

clean-iso:
	@rm -rf build/cdroot-user build/cdroot-developer \
		build/cdboot-user.img build/cdboot-developer.img \
		build/bleskernos-cd-user.iso build/bleskernos-cd-developer.iso
	@echo "[ISO] Artefactos del Installer CD eliminados."

# =============================================================================
# Instalador multidisquete BKL3
# =============================================================================
FLOPPY_INSTALL_DIR := build/img
FLOPPY_INSTALL_STAMP := $(FLOPPY_INSTALL_DIR)/.floppy-installer.stamp

.PHONY: floppy-installer _floppy-installer clean-floppy-installer run-floppy-installer

floppy-installer:
	@$(MAKE) --no-print-directory EDITION=user _floppy-installer

_floppy-installer: $(FLOPPY_INSTALL_STAMP)
	@cat $(FLOPPY_INSTALL_DIR)/MANIFEST.TXT

$(FLOPPY_INSTALL_STAMP): $(ATA_IMG) $(BOOT1_CD_BIN) \
		$(BOOT1_FAT32_BIN) $(BOOT2_BIN) $(KERNEL_BIN) \
		tools/build_floppy_installer.py
	@mkdir -p $(FLOPPY_INSTALL_DIR)
	@echo "[BKL3] Generando instalador multidisquete en $(FLOPPY_INSTALL_DIR)/..."
	@$(PYTHON) tools/build_floppy_installer.py \
		--ata-image $(ATA_IMG) \
		--boot1-installer $(BOOT1_CD_BIN) \
		--boot-fat32 $(BOOT1_FAT32_BIN) \
		--stage2 $(BOOT2_BIN) \
		--kernel $(KERNEL_BIN) \
		--output $(FLOPPY_INSTALL_DIR)
	@touch $@

run-floppy-installer: floppy-installer $(INSTALL_TARGET)
	@echo "[QEMU] Arrancando disk01.img; cambie medios desde el monitor de QEMU."
	$(QEMU) \
		-cpu qemu32,+rdrand \
		-drive file=$(INSTALL_TARGET),format=raw,if=ide \
		-drive file=$(FLOPPY_INSTALL_DIR)/disk01.img,format=raw,if=floppy \
		-boot a -m 256M -serial stdio -no-shutdown

clean-floppy-installer:
	@rm -rf $(FLOPPY_INSTALL_DIR)
	@echo "[BKL3] Imagenes de disquete eliminadas."

# >>> BLES CODEX PRO >>>
# Optional IDE/development-image integration.
-include mk/codex-pro.mk
# <<< BLES CODEX PRO <<<

# Standalone extras are opt-in and never enter the standard image.
.PHONY: extras
extras: $(TUXRACER_OBJ)

# Incremental offline update preserves the installed Wine prefix and user files.
.PHONY: user-update
user-update: $(KERNEL_BIN) $(WINE_GALLIUM_FILES) $(WINE_TINYX_BINARY)
user-update: $(VIEWER_OBJ) $(FONT_FILES) $(FONT_OBJ) $(BOOT2_BIN) $(VISION_OBJ) $(FILEBROWSER_OBJ) $(CONTROL_OBJS) $(WINE_DESKTOP_LAUNCHER) $(WINE_TOOL_LAUNCHER) $(WINE_OBJ) $(ISO9660_DVR) $(AC97_DVR) $(VIRTIOGPU_DVR) $(VMWARESVGA3D_DVR) $(WINE_GL_LIB) $(WINE_MESA_LIB)
	$(PYTHON) tools/update_user_image.py --image build/bleskernos-ata-user.img

.PHONY: fonts-regenerate
fonts-regenerate:
	$(PYTHON) tools/build_fonts.py

VIEWER_SOURCES := $(wildcard programs/viewer/*.c programs/viewer/lib/* programs/viewer/third_party/compat/*) programs/viewer/third_party/pl_mpeg.h
$(VIEWER_OBJ): $(VIEWER_SOURCES) programs/viewer/Makefile sdk/include/bleskernos_api.h sdk/include/bleskernos_theme.h | build
	$(MAKE) -C programs/viewer ROOT=../..
	cp programs/viewer/build/VIEWER.O $@

# Vision engine objects execute their raster/window/composition algorithms in CPL3.
build/system/services/engine_%.o: gui/%.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(VISION_DESKTOP_RENAMES) -DBLES_VISION_SERVICE=1 -c $< -o $@
build/system/services/vision_engine.o: system/services/vision_engine.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(VISION_DESKTOP_RENAMES) -DBLES_VISION_SERVICE=1 -c $< -o $@

# Fault-injected service is installed only by the isolated guest test runner.
build/tests/vision_fault_engine.o: tests/userland/vision_fault_engine.c system/services/vision_engine.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) $(VISION_DESKTOP_RENAMES) -DBLES_VISION_SERVICE=1 -c $< -o $@
build/tests/vision_fault.o: build/tests/vision_fault_engine.o $(VISION_COMPONENT_OBJS)
	$(LD) -m elf_i386 -r --gc-sections -u bleskernos_program_main $(filter-out build/system/services/vision_engine.o,$(VISION_COMPONENT_OBJS)) $< -o $@

build/system/services/vision_memory.o: system/services/vision_memory.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
build/system/services/vision_string.o: kernel/libc/string.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@

build/system/services/vision_raster_api.o: system/services/vision_raster_api.c kernel/api/gui_raster_api.inc | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -DBLES_VISION_SERVICE=1 -c $< -o $@

build/system/services/vision_windows.o: system/services/vision_windows.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -DBLES_VISION_SERVICE=1 -c $< -o $@

# Dialog policy and painting belong to the graphical server's user threads.
build/system/services/vision_file_dialog.o: kernel/ui/file_dialog.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -DBLES_VISION_SERVICE=1 -c $< -o $@
build/system/services/vision_about_dialog.o: kernel/ui/about_dialog.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -DBLES_VISION_SERVICE=1 -c $< -o $@

# Each chip has a dedicated source; only safety/lifecycle helpers are shared.
define LEGACY_NET_RULE
build/system/drivers/$(1).DVR: kernel/drivers/network/$(2).c kernel/drivers/network/legacy_net.h kernel/drivers/network/legacy_net_runtime.inc kernel/drivers/legacy_io.h | build
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CC_FLAGS) $$(INCLUDE_FLAGS) -c $$< -o $$@
endef
$(eval $(call LEGACY_NET_RULE,E1000,e1000))
$(eval $(call LEGACY_NET_RULE,PCNET,pcnet))
$(eval $(call LEGACY_NET_RULE,NE2KPCI,ne2k_pci))
$(eval $(call LEGACY_NET_RULE,TULIP,tulip))
$(eval $(call LEGACY_NET_RULE,RHINE,rhine))
$(eval $(call LEGACY_NET_RULE,SIS900,sis900))
$(eval $(call LEGACY_NET_RULE,NATSEMI,natsemi))
$(eval $(call LEGACY_NET_RULE,RTL8169,rtl8169))
.PHONY: legacy-drivers
legacy-drivers: $(LEGACY_NET_OBJS)

define LEGACY_AUDIO_RULE
build/system/drivers/$(1).DVR: kernel/drivers/audio/$(2).c kernel/drivers/audio/legacy_pcm.h kernel/drivers/audio/legacy_pcm_runtime.inc kernel/drivers/legacy_io.h | build
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CC_FLAGS) $$(INCLUDE_FLAGS) -c $$< -o $$@
endef
$(eval $(call LEGACY_AUDIO_RULE,ES1370,es1370))
$(eval $(call LEGACY_AUDIO_RULE,CMIPCI,cmipci))
$(eval $(call LEGACY_AUDIO_RULE,VIA686,via686))
$(eval $(call LEGACY_AUDIO_RULE,CS4281,cs4281))
legacy-drivers: $(LEGACY_AUDIO_OBJS)

build/system/drivers/LEGACYFB.DVR: kernel/drivers/graphics/legacy_fb.c $(wildcard kernel/drivers/graphics/families/*.inc) | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
legacy-drivers: build/system/drivers/LEGACYFB.DVR

$(ATA_IMG): tools/build_print_sources.py $(wildcard system/firmware/*) third_party/jbigkit/COPYING third_party/jbigkit/UPSTREAM.json docs/platform/legacy-drivers.md docs/platform/legacy-driver-sources.json docs/driver-source-layout.md
.PHONY: test-legacy-drivers
test-legacy-drivers:
	python3 tests/drivers/test_legacy_engines.py
	python3 tests/drivers/test_legacy_fb.py
	python3 tests/drivers/test_hp_firmware.py
	python3 tests/drivers/test_usb_printer_id.py
	python3 tests/drivers/test_print_zjs.py
	python3 tests/drivers/test_family_drivers.py
	python3 tests/drivers/test_ohci_transport.py
	python3 tests/drivers/test_ehci_completion.py
	python3 tests/drivers/test_ps2_optional.py
	python3 tests/drivers/test_service_late_ready.py

# Text editor links the public print client locally into its Ring-3 executable.
build/programs/texteditor.raw.o: programs/texteditor.c sdk/include/bleskernos_print.h | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
$(TEXTEDITOR_OBJ): build/programs/texteditor.raw.o $(SDK_PRINT_OBJ)
	$(LD) -m elf_i386 -r $(filter %.o,$^) -o $@

# Class-based Ethernet covers all compliant CDC-ECM adapters.
build/system/drivers/CDC_ECM.DVR: kernel/drivers/network/cdc_ecm.c kernel/include/usb_core.h | build
	@mkdir -p $(dir $@)
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -c $< -o $@
$(eval $(call LEGACY_NET_RULE,E100,e100))

# New family source/license payloads are part of normal image rebuilds too.
$(ATA_IMG): tools/license_notices.py $(shell $(PYTHON) -B tools/license_notices.py --list)
$(ATA_IMG): tools/build_family_sources.py docs/platform/family-drivers.md docs/family-expansion-2026-10-13.md docs/platform/family-driver-sources.json docs/platform/framebuffer-device-catalog.json LICENSES/GPL-2.0.txt LICENSES/README.md third_party/pciids/COPYING third_party/linux-eepro100/eepro100.c
.PHONY: test-family-drivers framebuffer-catalog
test-family-drivers: test-legacy-drivers
framebuffer-catalog:
	python3 tools/generate_framebuffer_families.py
