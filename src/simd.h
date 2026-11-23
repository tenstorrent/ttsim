// SPDX-FileCopyrightText: (c) 2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Lanes of integers as wide as the target's vector registers, on the GCC/Clang vector extensions: no intrinsics and
// no per-ISA code, only the width below and vclz's test for a per-lane leading-zero count. Every operation on lanes is
// integer arithmetic with its C meaning in each lane (a comparison gives 0 or -1, ?: picks per lane), or a conversion
// to float that is exact, so results don't depend on the host or the width; the compiler maps them to whatever the
// target has. The width is the target's own because GCC lowers a vector wider than the target has element by element
// wherever it compares or selects.
#pragma once
#include <stdint.h>
#include <string.h>
#include <type_traits>

#if defined(__AVX512F__)
#define VEC_BYTES 64
#elif defined(__AVX2__)
#define VEC_BYTES 32
#else
#define VEC_BYTES 16
#endif

// Whether the target counts leading zeros per lane (AVX-512CD, NEON, Zvbb), for vclz
#if defined(__AVX512CD__)
#define VEC_HAS_CLZ 1
#elif defined(__ARM_NEON)
#define VEC_HAS_CLZ 1
#elif defined(__riscv_zvbb)
#define VEC_HAS_CLZ 1
#else
#define VEC_HAS_CLZ 0
#endif

typedef uint32_t vec_u32 __attribute__((vector_size(VEC_BYTES)));
typedef int32_t vec_i32 __attribute__((vector_size(VEC_BYTES)));
typedef float vec_f32 __attribute__((vector_size(VEC_BYTES)));
typedef uint64_t vec_u64 __attribute__((vector_size(VEC_BYTES)));
typedef int64_t vec_i64 __attribute__((vector_size(VEC_BYTES)));
typedef uint32_t vec_u32_half __attribute__((vector_size(VEC_BYTES / 2)));
typedef int32_t vec_i32_half __attribute__((vector_size(VEC_BYTES / 2)));

// A vector from (or into) an array of its elements, at any alignment
template<class V, class T>
static inline V vec_load(const T *p) {
    V v;
    memcpy(&v, p, sizeof(v));
    return v;
}

template<class V, class T>
static inline void vec_store(T *p, V v) { memcpy(p, &v, sizeof(v)); }

// Half 'half' of a vector of 32-bit lanes, each lane widened to 64 bits as its type extends
template<class V>
static inline vec_i64 vec_widen_half(V v, uint32_t half) {
    typedef std::conditional_t<std::is_signed_v<decltype(+v[0])>, vec_i32_half, vec_u32_half> H;
    return __builtin_convertvector(vec_load<H>(reinterpret_cast<const char *>(&v) + half * sizeof(H)), vec_i64);
}

// Whether any lane of a mask is set
template<class V>
static inline bool vec_any(V mask) {
    bool any = false;
    for (uint32_t i = 0; i < sizeof(V) / sizeof(mask[0]); i++) {
        any |= mask[i] != 0;
    }
    return any;
}

// The larger of two lanes (or of two values: code using these is written once for lanes and plain integers alike)
template<class T>
static inline T vmax(T a, T b) { return (a > b) ? a : b; }

// The leading zeros of each WIDTH-bit lane (or of an unsigned integer that wide), WIDTH for a zero lane. A target with
// a leading-zero count per lane (AVX-512CD, NEON, Zvbb) gets a loop of the scalar count, which GCC and Clang vectorize
// into it; elsewhere GCC would turn that loop into one scalar count per lane, so lanes take a branch-free shift ladder
template<uint32_t WIDTH, class U>
static inline U vclz(U x) {
    if constexpr (std::is_integral_v<U>) {
        return x ? U((WIDTH == 64) ? __builtin_clzll(x) : __builtin_clz(x)) : U(WIDTH);
    } else if constexpr (VEC_HAS_CLZ) {
        for (uint32_t i = 0; i < sizeof(U) / sizeof(x[0]); i++) {
            x[i] = vclz<WIDTH>(x[i]);
        }
        return x;
    } else {
        U n = {};
        for (uint32_t bits = WIDTH / 2; bits; bits >>= 1) {
            auto high_clear = (x >> (WIDTH - bits)) == 0;
            n = high_clear ? n + bits : n;
            x = high_clear ? x << bits : x;
        }
        return (x == 0) ? n + 1 : n;
    }
}
