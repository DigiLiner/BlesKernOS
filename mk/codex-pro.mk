# Bles Codex Pro integration for BlesKernOS 0.9
# This file is included from the root Makefile by tools/install_codex_pro.py.

CODEX_PRO_DIR ?= extras/Bles-Codex32-standalone
CODEX_PRO_IMAGE ?= build/codex-pro-sdk.img
CODEX_PRO_BUILDER := tools/build_codex_pro_image.py
CODEX_PRO_VERIFY := tools/verify_codex_pro_image.py

.PHONY: codex-pro codex-pro-build codex-pro-image codex-pro-verify codex-pro-clean

codex-pro: codex-pro-image

codex-pro-build: sdk
	@test -d "$(CODEX_PRO_DIR)" || { echo "[CODEX] no existe $(CODEX_PRO_DIR)"; exit 1; }
	@echo "[CODEX] Compilando Bles Codex con el SDK actual..."
	@$(MAKE) -C "$(CODEX_PRO_DIR)" BLESKERNOS_SDK=../../sdk

# `selfhost` guarantees that GCC/binutils/make are available for the image.
codex-pro-image: codex-pro-build selfhost $(CODEX_PRO_BUILDER)
	@mkdir -p build
	@echo "[CODEX] Creando imagen de desarrollo con SDK + toolchain..."
	@$(PYTHON) $(CODEX_PRO_BUILDER) \
		--root . \
		--codex-dir "$(CODEX_PRO_DIR)" \
		--templates tools/codex_pro/templates \
		--output "$(CODEX_PRO_IMAGE)"
	@$(PYTHON) $(CODEX_PRO_VERIFY) "$(CODEX_PRO_IMAGE)"
	@echo "[OK] Bles Codex Pro: $(CODEX_PRO_IMAGE)"

codex-pro-verify:
	@$(PYTHON) $(CODEX_PRO_VERIFY) "$(CODEX_PRO_IMAGE)"

codex-pro-clean:
	@rm -f "$(CODEX_PRO_IMAGE)"
