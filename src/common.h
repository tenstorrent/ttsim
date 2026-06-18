// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Architecture-independent simulator core: the error taxonomy + reporting, bit/integer helpers,
// and value-semantic raw memory access.
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <bit>
#include <iterator>

enum class TTSimErrorCategory {
    UndefinedBehavior, // as defined in tt-isa-documentation
    UnpredictableValueUsed, // as defined in tt-isa-documentation
    NonContractualBehavior, // as defined in tt-isa-documentation
    AssertionFailure, // internal bug in simulator
    MissingSpecification, // tt-isa-documentation missing or inadequate to implement feature
    UntestedFunctionality, // implemented, but inadequate test coverage to enable yet
    UnimplementedFunctionality, // not implemented yet
    UnsupportedFunctionality, // planned to never be implemented
    SystemError, // OS errors and similar
    ConfigurationError, // bad command line options, env vars, configuration files, etc.
};

#define TTSIM_ERROR(category, fmt, ...) \
    ttsim_error(TTSimErrorCategory::category, __func__, fmt "\n", ##__VA_ARGS__)

#define TTSIM_ERROR_NOFMT(category) \
    ttsim_error(TTSimErrorCategory::category, __func__, nullptr)

#define TTSIM_VERIFY(cond, category, fmt, ...) \
    do { \
        if (!(cond)) [[unlikely]] { \
            ttsim_error(TTSimErrorCategory::category, __func__, fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

// Use ASSERT instead of VERIFY only in cases where a sufficiently smart compiler could
// prove that the condition will never be violated.
#if defined(DEBUG)
#define TTSIM_ASSERT(cond) \
    do { \
        if (!(cond)) [[unlikely]] { \
            ttsim_error(TTSimErrorCategory::AssertionFailure, __func__, "%s\n", #cond); \
        } \
    } while (0)
#else
#define TTSIM_ASSERT(cond) do {} while (0)
#endif

// Bitfield extracts analogous to Verilog x[hi:lo] notation
// Avoiding actual "class T" template arg to optimize compile times
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr uint32_t bits(uint32_t x) {
    static_assert((lo <= hi) && (hi < 32));
    return ((x >> lo) & ((2u << (hi - lo)) - 1u));
}
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr uint64_t bits(uint64_t x) {
    static_assert((lo <= hi) && (hi < 64));
    return ((x >> lo) & ((2ull << (hi - lo)) - 1ull));
}
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr int32_t signed_bits(uint32_t x) {
    static_assert((lo <= hi) && (hi < 32));
    return (int32_t(x << (31 - hi)) >> (31 - hi + lo));
}
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr int64_t signed_bits(uint64_t x) {
    static_assert((lo <= hi) && (hi < 64));
    return (int64_t(x << (63 - hi)) >> (63 - hi + lo));
}

template<int N> struct int_types;
template<> struct int_types<32> { using int_t = int32_t; using uint_t = uint32_t; };
template<> struct int_types<64> { using int_t = int64_t; using uint_t = uint64_t; };
template<> struct int_types<128> { using int_t = __int128; using uint_t = unsigned __int128; };

// Wrappers to provide value semantics without UB (similar to read/write_unaligned in Rust)
template<class T> [[nodiscard]] T mem_rd(const void *p) {
    T data;
    memcpy(&data, p, sizeof(data));
    return data;
}
template<class T> void mem_wr(void *p, T data) {
    memcpy(p, &data, sizeof(data));
}

const char *ttsim_error_category_name(TTSimErrorCategory category);
void ttsim_printf(const char *fmt, ...);
[[noreturn, gnu::cold]] void ttsim_error(TTSimErrorCategory category, const char *func, const char *fmt, ...);
uint64_t ttsim_get_clock();
