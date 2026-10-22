#ifndef BLESKERNOS_PRINT_LBS_H
#define BLESKERNOS_PRINT_LBS_H
#include <bleskernos_print.h>
#include <bleskernos_lbs.h>
/* Reads the real PrintSpool STS protocol, including archived terminal jobs.
   Returns 0 on success, -2 if absent, or another negative error code. */
int bk_print_job_status(const char *queue_path, char *text, uint32_t capacity);
/* Requests cancellation through PrintSpool's CAN control file. Success means
   the request was queued; the STS record confirms actual cancellation.
   Already-running jobs return BUSY; there remains an ordinary queue race. */
int bk_print_job_cancel_queued(const char *queue_path);
#endif
