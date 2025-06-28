//
// Created by tomer.cory on 1/10/25.
//

#ifndef C_CPP_SIMD_ATOMICS_H
#define C_CPP_SIMD_ATOMICS_H

#include <emmintrin.h> // For SSE2 intrinsics
#include <immintrin.h> 

inline void read_16_bytes_atomic(const __m128i *src, __m128i *dest){
    // Ensure src is 16-byte aligned and does not cross a cache line boundary!
    *(volatile __m128i*)(dest) = _mm_load_si128(src);
}

inline void write_16_bytes_atomic(__m128i *dest, const __m128i *src){
    // Ensure dest is 16-byte aligned
    _mm_store_si128(dest, *src);
}

#endif //C_CPP_SIMD_ATOMICS_H