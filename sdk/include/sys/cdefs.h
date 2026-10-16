#ifndef BLESKERNOS_SYS_CDEFS_H
#define BLESKERNOS_SYS_CDEFS_H
#define __BEGIN_DECLS
#define __END_DECLS
#define __THROW
#define __THROWNL
#define __NTH(f) f
#define __nonnull(params) __attribute__((nonnull params))
#define __attribute_malloc__ __attribute__((malloc))
#define __wur __attribute__((warn_unused_result))
#endif
