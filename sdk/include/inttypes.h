#ifndef BLESKERNOS_INTTYPES_H
#define BLESKERNOS_INTTYPES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {intmax_t quot, rem;} imaxdiv_t;
intmax_t imaxabs(intmax_t value);
imaxdiv_t imaxdiv(intmax_t numerator,intmax_t denominator);
intmax_t strtoimax(const char *text,char **end,int base);
uintmax_t strtoumax(const char *text,char **end,int base);
/* Matches the i386 ILP32 SDK typedefs. */
#define PRId8 "d"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRId16 "d"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIo32 "o"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu64 "llu"
#define PRIo64 "llo"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define PRIdMAX PRId64
#define PRIuMAX PRIu64
#define PRIxMAX PRIx64
#define PRIdPTR PRId32
#define PRIuPTR PRIu32
#define PRIxPTR PRIx32
#define SCNd32 "d"
#define SCNu32 "u"
#define SCNx32 "x"
#define SCNd64 "lld"
#define SCNu64 "llu"
#define SCNx64 "llx"
#ifdef __cplusplus
}
#endif
#endif
