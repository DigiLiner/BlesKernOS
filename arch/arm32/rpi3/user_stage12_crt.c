#include "../../../sdk/include/bleskernos_api.h"

extern void bleskernos_program_main(void *desktop);

void _start(void) NORETURN;
void _start(void) {
    bleskernos_program_main(NULL);
    bk_proc_exit_status(0);
}
