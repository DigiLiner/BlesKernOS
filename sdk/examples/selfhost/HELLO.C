#include <bleskernos_api.h>

void bleskernos_program_main(void *unused) {
    (void)unused;
    bk_console_write("Hola desde un programa construido dentro de BlesKernOS.\n");
}
