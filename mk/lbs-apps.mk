LBS_CLIENT_CONTRACT := build/sdk/lbs/client-contract.json
$(LBS_CLIENT_CONTRACT): $(LBS_FILES) tools/generate_lbs_contract.py
	$(PYTHON) tools/generate_lbs_contract.py --output $@

# Compile migration clients separately; image tools select these payloads.
LBS_APP_RULES := build/sdk/lbs/applications.mk
$(LBS_APP_RULES): sdk/lbs/applications.json tools/generate_lbs_app_rules.py
	$(PYTHON) tools/generate_lbs_app_rules.py --output $@
include $(LBS_APP_RULES)

.PHONY: lbs-apps lbs-clients-audit
lbs-apps: $(LBS_APP_FILES)
lbs-audit: lbs-apps $(BOOT2_BIN)
lbs-clients-audit: lbs-apps
	$(PYTHON) tests/lbs/audit_clients.py

$(ATA_IMG): $(LBS_APP_FILES) tools/lbs_payload.py
user-update: $(LBS_APP_FILES) $(LBS_FILES) build/programs/network_status.o build/sdk/libblesk_lbs.a build/sdk/lbs_host.o

# Use the SDK libc once; NetSurf's private compatibility libc is kept only in
# the historical comparison artifact, never mixed with the loader runtime.
build/programs/netsurf.lbs.raw.o: $(filter-out build/programs/netsurf/compat.o build/programs/netsurf/string.o,$(NETSURF_RAW_OBJS)) build/programs/netsurf/lbs_memory.o
	$(LD) -m elf_i386 -r $^ -o $@
	objcopy --redefine-sym malloc=bk_sys_alloc --redefine-sym free=bk_sys_free --redefine-sym realloc=bk_sys_realloc --redefine-sym calloc=nsbk_lbs_calloc --redefine-sym strdup=nsbk_lbs_strdup $@

.PHONY: lbs-extras
lbs-extras: lbs
	$(PYTHON) tools/build_lbs_extras.py
