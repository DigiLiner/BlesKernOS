AUDIT_OUT := tests/kernel/evidence/audit-stage2-2026-10-13/baseline
.PHONY: audit-stage2-baseline
audit-stage2-baseline:
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -Ikernel/api -c tests/kernel/evidence/audit-stage2-2026-10-13/syscall-before.c -o $(AUDIT_OUT)/syscall.o
	$(CC) $(CC_FLAGS) $(INCLUDE_FLAGS) -Ikernel/mm -c tests/kernel/evidence/audit-stage2-2026-10-13/paging-before.c -o $(AUDIT_OUT)/paging.o
	$(LD) $(LD_FLAGS) $(KERNEL_ENTRY_OBJ) $(KERNEL_ASM_OBJS) $(KERNEL_GAS_OBJS) $(foreach obj,$(KERNEL_OBJS),$(if $(filter build/kernel/api/syscall.o,$(obj)),$(AUDIT_OUT)/syscall.o,$(if $(filter build/kernel/mm/paging.o,$(obj)),$(AUDIT_OUT)/paging.o,$(obj)))) $(LIBC_A) -o $(AUDIT_OUT)/kernel.elf
	objcopy -O binary $(AUDIT_OUT)/kernel.elf $(AUDIT_OUT)/kernel.bin
