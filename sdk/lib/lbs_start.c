#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
#include <dlfcn.h>
/* Optional startup CRT for native ET_REL applications. All requirements are
   loaded before user code runs; failures unwind the completed prefix. */
extern const bk_lbs_requirement_t bk_lbs_requirements[];
extern void bk_lbs_program_main(void *argument);
void bleskernos_program_main(void *argument) {
    uint32_t i = 0;
    while (bk_lbs_requirements[i].name) {
        const bk_lbs_requirement_t *r = &bk_lbs_requirements[i];
        if (i == 32 || !r->handle || !(*r->handle = bk_lbs_open(r->name, r->major))) {
            bk_sys_log("[LBS] startup requirement failed\n");
            if (r->name)
                bk_sys_log(r->name);
            while (i) {
                const bk_lbs_requirement_t *p = &bk_lbs_requirements[--i];
                dlclose(*p->handle);
                *p->handle = 0;
            }
            return;
        }
        i++;
    }
    bk_lbs_program_main(argument);
    while (i) {
        const bk_lbs_requirement_t *r = &bk_lbs_requirements[--i];
        dlclose(*r->handle);
        *r->handle = 0;
    }
}
