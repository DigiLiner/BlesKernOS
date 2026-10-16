#include "common.h"
static int run(int argc,char **argv) {
    if(argc!=2)return command_error("sleep","uso: sleep segundos (0..4294967)");
    bool valid;uint32_t seconds=command_number(argv[1],&valid);
    if(!valid || seconds>4294967U)return command_error("sleep","duracion fuera de rango");
    bk_sys_sleep_ms(seconds*1000U);return 0;
}
BK_COMMAND_MAIN(run)
