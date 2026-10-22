#include "common.h"
#include <bleskernos_archive_lbs.h>
static int run(int argc, char **argv) {
    void *raw = NULL, *result = NULL;
    uint32_t size = 0, result_size = 0;
    if (argc != 3) return command_error("extract", "se requieren entrada y salida");
    if (!bk_file_read_all(argv[1], &raw, &size))
        return command_error("extract", "no se pudo leer la entrada");
    int error = bk_archive_unpack(raw, size, &result, &result_size);
    bk_sys_free(raw);
    if (error) return command_error("extract", "archivo invalido, limite excedido o sin memoria");
    bool written = bk_file_write_all(argv[2], result, result_size);
    bk_sys_free(result);
    if (!written) return command_error("extract", "no se pudo escribir la salida");
    kprintf("%u -> %u bytes (%s)\n", size, result_size, argv[2]);
    return 0;
}
BK_COMMAND_MAIN(run)
