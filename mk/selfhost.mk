# Build nativo de BlesKernOS. Este archivo se ejecuta con el GNU Make incluido
# en /SYSTEM/DEVEL y evita recetas que requieren un shell POSIX o Python.
EDITION ?= user
# A self-host rebuild starts from a source-only FAT image: there are no stale
# host-generated .d files to consume.  Tell the shared Makefile not to feed its
# optional dependency makefiles back into GNU Make while bootstrapping.
SELFHOST_NATIVE := 1
.SUFFIXES:
include Makefile

SELFHOST_IMAGE ?= build/BLES-SELFHOST.IMG
SELFHOST_IMAGE_KB ?= 65536
SELFHOST_KERNEL_SECTORS ?= 3584
FAT_RESERVED_SECTORS := 3593
OBJCOPY ?= objcopy
FATPACK ?= fatpack
AS ?= as

.PHONY: selfhost selfhost-kernel selfhost-sdk selfhost-programs selfhost-image selfhost-imag

selfhost: selfhost-image

# Alias tolerante para terminales FAT/DOS y para la forma abreviada que cabe
# en algunas líneas de comando de la consola.
selfhost-imag: selfhost-image

selfhost-kernel: $(BOOT1_FAT32_BIN) $(BOOT2_BIN) $(KERNEL_BIN)
	@echo BlesKernOS kernel y boot listos

selfhost-sdk: $(SDK_A) $(SDK_TINYGL_A) $(SDK_MESA_A) $(SDK_POSIX_A) $(SDK_MATH_A)
	@echo SDK nativo, TinyGL y Mesa listos

selfhost-programs: $(USER_EXTERNAL_APP_OBJS) $(DRIVER_OBJS)
	@echo Programas, comandos, paneles y drivers listos

# Recetas reemplazadas porque las originales usan test, wc, redirecciones,
# bloques de shell o cd &&. Las reglas de compilacion C/ASM del Makefile
# principal ya son portables gracias a echo, mkdir -p, NASM y Binutils.
$(BOOT1_BIN): $(BOOT1_SRC) | build
	$(NASM) $(NASM_FLAGS) -DRESERVED_SECTORS=$(FAT_RESERVED_SECTORS) $< -o $@

$(BOOT1_CD_BIN): $(BOOT1_CD_SRC) | build
	$(NASM) $(NASM_FLAGS) -DRESERVED_SECTORS=$(FAT_RESERVED_SECTORS) -DBOOT_MODE_MAGIC=0x54534E49 $< -o $@

$(BOOT1_FAT32_BIN): $(BOOT1_FAT32_SRC) | build
	$(NASM) $(NASM_FLAGS) $< -o $@

$(MBR_BIN): $(MBR_SRC) | build
	$(NASM) $(NASM_FLAGS) $< -o $@

$(BOOT2_BIN): $(BOOT2_SRC) $(KERNEL_BIN) | build
	$(NASM) $(NASM_FLAGS) -DKERNEL_SECTORS=$(SELFHOST_KERNEL_SECTORS) $< -o $@

# TinyCC incluye un ensamblador, pero el trampoline SMP usa .code16 y un
# salto lejano que debe procesar GNU as.
build/%.o: %.S | build
	mkdir -p $(dir $@)
	$(AS) --32 $< -o $@

$(LIBC_A): $(LIBC_OBJS) | build
	$(file >build/libc.rsp,$^)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ @build/libc.rsp
	rm -f build/libc.rsp

$(TINYGL_A): $(TINYGL_OBJS) | build
	$(file >build/tinygl.rsp,$^)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ @build/tinygl.rsp
	rm -f build/tinygl.rsp

$(MESA35_A): $(MESA35_OBJS) | build
	$(file >build/mesa35.rsp,$^)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ @build/mesa35.rsp
	rm -f build/mesa35.rsp

$(SDK_A): $(SDK_SYSCALL_OBJ) $(SDK_THREAD_OBJ) $(SDK_PRINT_OBJ) | build
	$(file >build/sdk.rsp,$^)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ @build/sdk.rsp
	rm -f build/sdk.rsp

$(SDK_TINYGL_A): $(SDK_TINYGL_OBJ) $(TINYGL_OBJS) | build
	$(file >build/sdk-tinygl.rsp,$^)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ @build/sdk-tinygl.rsp
	rm -f build/sdk-tinygl.rsp

$(SDK_MESA_A): $(SDK_MESA_OBJ) $(MESA35_OBJS) | build
	$(file >build/sdk-mesa.rsp,$^)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ @build/sdk-mesa.rsp
	rm -f build/sdk-mesa.rsp

$(BEARSSL_A): $(BEARSSL_OBJS) | build
	$(file >build/bearssl.rsp,$^)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ @build/bearssl.rsp
	rm -f build/bearssl.rsp

$(NETSURF_OBJ): $(NETSURF_RAW_OBJS)
	$(file >build/netsurf.rsp,$^)
	mkdir -p $(dir $@)
	$(LD) -m elf_i386 -r @build/netsurf.rsp -o $@
	rm -f build/netsurf.rsp
	@echo NetSurf frontend listo: $@

$(KERNEL_ELF): $(KERNEL_ENTRY_OBJ) $(KERNEL_ASM_OBJS) $(KERNEL_GAS_OBJS) $(KERNEL_OBJS) $(LIBC_A) kernel/arch/x86/linker_selfhost.ld | build
	$(file >build/kernel-link.rsp,$(KERNEL_ENTRY_OBJ) $(KERNEL_ASM_OBJS) $(KERNEL_GAS_OBJS) $(KERNEL_OBJS) $(LIBC_A))
	$(LD) $(filter-out -T kernel/arch/x86/linker.ld,$(LD_FLAGS)) -T kernel/arch/x86/linker_selfhost.ld @build/kernel-link.rsp -o $@
	rm -f build/kernel-link.rsp

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# La imagen parte del /SYSTEM que esta ejecutando el build. FATPACK importa
# recursivamente ese arbol y luego reemplaza kernel, programas, SDK y drivers
# por los resultados recien compilados.
selfhost-image: selfhost-kernel selfhost-sdk selfhost-programs
	$(FATPACK) create $(SELFHOST_IMAGE) $(SELFHOST_IMAGE_KB) $(FAT_RESERVED_SECTORS)
	$(FATPACK) import $(SELFHOST_IMAGE) /SYSTEM /SYSTEM
	$(FATPACK) boot $(SELFHOST_IMAGE) $(BOOT1_FAT32_BIN)
	$(FATPACK) raw $(SELFHOST_IMAGE) 1 $(BOOT2_BIN) 8
	$(FATPACK) raw $(SELFHOST_IMAGE) 9 $(KERNEL_BIN) $(SELFHOST_KERNEL_SECTORS)
	$(FATPACK) importflat $(SELFHOST_IMAGE) build/programs /SYSTEM/PROGRAMS .o .BEX
	$(FATPACK) add $(SELFHOST_IMAGE) $(SHELL_OBJ) /SYSTEM/PROGRAMS/SHELL.BEX
	$(FATPACK) add $(SELFHOST_IMAGE) $(FILEBROWSER_OBJ) /SYSTEM/PROGRAMS/FILE.BEX
	$(FATPACK) importflat $(SELFHOST_IMAGE) build/system/commands /SYSTEM/COMMANDS .o .BEX
	$(FATPACK) importflat $(SELFHOST_IMAGE) build/system/drivers /SYSTEM/DRIVERS .DVR .DVR
	$(FATPACK) importflat $(SELFHOST_IMAGE) build/system/control /SYSTEM/CONTROL .o .CPL
	$(FATPACK) add $(SELFHOST_IMAGE) $(CONTROL_PANEL_OBJ) /SYSTEM/CONTROL/CONTROL.BEX
	$(FATPACK) add $(SELFHOST_IMAGE) $(APPEAR_CPL_OBJ) /SYSTEM/CONTROL/APPEAR.CPL
	$(FATPACK) add $(SELFHOST_IMAGE) $(DEVMGR_CPL_OBJ) /SYSTEM/CONTROL/DEVMGR.CPL
	$(FATPACK) add $(SELFHOST_IMAGE) $(SCREENSAVERD_OBJ) /SYSTEM/SERVICES/SCREENSV.BEX
	$(FATPACK) add $(SELFHOST_IMAGE) $(PRINTSPOOL_OBJ) /SYSTEM/SERVICES/PRINTSPL.BEX
	$(FATPACK) add $(SELFHOST_IMAGE) $(SSLOGO_OBJ) /SYSTEM/SCREENS/SSLOGO.SCV
	$(FATPACK) add $(SELFHOST_IMAGE) $(SSPIPES_OBJ) /SYSTEM/SCREENS/SSPIPES.SCV
	$(FATPACK) add $(SELFHOST_IMAGE) $(SSBALLS_OBJ) /SYSTEM/SCREENS/SSBALLS.SCV
	$(FATPACK) add $(SELFHOST_IMAGE) $(SDK_A) /SYSTEM/SDK/LIB/LIBBLESK.A
	$(FATPACK) add $(SELFHOST_IMAGE) $(SDK_TINYGL_A) /SYSTEM/SDK/LIB/LIBTINYGL.A
	$(FATPACK) add $(SELFHOST_IMAGE) $(SDK_POSIX_A) /SYSTEM/SDK/LIB/LIBPOSIX.A
	$(FATPACK) add $(SELFHOST_IMAGE) $(SDK_MATH_A) /SYSTEM/SDK/LIB/LIBM.A
	$(FATPACK) add $(SELFHOST_IMAGE) $(SDK_MESA_A) /SYSTEM/SDK/LIB/LIBMESA.A
	@echo Imagen self-hosted lista: $(SELFHOST_IMAGE)
