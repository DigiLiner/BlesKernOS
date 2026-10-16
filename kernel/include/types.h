#ifndef TYPES_H
#define TYPES_H

/* Tipos de tamaño fijo para un kernel 32-bit freestanding */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint32_t size_t;
typedef int32_t intptr_t;
typedef uint32_t uintptr_t;

#define NULL ((void*)0)
#define true 1
#define false 0
typedef uint8_t bool;

/* Atributos GCC útiles para kernel */
#define PACKED   __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define UNUSED   __attribute__((unused))

/* TinyCC targets the same i486 ABI but does not provide GCC's legacy
 * __sync_* intrinsics. Keep source compatibility with small locked helpers;
 * all uses in BlesKernOS are 32-bit words or pointers. */
#ifdef __TINYC__
static inline uint32_t bk_tcc_atomic_xchg(volatile uint32_t *p,uint32_t v){__asm__ volatile("xchgl %0, %1":"+r"(v),"+m"(*p)::"memory");return v;}
static inline uint32_t bk_tcc_atomic_cas(volatile uint32_t *p,uint32_t old,uint32_t value){uint32_t seen;__asm__ volatile("lock cmpxchgl %3, %1":"=a"(seen),"+m"(*p):"0"(old),"r"(value):"memory","cc");return seen;}
static inline uint32_t bk_tcc_atomic_add(volatile uint32_t *p,uint32_t value){uint32_t old=value;__asm__ volatile("lock xaddl %0, %1":"+r"(old),"+m"(*p)::"memory","cc");return old;}
static inline uint32_t bk_tcc_atomic_or(volatile uint32_t *p,uint32_t value){uint32_t old=*p,seen;do{seen=bk_tcc_atomic_cas(p,old,old|value);if(seen==old)return old;old=seen;}while(1);}
static inline void bk_tcc_atomic_barrier(void){__asm__ volatile("lock addl $0, (%%esp)":::"memory","cc");}
#define __sync_synchronize() bk_tcc_atomic_barrier()
#define __sync_fetch_and_or(p,v) bk_tcc_atomic_or((volatile uint32_t*)(p),(uint32_t)(v))
#define __sync_fetch_and_add(p,v) bk_tcc_atomic_add((volatile uint32_t*)(p),(uint32_t)(v))
#define __sync_add_and_fetch(p,v) (bk_tcc_atomic_add((volatile uint32_t*)(p),(uint32_t)(v))+(v))
#define __sync_sub_and_fetch(p,v) (bk_tcc_atomic_add((volatile uint32_t*)(p),(uint32_t)-(int32_t)(v))-(v))
#define __sync_lock_test_and_set(p,v) ((__typeof__(*(p)))bk_tcc_atomic_xchg((volatile uint32_t*)(p),(uint32_t)(uintptr_t)(v)))
#define __sync_val_compare_and_swap(p,o,v) ((__typeof__(*(p)))bk_tcc_atomic_cas((volatile uint32_t*)(p),(uint32_t)(uintptr_t)(o),(uint32_t)(uintptr_t)(v)))
#define __sync_bool_compare_and_swap(p,o,v) (bk_tcc_atomic_cas((volatile uint32_t*)(p),(uint32_t)(uintptr_t)(o),(uint32_t)(uintptr_t)(v))==(uint32_t)(uintptr_t)(o))
#define __sync_lock_release(p) do{bk_tcc_atomic_barrier();*(p)=0;}while(0)
#endif

#endif
