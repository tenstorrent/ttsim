// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Full-system RV64 F/D floating point with RISC-V semantics (NaN-boxing, canonical NaN, fflags, FS).
#include "rv64_fpu.h"

#define FFLAG_NX (1u << 0) // inexact
#define FFLAG_UF (1u << 1) // underflow
#define FFLAG_OF (1u << 2) // overflow
#define FFLAG_DZ (1u << 3) // divide by zero
#define FFLAG_NV (1u << 4) // invalid

static void set_flags(Rv64SysHartState *h, uint32_t f) {
    h->fcsr |= f;
}

static inline void fp_mark_dirty(Rv64SysHartState *h) {
    h->mstatus |= MSTATUS_FS | MSTATUS_SD;
}

static inline uint32_t hostfp_result_s(float f) {
    uint32_t b = std::bit_cast<uint32_t>(f);
    return ((b & 0x7FFFFFFF) > 0x7F800000) ? 0x7FC00000 : b;
}

static inline uint64_t hostfp_result_d(double d) {
    uint64_t b = std::bit_cast<uint64_t>(d);
    return ((b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull) ? 0x7FF8000000000000ull : b;
}

#if defined(__x86_64__)
#include <immintrin.h>

static uint32_t s_saved_mxcsr;

static inline uint32_t hostfp_status_read() {
    uint32_t v;
    asm volatile("stmxcsr %0" : "=m"(v) :: "memory");
    return v;
}

static inline void hostfp_status_write(uint32_t v) {
    asm volatile("ldmxcsr %0" :: "m"(v) : "memory");
}

static inline uint32_t hostfp_round_bits(uint32_t rm) {
    switch (rm) {
        case 0: return 0u << 13; // RNE
        case 1: return 3u << 13; // RTZ
        case 2: return 1u << 13; // RDN
        case 3: return 2u << 13; // RUP
        case 4: TTSIM_ERROR(UnimplementedFunctionality, "rm=RMM");
        default: TTSIM_ERROR(AssertionFailure, "fp-rm=%u", rm);
    }
}

static __attribute__((noinline)) void fp_begin(uint32_t rm) {
    s_saved_mxcsr = hostfp_status_read();
    uint32_t mxcsr = s_saved_mxcsr;
    mxcsr &= ~0x603Fu; // clear exception flags + rounding-control
    mxcsr |= 0x1F80u; // mask host FP exceptions while we collect guest flags
    mxcsr |= hostfp_round_bits(rm);
    hostfp_status_write(mxcsr);
}

static __attribute__((noinline)) uint32_t fp_end_flags() {
    uint32_t e = hostfp_status_read();
    hostfp_status_write(s_saved_mxcsr);
    uint32_t f = 0;
    if (e & (1u << 0)) {
        f |= 1u << 4; // IE -> NV
    }
    if (e & (1u << 2)) {
        f |= 1u << 3; // ZE -> DZ
    }
    if (e & (1u << 3)) {
        f |= 1u << 2; // OE -> OF
    }
    if (e & (1u << 4)) {
        f |= 1u << 1; // UE -> UF
    }
    if (e & (1u << 5)) {
        f |= 1u << 0; // PE -> NX
    }
    return f;
}

static inline float fp_fma_s(float a, float b, float c) {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}

static inline double fp_fma_d(double a, double b, double c) {
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
}

static inline float fp_sqrt_s(float a) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(a)));
}

static inline double fp_sqrt_d(double a) {
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(a)));
}

#elif defined(__aarch64__)

static uint64_t s_saved_fpcr, s_saved_fpsr;

static inline uint64_t hostfp_fpcr_read() {
    uint64_t v;
    asm volatile("mrs %0, fpcr" : "=r"(v) :: "memory");
    return v;
}

static inline void hostfp_fpcr_write(uint64_t v) {
    asm volatile("msr fpcr, %0" :: "r"(v) : "memory");
}

static inline uint64_t hostfp_fpsr_read() {
    uint64_t v;
    asm volatile("mrs %0, fpsr" : "=r"(v) :: "memory");
    return v;
}

static inline void hostfp_fpsr_write(uint64_t v) {
    asm volatile("msr fpsr, %0" :: "r"(v) : "memory");
}

static inline uint64_t hostfp_round_bits(uint32_t rm) {
    switch (rm) {
        case 0: return 0ull << 22; // RNE
        case 1: return 3ull << 22; // RTZ
        case 2: return 2ull << 22; // RDN
        case 3: return 1ull << 22; // RUP
        case 4: TTSIM_ERROR(UnimplementedFunctionality, "rm=RMM");
        default: TTSIM_ERROR(AssertionFailure, "fp-rm=%u", rm);
    }
}

static __attribute__((noinline)) void fp_begin(uint32_t rm) {
    s_saved_fpcr = hostfp_fpcr_read();
    s_saved_fpsr = hostfp_fpsr_read();
    uint64_t fpcr = s_saved_fpcr;
    fpcr &= ~(3ull << 22);
    fpcr |= hostfp_round_bits(rm);
    hostfp_fpsr_write(0);
    hostfp_fpcr_write(fpcr);
}

static __attribute__((noinline)) uint32_t fp_end_flags() {
    uint64_t e = hostfp_fpsr_read();
    hostfp_fpcr_write(s_saved_fpcr);
    hostfp_fpsr_write(s_saved_fpsr);
    uint32_t f = 0;
    if (e & (1u << 0)) {
        f |= 1u << 4; // IOC -> NV
    }
    if (e & (1u << 1)) {
        f |= 1u << 3; // DZC -> DZ
    }
    if (e & (1u << 2)) {
        f |= 1u << 2; // OFC -> OF
    }
    if (e & (1u << 3)) {
        f |= 1u << 1; // UFC -> UF
    }
    if (e & (1u << 4)) {
        f |= 1u << 0; // IXC -> NX
    }
    return f;
}

static inline float fp_fma_s(float a, float b, float c) {
    float r;
    asm volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
    return r;
}

static inline double fp_fma_d(double a, double b, double c) {
    double r;
    asm volatile("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
    return r;
}

static inline float fp_sqrt_s(float a) {
    float r;
    asm volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(a));
    return r;
}

static inline double fp_sqrt_d(double a) {
    double r;
    asm volatile("fsqrt %d0, %d1" : "=w"(r) : "w"(a));
    return r;
}

#else

#include <cfenv>
#include <cmath>

static fenv_t s_saved_fenv;

static inline int hostfp_round_mode(uint32_t rm) {
    switch (rm) {
        case 0: return FE_TONEAREST;
        case 1: return FE_TOWARDZERO;
        case 2: return FE_DOWNWARD;
        case 3: return FE_UPWARD;
        case 4: TTSIM_ERROR(UnimplementedFunctionality, "rm=RMM");
        default: TTSIM_ERROR(AssertionFailure, "fp-rm=%u", rm);
    }
}

static __attribute__((noinline)) void fp_begin(uint32_t rm) {
    fegetenv(&s_saved_fenv);
    feclearexcept(FE_ALL_EXCEPT);
    fesetround(hostfp_round_mode(rm));
}

static __attribute__((noinline)) uint32_t fp_end_flags() {
    int e = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(&s_saved_fenv);
    uint32_t f = 0;
    if (e & FE_INVALID) {
        f |= 1u << 4;
    }
    if (e & FE_DIVBYZERO) {
        f |= 1u << 3;
    }
    if (e & FE_OVERFLOW) {
        f |= 1u << 2;
    }
    if (e & FE_UNDERFLOW) {
        f |= 1u << 1;
    }
    if (e & FE_INEXACT) {
        f |= 1u << 0;
    }
    return f;
}

static inline float fp_fma_s(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
static inline double fp_fma_d(double a, double b, double c) { return __builtin_fma(a, b, c); }
static inline float fp_sqrt_s(float a) { return __builtin_sqrtf(a); }
static inline double fp_sqrt_d(double a) { return __builtin_sqrt(a); }

#endif

// RISC-V rounding modes (frm / inst rm field).
enum {
    RV_RNE = 0, // round to nearest, ties to even
    RV_RTZ = 1, // toward zero
    RV_RDN = 2, // down (toward -inf)
    RV_RUP = 3, // up (toward +inf)
    RV_RMM = 4, // to nearest, ties to max magnitude
};

// RISC-V fflags bits.
enum {
    SF_NX = 1 << 0, // inexact
    SF_UF = 1 << 1, // underflow
    SF_OF = 1 << 2, // overflow
    SF_DZ = 1 << 3, // divide by zero
    SF_NV = 1 << 4, // invalid
};

template<class U, class U2, int TOT, int MB>
struct SoftFloat {
    static constexpr int EB = TOT - 1 - MB;
    static constexpr int BIAS = (1 << (EB - 1)) - 1;
    static constexpr U EMAXFIELD = (U(1) << EB) - 1;
    static constexpr U MMASK = (U(1) << MB) - 1;
    static constexpr U SIGN = U(1) << (TOT - 1);
    static constexpr U QNAN = (EMAXFIELD << MB) | (U(1) << (MB - 1));
    static constexpr U INF = EMAXFIELD << MB;

    static bool is_nan(U a) { return ((a & (EMAXFIELD << MB)) == (EMAXFIELD << MB)) && (a & MMASK); }
    static bool is_snan(U a) { return is_nan(a) && !(a & (U(1) << (MB - 1))); }
    static bool is_inf(U a) { return ((a & (EMAXFIELD << MB)) == (EMAXFIELD << MB)) && !(a & MMASK); }
    static bool is_zero(U a) { return !(a & ~SIGN); }
    static bool sign_of(U a) { return (a & SIGN) != 0; }

    struct Unp { bool sign; int exp; U2 mant; bool zero; bool inf; bool nan; bool snan; };
    static Unp unpack(U a) {
        Unp u;
        u.sign = sign_of(a);
        U ef = (a >> MB) & EMAXFIELD;
        U mf = a & MMASK;
        u.nan = (ef == EMAXFIELD) && mf;
        u.snan = u.nan && !(mf & (U(1) << (MB - 1)));
        u.inf = (ef == EMAXFIELD) && !mf;
        u.zero = (ef == 0) && !mf;
        if (ef == 0) {
            // Subnormal (or zero): normalize so the MSB sits at bit MB (like a normal significand)
            // and lower the exponent to match, giving div/sqrt full quotient precision. Value-
            // preserving, and add/mul use the actual MSB so they are unaffected.
            u.exp = 1 - BIAS;
            u.mant = mf;
            if (mf != 0) {
                int msb = 0;
                {
                    U2 t = mf;
                    while (t >>= 1) {
                        msb++;
                    }
                }
                int sh = MB - msb;
                u.mant = mf << sh;
                u.exp -= sh;
            }
        } else {
            u.exp = int(ef) - BIAS;
            u.mant = mf | (U2(1) << MB); // normal (implicit bit)
        }
        return u;
    }

    // Convert this float to an integer; clamp on overflow/NaN and set NV. `bits` in {32,64}, signed/unsigned.
    static uint64_t to_int(U a, uint32_t rm, uint32_t *fl, int bits, bool is_signed) {
        // These are the result REGISTER values (32-bit W/WU results are sign-extended to 64 bits, per
        // spec). For unsigned 32 that is 0xFFFFFFFF sign-extended = all-ones, NOT 0x00000000FFFFFFFF;
        // the early NaN/Inf/overflow returns below use maxpos directly, so it must already be extended.
        uint64_t maxpos = is_signed ? ((bits == 32) ? 0x7FFFFFFFull : 0x7FFFFFFFFFFFFFFFull)
                                    : ((bits == 32) ? 0xFFFFFFFFFFFFFFFFull : 0xFFFFFFFFFFFFFFFFull);
        uint64_t minneg = is_signed ? ((bits == 32) ? 0xFFFFFFFF80000000ull : 0x8000000000000000ull) : 0;
        if (is_nan(a)) {
            *fl |= SF_NV;
            return maxpos;
        }
        bool sign = sign_of(a);
        if (is_inf(a)) {
            *fl |= SF_NV;
            return sign ? minneg : maxpos;
        }
        Unp x = unpack(a);
        if (x.zero) {
            return 0;
        }
        int sh = x.exp - MB; // value = mant * 2^sh
        uint64_t mant = uint64_t(x.mant);
        uint64_t intval;
        bool inexact = false;
        if (sh >= 0) {
            if (sh >= 64 || (mant != 0 && msb64(mant) + sh >= 64)) {
                *fl |= SF_NV;
                return sign ? minneg : maxpos;
            }
            intval = mant << sh;
        } else {
            int rs = -sh; // rs >= 1 here (sh < 0); value = mant * 2^-rs, all below the integer's LSB
            uint64_t lost = (rs < 64) ? (mant & ((uint64_t(1) << rs) - 1)) : mant;
            intval = (rs < 64) ? (mant >> rs) : 0;
            inexact = lost != 0;
            // Round the integer per rm using the discarded fraction. The half-ulp threshold is 2^(rs-1);
            // when rs > 64 it exceeds any 64-bit `lost`, so the value is strictly below 0.5 and the
            // nearest modes must NOT round up (the old `: 0` made half=0, rounding tiny values to 1).
            uint64_t half = (rs <= 64) ? (uint64_t(1) << (rs - 1)) : ~uint64_t(0);
            uint64_t frac = lost;
            bool roundup = false;
            switch (rm) {
                case RV_RNE: roundup = (frac > half) || (frac == half && (intval & 1)); break;
                case RV_RTZ: roundup = false; break;
                case RV_RDN: roundup = sign && frac; break;
                case RV_RUP: roundup = !sign && frac; break;
                case RV_RMM: roundup = (frac >= half) && (half != 0); break;
                default: roundup = (frac > half) || (frac == half && (intval & 1)); break;
            }
            if (roundup) {
                intval++;
            }
        }
        if (inexact) {
            *fl |= SF_NX;
        }
        if (is_signed) {
            // range check against signed bounds
            uint64_t lim = (bits == 32) ? 0x80000000ull : 0x8000000000000000ull;
            if (!sign && intval > lim - 1) {
                *fl |= SF_NV;
                return maxpos;
            }
            if (sign && intval > lim) {
                *fl |= SF_NV;
                return minneg;
            }
            uint64_t r = sign ? (~intval + 1) : intval;
            if (bits == 32) { // sign-extend W result
                r = uint64_t(int64_t(int32_t(uint32_t(r))));
            }
            return r;
        } else {
            if (sign) {
                if (intval != 0) {
                    *fl |= SF_NV;
                    return 0;
                }
                return 0;
            }
            if (bits == 32 && intval > 0xFFFFFFFFull) {
                *fl |= SF_NV;
                return maxpos;
            }
            uint64_t r = intval;
            if (bits == 32) { // FCVT.WU sign-extends per spec
                r = uint64_t(int64_t(int32_t(uint32_t(r))));
            }
            return r;
        }
    }

    static int msb64(uint64_t v) {
        int m = 0;
        while (v >>= 1) {
            m++;
        }
        return m;
    }
};

using SF32 = SoftFloat<uint32_t, uint64_t, 32, 23>;
using SF64 = SoftFloat<uint64_t, unsigned __int128, 64, 52>;

static const uint32_t F32_CANON_NAN = 0x7FC00000u;
static const uint64_t F64_CANON_NAN = 0x7FF8000000000000ull;

static uint64_t box32(uint32_t v) {
    return 0xFFFFFFFF00000000ull | v;
}

static uint32_t unbox32(uint64_t r) {
    return ((r >> 32) == 0xFFFFFFFFu) ? uint32_t(r) : F32_CANON_NAN;
}

static bool f32_is_nan(uint32_t a) {
    return ((a & 0x7F800000u) == 0x7F800000u) && (a & 0x007FFFFFu);
}

static bool f32_is_snan(uint32_t a) {
    return f32_is_nan(a) && !(a & 0x00400000u);
}

static bool f64_is_nan(uint64_t a) {
    return ((a & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) && (a & 0x000FFFFFFFFFFFFFull);
}

static bool f64_is_snan(uint64_t a) {
    return f64_is_nan(a) && !(a & 0x0008000000000000ull);
}

static inline bool fp_off(Rv64SysHartState *h) {
    return (h->mstatus & MSTATUS_FS) == 0;
}

static bool rv64_fpu_op_encoding_ok(uint32_t funct7, uint32_t funct3, uint32_t r2) {
    switch (funct7) {
        case 0x00: case 0x01: case 0x04: case 0x05: // FADD/FSUB (rm in funct3, checked at use)
        case 0x08: case 0x09: case 0x0C: case 0x0D: return true; // FMUL/FDIV
        case 0x2C: case 0x2D: return r2 == 0;                    // FSQRT.S/.D (rs2 reserved)
        case 0x10: case 0x11: return funct3 <= 2;                // FSGNJ[N/X].S/.D
        case 0x14: case 0x15: return funct3 <= 1;                // FMIN/FMAX.S/.D
        case 0x50: case 0x51: return funct3 <= 2;                // FLE/FLT/FEQ.S/.D
        case 0x20: return r2 == 1;                               // FCVT.S.D (source = D)
        case 0x21: return r2 == 0;                               // FCVT.D.S (source = S)
        case 0x60: case 0x61: case 0x68: case 0x69: return r2 <= 3; // FCVT int<->fp (rs2 selects width/sign)
        case 0x70: case 0x71: return (funct3 <= 1) && (r2 == 0); // FMV.X.W/D, FCLASS.S/.D
        case 0x78: case 0x79: return (funct3 == 0) && (r2 == 0); // FMV.W.X / FMV.D.X
        default: return false;                                   // reserved funct7
    }
}

void rv64_fpu_load(Rv64SysHartState *h, uint32_t inst, uint32_t size) {
    if (fp_off(h)) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    uint32_t r_base = bits<19,15>(inst);
    uint32_t r_dst = bits<11,7>(inst);
    int64_t imm = int32_t(inst) >> 20; // I-immediate
    uint64_t addr = h->x_regs[r_base] + imm;
    if (size == 4) {
        uint32_t v;
        if (!rv64_sys_load(h, addr, &v, 4)) {
            return;
        }
        h->f_regs[r_dst] = box32(v); // NaN-box FLW
    } else { // size == 8
        uint64_t v;
        if (!rv64_sys_load(h, addr, &v, 8)) {
            return;
        }
        h->f_regs[r_dst] = v;
    }
    fp_mark_dirty(h);
}

void rv64_fpu_store(Rv64SysHartState *h, uint32_t inst, uint32_t size) {
    if (fp_off(h)) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    uint32_t r_base = bits<19,15>(inst);
    uint32_t r_src = bits<24,20>(inst);
    int64_t imm = int32_t((inst & 0xFE000000u) | ((inst & 0xF80u) << 13)) >> 20; // S-immediate
    uint64_t addr = h->x_regs[r_base] + imm;
    if (size == 4) {
        uint32_t v = uint32_t(h->f_regs[r_src]);
        if (!rv64_sys_store(h, addr, &v, 4)) {
            return;
        }
    } else {
        uint64_t v = h->f_regs[r_src];
        if (!rv64_sys_store(h, addr, &v, 8)) {
            return;
        }
    }
}

template<class U>
static U fsgnj(U a, U b, uint32_t mode, U sign_mask) {
    U s = (mode == 0) ? (b & sign_mask) : (mode == 1) ? ((~b) & sign_mask) : ((a ^ b) & sign_mask);
    return (a & ~sign_mask) | s;
}

static uint32_t hostfp_add_s(uint32_t a, uint32_t b, uint32_t rm, uint32_t *fl, bool sub) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    fp_begin(rm);
    float r = sub ? fa - fb : fa + fb;
    *fl |= fp_end_flags();
    return hostfp_result_s(r);
}

static uint64_t hostfp_add_d(uint64_t a, uint64_t b, uint32_t rm, uint32_t *fl, bool sub) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b);
    fp_begin(rm);
    double r = sub ? fa - fb : fa + fb;
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

static uint32_t hostfp_mul_s(uint32_t a, uint32_t b, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    fp_begin(rm);
    float r = fa * fb;
    *fl |= fp_end_flags();
    return hostfp_result_s(r);
}

static uint64_t hostfp_mul_d(uint64_t a, uint64_t b, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b);
    fp_begin(rm);
    double r = fa * fb;
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

static uint32_t hostfp_div_s(uint32_t a, uint32_t b, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    fp_begin(rm);
    float r = fa / fb;
    *fl |= fp_end_flags();
    return hostfp_result_s(r);
}

static uint64_t hostfp_div_d(uint64_t a, uint64_t b, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b);
    fp_begin(rm);
    double r = fa / fb;
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

static uint32_t hostfp_sqrt_s(uint32_t a, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a);
    fp_begin(rm);
    float r = fp_sqrt_s(fa);
    *fl |= fp_end_flags();
    return hostfp_result_s(r);
}

static uint64_t hostfp_sqrt_d(uint64_t a, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a);
    fp_begin(rm);
    double r = fp_sqrt_d(fa);
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

static uint32_t hostfp_fma_s(uint32_t a, uint32_t b, uint32_t c, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b), fc = std::bit_cast<float>(c);
    fp_begin(rm);
    float r = fp_fma_s(fa, fb, fc);
    *fl |= fp_end_flags();
    return hostfp_result_s(r);
}

static uint64_t hostfp_fma_d(uint64_t a, uint64_t b, uint64_t c, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b), fc = std::bit_cast<double>(c);
    fp_begin(rm);
    double r = fp_fma_d(fa, fb, fc);
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

static uint32_t hostfp_cvt_s_d(uint64_t d, uint32_t rm, uint32_t *fl) { // FCVT.S.D : double -> single
    double x = std::bit_cast<double>(d);
    fp_begin(rm);
    float r = float(x);
    *fl |= fp_end_flags();
    return hostfp_result_s(r);
}

static uint64_t hostfp_cvt_d_s(uint32_t f, uint32_t rm, uint32_t *fl) { // FCVT.D.S : single -> double
    float x = std::bit_cast<float>(f);
    fp_begin(rm);
    double r = double(x);
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

static uint64_t hostfp_to_int_s(uint32_t a, uint32_t rm, uint32_t *fl, int bits, bool sgn) {
    return SF32::to_int(a, rm, fl, bits, sgn);
}

static uint64_t hostfp_to_int_d(uint64_t a, uint32_t rm, uint32_t *fl, int bits, bool sgn) {
    return SF64::to_int(a, rm, fl, bits, sgn);
}

static uint32_t hostfp_from_int_s(uint64_t xr, uint32_t r2, uint32_t rm, uint32_t *fl) {
    fp_begin(rm);
    float r = (r2 == 0) ? float(int32_t(xr)) : (r2 == 1) ? float(uint32_t(xr))
            : (r2 == 2) ? float(int64_t(xr)) : float(uint64_t(xr));
    *fl |= fp_end_flags();
    return std::bit_cast<uint32_t>(r);
}

static uint64_t hostfp_from_int_d(uint64_t xr, uint32_t r2, uint32_t rm, uint32_t *fl) {
    fp_begin(rm);
    double r = (r2 == 0) ? double(int32_t(xr)) : (r2 == 1) ? double(uint32_t(xr))
             : (r2 == 2) ? double(int64_t(xr)) : double(uint64_t(xr));
    *fl |= fp_end_flags();
    return std::bit_cast<uint64_t>(r);
}

void rv64_fpu_op(Rv64SysHartState *h, uint32_t inst) {
    if (fp_off(h)) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    uint32_t funct7 = bits<31,25>(inst);
    uint32_t funct3 = bits<14,12>(inst);
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r1 = bits<19,15>(inst);
    uint32_t r2 = bits<24,20>(inst);
    bool is_double = funct7 & 1; // fmt: 00=S, 01=D
    // Reject reserved funct3/rs2/funct7 encodings before mutating any FP state.
    if (!rv64_fpu_op_encoding_ok(funct7, funct3, r2)) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    // Arithmetic/convert ops use funct3 as the rounding mode; an illegal mode (reserved static 5/6, or
    // dynamic rm=7 resolving to a reserved frm>4) must trap WITHOUT mutating architectural state, so
    // validate it BEFORE fp_mark_dirty below. The move/sign/compare/classify/move-X ops
    // (funct7 0x10/0x11/0x14/0x15/0x50/0x51/0x70/0x71/0x78/0x79) use funct3 as a sub-op, not a mode.
    bool rm_op = !((funct7 == 0x10) || (funct7 == 0x11) || (funct7 == 0x14) || (funct7 == 0x15) ||
                   (funct7 == 0x50) || (funct7 == 0x51) || (funct7 == 0x70) || (funct7 == 0x71) ||
                   (funct7 == 0x78) || (funct7 == 0x79));
    if (rm_op && ((funct3 == 7 ? ((h->fcsr >> 5) & 7) : funct3) > 4)) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    // All FP ALU/convert/move/compare ops write an f-register or fcsr (-> FS Dirty) EXCEPT the
    // fp->x moves FMV.X.*/FCLASS (funct7 0x70/0x71), which read only. FP ops do not fault, so
    // marking here (before execution) is equivalent to marking at retirement.
    if ((funct7 != 0x70) && (funct7 != 0x71)) {
        fp_mark_dirty(h);
    }

    switch (funct7) {
        case 0x10: case 0x11: { // FSGNJ[N/X].S / .D
            if (is_double) {
                h->f_regs[r_dst] = fsgnj<uint64_t>(h->f_regs[r1], h->f_regs[r2], funct3, 0x8000000000000000ull);
            } else {
                h->f_regs[r_dst] = box32(fsgnj<uint32_t>(unbox32(h->f_regs[r1]), unbox32(h->f_regs[r2]), funct3, 0x80000000u));
            }
            return;
        }
        case 0x14: case 0x15: { // FMIN/FMAX .S/.D (funct3 0=min, 1=max)
            bool want_max = (funct3 == 1);
            if (is_double) {
                uint64_t a = h->f_regs[r1], b = h->f_regs[r2];
                if (f64_is_snan(a) || f64_is_snan(b)) {
                    set_flags(h, FFLAG_NV);
                }
                bool an = f64_is_nan(a), bn = f64_is_nan(b);
                uint64_t res;
                if (an && bn) {
                    res = F64_CANON_NAN;
                } else if (an) {
                    res = b;
                } else if (bn) {
                    res = a;
                } else {
                    // signed compare with -0 < +0
                    auto lt = [](uint64_t x, uint64_t y) {
                        bool xs = x >> 63, ys = y >> 63;
                        if (xs != ys) {
                            return xs;
                        }
                        return xs ? (x > y) : (x < y);
                    };
                    bool a_lt_b = (a != b) && lt(a, b);
                    res = (want_max ? !a_lt_b : a_lt_b) ? a : b;
                }
                h->f_regs[r_dst] = res;
            } else {
                uint32_t a = unbox32(h->f_regs[r1]), b = unbox32(h->f_regs[r2]);
                if (f32_is_snan(a) || f32_is_snan(b)) {
                    set_flags(h, FFLAG_NV);
                }
                bool an = f32_is_nan(a), bn = f32_is_nan(b);
                uint32_t res;
                if (an && bn) {
                    res = F32_CANON_NAN;
                } else if (an) {
                    res = b;
                } else if (bn) {
                    res = a;
                } else {
                    auto lt = [](uint32_t x, uint32_t y) {
                        bool xs = x >> 31, ys = y >> 31;
                        if (xs != ys) {
                            return xs;
                        }
                        return xs ? (x > y) : (x < y);
                    };
                    bool a_lt_b = (a != b) && lt(a, b);
                    res = (want_max ? !a_lt_b : a_lt_b) ? a : b;
                }
                h->f_regs[r_dst] = box32(res);
            }
            return;
        }
        case 0x50: case 0x51: { // FCMP .S/.D : funct3 0=FLE, 1=FLT, 2=FEQ
            uint32_t result;
            if (is_double) {
                uint64_t a = h->f_regs[r1], b = h->f_regs[r2];
                bool an = f64_is_nan(a), bn = f64_is_nan(b);
                bool sig_invalid = (funct3 == 2) ? (f64_is_snan(a) || f64_is_snan(b)) : (an || bn);
                if (sig_invalid) {
                    set_flags(h, FFLAG_NV);
                }
                if (an || bn) {
                    result = 0;
                } else {
                    auto lt = [](uint64_t x, uint64_t y) {
                        bool xs = x >> 63, ys = y >> 63;
                        if (xs != ys) {
                            return xs;
                        }
                        return xs ? (x > y) : (x < y);
                    };
                    bool eq = (a == b) || (((a << 1) == 0) && ((b << 1) == 0)); // +0 == -0
                    bool less = !eq && lt(a, b);
                    result = (funct3 == 0) ? (less || eq) : (funct3 == 1) ? less : eq;
                }
            } else {
                uint32_t a = unbox32(h->f_regs[r1]), b = unbox32(h->f_regs[r2]);
                bool an = f32_is_nan(a), bn = f32_is_nan(b);
                bool sig_invalid = (funct3 == 2) ? (f32_is_snan(a) || f32_is_snan(b)) : (an || bn);
                if (sig_invalid) {
                    set_flags(h, FFLAG_NV);
                }
                if (an || bn) {
                    result = 0;
                } else {
                    auto lt = [](uint32_t x, uint32_t y) {
                        bool xs = x >> 31, ys = y >> 31;
                        if (xs != ys) {
                            return xs;
                        }
                        return xs ? (x > y) : (x < y);
                    };
                    bool eq = (a == b) || (((a << 1) == 0) && ((b << 1) == 0));
                    bool less = !eq && lt(a, b);
                    result = (funct3 == 0) ? (less || eq) : (funct3 == 1) ? less : eq;
                }
            }
            if (r_dst) {
                h->x_regs[r_dst] = result;
            }
            return;
        }
        case 0x70: case 0x71: { // funct3 0 = FMV.X.W/D, funct3 1 = FCLASS
            if (funct3 == 0) { // FMV.X.W / FMV.X.D
                if (r_dst) {
                    if (is_double) {
                        h->x_regs[r_dst] = h->f_regs[r1];
                    } else { // sign-extend
                        h->x_regs[r_dst] = int64_t(int32_t(uint32_t(h->f_regs[r1])));
                    }
                }
            } else { // FCLASS
                uint32_t cls;
                if (is_double) {
                    uint64_t a = h->f_regs[r1];
                    bool sign = a >> 63;
                    uint64_t exp = (a >> 52) & 0x7FF, man = a & 0xFFFFFFFFFFFFFull;
                    if ((exp == 0x7FF) && man) {
                        cls = (a & 0x8000000000000ull) ? (1u << 9) : (1u << 8);
                    } else if (exp == 0x7FF) {
                        cls = sign ? (1u << 0) : (1u << 7);
                    } else if ((exp == 0) && (man == 0)) {
                        cls = sign ? (1u << 3) : (1u << 4);
                    } else if (exp == 0) {
                        cls = sign ? (1u << 2) : (1u << 5);
                    } else {
                        cls = sign ? (1u << 1) : (1u << 6);
                    }
                } else {
                    uint32_t a = unbox32(h->f_regs[r1]);
                    bool sign = a >> 31;
                    uint32_t exp = (a >> 23) & 0xFF, man = a & 0x7FFFFF;
                    if ((exp == 0xFF) && man) {
                        cls = (a & 0x400000u) ? (1u << 9) : (1u << 8);
                    } else if (exp == 0xFF) {
                        cls = sign ? (1u << 0) : (1u << 7);
                    } else if ((exp == 0) && (man == 0)) {
                        cls = sign ? (1u << 3) : (1u << 4);
                    } else if (exp == 0) {
                        cls = sign ? (1u << 2) : (1u << 5);
                    } else {
                        cls = sign ? (1u << 1) : (1u << 6);
                    }
                }
                if (r_dst) {
                    h->x_regs[r_dst] = cls;
                }
            }
            return;
        }
        case 0x78: // FMV.W.X
            h->f_regs[r_dst] = box32(uint32_t(h->x_regs[r1]));
            return;
        case 0x79: // FMV.D.X
            h->f_regs[r_dst] = h->x_regs[r1];
            return;
        default: break; // arithmetic + conversions handled below
    }

    // Arithmetic and conversions: funct3 is the rounding mode (7 = dynamic -> fcsr.frm). Its legality
    // was already validated above (before fp_mark_dirty), so rm is guaranteed in 0..4 here.
    uint32_t rm = (funct3 == 7) ? ((h->fcsr >> 5) & 7) : funct3;
    uint32_t fl = 0;
    uint32_t s32a = unbox32(h->f_regs[r1]), s32b = unbox32(h->f_regs[r2]);
    uint64_t d64a = h->f_regs[r1], d64b = h->f_regs[r2];
    switch (funct7) {
        case 0x00: h->f_regs[r_dst] = box32(hostfp_add_s(s32a, s32b, rm, &fl, false)); break; // FADD.S
        case 0x01: h->f_regs[r_dst] = hostfp_add_d(d64a, d64b, rm, &fl, false); break;        // FADD.D
        case 0x04: h->f_regs[r_dst] = box32(hostfp_add_s(s32a, s32b, rm, &fl, true)); break;  // FSUB.S
        case 0x05: h->f_regs[r_dst] = hostfp_add_d(d64a, d64b, rm, &fl, true); break;         // FSUB.D
        case 0x08: h->f_regs[r_dst] = box32(hostfp_mul_s(s32a, s32b, rm, &fl)); break;        // FMUL.S
        case 0x09: h->f_regs[r_dst] = hostfp_mul_d(d64a, d64b, rm, &fl); break;               // FMUL.D
        case 0x0C: h->f_regs[r_dst] = box32(hostfp_div_s(s32a, s32b, rm, &fl)); break;        // FDIV.S
        case 0x0D: h->f_regs[r_dst] = hostfp_div_d(d64a, d64b, rm, &fl); break;               // FDIV.D
        case 0x2C: h->f_regs[r_dst] = box32(hostfp_sqrt_s(s32a, rm, &fl)); break;             // FSQRT.S
        case 0x2D: h->f_regs[r_dst] = hostfp_sqrt_d(d64a, rm, &fl); break;                    // FSQRT.D
        case 0x20: h->f_regs[r_dst] = box32(hostfp_cvt_s_d(d64a, rm, &fl)); break;            // FCVT.S.D
        case 0x21: h->f_regs[r_dst] = hostfp_cvt_d_s(s32a, rm, &fl); break;                   // FCVT.D.S
        case 0x60: case 0x61: { // FCVT.{W,WU,L,LU}.{S,D}
            int bits = (r2 & 2) ? 64 : 32;
            bool sgn = !(r2 & 1);
            // Always run the conversion: rd=x0 suppresses only the integer writeback, NOT the fflags
            // side effects (NV/NX on invalid/overflow/inexact still update fcsr).
            uint64_t iv = is_double ? hostfp_to_int_d(d64a, rm, &fl, bits, sgn)
                                    : hostfp_to_int_s(s32a, rm, &fl, bits, sgn);
            if (r_dst) {
                h->x_regs[r_dst] = iv;
            }
            break;
        }
        case 0x68: h->f_regs[r_dst] = box32(hostfp_from_int_s(h->x_regs[r1], r2, rm, &fl)); break; // FCVT.S.int
        case 0x69: h->f_regs[r_dst] = hostfp_from_int_d(h->x_regs[r1], r2, rm, &fl); break;        // FCVT.D.int
        default:
            return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    h->fcsr |= (fl & 0x1F);
}

void rv64_fpu_fma(Rv64SysHartState *h, uint32_t inst, bool neg_product, bool neg_addend) {
    if (fp_off(h)) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    uint32_t fmt = bits<26,25>(inst);
    uint32_t funct3 = bits<14,12>(inst);
    uint32_t rm = (funct3 == 7) ? ((h->fcsr >> 5) & 7) : funct3;
    if (fmt > 1) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    if (rm > 4) {
        return rv64_sys_raise(h, EXC_ILLEGAL_INST, inst);
    }
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r1 = bits<19,15>(inst);
    uint32_t r2 = bits<24,20>(inst);
    uint32_t r3 = bits<31,27>(inst);
    fp_mark_dirty(h); // FMA always writes an f-register + fcsr
    uint32_t fl = 0;
    if (fmt == 0) { // single
        uint32_t a = unbox32(h->f_regs[r1]) ^ (neg_product ? 0x80000000u : 0);
        uint32_t b = unbox32(h->f_regs[r2]);
        uint32_t c = unbox32(h->f_regs[r3]) ^ (neg_addend ? 0x80000000u : 0);
        h->f_regs[r_dst] = box32(hostfp_fma_s(a, b, c, rm, &fl));
    } else { // double (fmt == 1)
        uint64_t a = h->f_regs[r1] ^ (neg_product ? 0x8000000000000000ull : 0);
        uint64_t b = h->f_regs[r2];
        uint64_t c = h->f_regs[r3] ^ (neg_addend ? 0x8000000000000000ull : 0);
        h->f_regs[r_dst] = hostfp_fma_d(a, b, c, rm, &fl);
    }
    h->fcsr |= (fl & 0x1F);
}
