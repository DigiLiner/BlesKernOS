#ifndef BLESKERNOS_POSIX_MATH_H
#define BLESKERNOS_POSIX_MATH_H
#ifdef __cplusplus
extern "C" {
#endif
#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LN2 0.69314718055994530942
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
double fabs(double x); double floor(double x); double ceil(double x);
double sqrt(double x); double sin(double x); double cos(double x);
double tan(double x); double atan(double x); double atan2(double y,double x);
double asin(double x); double acos(double x);
double exp(double x); double log(double x); double log10(double x);
double pow(double base,double exponent); double fmod(double x,double y);
double modf(double x,double *integer); double frexp(double x,int *exponent);
double ldexp(double x,int exponent);
float fabsf(float x); float sqrtf(float x); float sinf(float x); float cosf(float x);
#ifdef __cplusplus
}
#endif
#endif
