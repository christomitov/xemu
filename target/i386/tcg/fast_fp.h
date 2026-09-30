/*
 * x86 FPU/SSE: exact fast paths using host float/double arithmetic.
 *
 * Every fast path in this file either produces exactly the result and the
 * exception flags that the corresponding softfloat routine would produce for
 * the same inputs and float_status, or declines (returns false / falls
 * through) so that the caller runs softfloat.  The gates are chosen so that
 * the host's IEEE-754 binary32/binary64 arithmetic in round-to-nearest-even
 * provably matches; see the per-function comments.  A native differential
 * test harness checks this against softfloat.
 *
 * Runtime switch: enabled by default only in Emscripten (browser) builds,
 * where softfloat is comparatively very expensive.  XEMU_FAST_FP=0 disables
 * it there; XEMU_FAST_FP=1 enables it elsewhere.  Defining FFP_DISABLE
 * before including this header compiles every fast path out.
 *
 * Requirements on the host: IEEE binary32/binary64 with round-to-nearest-even
 * as the (never changed) host rounding mode, and FLT_EVAL_METHOD == 0 (no
 * excess precision).  Both hold for WebAssembly and x86-64 SSE.
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 */

#ifndef I386_TCG_FAST_FP_H
#define I386_TCG_FAST_FP_H

#include <float.h>
#include <math.h>
#include "qemu/host-utils.h"
#include "fpu/softfloat.h"

#if defined(FFP_DISABLE) || !defined(FLT_EVAL_METHOD) || \
    FLT_EVAL_METHOD != 0 || defined(__FAST_MATH__)
#define FFP_SUPPORTED 0
#else
#define FFP_SUPPORTED 1
#endif

/* -1: not yet initialised from the environment; 0: off; 1: on. */
extern int xemu_fast_fp_state;
int xemu_fast_fp_init(void);

static inline bool ffp_enabled(void)
{
    if (!FFP_SUPPORTED) {
        return false;
    } else {
        int v = xemu_fast_fp_state;
        if (unlikely(v < 0)) {
            v = xemu_fast_fp_init();
        }
        return v;
    }
}

/* Bit casts */
static inline uint64_t ffp_d2u(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static inline double ffp_u2d(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static inline uint32_t ffp_f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static inline float ffp_u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline void ffp_raise_inexact(bool inexact, float_status *s)
{
    if (inexact) {
        s->float_exception_flags |= float_flag_inexact;
    }
}

/*
 * ------------------------------------------------------------------------
 * Exactness tests for binary64 operations.
 *
 * Preconditions: operands are +-0 or normal doubles, and the rounded
 * result d is +-0 or has |d| >= 2^-1021 (biased exponent >= 2) and is
 * finite.  Then the exact result is not in the subnormal range, so "the
 * exact result is representable in binary64" is equivalent to "d equals
 * the exact result", and depends only on the significands:
 * ------------------------------------------------------------------------
 */

/* Odd part of the 53-bit integer significand of a normal double. */
static inline uint64_t ffp_odd_sig(double d)
{
    uint64_t m = (ffp_d2u(d) & ((1ULL << 52) - 1)) | (1ULL << 52);
    return m >> ctz64(m);
}

static inline int ffp_bitlen(uint64_t v)
{
    return 64 - clz64(v);
}

/*
 * a*b is exact iff odd(a)*odd(b) (an odd integer) fits in 53 bits.  With
 * la, lb the bit lengths, the product has la+lb-1 or la+lb bits.
 */
static inline bool ffp_mul_exact(double a, double b)
{
    uint64_t oa, ob;
    int l;

    if (a == 0 || b == 0) {
        return true;
    }
    oa = ffp_odd_sig(a);
    ob = ffp_odd_sig(b);
    l = ffp_bitlen(oa) + ffp_bitlen(ob);
    if (l <= 53) {
        return true;
    }
    if (l > 54) {
        return false;
    }
    return ((oa * ob) >> 53) == 0;      /* oa*ob < 2^54: no wraparound */
}

/*
 * a/b = (odd(a)/odd(b)) * 2^k is a dyadic rational (hence possibly
 * representable) iff odd(b) divides odd(a); the quotient is then an odd
 * integer <= odd(a) < 2^53, so it is representable.
 */
static inline bool ffp_div_exact(double a, double b)
{
    uint64_t oa, ob;

    if (a == 0) {
        return true;
    }
    oa = ffp_odd_sig(a);
    ob = ffp_odd_sig(b);
    return ob == 1 || (oa % ob) == 0;
}

/*
 * Knuth's TwoSum: for finite s = RN(a+b), err is exactly (a+b) - s
 * (no overflow is possible in the intermediate steps when s is finite).
 */
static inline bool ffp_add_exact(double a, double b, double s)
{
    double bb = s - a;
    double err = (a - (s - bb)) + (b - bb);
    return err == 0;
}

/*
 * r = RN(sqrt(a)).  sqrt(a) is exact iff r*r == a exactly, i.e. iff
 * odd(r)^2 == odd(a) and the exponents agree.  odd(a) < 2^53, so odd(r)
 * must have at most 27 bits (then odd(r)^2 < 2^54 fits in 64 bits).  When
 * odd(r)^2 == odd(a) the double product r*r is exact, so comparing it with
 * a checks the exponent.
 */
static inline bool ffp_sqrt_exact(double a, double r)
{
    uint64_t orr;

    if (a == 0) {
        return true;
    }
    orr = ffp_odd_sig(r);
    if (ffp_bitlen(orr) > 27) {
        return false;
    }
    return orr * orr == ffp_odd_sig(a) && r * r == a;
}

/*
 * Round a double to a 24-bit significand, nearest-even, with unbounded
 * exponent (x87 precision control "single": the significand is rounded but
 * the 15-bit exponent range is kept).  The caller must check the result is
 * finite (a carry out of the largest binade gives +-inf).
 */
static inline double ffp_round24(double d)
{
    uint64_t u = ffp_d2u(d);

    u += 0x0fffffffULL + ((u >> 29) & 1);
    u &= ~0x1fffffffULL;
    return ffp_u2d(u);
}

/*
 * ------------------------------------------------------------------------
 * x87 (floatx80) fast paths
 * ------------------------------------------------------------------------
 */

#define FFP_X80_BIAS 16383

/*
 * floatx80 -> double, exactly.  Accepts only +-0 and normal floatx80
 * values (explicit integer bit set) whose value is a normal double, i.e.
 * the low 11 significand bits are zero and the exponent is in range.
 * *sig24 is set if the significand also fits in 24 bits.
 */
static inline bool ffp_x80_to_f64(floatx80 a, double *d, bool *sig24)
{
    uint64_t m = a.low;
    uint32_t e = a.high & 0x7fff;
    uint64_t sign = (uint64_t)(a.high >> 15) << 63;
    int32_t de;

    if (e == 0) {
        if (m != 0) {
            return false;               /* denormal / pseudo-denormal */
        }
        *d = ffp_u2d(sign);
        *sig24 = true;
        return true;
    }
    if (!(m >> 63) || (m & 0x7ff)) {
        return false;                   /* unnormal, or > 53 bits */
    }
    de = (int32_t)e - FFP_X80_BIAS + 1023;
    if (de < 1 || de > 2046) {
        return false;                   /* also rejects inf/NaN (e=0x7fff) */
    }
    *d = ffp_u2d(sign | (uint64_t)de << 52 | ((m << 1) >> 12));
    *sig24 = (m & ((1ULL << 40) - 1)) == 0;
    return true;
}

/* double (+-0 or normal) -> floatx80, exactly. */
static inline floatx80 ffp_f64_to_x80(double d)
{
    uint64_t u = ffp_d2u(d);
    uint16_t sign = (u >> 63) << 15;
    uint32_t de = (u >> 52) & 0x7ff;

    if (de == 0) {
        return make_floatx80(sign, 0);
    }
    return make_floatx80(sign | (de - 1023 + FFP_X80_BIAS),
                         (1ULL << 63) | ((u << 12) >> 1));
}

enum { FFP_ADD, FFP_SUB, FFP_MUL, FFP_DIV, FFP_SQRT };

/*
 * x87 add/sub/mul/div/sqrt.  softfloat computes the exact result and rounds
 * it once to the precision-control width (24/53/64 bits) with the floatx80
 * exponent range; the only flag it can raise for our accepted inputs is
 * inexact.
 *
 * We compute d = RN53(exact) with the host, require d to be 0 or normal
 * with biased exponent >= 2 (so neither d nor the exact result is in or
 * near the binary64 subnormal range; binary64's range is inside floatx80's
 * so softfloat cannot overflow/underflow either) and finite, and decide
 * whether d == exact with the exactness tests above.  Then:
 *  - PC=53: result d; inexact = !exact.
 *  - PC=64: if exact, result d with no flags; otherwise decline.
 *  - PC=24: result r = RN24(d).  If exact, RN24(d) = RN24(exact) trivially.
 *    If not, we require both inputs to have 24-bit significands; then
 *    RN24(RN53(x)) = RN24(x) for x = a op b, op in {+,-,*,/,sqrt}, because
 *    53 >= 2*24 + 2 (Figueroa, "When is double rounding innocuous?", 1995).
 *    inexact = !exact || r != d (if d != exact, r == exact would make
 *    exact 24-bit representable, hence d == exact: contradiction).
 */
static inline bool ffp_x87_finish(double d, bool exact, bool in24,
                                  float_status *s, floatx80 *res)
{
    uint32_t de = (ffp_d2u(d) >> 52) & 0x7ff;
    bool inexact;

    if (de == 0x7ff || (de < 2 && d != 0)) {
        return false;
    }
    switch (s->floatx80_rounding_precision) {
    case floatx80_precision_d:
        inexact = !exact;
        break;
    case floatx80_precision_s: {
        double r;
        if (!exact && !in24) {
            return false;
        }
        r = ffp_round24(d);
        if (((ffp_d2u(r) >> 52) & 0x7ff) == 0x7ff) {
            return false;
        }
        inexact = !exact || r != d;
        d = r;
        break;
    }
    case floatx80_precision_x:
        if (!exact) {
            return false;
        }
        inexact = false;
        break;
    default:
        return false;
    }
    ffp_raise_inexact(inexact, s);
    *res = ffp_f64_to_x80(d);
    return true;
}

static inline bool ffp_x87_arith(int op, floatx80 a, floatx80 b,
                                 float_status *s, floatx80 *res)
{
    double da, db, d;
    bool a24, b24, exact;

    if (!ffp_enabled() ||
        s->float_rounding_mode != float_round_nearest_even) {
        return false;
    }
    if (!ffp_x80_to_f64(a, &da, &a24)) {
        return false;
    }
    if (op == FFP_SQRT) {
        if (da < 0) {
            return false;               /* invalid; -0 is fine (da == 0) */
        }
        d = __builtin_sqrt(da);
        exact = ffp_sqrt_exact(da, d);
        return ffp_x87_finish(d, exact, a24, s, res);
    }
    if (!ffp_x80_to_f64(b, &db, &b24)) {
        return false;
    }
    switch (op) {
    case FFP_ADD:
        d = da + db;
        exact = ffp_add_exact(da, db, d);
        break;
    case FFP_SUB:
        d = da - db;
        exact = ffp_add_exact(da, -db, d);
        break;
    case FFP_MUL:
        d = da * db;
        if (d == 0 && da != 0 && db != 0) {
            return false;               /* underflow */
        }
        exact = ffp_mul_exact(da, db);
        break;
    case FFP_DIV:
        if (db == 0) {
            return false;               /* divide-by-zero / invalid */
        }
        d = da / db;
        if (d == 0 && da != 0) {
            return false;               /* underflow */
        }
        exact = ffp_div_exact(da, db);
        break;
    default:
        return false;
    }
    return ffp_x87_finish(d, exact, a24 && b24, s, res);
}

static inline floatx80 ffp_floatx80_add(floatx80 a, floatx80 b,
                                        float_status *s)
{
    floatx80 r;
    return ffp_x87_arith(FFP_ADD, a, b, s, &r) ? r : floatx80_add(a, b, s);
}

static inline floatx80 ffp_floatx80_sub(floatx80 a, floatx80 b,
                                        float_status *s)
{
    floatx80 r;
    return ffp_x87_arith(FFP_SUB, a, b, s, &r) ? r : floatx80_sub(a, b, s);
}

static inline floatx80 ffp_floatx80_mul(floatx80 a, floatx80 b,
                                        float_status *s)
{
    floatx80 r;
    return ffp_x87_arith(FFP_MUL, a, b, s, &r) ? r : floatx80_mul(a, b, s);
}

static inline floatx80 ffp_floatx80_div(floatx80 a, floatx80 b,
                                        float_status *s)
{
    floatx80 r;
    return ffp_x87_arith(FFP_DIV, a, b, s, &r) ? r : floatx80_div(a, b, s);
}

static inline floatx80 ffp_floatx80_sqrt(floatx80 a, float_status *s)
{
    floatx80 r;
    return ffp_x87_arith(FFP_SQRT, a, a, s, &r) ? r : floatx80_sqrt(a, s);
}

/*
 * floatx80 compare.  For +-0, normal (integer bit set) and infinite
 * operands softfloat raises no flags and orders them numerically; so do we,
 * using sign-magnitude ordering of (exponent, significand).
 */
static inline bool ffp_x80_cmp_ok(floatx80 a)
{
    uint32_t e = a.high & 0x7fff;

    if (e == 0) {
        return a.low == 0;
    }
    if (e == 0x7fff) {
        return a.low == (1ULL << 63);   /* infinity */
    }
    return a.low >> 63;
}

static inline FloatRelation ffp_x80_relation(floatx80 a, floatx80 b)
{
    bool sa = a.high >> 15, sb = b.high >> 15;
    uint32_t ea = a.high & 0x7fff, eb = b.high & 0x7fff;
    bool a_lt_b_mag, eq_mag;

    if ((ea | eb) == 0) {
        return float_relation_equal;    /* +-0 == +-0 */
    }
    if (sa != sb) {
        return sa ? float_relation_less : float_relation_greater;
    }
    eq_mag = ea == eb && a.low == b.low;
    if (eq_mag) {
        return float_relation_equal;
    }
    a_lt_b_mag = ea < eb || (ea == eb && a.low < b.low);
    return (a_lt_b_mag != sa) ? float_relation_less : float_relation_greater;
}

static inline FloatRelation ffp_floatx80_compare(floatx80 a, floatx80 b,
                                                 float_status *s)
{
    if (ffp_enabled() && ffp_x80_cmp_ok(a) && ffp_x80_cmp_ok(b)) {
        return ffp_x80_relation(a, b);
    }
    return floatx80_compare(a, b, s);
}

static inline FloatRelation ffp_floatx80_compare_quiet(floatx80 a,
                                                       floatx80 b,
                                                       float_status *s)
{
    if (ffp_enabled() && ffp_x80_cmp_ok(a) && ffp_x80_cmp_ok(b)) {
        return ffp_x80_relation(a, b);
    }
    return floatx80_compare_quiet(a, b, s);
}

/*
 * Loads: float32/float64 -> floatx80 is exact for +-0, normals and
 * infinities and raises no flag.  Denormals (DE flag) and NaNs (SNaN
 * quieting, IE) go to softfloat.
 */
static inline bool ffp_f32_to_x80(uint32_t v, floatx80 *r)
{
    uint32_t e = (v >> 23) & 0xff;
    uint32_t f = v & 0x7fffff;
    uint16_t sign = (v >> 31) << 15;

    if (e == 0) {
        if (f) {
            return false;
        }
        *r = make_floatx80(sign, 0);
    } else if (e == 0xff) {
        if (f) {
            return false;
        }
        *r = make_floatx80(sign | 0x7fff, 1ULL << 63);
    } else {
        *r = make_floatx80(sign | (e - 127 + FFP_X80_BIAS),
                           (1ULL << 63) | ((uint64_t)f << 40));
    }
    return true;
}

static inline bool ffp_f64_to_x80_load(uint64_t v, floatx80 *r)
{
    uint32_t e = (v >> 52) & 0x7ff;
    uint64_t f = v & ((1ULL << 52) - 1);
    uint16_t sign = (v >> 63) << 15;

    if (e == 0) {
        if (f) {
            return false;
        }
        *r = make_floatx80(sign, 0);
    } else if (e == 0x7ff) {
        if (f) {
            return false;
        }
        *r = make_floatx80(sign | 0x7fff, 1ULL << 63);
    } else {
        *r = make_floatx80(sign | (e - 1023 + FFP_X80_BIAS),
                           (1ULL << 63) | (f << 11));
    }
    return true;
}

static inline floatx80 ffp_float32_to_floatx80(float32 v, float_status *s)
{
    floatx80 r;
    if (ffp_enabled() && ffp_f32_to_x80(float32_val(v), &r)) {
        return r;
    }
    return float32_to_floatx80(v, s);
}

/*
 * Note that softfloat's conversions to floatx80 round to the current
 * precision control (QEMU applies PC to loads too), so a double with more
 * than 24 significant bits under PC=single is left to softfloat.
 */
static inline floatx80 ffp_float64_to_floatx80(float64 v, float_status *s)
{
    floatx80 r;
    if (ffp_enabled() &&
        (s->floatx80_rounding_precision != floatx80_precision_s ||
         (float64_val(v) & 0x1fffffff) == 0) &&
        ffp_f64_to_x80_load(float64_val(v), &r)) {
        return r;
    }
    return float64_to_floatx80(v, s);
}

/*
 * Stores: floatx80 -> float32/float64.  Accept +-0, infinity, and normal
 * values whose exponent is in the target's normal range (so no tininess
 * and, if the rounded significand does not carry out of the top binade, no
 * overflow).  If low significand bits are discarded we round to nearest
 * even (declining in other rounding modes) and raise inexact.
 */
static inline bool ffp_x80_store(floatx80 a, float_status *s, int fbits,
                                 int ebias, uint32_t emax, uint64_t *out)
{
    uint64_t m = a.low;
    uint32_t e = a.high & 0x7fff;
    uint64_t sign = (uint64_t)(a.high >> 15) << (fbits + (fbits == 23 ? 8 : 11));
    int shift = 63 - fbits;             /* discarded low bits */
    uint64_t low, sig, half;
    int32_t te;

    if (e == 0) {
        if (m) {
            return false;
        }
        *out = sign;
        return true;
    }
    if (e == 0x7fff) {
        if (m != (1ULL << 63)) {
            return false;
        }
        *out = sign | ((uint64_t)emax + 1) << fbits;
        return true;
    }
    if (!(m >> 63)) {
        return false;
    }
    te = (int32_t)e - FFP_X80_BIAS + ebias;
    if (te < 1 || te > (int32_t)emax) {
        return false;
    }
    sig = m >> shift;
    low = m & ((1ULL << shift) - 1);
    if (low) {
        if (s->float_rounding_mode != float_round_nearest_even) {
            return false;
        }
        half = 1ULL << (shift - 1);
        if (low > half || (low == half && (sig & 1))) {
            sig++;
            if (sig >> (fbits + 1)) {
                sig >>= 1;
                te++;
                if (te > (int32_t)emax) {
                    return false;       /* overflow */
                }
            }
        }
        s->float_exception_flags |= float_flag_inexact;
    }
    *out = sign | (uint64_t)te << fbits | (sig & ((1ULL << fbits) - 1));
    return true;
}

static inline float32 ffp_floatx80_to_float32(floatx80 a, float_status *s)
{
    uint64_t r;
    if (ffp_enabled() && ffp_x80_store(a, s, 23, 127, 254, &r)) {
        return make_float32((uint32_t)r);
    }
    return floatx80_to_float32(a, s);
}

static inline float64 ffp_floatx80_to_float64(floatx80 a, float_status *s)
{
    uint64_t r;
    if (ffp_enabled() && ffp_x80_store(a, s, 52, 1023, 2046, &r)) {
        return make_float64(r);
    }
    return floatx80_to_float64(a, s);
}

/*
 * floatx80 -> integer with rounding mode rm, for +-0 and normal values with
 * |a| < 2^62.  Sets *inexact if fraction bits were discarded.  The caller
 * range-checks the result for the destination width and declines (before
 * raising anything) if it does not fit, leaving overflow/invalid handling
 * to softfloat.
 */
static inline bool ffp_x80_to_int(floatx80 a, FloatRoundMode rm,
                                  int64_t *out, bool *inexact)
{
    uint64_t m = a.low;
    uint32_t e = a.high & 0x7fff;
    bool sign = a.high >> 15;
    int sh = (int)e - FFP_X80_BIAS;     /* value = m * 2^(sh - 63) */
    uint64_t ip, fr;
    bool up;

    if (e == 0) {
        if (m) {
            return false;
        }
        *out = 0;
        *inexact = false;
        return true;
    }
    if (!(m >> 63) || sh >= 62) {
        return false;                   /* unnormal, too large, inf, NaN */
    }
    if (sh >= 0) {
        ip = m >> (63 - sh);
        fr = m << (sh + 1);             /* fraction, scaled by 2^64 */
    } else if (sh == -1) {
        ip = 0;
        fr = m;                         /* value in [0.5, 1) */
    } else {
        ip = 0;
        fr = 1;                         /* nonzero, below one half */
    }
    switch (rm) {
    case float_round_nearest_even:
        up = fr > (1ULL << 63) || (fr == (1ULL << 63) && (ip & 1));
        break;
    case float_round_to_zero:
        up = false;
        break;
    case float_round_down:
        up = fr != 0 && sign;
        break;
    case float_round_up:
        up = fr != 0 && !sign;
        break;
    default:
        return false;
    }
    ip += up;
    *out = sign ? -(int64_t)ip : (int64_t)ip;
    *inexact = fr != 0;
    return true;
}

static inline int32_t ffp_floatx80_to_int32_rm(floatx80 a, FloatRoundMode rm,
                                               float_status *s, bool *ok)
{
    int64_t r = 0;
    bool inexact;

    *ok = ffp_enabled() && ffp_x80_to_int(a, rm, &r, &inexact) &&
          r == (int32_t)r;
    if (*ok) {
        ffp_raise_inexact(inexact, s);
    }
    return (int32_t)r;
}

static inline int32_t ffp_floatx80_to_int32(floatx80 a, float_status *s)
{
    bool ok;
    int32_t r = ffp_floatx80_to_int32_rm(a, s->float_rounding_mode, s, &ok);
    return ok ? r : floatx80_to_int32(a, s);
}

static inline int32_t ffp_floatx80_to_int32_round_to_zero(floatx80 a,
                                                          float_status *s)
{
    bool ok;
    int32_t r = ffp_floatx80_to_int32_rm(a, float_round_to_zero, s, &ok);
    return ok ? r : floatx80_to_int32_round_to_zero(a, s);
}

static inline int64_t ffp_floatx80_to_int64(floatx80 a, float_status *s)
{
    int64_t r;
    bool inexact;

    if (ffp_enabled() &&
        ffp_x80_to_int(a, s->float_rounding_mode, &r, &inexact)) {
        ffp_raise_inexact(inexact, s);
        return r;
    }
    return floatx80_to_int64(a, s);
}

static inline int64_t ffp_floatx80_to_int64_round_to_zero(floatx80 a,
                                                          float_status *s)
{
    int64_t r;
    bool inexact;

    if (ffp_enabled() &&
        ffp_x80_to_int(a, float_round_to_zero, &r, &inexact)) {
        ffp_raise_inexact(inexact, s);
        return r;
    }
    return floatx80_to_int64_round_to_zero(a, s);
}

/*
 * Integer -> floatx80 with a 64-bit significand is always exact and raises
 * no flags (the fild helpers force PC=extended).  softfloat returns +0
 * for 0.
 */
static inline floatx80 ffp_int64_to_x80(int64_t v)
{
    uint64_t a;
    int sh;
    uint16_t sign = v < 0 ? 0x8000 : 0;

    if (v == 0) {
        return make_floatx80(0, 0);
    }
    a = v < 0 ? -(uint64_t)v : (uint64_t)v;
    sh = clz64(a);
    return make_floatx80(sign | (FFP_X80_BIAS + 63 - sh), a << sh);
}

/*
 * int32 -> floatx80 under the current precision control (fildl into FT0,
 * which does not force PC=extended): exact, hence identical, whenever the
 * integer's significant bits fit the PC width.
 */
static inline floatx80 ffp_int32_to_floatx80(int32_t v, float_status *s)
{
    if (ffp_enabled()) {
        uint64_t a = v < 0 ? -(uint64_t)v : (uint64_t)v;
        if (s->floatx80_rounding_precision != floatx80_precision_s ||
            a == 0 || ffp_bitlen(a >> ctz64(a)) <= 24) {
            return ffp_int64_to_x80(v);
        }
    }
    return int32_to_floatx80(v, s);
}

/*
 * ------------------------------------------------------------------------
 * SSE float32 fast paths
 *
 * Accepted operands are +-0 and normal floats (no denormals: they would
 * raise DE or be flushed by DAZ; no NaNs: x86 NaN propagation; no
 * infinities for arithmetic: inf-inf, 0*inf, inf/inf make NaNs).  The
 * rounding mode must be nearest-even.  The host computes r = RN24(exact)
 * directly in binary32 (correctly rounded), and we decline if r is
 * infinite (overflow) or 0 < |r| <= FLT_MIN (possible underflow/FTZ), or
 * r == 0 from nonzero multiplicands/dividend (underflow).  Otherwise
 * softfloat produces the same r and only possibly the inexact flag, which
 * we compute exactly:
 *   add/sub: TwoSum error term (exact in binary32 absent overflow);
 *   mul:     (double)a*(double)b is exact (48 bits) - compare with r;
 *   div:     r*b in double is exact (48 bits) - compare with a;
 *   sqrt:    r*r in double is exact - compare with a.
 * ------------------------------------------------------------------------
 */

/* +-0 or normal */
static inline bool ffp_f32_zon(uint32_t v)
{
    uint32_t e = (v >> 23) & 0xff;
    return (e - 1) < 254u || (v << 1) == 0;
}

/* not NaN and not denormal: +-0, normal or infinity */
static inline bool ffp_f32_cmp_ok(uint32_t v)
{
    uint32_t e = (v >> 23) & 0xff;
    return (e - 1) < 254u || (v & 0x7fffff) == 0;
}

/* r finite, and r == 0 or |r| > FLT_MIN */
static inline bool ffp_f32_res_ok(float r)
{
    uint32_t e = (ffp_f2u(r) >> 23) & 0xff;
    return (e - 2) < 253u || (ffp_f2u(r) << 1) == 0 ||
           (e == 1 && (ffp_f2u(r) & 0x7fffff) != 0);
}

/* Core of the SSE float32 add/sub/mul/div/sqrt fast path. */
static inline bool ffp_f32_arith(int op, float32 a, float32 b,
                                 float_status *s, float32 *out)
{
    uint32_t va = float32_val(a), vb = float32_val(b);
    float fa, fb, r;
    bool inexact;

    if (!ffp_enabled() || s->float_rounding_mode != float_round_nearest_even ||
        !ffp_f32_zon(va)) {
        return false;
    }
    fa = ffp_u2f(va);
    if (op == FFP_SQRT) {
        if ((va >> 31) && va != 0x80000000u) {
            return false;               /* sqrt(negative): invalid */
        }
        r = __builtin_sqrtf(fa);        /* normal or +-0: never tiny */
        inexact = (double)r * (double)r != (double)fa;
        goto done;
    }
    if (!ffp_f32_zon(vb)) {
        return false;
    }
    fb = ffp_u2f(vb);
    switch (op) {
    case FFP_SUB:
        fb = -fb;                       /* a - b == a + (-b), exactly */
        /* fall through */
    case FFP_ADD: {
        float bb, err;
        r = fa + fb;
        if (!ffp_f32_res_ok(r)) {
            return false;
        }
        bb = r - fa;
        err = (fa - (r - bb)) + (fb - bb);
        inexact = err != 0;
        break;
    }
    case FFP_MUL:
        r = fa * fb;
        if (!ffp_f32_res_ok(r) || (r == 0 && fa != 0 && fb != 0)) {
            return false;
        }
        inexact = (double)r != (double)fa * (double)fb;
        break;
    case FFP_DIV:
        if (fb == 0) {
            return false;
        }
        r = fa / fb;
        if (!ffp_f32_res_ok(r) || (r == 0 && fa != 0)) {
            return false;
        }
        inexact = (double)r * (double)fb != (double)fa;
        break;
    default:
        return false;
    }
done:
    ffp_raise_inexact(inexact, s);
    *out = make_float32(ffp_f2u(r));
    return true;
}

static inline float32 ffp_float32_add(float32 a, float32 b, float_status *s)
{
    float32 r;
    return ffp_f32_arith(FFP_ADD, a, b, s, &r) ? r : float32_add(a, b, s);
}

static inline float32 ffp_float32_sub(float32 a, float32 b, float_status *s)
{
    float32 r;
    return ffp_f32_arith(FFP_SUB, a, b, s, &r) ? r : float32_sub(a, b, s);
}

static inline float32 ffp_float32_mul(float32 a, float32 b, float_status *s)
{
    float32 r;
    return ffp_f32_arith(FFP_MUL, a, b, s, &r) ? r : float32_mul(a, b, s);
}

static inline float32 ffp_float32_div(float32 a, float32 b, float_status *s)
{
    float32 r;
    return ffp_f32_arith(FFP_DIV, a, b, s, &r) ? r : float32_div(a, b, s);
}

static inline float32 ffp_float32_sqrt(float32 a, float_status *s)
{
    float32 r;
    return ffp_f32_arith(FFP_SQRT, a, a, s, &r) ? r : float32_sqrt(a, s);
}

/*
 * Comparisons: for non-NaN, non-denormal operands softfloat raises no flag
 * (DE only for denormals, IE only for NaNs) and orders numerically with
 * -0 == +0, as the host does.
 */
static inline FloatRelation ffp_f32_relation(float32 a, float32 b)
{
    float fa = ffp_u2f(float32_val(a)), fb = ffp_u2f(float32_val(b));
    return fa < fb ? float_relation_less :
           fa > fb ? float_relation_greater : float_relation_equal;
}

static inline bool ffp_f32_cmp_ok2(float32 a, float32 b)
{
    return ffp_enabled() && ffp_f32_cmp_ok(float32_val(a)) &&
           ffp_f32_cmp_ok(float32_val(b));
}

static inline FloatRelation ffp_float32_compare(float32 a, float32 b,
                                                float_status *s)
{
    if (ffp_f32_cmp_ok2(a, b)) {
        return ffp_f32_relation(a, b);
    }
    return float32_compare(a, b, s);
}

static inline FloatRelation ffp_float32_compare_quiet(float32 a, float32 b,
                                                      float_status *s)
{
    if (ffp_f32_cmp_ok2(a, b)) {
        return ffp_f32_relation(a, b);
    }
    return float32_compare_quiet(a, b, s);
}

static inline bool ffp_float32_lt(float32 a, float32 b, float_status *s)
{
    if (ffp_f32_cmp_ok2(a, b)) {
        return ffp_u2f(float32_val(a)) < ffp_u2f(float32_val(b));
    }
    return float32_lt(a, b, s);
}

/*
 * rcpss/rsqrtss as implemented by QEMU: RN(1/x) and RN(1/RN(sqrt(x))) with
 * the flags discarded by the caller.  Operands: +-0, normal, infinity;
 * rsqrt additionally requires x >= 0 (or -0).  Decline on a tiny nonzero
 * result (FTZ could apply).  Returns false to fall back.
 */
static inline bool ffp_f32_rcp(float32 a, float_status *s, float32 *out)
{
    uint32_t v = float32_val(a);
    float r;

    if (!ffp_enabled() || s->float_rounding_mode != float_round_nearest_even ||
        !ffp_f32_cmp_ok(v)) {
        return false;
    }
    r = 1.0f / ffp_u2f(v);
    if (!(fabsf(r) > FLT_MIN) && r != 0 && !isinf(r)) {
        return false;
    }
    *out = make_float32(ffp_f2u(r));
    return true;
}

static inline bool ffp_f32_rsqrt(float32 a, float_status *s, float32 *out)
{
    uint32_t v = float32_val(a);
    float r;

    if (!ffp_enabled() || s->float_rounding_mode != float_round_nearest_even ||
        !ffp_f32_cmp_ok(v) || ((v >> 31) && v != 0x80000000u)) {
        return false;
    }
    r = 1.0f / __builtin_sqrtf(ffp_u2f(v));
    if (!(fabsf(r) > FLT_MIN) && r != 0 && !isinf(r)) {
        return false;
    }
    *out = make_float32(ffp_f2u(r));
    return true;
}

/* int32 -> float32: host conversion is RN; inexact iff not round-trip. */
static inline float32 ffp_int32_to_float32(int32_t v, float_status *s)
{
    if (ffp_enabled() && s->float_rounding_mode == float_round_nearest_even) {
        float r = (float)v;
        ffp_raise_inexact((double)r != (double)v, s);
        return make_float32(ffp_f2u(r));
    }
    return int32_to_float32(v, s);
}

/*
 * float32 -> int32 with rounding mode rm, via the exact floatx80 form.
 * Returns false (no flags touched) for NaN/inf/denormal or out-of-range.
 */
static inline bool ffp_f32_to_int32(float32 a, FloatRoundMode rm,
                                    float_status *s, int32_t *out)
{
    floatx80 x;
    int64_t r;
    bool inexact;

    if (!ffp_enabled() || !ffp_f32_to_x80(float32_val(a), &x) ||
        !ffp_x80_to_int(x, rm, &r, &inexact) || r != (int32_t)r) {
        return false;
    }
    ffp_raise_inexact(inexact, s);
    *out = (int32_t)r;
    return true;
}

/*
 * float64 (SSE2) has no fast path: the emulated Pentium III has no SSE2.
 * These let the ops_sse.h size-generic macros name ffp_float##size##_op.
 */
#define ffp_float64_add           float64_add
#define ffp_float64_sub           float64_sub
#define ffp_float64_mul           float64_mul
#define ffp_float64_div           float64_div
#define ffp_float64_lt            float64_lt
#define ffp_float64_compare       float64_compare
#define ffp_float64_compare_quiet float64_compare_quiet

#endif /* I386_TCG_FAST_FP_H */
