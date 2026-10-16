/* Initial-exec i386 TLS exercises PT_TLS, GS and R_386_TLS_TPOFF without
 * requiring a libc implementation of __tls_get_addr. */
static __thread int tls_value=40;

static void __attribute__((constructor)) tls_constructor(void) {
    tls_value++;
}

int blesk_tls_value(void) { return tls_value; }

