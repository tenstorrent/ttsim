// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Full-system RV64 F/D floating point with RISC-V semantics (NaN-boxing, canonical NaN, fflags, FS).
#include "rv64_fpu.h"

#define FFLAG_NX (1u << 0) // inexact
#define FFLAG_UF (1u << 1) // underflow
#define FFLAG_OF (1u << 2) // overflow
#define FFLAG_DZ (1u << 3) // divide by zero
#define FFLAG_NV (1u << 4) // invalid

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
        case 4: return 0u << 13; // RMM: RNE, then rmm() rounds ties away
        default: TTSIM_UNREACHABLE("fp-rm=%u", rm);
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
    f |= (e & (1u << 0)) ? FFLAG_NV : 0; // IE
    f |= (e & (1u << 2)) ? FFLAG_DZ : 0; // ZE
    f |= (e & (1u << 3)) ? FFLAG_OF : 0; // OE
    f |= (e & (1u << 4)) ? FFLAG_UF : 0; // UE
    f |= (e & (1u << 5)) ? FFLAG_NX : 0; // PE
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
        case 4: return 0ull << 22; // RMM: RNE, then rmm() rounds ties away
        default: TTSIM_UNREACHABLE("fp-rm=%u", rm);
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
    f |= (e & (1u << 0)) ? FFLAG_NV : 0; // IOC
    f |= (e & (1u << 1)) ? FFLAG_DZ : 0; // DZC
    f |= (e & (1u << 2)) ? FFLAG_OF : 0; // OFC
    f |= (e & (1u << 3)) ? FFLAG_UF : 0; // UFC
    f |= (e & (1u << 4)) ? FFLAG_NX : 0; // IXC
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

static fenv_t s_saved_fenv;

static inline int hostfp_round_mode(uint32_t rm) {
    switch (rm) {
        case 0: return FE_TONEAREST;
        case 1: return FE_TOWARDZERO;
        case 2: return FE_DOWNWARD;
        case 3: return FE_UPWARD;
        case 4: return FE_TONEAREST; // RMM: RNE, then rmm() rounds ties away
        default: TTSIM_UNREACHABLE("fp-rm=%u", rm);
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
    f |= (e & FE_INVALID) ? FFLAG_NV : 0;
    f |= (e & FE_DIVBYZERO) ? FFLAG_DZ : 0;
    f |= (e & FE_OVERFLOW) ? FFLAG_OF : 0;
    f |= (e & FE_UNDERFLOW) ? FFLAG_UF : 0;
    f |= (e & FE_INEXACT) ? FFLAG_NX : 0;
    return f;
}

static inline float fp_fma_s(float a, float b, float c) {
    return __builtin_fmaf(a, b, c);
}
static inline double fp_fma_d(double a, double b, double c) {
    return __builtin_fma(a, b, c);
}
static inline float fp_sqrt_s(float a) {
    return __builtin_sqrtf(a);
}
static inline double fp_sqrt_d(double a) {
    return __builtin_sqrt(a);
}
#endif

// fp_begin() sets the host rounding mode and fp_end_flags() reads the host flags, but nothing else ties a host FP
// operation between them: compilers move register-only arithmetic across the calls (-frounding-math doesn't stop
// them) and compute both arms of a select. So each operation takes pinned operands and its result is pinned (an empty
// asm the compiler can't move or speculate), and exactly one runs per window: a - b is computed as a + -b, which IEEE
// 754 defines it to be, and a conversion picked by a select pins its input inside its arm.
template<class T> static inline T fp_pin(T v) {
    if constexpr (std::is_integral_v<T>) {
        asm volatile("" : "+r"(v));
    } else {
#if defined(__x86_64__)
        asm volatile("" : "+x"(v));
#elif defined(__aarch64__)
        asm volatile("" : "+w"(v));
#elif defined(__riscv) && defined(__riscv_d)
        asm volatile("" : "+f"(v));
#else
        asm volatile("" : "+m"(v));
#endif
    }
    return v;
}

// RISC-V rounding modes (frm / inst rm field).
enum {
    RV_RNE = 0, // round to nearest, ties to even
    RV_RTZ = 1, // toward zero
    RV_RDN = 2, // down (toward -inf)
    RV_RUP = 3, // up (toward +inf)
    RV_RMM = 4, // to nearest, ties to max magnitude
};

template<class U, int TOT, int MB>
struct SoftFloat {
    static constexpr int EB = TOT - 1 - MB;
    static constexpr U EMAXFIELD = (U(1) << EB) - 1;
    static constexpr U MMASK = (U(1) << MB) - 1;
    static constexpr U SIGN = U(1) << (TOT - 1);
    static constexpr U QNAN = (EMAXFIELD << MB) | (U(1) << (MB - 1));
    static constexpr U INF = EMAXFIELD << MB;

    static bool is_nan(U a) { return ((a & (EMAXFIELD << MB)) == (EMAXFIELD << MB)) && (a & MMASK); }
    static bool is_snan(U a) { return is_nan(a) && !(a & (U(1) << (MB - 1))); }
    static bool is_inf(U a) { return ((a & (EMAXFIELD << MB)) == (EMAXFIELD << MB)) && !(a & MMASK); }
    static bool is_zero(U a) { return !(a & ~SIGN); }
};

using SF32 = SoftFloat<uint32_t, 32, 23>;
using SF64 = SoftFloat<uint64_t, 64, 52>;

static const uint32_t F32_CANON_NAN = 0x7FC00000u;

static uint64_t box32(uint32_t v) {
    return 0xFFFFFFFF00000000ull | v;
}

static uint32_t unbox32(uint64_t r) {
    return ((r >> 32) == 0xFFFFFFFFu) ? uint32_t(r) : F32_CANON_NAN;
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

// |v| = m * 2^e, m odd or 0
struct Dyadic {
    bool neg;
    unsigned __int128 m;
    int e;
};

// f, or with half = 1 the midpoint between f and the next value away from zero
template<class F> static Dyadic dyadic(F f, uint32_t half = 0) {
    using U = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
    constexpr int MB = (sizeof(F) == 4) ? 23 : 52, BIAS = (sizeof(F) == 4) ? 127 : 1023;
    U b = std::bit_cast<U>(f), ef = (b << 1) >> (MB + 1);
    uint64_t m = 2 * ((b & ((U(1) << MB) - 1)) | (U(ef != 0) << MB)) + half;
    int z = m ? std::countr_zero(m) : 0;
    return {bool(b >> (8 * sizeof(F) - 1)), m >> z, (ef ? int(ef) : 1) - BIAS - MB - 1 + z};
}

static Dyadic dmul(Dyadic a, Dyadic b) {
    return {a.neg != b.neg, a.m * b.m, a.e + b.e};
}

// The host rounded x = x0 + x1, or x0 / y, to nearest even as r; for RMM, round a tie (x0 + x1 == y * the midpoint)
// away from zero instead, with the same flags. Terms that sum to 0 lie within 108 bits: two share the lowest set bit,
// and the highest is at most one above theirs.
template<class F> static F rmm(F r, uint32_t fl, Dyadic x0, Dyadic x1 = {}, Dyadic y = {false, 1, 0}) {
    using U = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
    U b = std::bit_cast<U>(r);
    if (!(fl & FFLAG_NX) || ((b << 1) >= (~U(0) << ((sizeof(F) == 4) ? 24 : 53)))) { // exact, Inf or NaN
        return r;
    }
    Dyadic m = dmul(dyadic(r, 1), y), t[3] = {x0, x1, {!m.neg, m.m, m.e}};
    int lo = 1 << 30, hi = -lo;
    for (const Dyadic &d : t) {
        uint64_t top = uint64_t(d.m >> 64);
        int h = d.e + (top ? 128 - std::countl_zero(top) : 64 - std::countl_zero(uint64_t(d.m)));
        lo = (d.m && d.e < lo) ? d.e : lo;
        hi = (d.m && h > hi) ? h : hi;
    }
    if (hi - lo > 108) {
        return r;
    }
    __int128 sum = 0;
    for (const Dyadic &d : t) {
        sum += d.m ? (d.neg ? -1 : 1) * __int128(d.m << (d.e - lo)) : 0;
    }
    return sum ? r : std::bit_cast<F>(U(b + 1));
}

static uint32_t hostfp_add_s(uint32_t a, uint32_t b, uint32_t rm, uint32_t *fl, bool sub) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    float nb = sub ? -fb : fb;
    fp_begin(rm);
    float r = fp_pin(fp_pin(fa) + fp_pin(nb));
    *fl |= fp_end_flags();
    r = (rm == RV_RMM) ? rmm(r, *fl, dyadic(fa), dyadic(nb)) : r;
    return hostfp_result_s(r);
}

static uint64_t hostfp_add_d(uint64_t a, uint64_t b, uint32_t rm, uint32_t *fl, bool sub) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b);
    double nb = sub ? -fb : fb;
    fp_begin(rm);
    double r = fp_pin(fp_pin(fa) + fp_pin(nb));
    *fl |= fp_end_flags();
    r = (rm == RV_RMM) ? rmm(r, *fl, dyadic(fa), dyadic(nb)) : r;
    return hostfp_result_d(r);
}

static uint32_t hostfp_mul_s(uint32_t a, uint32_t b, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    fp_begin(rm);
    float r = fp_pin(fp_pin(fa) * fp_pin(fb));
    *fl |= fp_end_flags();
    r = (rm == RV_RMM) ? rmm(r, *fl, dmul(dyadic(fa), dyadic(fb))) : r;
    return hostfp_result_s(r);
}

static uint64_t hostfp_mul_d(uint64_t a, uint64_t b, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b);
    fp_begin(rm);
    double r = fp_pin(fp_pin(fa) * fp_pin(fb));
    *fl |= fp_end_flags();
    r = (rm == RV_RMM) ? rmm(r, *fl, dmul(dyadic(fa), dyadic(fb))) : r;
    return hostfp_result_d(r);
}

static uint32_t hostfp_div_s(uint32_t a, uint32_t b, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    fp_begin(rm);
    float r = fp_pin(fp_pin(fa) / fp_pin(fb));
    *fl |= fp_end_flags();
    r = (rm == RV_RMM) ? rmm(r, *fl, dyadic(fa), {}, dyadic(fb)) : r;
    return hostfp_result_s(r);
}

static uint64_t hostfp_div_d(uint64_t a, uint64_t b, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b);
    fp_begin(rm);
    double r = fp_pin(fp_pin(fa) / fp_pin(fb));
    *fl |= fp_end_flags();
    r = (rm == RV_RMM) ? rmm(r, *fl, dyadic(fa), {}, dyadic(fb)) : r;
    return hostfp_result_d(r);
}

static uint32_t hostfp_sqrt_s(uint32_t a, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a);
    fp_begin(rm);
    float r = fp_pin(fp_sqrt_s(fp_pin(fa)));
    *fl |= fp_end_flags();
    return hostfp_result_s(r);
}

static uint64_t hostfp_sqrt_d(uint64_t a, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a);
    fp_begin(rm);
    double r = fp_pin(fp_sqrt_d(fp_pin(fa)));
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

static uint32_t hostfp_fma_s(uint32_t a, uint32_t b, uint32_t c, uint32_t rm, uint32_t *fl) {
    float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b), fc = std::bit_cast<float>(c);
    fp_begin(rm);
    float r = fp_pin(fp_fma_s(fp_pin(fa), fp_pin(fb), fp_pin(fc)));
    *fl |= fp_end_flags();
    // Inf * 0 is invalid even with a quiet NaN addend, which x86 doesn't flag
    *fl |= ((SF32::is_inf(a) && SF32::is_zero(b)) || (SF32::is_zero(a) && SF32::is_inf(b))) ? FFLAG_NV : 0;
    r = (rm == RV_RMM) ? rmm(r, *fl, dmul(dyadic(fa), dyadic(fb)), dyadic(fc)) : r;
    return hostfp_result_s(r);
}

static uint64_t hostfp_fma_d(uint64_t a, uint64_t b, uint64_t c, uint32_t rm, uint32_t *fl) {
    double fa = std::bit_cast<double>(a), fb = std::bit_cast<double>(b), fc = std::bit_cast<double>(c);
    fp_begin(rm);
    double r = fp_pin(fp_fma_d(fp_pin(fa), fp_pin(fb), fp_pin(fc)));
    *fl |= fp_end_flags();
    // Inf * 0 is invalid even with a quiet NaN addend, which x86 doesn't flag
    *fl |= ((SF64::is_inf(a) && SF64::is_zero(b)) || (SF64::is_zero(a) && SF64::is_inf(b))) ? FFLAG_NV : 0;
    r = (rm == RV_RMM) ? rmm(r, *fl, dmul(dyadic(fa), dyadic(fb)), dyadic(fc)) : r;
    return hostfp_result_d(r);
}

static uint32_t hostfp_cvt_s_d(uint64_t d, uint32_t rm, uint32_t *fl) { // FCVT.S.D : double -> single
    double x = std::bit_cast<double>(d);
    fp_begin(rm);
    float r = fp_pin(float(fp_pin(x)));
    *fl |= fp_end_flags();
    r = (rm == RV_RMM) ? rmm(r, *fl, dyadic(x)) : r;
    return hostfp_result_s(r);
}

static uint64_t hostfp_cvt_d_s(uint32_t f, uint32_t rm, uint32_t *fl) { // FCVT.D.S : single -> double
    float x = std::bit_cast<float>(f);
    fp_begin(rm);
    double r = fp_pin(double(fp_pin(x)));
    *fl |= fp_end_flags();
    return hostfp_result_d(r);
}

// FCVT.{W,WU,L,LU}.{S,D}: a result in range raises NX when inexact, and one out of range (or a NaN) is clipped and
// raises NV alone. Below 2^52 the value splits exactly into its truncation and a fraction that picks the rounding; from
// 2^52 up it is an integer already. Each host operation is exact or truncates, so none depends on the rounding mode.
template<class F> static uint64_t hostfp_to_int(F x, uint32_t rm, uint32_t *fl, int bits, bool is_signed) {
    double r = x;
    double frac = 0;
    if ((r > -0x1p52) && (r < 0x1p52)) {
        frac = r - double(int64_t(r));
        r -= frac;
        double mag = (frac < 0) ? -frac : frac;
        bool away = (rm == RV_RNE) ? ((mag > 0.5) || ((mag == 0.5) && (int64_t(r) & 1)))
                  : (rm == RV_RDN) ? (frac < 0) : (rm == RV_RUP) ? (frac > 0) : ((rm == RV_RMM) && (mag >= 0.5));
        if (away) {
            r += (frac < 0) ? -1.0 : 1.0;
        }
    }
    double limit = is_signed ? ((bits == 32) ? 0x1p31 : 0x1p63) : ((bits == 32) ? 0x1p32 : 0x1p64);
    if (!((r < limit) && (is_signed ? (r >= -limit) : (r > -1.0)))) { // a NaN fails both
        *fl |= FFLAG_NV;
        uint64_t sat = is_signed ? ((bits == 32) ? 0x7FFFFFFFull : 0x7FFFFFFFFFFFFFFFull) : ~0ull;
        return (r < 0) ? ~sat : sat; // W and WU results are sign-extended
    }
    *fl |= (frac != 0) ? FFLAG_NX : 0;
    uint64_t v = is_signed ? uint64_t(int64_t(r)) : uint64_t(r);
    return (bits == 32) ? uint64_t(int64_t(int32_t(uint32_t(v)))) : v;
}

// The host can't round to nearest with ties away (RMM), so round |x| to P bits here; the conversion is then exact
template<class F, int P> static F from_int_rmm(uint64_t xr, uint32_t r2, uint32_t *fl) {
    int64_t sx = (r2 == 0) ? int32_t(xr) : (r2 == 2) ? int64_t(xr) : 0;
    uint64_t m = (r2 == 1) ? uint32_t(xr) : (r2 == 3) ? xr : (sx < 0) ? -uint64_t(sx) : uint64_t(sx);
    int drop = 64 - std::countl_zero(m) - P;
    F r = F(m);
    if (drop > 0) {
        uint64_t ulp = 1ull << drop, sum = m + ulp / 2;
        r = (sum < m) ? F(18446744073709551616.0) : F(sum & -ulp); // a sum that wraps is 2^64
        *fl |= (m & (ulp - 1)) ? FFLAG_NX : 0;
    }
    return (sx < 0) ? -r : r;
}

static uint32_t hostfp_from_int_s(uint64_t xr, uint32_t r2, uint32_t rm, uint32_t *fl) {
    if (rm == 4) {
        return std::bit_cast<uint32_t>(from_int_rmm<float, 24>(xr, r2, fl));
    }
    fp_begin(rm);
    float r = fp_pin((r2 == 0) ? float(int32_t(fp_pin(xr))) : (r2 == 1) ? float(uint32_t(fp_pin(xr)))
            : (r2 == 2) ? float(int64_t(fp_pin(xr))) : float(fp_pin(xr)));
    *fl |= fp_end_flags();
    return std::bit_cast<uint32_t>(r);
}

static uint64_t hostfp_from_int_d(uint64_t xr, uint32_t r2, uint32_t rm, uint32_t *fl) {
    if (rm == 4) {
        return std::bit_cast<uint64_t>(from_int_rmm<double, 53>(xr, r2, fl));
    }
    fp_begin(rm);
    double r = fp_pin((r2 == 0) ? double(int32_t(fp_pin(xr))) : (r2 == 1) ? double(uint32_t(fp_pin(xr)))
             : (r2 == 2) ? double(int64_t(fp_pin(xr))) : double(fp_pin(xr)));
    *fl |= fp_end_flags();
    return std::bit_cast<uint64_t>(r);
}

template<class F> using FloatBits = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
template<class F> using FloatFormat = std::conditional_t<sizeof(F) == 4, SF32, SF64>;

// FMIN/FMAX: a NaN operand gives the other (two NaNs give the canonical NaN), and -0 is below +0
template<class F> static FloatBits<F> fmin_max(FloatBits<F> a, FloatBits<F> b, bool is_max, uint32_t *fl) {
    using SF = FloatFormat<F>;
    *fl |= (SF::is_snan(a) || SF::is_snan(b)) ? FFLAG_NV : 0;
    if (SF::is_nan(a)) {
        return SF::is_nan(b) ? SF::QNAN : b;
    }
    if (SF::is_nan(b)) {
        return a;
    }
    F fa = std::bit_cast<F>(a), fb = std::bit_cast<F>(b);
    if (fa == fb) {
        return is_max ? (a & b) : (a | b); // only +0 and -0 differ here: OR the signs for min, AND them for max
    }
    return ((fa < fb) != is_max) ? a : b;
}

// FLE (funct3 0), FLT (1), FEQ (2): false with a NaN operand, which is invalid for FLE and FLT, and for FEQ if
// signaling
template<class F> static uint32_t fcmp(FloatBits<F> a, FloatBits<F> b, uint32_t funct3, uint32_t *fl) {
    using SF = FloatFormat<F>;
    bool invalid = (funct3 == 2) ? (SF::is_snan(a) || SF::is_snan(b)) : (SF::is_nan(a) || SF::is_nan(b));
    *fl |= invalid ? FFLAG_NV : 0;
    F fa = std::bit_cast<F>(a), fb = std::bit_cast<F>(b);
    return (funct3 == 0) ? (fa <= fb) : (funct3 == 1) ? (fa < fb) : (fa == fb);
}

// FCLASS: one bit, from 0 (-Inf) through 7 (+Inf) to 8 (signaling NaN) and 9 (quiet NaN)
template<class F> static uint32_t fclass(FloatBits<F> a) {
    using SF = FloatFormat<F>;
    bool neg = a & SF::SIGN;
    if (SF::is_nan(a)) {
        return SF::is_snan(a) ? (1u << 8) : (1u << 9);
    } else if (SF::is_inf(a)) {
        return neg ? (1u << 0) : (1u << 7);
    } else if (SF::is_zero(a)) {
        return neg ? (1u << 3) : (1u << 4);
    } else if (!(a & SF::INF)) { // subnormal
        return neg ? (1u << 2) : (1u << 5);
    } else {
        return neg ? (1u << 1) : (1u << 6);
    }
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

    uint32_t fl = 0;
    switch (funct7) {
        case 0x10: case 0x11: { // FSGNJ[N/X].S / .D
            if (is_double) {
                h->f_regs[r_dst] = fsgnj<uint64_t>(h->f_regs[r1], h->f_regs[r2], funct3, 0x8000000000000000ull);
            } else {
                h->f_regs[r_dst] = box32(fsgnj<uint32_t>(unbox32(h->f_regs[r1]), unbox32(h->f_regs[r2]), funct3, 0x80000000u));
            }
            return;
        }
        case 0x14: case 0x15: // FMIN/FMAX .S/.D (funct3 0=min, 1=max)
            if (is_double) {
                h->f_regs[r_dst] = fmin_max<double>(h->f_regs[r1], h->f_regs[r2], funct3 == 1, &fl);
            } else {
                h->f_regs[r_dst] = box32(fmin_max<float>(unbox32(h->f_regs[r1]), unbox32(h->f_regs[r2]), funct3 == 1, &fl));
            }
            h->fcsr |= fl;
            return;
        case 0x50: case 0x51: { // FCMP .S/.D : funct3 0=FLE, 1=FLT, 2=FEQ
            uint32_t result = is_double ? fcmp<double>(h->f_regs[r1], h->f_regs[r2], funct3, &fl)
                                        : fcmp<float>(unbox32(h->f_regs[r1]), unbox32(h->f_regs[r2]), funct3, &fl);
            h->fcsr |= fl;
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
                uint32_t cls = is_double ? fclass<double>(h->f_regs[r1]) : fclass<float>(unbox32(h->f_regs[r1]));
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
            uint64_t iv = is_double ? hostfp_to_int(std::bit_cast<double>(d64a), rm, &fl, bits, sgn)
                                    : hostfp_to_int(std::bit_cast<float>(s32a), rm, &fl, bits, sgn);
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
