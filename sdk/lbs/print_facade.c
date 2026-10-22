#include <bleskernos_print.h>
#include "resources.h"
extern bk_print_job_t *bk_lbs_print_begin_inner(const char *, const char *);
extern void bk_lbs_print_cancel_inner(bk_print_job_t *);
extern bool bk_lbs_print_submit_inner(bk_print_job_t *);
bk_print_job_t *print_begin(const char *title, const char *printer) {
    bk_print_job_t *job = bk_lbs_print_begin_inner(title, printer);
    if (job)
        bk_lbs_resource_acquire();
    return job;
}
void print_cancel(bk_print_job_t *job) {
    if (job) {
        bk_lbs_print_cancel_inner(job);
        bk_lbs_resource_release();
    }
}
bool print_submit(bk_print_job_t *job) {
    bool ok = bk_lbs_print_submit_inner(job);
    if (job)
        bk_lbs_resource_release();
    return ok;
}
