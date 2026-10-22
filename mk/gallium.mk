# Native POSIX/ELF32 Gallium applications. Separate from freestanding LBS:
# these need the real ELF TLS loader and Mesa's C++ runtime.
.PHONY: gallium-native gallium-native-test
GALLIUM_TEST_OUT ?= build/native-gallium/validation
MESA_DEMOS_TEST_OUT ?= build/native-mesa-demos/validation
gallium-native:
	$(PYTHON) ports/gallium/build.py
gallium-native-test: gallium-native
	$(PYTHON) tests/graphics/run_native_gallium.py --pacing-trace --out $(GALLIUM_TEST_OUT)

.PHONY: mesa-demos-native mesa-demos-native-test
mesa-demos-native: gallium-native
	$(PYTHON) ports/mesa-demos/build.py
mesa-demos-native-test: mesa-demos-native
	$(PYTHON) tests/graphics/run_native_gallium.py --mesa-demos --pacing-trace --out $(MESA_DEMOS_TEST_OUT) --seconds 400

# Temporarily include the native runtime and 3D demos in normal OS builds.
# A real file target keeps ordinary incremental builds from rebuilding Mesa.
NATIVE_3D_MANIFEST := build/native-gallium/manifest.json
NATIVE_3D_SOURCES := $(wildcard ports/gallium/*.[ch] ports/gallium/*.py ports/gallium/include/*.h \
	ports/mesa-demos/*.[ch] ports/mesa-demos/*.py ports/mesa-demos/include/GL/*.h \
	ports/mesa-demos/vendor/*.c)
$(NATIVE_3D_MANIFEST): $(NATIVE_3D_SOURCES) ports/wine/graphics/build-gallium.py \
	ports/wine/graphics/build-mesa.py ports/wine/graphics/mesa-blesk-frontend.patch \
	ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h \
	ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h \
	$(MESA35_A) $(LBS_DIR)/NGS3D.LBS build/tests/userland/LBSTRI.BEX \
	$(WINE_MESA_LIB) $(WINE_GALLIUM_FILES)
	$(PYTHON) ports/gallium/build.py
	$(PYTHON) ports/mesa-demos/build.py

$(ATA_IMG): $(NATIVE_3D_MANIFEST) tools/native_graphics_payload.py mk/gallium.mk \
	docs/graphics/NORMAL_3D_TESTS.md

user-update: $(NATIVE_3D_MANIFEST) $(SSJELLY_OBJ)
