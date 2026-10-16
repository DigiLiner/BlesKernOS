# Raspberry Pi 3 / ARM32 bootstrap for BlesKernOS.
# Kept independent from the x86 Makefile while the architecture boundary is
# being established.

CLANG       ?= clang
LD_LLD      ?= ld.lld
OBJCOPY     ?= llvm-objcopy

ARM_TARGET  := arm-none-eabi
ARM_CPU     := cortex-a7
ARM_CFLAGS  := --target=$(ARM_TARGET) -mcpu=$(ARM_CPU) -marm -mfloat-abi=soft \
               -ffreestanding -fno-builtin -nostdlib -nostdinc -Os -Wall \
               -Wextra -fno-stack-protector -fno-pic -fno-pie -ffunction-sections -fdata-sections \
               -DBLES_ARCH_ARM32_RPI3=1 -Ikernel/libc/include -I.
ARM_ASFLAGS := --target=$(ARM_TARGET) -mcpu=$(ARM_CPU) -marm -c
ARM_LDFLAGS := -m armelf -T arch/arm32/rpi3/linker.ld -nostdlib --gc-sections --allow-multiple-definition --error-limit=0
ARM_USER_CFLAGS := --target=$(ARM_TARGET) -mcpu=$(ARM_CPU) -marm -mfloat-abi=soft \
                   -ffreestanding -fno-builtin -nostdlib -nostdinc -Os -Wall \
                   -Wextra -fno-stack-protector -fno-pic -fno-pie \
                   -ffunction-sections -fdata-sections \
                   -fno-unwind-tables -fno-asynchronous-unwind-tables \
                   -DBLES_ARCH_ARM32_RPI3=1 -Ikernel/libc/include -I. -Isdk/include -Isystem/commands

ifneq ($(RPI3_BEX_SMOKE),)
ARM_CFLAGS += -DRPI3_BEX_SMOKE_TEST=1
endif

RPI3_DIR    := build/rpi3
RPI3_OBJS   := $(RPI3_DIR)/boot.o \
               $(RPI3_DIR)/vectors.o \
               $(RPI3_DIR)/task_switch.o \
               $(RPI3_DIR)/uart.o \
               $(RPI3_DIR)/exceptions.o \
               $(RPI3_DIR)/irq.o \
               $(RPI3_DIR)/timer.o \
               $(RPI3_DIR)/task_arm.o \
               $(RPI3_DIR)/syscall_arm.o \
               $(RPI3_DIR)/elf_arm.o \
               $(RPI3_DIR)/console_stage12.o \
               $(RPI3_DIR)/api_stage12.o \
               $(RPI3_DIR)/original_apps_compat.o \
               $(RPI3_DIR)/api_full.o \
               $(RPI3_DIR)/arm_div.o \
               $(RPI3_DIR)/arm_eabi.o \
               $(RPI3_DIR)/shell_stage12.o \
               $(RPI3_DIR)/cache.o \
               $(RPI3_DIR)/mailbox.o \
               $(RPI3_DIR)/framebuffer.o \
               $(RPI3_DIR)/audio_pwm.o \
               $(RPI3_DIR)/sound_core.o \
               $(RPI3_DIR)/usb_dwc2_hid.o \
               $(RPI3_DIR)/input_stage9.o \
               $(RPI3_DIR)/gfx_rpi.o \
               $(RPI3_DIR)/ui_stage8.o \
               $(RPI3_DIR)/sd.o \
               $(RPI3_DIR)/mmu.o \
               $(RPI3_DIR)/physmem.o \
               $(RPI3_DIR)/os_memory.o \
               $(RPI3_DIR)/os_compat.o \
               $(RPI3_DIR)/network_core.o \
               $(RPI3_DIR)/netstack_core.o \
               $(RPI3_DIR)/block_core.o \
               $(RPI3_DIR)/fat_core.o \
               $(RPI3_DIR)/vfs_core.o \
               $(RPI3_DIR)/gfx_core.o \
               $(RPI3_DIR)/gui_gfx.o \
               $(RPI3_DIR)/gui_font.o \
               $(RPI3_DIR)/gui_real_font.o \
               $(RPI3_DIR)/gui_desktop.o \
               $(RPI3_DIR)/gui_windows.o \
               $(RPI3_DIR)/gui_compositor.o \
               $(RPI3_DIR)/gui_widget.o \
               $(RPI3_DIR)/gui_image.o \
               $(RPI3_DIR)/gui_event.o \
               $(RPI3_DIR)/keyboard_core.o \
               $(RPI3_DIR)/mouse_core.o \
               $(RPI3_DIR)/input_preferences.o \
               $(RPI3_DIR)/inflate_core.o \
               $(RPI3_DIR)/graphics_resources.o \
               $(RPI3_DIR)/language_core.o \
               $(RPI3_DIR)/deskmanager.o \
               $(RPI3_DIR)/deskbar.o \
               $(RPI3_DIR)/shelllauncher.o \
               $(RPI3_DIR)/about_app.o \
               $(RPI3_DIR)/runbox_app.o \
               $(RPI3_DIR)/calendar_app.o \
               $(RPI3_DIR)/texteditor_app.o \
               $(RPI3_DIR)/filebrowser_app.o \
               $(RPI3_DIR)/imageviewer_app.o \
               $(RPI3_DIR)/paint_app.o \
               $(RPI3_DIR)/processmanager_app.o \
               $(RPI3_DIR)/scandisk_app.o \
               $(RPI3_DIR)/help_center_app.o \
               $(RPI3_DIR)/find_files_app.o \
               $(RPI3_DIR)/archive_manager_app.o \
               $(RPI3_DIR)/disk_tools_app.o \
               $(RPI3_DIR)/network_status_app.o \
               $(RPI3_DIR)/clipboard_viewer_app.o \
               $(RPI3_DIR)/performance_monitor_app.o \
               $(RPI3_DIR)/control_panel_cpl.o \
               $(RPI3_DIR)/mouse_cpl.o \
               $(RPI3_DIR)/keyboard_cpl.o \
               $(RPI3_DIR)/language_cpl.o \
               $(RPI3_DIR)/appearance_cpl.o \
               $(RPI3_DIR)/display_cpl.o \
               $(RPI3_DIR)/sound_cpl.o \
               $(RPI3_DIR)/datetime_cpl.o \
               $(RPI3_DIR)/system_cpl.o \
               $(RPI3_DIR)/device_manager_cpl.o \
               $(RPI3_DIR)/internet_cpl.o \
               $(RPI3_DIR)/modem_cpl.o \
               $(RPI3_DIR)/bootsplash_core.o \
               $(RPI3_DIR)/os_bridge.o \
               $(RPI3_DIR)/platform.o
RPI3_ELF       := $(RPI3_DIR)/kernel7.elf
RPI3_IMG       := $(RPI3_DIR)/kernel7.img
RPI3_QEMU_ELF  := $(RPI3_DIR)/kernel7-qemu.elf
RPI3_QEMU_IMG  := $(RPI3_DIR)/kernel7-qemu.img
RPI3_BASE_DISK ?= build/bleskernos-ata-user.img
RPI3_DISK      := $(RPI3_DIR)/bleskernos-rpi3.img
RPI3_ARM_PROG_DIR := $(RPI3_DIR)/arm-programs
RPI3_ARM_APP_NAMES := help echo sh about uname hostname uptime date time shutdown reboot sleep bosrecovery \
                      fdisk format mount unmount label checkdisk fsinfo backup dir ls copy cp move delete rm mkdir \
                      rmdir rename touch tree find attrib chmod type more cat diff ps kill tasklist taskkill top nice \
                      pci usb lspci lsusb cpuinfo mem soundtest ipconfig ping netstat ftp wget curl compile link gcc \
                      cc ld as ar ranlib build nasm ndisasm make fatpack addr2line cxxfilt elfedit objcopy objdump \
                      readelf nm size strip hexdump strings calc hexedit compress extract checksum benchmark start
RPI3_ARM_APP_BEX := $(addprefix $(RPI3_ARM_PROG_DIR)/,$(addsuffix .BEX,$(RPI3_ARM_APP_NAMES)))
RPI3_ARM_GUI_BEX := $(RPI3_ARM_PROG_DIR)/CALCULATOR.BEX \
                    $(RPI3_ARM_PROG_DIR)/SHELL.BEX \
                    $(RPI3_ARM_PROG_DIR)/ABOUT.BEX \
                    $(RPI3_ARM_PROG_DIR)/RUNBOX.BEX \
                    $(RPI3_ARM_PROG_DIR)/CALENDAR.BEX \
                    $(RPI3_ARM_PROG_DIR)/TEXTEDITOR.BEX \
                    $(RPI3_ARM_PROG_DIR)/FILE.BEX \
                    $(RPI3_ARM_PROG_DIR)/IMAGEVIEWER.BEX \
                    $(RPI3_ARM_PROG_DIR)/PAINT.BEX \
                    $(RPI3_ARM_PROG_DIR)/PROCESSMANAGER.BEX \
                    $(RPI3_ARM_PROG_DIR)/CONTROL.BEX \
                    $(RPI3_ARM_PROG_DIR)/MOUSE.CPL \
                    $(RPI3_ARM_PROG_DIR)/KEYBOARD.CPL \
                    $(RPI3_ARM_PROG_DIR)/LANGUAGE.CPL
RPI3_ARM_SYSTEM_BEX := $(RPI3_ARM_PROG_DIR)/SCANDISK.BEX \
                       $(RPI3_ARM_PROG_DIR)/HELP_CENTER.BEX \
                       $(RPI3_ARM_PROG_DIR)/FIND_FILES.BEX \
                       $(RPI3_ARM_PROG_DIR)/ARCHIVE_MANAGER.BEX \
                       $(RPI3_ARM_PROG_DIR)/DISK_TOOLS.BEX \
                       $(RPI3_ARM_PROG_DIR)/NETWORK_STATUS.BEX \
                       $(RPI3_ARM_PROG_DIR)/CLIPBOARD_VIEWER.BEX \
                       $(RPI3_ARM_PROG_DIR)/PERFORMANCE_MONITOR.BEX \
                       $(RPI3_ARM_PROG_DIR)/APPEAR.CPL \
                       $(RPI3_ARM_PROG_DIR)/DISPLAY.CPL \
                       $(RPI3_ARM_PROG_DIR)/SOUND.CPL \
                       $(RPI3_ARM_PROG_DIR)/DATETIME.CPL \
                       $(RPI3_ARM_PROG_DIR)/SYSTEM.CPL \
                       $(RPI3_ARM_PROG_DIR)/DEVMGR.CPL \
                       $(RPI3_ARM_PROG_DIR)/INTERNET.CPL \
                       $(RPI3_ARM_PROG_DIR)/MODEM.CPL
RPI3_ARM_USER_LIBS := $(RPI3_ARM_PROG_DIR)/crt.o \
                      $(RPI3_ARM_PROG_DIR)/common.o \
                      $(RPI3_ARM_PROG_DIR)/api.o \
                      $(RPI3_ARM_PROG_DIR)/runtime.o \
                      $(RPI3_ARM_PROG_DIR)/div.o \
                      $(RPI3_ARM_PROG_DIR)/eabi.o

.PHONY: rpi3 rpi3-qemu rpi3-all rpi3-userland rpi3-disk rpi3-clean rpi3-info

# Real Raspberry Pi firmware loads 32-bit kernel7.img at 0x8000.
rpi3: $(RPI3_IMG)
	@echo "Raspberry Pi 3 Stage 13 stability/native-GUI image (0x8000): $(RPI3_IMG)"

# QEMU's generic 32-bit ARM -kernel loader places raw images at 0x10000.
# Link a second image at that address instead of relying on accidental
# position-independence of the bootstrap.
rpi3-qemu: $(RPI3_QEMU_IMG) $(RPI3_DISK)
	@echo "QEMU raspi2b Stage 13 stability/native-GUI image (0x10000): $(RPI3_QEMU_IMG)"
	@echo "ARM32 Ring-3 FAT image: $(RPI3_DISK)"

rpi3-all: rpi3-userland rpi3 rpi3-qemu

rpi3-userland: $(RPI3_ARM_APP_BEX) $(RPI3_ARM_GUI_BEX) $(RPI3_ARM_SYSTEM_BEX)
	@echo "ARM32 native Ring-3 commands: $(RPI3_ARM_PROG_DIR)"

rpi3-disk: $(RPI3_DISK)

$(RPI3_DISK): $(RPI3_ARM_APP_BEX) $(RPI3_ARM_GUI_BEX) $(RPI3_ARM_SYSTEM_BEX) | $(RPI3_DIR)
	@test -f "$(RPI3_BASE_DISK)" || { echo "Falta imagen FAT base: $(RPI3_BASE_DISK)"; exit 1; }
	cp "$(RPI3_BASE_DISK)" "$@"
	-@mdel -i "$@" "::/SYSTEM/COMMANDS/*" >/dev/null 2>&1
	@for app in $(RPI3_ARM_APP_BEX); do \
		mcopy -o -i "$@" "$$app" "::/SYSTEM/COMMANDS/$$(basename "$$app")" || exit 1; \
	done
	@mcopy -o -i "$@" "$(RPI3_ARM_PROG_DIR)/CALCULATOR.BEX" "::/SYSTEM/PROGRAMS/CALCULATOR.BEX"
	@for app in SHELL ABOUT RUNBOX CALENDAR TEXTEDITOR FILE IMAGEVIEWER PAINT PROCESSMANAGER; do \
		mcopy -o -i "$@" "$(RPI3_ARM_PROG_DIR)/$$app.BEX" "::/SYSTEM/PROGRAMS/$$app.BEX" || exit 1; \
	done
	@mcopy -o -i "$@" "$(RPI3_ARM_PROG_DIR)/CONTROL.BEX" "::/SYSTEM/CONTROL/CONTROL.BEX"
	@for app in MOUSE KEYBOARD LANGUAGE; do \
		mcopy -o -i "$@" "$(RPI3_ARM_PROG_DIR)/$$app.CPL" "::/SYSTEM/CONTROL/$$app.CPL" || exit 1; \
	done
	@for app in SCANDISK HELP_CENTER FIND_FILES ARCHIVE_MANAGER DISK_TOOLS NETWORK_STATUS CLIPBOARD_VIEWER PERFORMANCE_MONITOR; do \
		mcopy -o -i "$@" "$(RPI3_ARM_PROG_DIR)/$$app.BEX" "::/SYSTEM/PROGRAMS/$$app.BEX" || exit 1; \
	done
	@for app in APPEAR DISPLAY SOUND DATETIME SYSTEM DEVMGR INTERNET MODEM; do \
		mcopy -o -i "$@" "$(RPI3_ARM_PROG_DIR)/$$app.CPL" "::/SYSTEM/CONTROL/$$app.CPL" || exit 1; \
	done
	@echo "[FAT:ARM] $$(echo $(RPI3_ARM_APP_NAMES) | wc -w) BEX EM_ARM instalados en /SYSTEM/COMMANDS"

$(RPI3_DIR):
	mkdir -p $@

$(RPI3_ARM_PROG_DIR): | $(RPI3_DIR)
	mkdir -p $@

$(RPI3_DIR)/boot.o: arch/arm32/rpi3/boot.S | $(RPI3_DIR)
	$(CLANG) $(ARM_ASFLAGS) $< -o $@

$(RPI3_DIR)/vectors.o: arch/arm32/rpi3/vectors.S | $(RPI3_DIR)
	$(CLANG) $(ARM_ASFLAGS) $< -o $@

$(RPI3_DIR)/task_switch.o: arch/arm32/rpi3/task_switch.S | $(RPI3_DIR)
	$(CLANG) $(ARM_ASFLAGS) $< -o $@


$(RPI3_ARM_PROG_DIR)/crt.o: arch/arm32/rpi3/user_stage12_crt.c | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_USER_CFLAGS) -c $< -o $@

$(RPI3_ARM_PROG_DIR)/common.o: system/commands/common.c | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_USER_CFLAGS) -c $< -o $@

$(RPI3_ARM_PROG_DIR)/api.o: arch/arm32/rpi3/user_stage12_api.c | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_USER_CFLAGS) -c $< -o $@

$(RPI3_ARM_PROG_DIR)/runtime.o: arch/arm32/rpi3/user_stage12_runtime.c | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_USER_CFLAGS) -c $< -o $@

$(RPI3_ARM_PROG_DIR)/div.o: arch/arm32/rpi3/user_stage12_div.c | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_USER_CFLAGS) -c $< -o $@

$(RPI3_ARM_PROG_DIR)/eabi.o: arch/arm32/rpi3/user_stage12_eabi.S | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_ASFLAGS) $< -o $@

$(RPI3_ARM_PROG_DIR)/%.cmd.o: system/commands/%.c | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_USER_CFLAGS) -c $< -o $@

$(RPI3_ARM_PROG_DIR)/calculator.gui.o: programs/arm32/calculator_bex.c | $(RPI3_ARM_PROG_DIR)
	$(CLANG) $(ARM_USER_CFLAGS) -c $< -o $@

$(RPI3_ARM_PROG_DIR)/CALCULATOR.BEX: $(RPI3_ARM_PROG_DIR)/calculator.gui.o $(RPI3_ARM_USER_LIBS)
	$(LD_LLD) -m armelf -r --gc-sections --undefined=_start \
		$(RPI3_ARM_PROG_DIR)/crt.o $< $(RPI3_ARM_PROG_DIR)/api.o \
		$(RPI3_ARM_PROG_DIR)/runtime.o $(RPI3_ARM_PROG_DIR)/div.o \
		$(RPI3_ARM_PROG_DIR)/eabi.o -o $@
	@test -z "$$(nm -u $@ 2>/dev/null)" || { nm -u $@; false; }

define RPI3_DESKTOP_APP
$(RPI3_ARM_PROG_DIR)/$(1).gui.o: programs/arm32/desktop_app_bex.c | $(RPI3_ARM_PROG_DIR)
	$$(CLANG) $$(ARM_USER_CFLAGS) -DARM_APP_MODE=$(2) -DARM_APP_TITLE='"$(3)"' -c $$< -o $$@

$(RPI3_ARM_PROG_DIR)/$(1).$(4): $(RPI3_ARM_PROG_DIR)/$(1).gui.o $(RPI3_ARM_USER_LIBS)
	$$(LD_LLD) -m armelf -r --gc-sections --undefined=_start \
		$$(RPI3_ARM_PROG_DIR)/crt.o $$< $$(RPI3_ARM_PROG_DIR)/api.o \
		$$(RPI3_ARM_PROG_DIR)/runtime.o $$(RPI3_ARM_PROG_DIR)/div.o \
		$$(RPI3_ARM_PROG_DIR)/eabi.o -o $$@
	@test -z "$$$$(nm -u $$@ 2>/dev/null)" || { nm -u $$@; false; }
endef

$(eval $(call RPI3_DESKTOP_APP,SHELL,3,Terminal,BEX))
$(eval $(call RPI3_DESKTOP_APP,ABOUT,1,Acerca de BlesKernOS,BEX))
$(eval $(call RPI3_DESKTOP_APP,RUNBOX,3,Ejecutar,BEX))
$(eval $(call RPI3_DESKTOP_APP,CALENDAR,2,Calendario,BEX))
$(eval $(call RPI3_DESKTOP_APP,TEXTEDITOR,4,Editor de texto,BEX))
$(eval $(call RPI3_DESKTOP_APP,FILE,5,Archivos,BEX))
$(eval $(call RPI3_DESKTOP_APP,IMAGEVIEWER,6,Visor de imagenes,BEX))
$(eval $(call RPI3_DESKTOP_APP,PAINT,7,Paint,BEX))
$(eval $(call RPI3_DESKTOP_APP,PROCESSMANAGER,8,Administrador de Procesos,BEX))
$(eval $(call RPI3_DESKTOP_APP,CONTROL,10,Panel de control,BEX))
$(eval $(call RPI3_DESKTOP_APP,MOUSE,11,Mouse,CPL))
$(eval $(call RPI3_DESKTOP_APP,KEYBOARD,12,Teclado,CPL))
$(eval $(call RPI3_DESKTOP_APP,LANGUAGE,13,Idioma,CPL))
$(eval $(call RPI3_DESKTOP_APP,SCANDISK,16,Scandisk,BEX))
$(eval $(call RPI3_DESKTOP_APP,HELP_CENTER,1,Centro de ayuda,BEX))
$(eval $(call RPI3_DESKTOP_APP,FIND_FILES,5,Buscar archivos,BEX))
$(eval $(call RPI3_DESKTOP_APP,ARCHIVE_MANAGER,5,Administrador de archivos,BEX))
$(eval $(call RPI3_DESKTOP_APP,DISK_TOOLS,16,Herramientas de disco,BEX))
$(eval $(call RPI3_DESKTOP_APP,NETWORK_STATUS,15,Estado de red,BEX))
$(eval $(call RPI3_DESKTOP_APP,CLIPBOARD_VIEWER,17,Portapapeles,BEX))
$(eval $(call RPI3_DESKTOP_APP,PERFORMANCE_MONITOR,8,Rendimiento,BEX))
$(eval $(call RPI3_DESKTOP_APP,APPEAR,10,Apariencia,CPL))
$(eval $(call RPI3_DESKTOP_APP,DISPLAY,10,Pantalla,CPL))
$(eval $(call RPI3_DESKTOP_APP,SOUND,14,Sonido,CPL))
$(eval $(call RPI3_DESKTOP_APP,DATETIME,2,Fecha y hora,CPL))
$(eval $(call RPI3_DESKTOP_APP,SYSTEM,1,Sistema,CPL))
$(eval $(call RPI3_DESKTOP_APP,DEVMGR,16,Administrador de dispositivos,CPL))
$(eval $(call RPI3_DESKTOP_APP,INTERNET,15,Internet,CPL))
$(eval $(call RPI3_DESKTOP_APP,MODEM,10,Modem,CPL))

$(RPI3_ARM_PROG_DIR)/%.BEX: $(RPI3_ARM_PROG_DIR)/%.cmd.o $(RPI3_ARM_USER_LIBS)
	$(LD_LLD) -m armelf -r --gc-sections --undefined=_start \
		$(RPI3_ARM_PROG_DIR)/crt.o $< $(RPI3_ARM_PROG_DIR)/common.o \
		$(RPI3_ARM_PROG_DIR)/api.o $(RPI3_ARM_PROG_DIR)/runtime.o \
		$(RPI3_ARM_PROG_DIR)/div.o $(RPI3_ARM_PROG_DIR)/eabi.o -o $@
	@test -z "$$(nm -u $@ 2>/dev/null)" || { echo "Stage 12 unresolved symbols in $@"; nm -u $@; false; }

$(RPI3_DIR)/%.o: arch/arm32/rpi3/%.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@


$(RPI3_DIR)/block_core.o: kernel/drivers/core/block.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/fat_core.o: kernel/drivers/storage/fat.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/vfs_core.o: kernel/drivers/core/vfs.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gfx_core.o: kernel/drivers/core/gfx.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/network_core.o: kernel/net/network.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/sound_core.o: kernel/drivers/audio/sound_core.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/netstack_core.o: kernel/drivers/network/netstack.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/api_full.o: kernel/api/api.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Ikernel/libc/include -Ikernel -c $< -o $@

$(RPI3_DIR)/arm_div.o: arch/arm32/rpi3/user_stage12_div.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/arm_eabi.o: arch/arm32/rpi3/user_stage12_eabi.S | $(RPI3_DIR)
	$(CLANG) $(ARM_ASFLAGS) $< -o $@

$(RPI3_DIR)/gui_gfx.o: gui/gfx.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gui_font.o: gui/font.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gui_desktop.o: gui/desktop.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gui_windows.o: gui/windows.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gui_compositor.o: gui/compositor.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gui_widget.o: gui/widget.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gui_image.o: gui/image.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/gui_event.o: gui/event.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/keyboard_core.o: kernel/drivers/input/keyboard.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/mouse_core.o: kernel/drivers/input/mouse_core.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/inflate_core.o: kernel/libc/inflate.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/graphics_resources.o: kernel/ui/graphics_resources.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/language_core.o: kernel/ui/language.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Ikernel/libc/include -Ikernel -c $< -o $@

$(RPI3_DIR)/deskmanager.o: system/desktop/deskmanager.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Iprograms -c $< -o $@

$(RPI3_DIR)/deskbar.o: system/desktop/deskbar.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Iprograms -c $< -o $@

$(RPI3_DIR)/shelllauncher.o: programs/shelllauncher.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Iprograms -c $< -o $@

$(RPI3_DIR)/calculator_app.o: programs/calculator.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Iprograms -Dbleskernos_program_main=calculator_arm_kernel_main -c $< -o $@

$(RPI3_DIR)/%_app.o: programs/%.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Ikernel/libc/include -Ikernel -Iprograms -Isdk/include -Ilibs/tinygl -Dbleskernos_program_main=$*_arm_kernel_main -c $< -o $@

$(RPI3_DIR)/%_cpl.o: system/control/%.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -Ikernel/libc/include -Ikernel -Iprograms -Isdk/include -Ilibs/tinygl \
		-Dbleskernos_program_main=$*_arm_kernel_main \
		-Dss_balls_preview_draw=$*_ss_balls_preview_draw -c $< -o $@

$(RPI3_DIR)/bootsplash_core.o: kernel/ui/bootsplash.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_ELF): $(RPI3_OBJS) arch/arm32/rpi3/linker.ld
	$(LD_LLD) $(ARM_LDFLAGS) --defsym=KERNEL_LOAD_BASE=0x00008000 $(RPI3_OBJS) -o $@

$(RPI3_IMG): $(RPI3_ELF)
	$(OBJCOPY) -O binary $< $@

$(RPI3_QEMU_ELF): $(RPI3_OBJS) arch/arm32/rpi3/linker.ld
	$(LD_LLD) $(ARM_LDFLAGS) --defsym=KERNEL_LOAD_BASE=0x00010000 $(RPI3_OBJS) -o $@

$(RPI3_QEMU_IMG): $(RPI3_QEMU_ELF)
	$(OBJCOPY) -O binary $< $@

rpi3-info: $(RPI3_ELF) $(RPI3_QEMU_ELF)
	@echo "Hardware ELF: $(RPI3_ELF)"
	@readelf -h $(RPI3_ELF) | grep -E 'Class:|Data:|Machine:|Flags:|Entry point'
	@echo "QEMU ELF: $(RPI3_QEMU_ELF)"
	@readelf -h $(RPI3_QEMU_ELF) | grep -E 'Class:|Data:|Machine:|Flags:|Entry point'
	@size $(RPI3_ELF) $(RPI3_QEMU_ELF)

rpi3-clean:
	rm -rf $(RPI3_DIR)

$(RPI3_DIR)/gui_real_font.o: gui/real_font.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@

$(RPI3_DIR)/input_preferences.o: kernel/ui/input_preferences.c | $(RPI3_DIR)
	$(CLANG) $(ARM_CFLAGS) -c $< -o $@
