#ifndef BLES_ARCH_ARM32_RPI3_ELF_ARM_H
#define BLES_ARCH_ARM32_RPI3_ELF_ARM_H

#include "../../../kernel/include/types.h"

/* Stage 11 loader for BlesKernOS native AArch32 applications.  ARM .BEX files
 * are ELF32 little-endian ET_REL/EM_ARM objects; unresolved kernel/API symbols
 * are deliberately rejected until the Stage 12 public-API thunk layer. */
int rpi3_elf_spawn(const char *path, const char *launch_arg);
int rpi3_elf_spawn_memory(const char *name, const void *data, uint32_t size,
                          const char *launch_arg);
const char *rpi3_elf_last_error(void);

#endif
