#ifndef BLESKERNOS_LBS_H
#define BLESKERNOS_LBS_H
#include <bleskernos.h>
#include <stdint.h>
#define BK_LBS_ABI_MAJOR 1U
#define BK_LBS_DIRECTORY "/SYSTEM/"
#define BK_LBS_OK 0
#define BK_LBS_INVALID (-22)
#define BK_LBS_UNSUPPORTED (-38)
#define BK_LBS_BUSY (-16)
#define BK_LBS_NOMEM (-12)
/* Append-only within a major version. Query size before reading new fields. */
typedef struct {
    uint32_t size, abi_major, abi_minor;
    const char *name;
    uint32_t syscall_abi;
} bk_lbs_info_t;
typedef struct {
    const char *name;
    uint32_t major;
    void **handle;
} bk_lbs_requirement_t;
/* Optional CRT: terminate this application-defined array with {0,0,0}, and
   implement bk_lbs_program_main instead of bleskernos_program_main. */
extern const bk_lbs_requirement_t bk_lbs_requirements[];
void bk_lbs_program_main(void *argument);
/* Every module exports this symbol; use dlsym on its own handle. */
const bk_lbs_info_t *bk_lbs_info(void);
void *bk_lbs_open(const char *name, uint32_t major);
int bk_dl_pin(void *module);
/* Selectively load modules, retain handles while their objects are alive. */
#endif
