#include "common.h"

static int run(int argc UNUSED, char **argv UNUSED) { rtc_datetime_t dt; if(!bk_time_datetime(&dt)) return command_error("time","@H52516EB5"); kprintf("%02u:%02u:%02u\n",dt.time.hour,dt.time.minute,dt.time.second); return 0; }

BK_COMMAND_MAIN(run)
