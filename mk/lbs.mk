LBS_NATIVE_A := build/sdk/libblesk_lbs_native.a
LBS_POSIX_A := build/sdk/libblesk_lbs_posix.a
$(LBS_NATIVE_A): $(SDK_A)
	objcopy --redefine-sym bk_file_size=__bk_lbs_fd_size --redefine-sym bk_file_write=__bk_lbs_fd_write $< $@
$(LBS_POSIX_A): $(SDK_POSIX_A)
	objcopy --redefine-sym bk_file_size=__bk_lbs_fd_size --redefine-sym bk_file_write=__bk_lbs_fd_write $< $@
lbs: $(LBS_NATIVE_A) $(LBS_POSIX_A)
# Native ELF32 shared objects. Existing SO/Linux build rules are untouched.
LBS_NAMES := SYSTEM INPUT FONT NGS SOUND NETWORK VISION NGS3D MEDIA PRINT CRYPTO SECURITY TINYGL ARCHIVE COMMAND
LBS_DIR := build/system/libs/native
LBS_FILES := $(addprefix $(LBS_DIR)/,$(addsuffix .LBS,$(LBS_NAMES)))
LBS_GEN := build/sdk/lbs/generated
LBS_FLAGS := -Werror=implicit-function-declaration $(filter-out -fno-pic -fno-pie,$(SDK_CFLAGS)) -fPIC -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
LBS_MESA_OBJ := $(patsubst %.c,build/sdk/lbs/pic/%.o,$(MESA35_SOURCES))
LBS_VIDEO_SOURCES := $(addprefix libs/native/media/,video.c bkv.c avi.c gif.c mpeg.c pl_mpeg_bles.c render.c)
LBS_VIDEO_OBJ := $(patsubst %.c,build/sdk/lbs/pic/%.o,$(LBS_VIDEO_SOURCES))

.PHONY: lbs lbs-test
lbs: $(LBS_FILES) build/sdk/lbs_host.o build/sdk/lbs.o

$(LBS_GEN)/stamp: tools/generate_lbs.py sdk/lbs/manifest.json sdk/lbs/dependencies.json sdk/lbs/legacy_exports.json sdk/lbs/exports.map sdk/include/bleskernos_api.h
	$(PYTHON) tools/generate_lbs.py
	@touch $@

$(LBS_GEN)/%.o: $(LBS_GEN)/stamp
	$(CC) -m32 -fPIC -c $(LBS_GEN)/$*.S -o $@
$(LBS_GEN)/%_info.o: $(LBS_GEN)/stamp sdk/include/bleskernos_lbs.h
	$(CC) $(LBS_FLAGS) -c $(LBS_GEN)/$*_info.c -o $@

build/sdk/lbs/%.o: sdk/lbs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LBS_FLAGS) -Ilibs/bearssl/inc -c $< -o $@

build/sdk/lbs/pic/libs/Mesa-3.5/%.o: libs/Mesa-3.5/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -fno-pic -fno-pie -DUSE_SSE_ASM -DUSE_MMX_ASM,$(MESA35_CFLAGS)) -fPIC -ffunction-sections -fdata-sections -c $< -o $@

build/sdk/lbs/pic/libs/native/media/%.o: libs/native/media/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LBS_FLAGS) -DPLM_NO_STDIO=1 -Iprograms/viewer/third_party/compat -c $< -o $@

build/sdk/lbs/mesa.o: sdk/lib/mesa.c
	$(CC) $(LBS_FLAGS) -c $< -o $@
build/sdk/lbs/print.o: sdk/lib/print.c mk/lbs.mk
	$(CC) $(LBS_FLAGS) -Dprint_begin=bk_lbs_print_begin_inner -Dprint_cancel=bk_lbs_print_cancel_inner -Dprint_submit=bk_lbs_print_submit_inner -c $< -o $@
build/sdk/lbs/sha2small.o: libs/bearssl/src/hash/sha2small.c
	$(CC) $(LBS_FLAGS) -Ilibs/bearssl/inc -Ilibs/bearssl/src -c $< -o $@

# An anonymous export script hides implementation helpers, avoiding GL/runtime
# interposition across unrelated modules. SysV hash is required by native dlopen.
$(LBS_DIR)/%.LBS: $(LBS_GEN)/%.o $(LBS_GEN)/%_info.o sdk/lbs/exports.map
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -shared --hash-style=sysv --gc-sections -soname $*.LBS -z noexecstack \
	  --version-script=$(LBS_GEN)/exports.map -o $@ $(filter %.o,$^) \
	  --no-as-needed $(filter %.LBS,$^) --as-needed

$(LBS_DIR)/SYSTEM.LBS: build/sdk/lbs/system.o
$(filter-out $(LBS_DIR)/SYSTEM.LBS,$(LBS_FILES)): $(LBS_DIR)/SYSTEM.LBS
$(LBS_DIR)/NGS.LBS: build/sdk/lbs/ngs.o
$(LBS_DIR)/SOUND.LBS: build/sdk/lbs/sound.o
$(LBS_DIR)/NGS3D.LBS: build/sdk/lbs/ngs3d.o build/sdk/lbs/mesa.o $(LBS_MESA_OBJ) $(LBS_DIR)/NGS.LBS $(LBS_DIR)/VISION.LBS
$(LBS_DIR)/MEDIA.LBS: build/sdk/lbs/media.o $(LBS_VIDEO_OBJ) $(LBS_DIR)/NGS.LBS $(LBS_DIR)/SOUND.LBS
$(LBS_DIR)/PRINT.LBS: build/sdk/lbs/print.o build/sdk/lbs/print_facade.o build/sdk/lbs/print_queue.o
$(LBS_DIR)/CRYPTO.LBS: $(LBS_DIR)/NETWORK.LBS build/sdk/lbs/crypto.o build/sdk/lbs/sha2small.o build/sdk/lbs/dec32be.o build/sdk/lbs/enc32be.o
$(LBS_DIR)/SECURITY.LBS: build/sdk/lbs/security.o
$(LBS_DIR)/VISION.LBS: $(LBS_DIR)/NGS.LBS $(LBS_DIR)/FONT.LBS $(LBS_DIR)/INPUT.LBS

build/sdk/lbs/syscalls.o: sdk/lib/syscalls.c
	$(CC) $(LBS_FLAGS) -c $< -o $@

build/sdk/lbs.o: sdk/lib/lbs.c sdk/include/bleskernos_lbs.h
	$(CC) $(SDK_CFLAGS) -c $< -o $@

build/sdk/lbs_host.c: $(LBS_FILES) tools/generate_lbs_host.py
	$(PYTHON) tools/generate_lbs_host.py
build/sdk/lbs_host.o: build/sdk/lbs_host.c
	$(CC) $(SDK_CFLAGS) -c $< -o $@

sdk: lbs
$(ATA_IMG): $(LBS_FILES)

build/sdk/lbs/dec32be.o: libs/bearssl/src/codec/dec32be.c
	$(CC) $(LBS_FLAGS) -Ilibs/bearssl/inc -Ilibs/bearssl/src -c $< -o $@
build/sdk/lbs/enc32be.o: libs/bearssl/src/codec/enc32be.c
	$(CC) $(LBS_FLAGS) -Ilibs/bearssl/inc -Ilibs/bearssl/src -c $< -o $@

.SECONDARY: $(addprefix $(LBS_GEN)/,$(addsuffix .o,$(LBS_NAMES))) $(addprefix $(LBS_GEN)/,$(addsuffix _info.o,$(LBS_NAMES)))
-include $(wildcard build/sdk/lbs/*.d) $(LBS_MESA_OBJ:.o=.d) $(LBS_VIDEO_OBJ:.o=.d)

build/tests/userland/lbs_demo.raw.o: sdk/examples/lbs_demo.c
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/tests/userland/LBSTEST.BEX: build/tests/userland/lbs_demo.raw.o build/sdk/lbs_host.o build/sdk/lbs.o $(LBS_POSIX_A) $(LBS_NATIVE_A)
	$(LD) -m elf_i386 -r $^ -o $@
lbs-test: lbs build/tests/userland/LBSTEST.BEX
	$(PYTHON) tests/lbs/run_loader_host.py

build/tests/lbs/stamp: tests/lbs/build_fixtures.py
	$(PYTHON) tests/lbs/build_fixtures.py
	@touch $@
lbs-test: build/tests/lbs/stamp

# Selected app migration; keep the former UI and public native syscalls.
build/programs/network_status.raw.o: programs/network_status.c programs/system_tools_common.h sdk/include/bleskernos_lbs.h
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/programs/network_status.o: build/programs/network_status.raw.o build/sdk/lbs_host_network.o build/sdk/lbs.o $(LBS_POSIX_A) $(LBS_NATIVE_A)
	$(LD) -m elf_i386 -r build/programs/network_status.raw.o build/sdk/lbs_host_network.o build/sdk/lbs.o $(LBS_POSIX_A) $(LBS_NATIVE_A) -o $@

build/sdk/lbs_host_network.c: $(LBS_DIR)/NETWORK.LBS tools/generate_lbs_host.py
	$(PYTHON) tools/generate_lbs_host.py --modules NETWORK --output $@
build/sdk/lbs_host_network.o: build/sdk/lbs_host_network.c
	$(CC) $(SDK_CFLAGS) -c $< -o $@

build/sdk/lbs_start.o: sdk/lib/lbs_start.c sdk/include/bleskernos_lbs.h
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/sdk/libblesk_lbs.a: build/sdk/lbs.o build/sdk/lbs_start.o
	$(AR) rcs $@ $^
lbs: build/sdk/libblesk_lbs.a
build/tests/userland/lbs_startup_demo.raw.o: sdk/examples/lbs_startup_demo.c
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/tests/userland/LBSSTART.BEX: build/tests/userland/lbs_startup_demo.raw.o build/sdk/libblesk_lbs.a build/sdk/lbs_host_network.o $(LBS_POSIX_A) $(LBS_NATIVE_A)
	$(LD) -m elf_i386 -r -u bleskernos_program_main $^ -o $@
lbs-test: build/tests/userland/LBSSTART.BEX

lbs-test: $(POSIX_TEST_OBJ) $(SDL_DEMO_OBJ) $(DL_TEST_SO) $(DL_DEP_SO)

$(ATA_IMG): build/sdk/libblesk_lbs.a build/sdk/lbs_host.o

LBS_TINYGL_OBJ := $(patsubst %.c,build/sdk/lbs/pic/%.o,$(TINYGL_SOURCES))
build/sdk/lbs/pic/libs/src/%.o: libs/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -fno-pic -fno-pie,$(CC_FLAGS)) $(INCLUDE_FLAGS) -fPIC -c $< -o $@
build/sdk/lbs/tinygl.o: sdk/lib/tinygl.c
	$(CC) $(LBS_FLAGS) -c $< -o $@
$(LBS_DIR)/TINYGL.LBS: build/sdk/lbs/tinygl_backend.o build/sdk/lbs/tinygl.o $(LBS_TINYGL_OBJ) $(LBS_DIR)/NGS.LBS $(LBS_DIR)/VISION.LBS
-include $(LBS_TINYGL_OBJ:.o=.d)

.PHONY: lbs-audit
lbs-audit: lbs-test
	$(PYTHON) tests/lbs/audit_artifacts.py
	$(PYTHON) tests/lbs/audit_clients.py

$(LBS_DIR)/INPUT.LBS: build/sdk/lbs/input.o

build/tests/userland/lbs_apps.raw.o: tests/lbs/app_smoke.c
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/tests/userland/LBSAPPS.BEX: build/tests/userland/lbs_apps.raw.o $(SDK_A)
	$(LD) -m elf_i386 -r $^ -o $@
lbs-test: build/tests/userland/LBSAPPS.BEX
lbs-test: build/programs/network_status.o

build/sdk/lbs/hmac.o: libs/bearssl/src/mac/hmac.c
	$(CC) $(LBS_FLAGS) -Ilibs/bearssl/inc -Ilibs/bearssl/src -c $< -o $@
$(LBS_DIR)/CRYPTO.LBS: build/sdk/lbs/hmac.o

build/tests/userland/lbs_triangle.raw.o: sdk/examples/lbs_triangle.c
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/tests/userland/LBSTRI.BEX: build/tests/userland/lbs_triangle.raw.o build/sdk/libblesk_lbs.a build/sdk/lbs_host.o $(LBS_POSIX_A) $(LBS_NATIVE_A)
	$(LD) -m elf_i386 -r -u bleskernos_program_main $^ -o $@
lbs: build/tests/userland/LBSTRI.BEX

include mk/lbs-apps.mk


build/tests/userland/sharing_probe.raw.o: tests/lbs/sharing_probe.c sdk/include/bleskernos_lbs_stats.h
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) -c $< -o $@
build/tests/userland/LBSSHARE.BEX: build/tests/userland/sharing_probe.raw.o build/sdk/lbs_host.o build/sdk/lbs.o $(LBS_POSIX_A) $(LBS_NATIVE_A)
	$(LD) -m elf_i386 -r $^ -o $@

$(LBS_DIR)/ARCHIVE.LBS: build/sdk/lbs/archive.o

$(LBS_DIR)/VISION.LBS: build/sdk/lbs/vision_widgets.o
build/sdk/lbs/command_common.o: system/commands/common.c system/commands/common.h sdk/include/bleskernos_command_lbs.h $(wildcard system/commands/*.inc)
	$(CC) $(LBS_FLAGS) -c $< -o $@
$(LBS_DIR)/COMMAND.LBS: build/sdk/lbs/command_common.o $(LBS_DIR)/NETWORK.LBS

$(LBS_DIR)/NGS.LBS: build/sdk/lbs/image_raster.o

LBS_HZIP_OBJ := $(addprefix build/sdk/lbs/pic/libs/native/archive/hyperzip/,hyperzip.o hz_crc32.o hz_inflate.o)
build/sdk/lbs/pic/libs/native/archive/hyperzip/%.o: libs/native/archive/hyperzip/%.c sdk/include/bleskernos_hyperzip_lbs.h sdk/include/bleskernos_inflate_lbs.h
	@mkdir -p $(dir $@)
	$(CC) $(LBS_FLAGS) -DBK_LBS_HZIP -Isdk/lbs -c $< -o $@
$(LBS_DIR)/ARCHIVE.LBS: $(LBS_HZIP_OBJ)
build/sdk/lbs/pic/libs/native/media/pl_mpeg_bles.o: LBS_FLAGS += -O2
