/*
 * common definitions shared by all modules
 */
#ifndef COMMON_H_
#define COMMON_H_

#include <atomic_ops.h>

#define VOLATILE /* volatile */
#define BARRIER() asm volatile("" ::: "memory");

#define CAS(_m, _o, _n) \
    AO_compare_and_swap_full(((volatile AO_t*) _m), ((AO_t) _o), ((AO_t) _n))

#define WIDE_CAS(_m, _o1, _o2, _n1, _n2) \
    AO_compare_double_and_swap_double_full(((volatile AO_t*) _m), ((AO_t) _o1), ((AO_t) _o2), ((AO_t) _n1), , ((AO_t) _n2))

#define FAI(a) AO_fetch_and_add_full((volatile AO_t*) (a), 1)
#define FAD(a) AO_fetch_and_add_full((volatile AO_t*) (a), -1)

/*
 * Allow us to efficiently align and pad structures so that shared fields
 * don't cause contention on thread-local or read-only fields.
 */
#define CACHE_PAD(_n) char __pad ## _n [CACHE_LINE_SIZE]
#define ALIGNED_ALLOC(_s)                                       \
    ((void *)(((unsigned long)malloc((_s)+CACHE_LINE_SIZE*2) +  \
        CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE-1)))

#define CACHE_LINE_SIZE 64



#endif /* COMMON_H_ */
