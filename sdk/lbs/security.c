#include <bleskernos_lbs_services.h>
int bk_security_context(bk_security_context_t *c) {
    if (!c || c->size < sizeof(*c))
        return BK_LBS_INVALID;
    c->pid = (uint32_t)bk_getpid();
    c->syscall_abi = (uint32_t)bk_abi_version();
    c->flags = BK_SECURITY_RING3;
    return 0;
}
int bk_security_authorize(uint32_t operation, const char *resource) {
    (void)operation;
    (void)resource;
    return BK_LBS_UNSUPPORTED;
}
int bk_security_set_privileges(uint32_t privileges) {
    (void)privileges;
    return BK_LBS_UNSUPPORTED;
}
