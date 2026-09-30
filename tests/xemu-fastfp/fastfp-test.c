/*
 * Differential test: target/i386/tcg/fast_fp.h fast paths vs softfloat.
 *
 * Native host program; links fpu/softfloat.c built with a tiny osdep shim.
 * Build and run with ./run.sh (see there).  For every input the fast path
 * accepts, the result bits and the exception flags must be identical to
 * softfloat's, starting from the same float_status.  Inputs the fast path
 * must never accept (NaNs, denormals, non-RNE rounding where rounding
 * matters, ...) are counted as "declined" and double-checked with explicit
 * gate assertions.
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "fpu/softfloat.h"
#include "../../target/i386/tcg/fast_fp.h"

int xemu_fast_fp_state = 1;
int ffp_x87_cmp_ok_dummy(void);
int xemu_fast_fp_init(void)
{
    return xemu_fast_fp_state = 1;
}

/* ---------------------------------------------------------------- RNG */
static uint64_t rng_s = 0x9E3779B97F4A7C15ULL;
static inline uint64_t rnd64(void)
{
    uint64_t x = rng_s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_s = x;
}
static inline uint32_t rnd32(void) { return (uint32_t)(rnd64() >> 32); }
static inline uint32_t rndn(uint32_t n) { return (uint32_t)(rnd64() % n); }

/* ------------------------------------------------------------ stats */
static unsigned long long n_tests, n_fast, n_mismatch, n_gate_fail;
static int mismatch_prints;

#define MISMATCH(...) do {                                          \
        n_mismatch++;                                               \
        if (mismatch_prints++ < 40) {                               \
            fprintf(stderr, "MISMATCH: " __VA_ARGS__);              \
        }                                                           \
    } while (0)

static const FloatRoundMode rmodes[4] = {
    float_round_nearest_even, float_round_down, float_round_up,
    float_round_to_zero,
};
static const char *rmname[4] = { "rne", "down", "up", "rz" };

static void init_x87_status(float_status *s, int rm, FloatX80RoundPrec pc)
{
    memset(s, 0, sizeof(*s));
    set_float_2nan_prop_rule(float_2nan_prop_x87, s);
    set_float_default_nan_pattern(0b11000000, s);
    set_float_ftz_detection(float_ftz_after_rounding, s);
    set_float_rounding_mode(rmodes[rm], s);
    set_floatx80_rounding_precision(pc, s);
}

static void init_sse_status(float_status *s, int rm, bool ftz, bool daz,
                            int flags)
{
    memset(s, 0, sizeof(*s));
    set_float_2nan_prop_rule(float_2nan_prop_x87, s);
    set_float_infzeronan_rule(float_infzeronan_dnan_never |
                              float_infzeronan_suppress_invalid, s);
    set_float_3nan_prop_rule(float_3nan_prop_abc, s);
    set_float_default_nan_pattern(0b11000000, s);
    set_float_ftz_detection(float_ftz_after_rounding, s);
    set_float_rounding_mode(rmodes[rm], s);
    set_flush_inputs_to_zero(daz, s);
    set_flush_to_zero(ftz, s);
    set_float_exception_flags(flags, s);
}

/* ---------------------------------------------------- value generators */

/* float32 bit patterns biased towards interesting classes */
static uint32_t gen_f32(void)
{
    uint32_t sign = rnd32() & 0x80000000u;
    uint32_t frac = rnd32() & 0x7fffff;
    uint32_t e;

    switch (rndn(16)) {
    case 0:
        return rnd32();                                 /* anything */
    case 1: {                                           /* specials */
        static const uint32_t sp[] = {
            0x00000000, 0x80000000, 0x7f800000, 0xff800000, /* 0, inf */
            0x7fc00000, 0xffc00000, 0x7f800001, 0x7fbfffff, /* NaNs */
            0x00000001, 0x007fffff, 0x80400000,            /* denormals */
            0x00800000, 0x80800000, 0x00800001, 0x7f7fffff, 0xff7fffff,
            0x3f800000, 0xbf800000, 0x40000000, 0x3f000000,
            0x4b000000, 0x4f000000, 0xcf000000, 0x4effffff, 0xcf000001,
            0x3effffff, 0x3f7fffff, 0x3fc00000, 0x40200000, 0xbfc00000,
        };
        return sp[rndn(ARRAY_SIZE(sp))];
    }
    case 2:                                             /* denormal */
        return sign | (frac ? frac : 1);
    case 3:                                             /* near FLT_MIN */
        e = 1 + rndn(24);
        return sign | e << 23 | frac;
    case 4:                                             /* near FLT_MAX */
        e = 254 - rndn(24);
        return sign | e << 23 | frac;
    case 5: {                                           /* small integers */
        float f = (float)(int32_t)(rnd32() % 2001 - 1000);
        return ffp_f2u(f);
    }
    case 6: {                                           /* few bits */
        uint32_t fr = frac & (0x7fffff << rndn(23));
        e = 100 + rndn(55);
        return sign | e << 23 | (fr & 0x7fffff);
    }
    case 7:                                             /* ints near 2^31 */
        e = 127 + 28 + rndn(5);
        return sign | e << 23 | frac;
    default:                                            /* typical range */
        e = 127 - 20 + rndn(41);
        return sign | e << 23 | frac;
    }
}

static floatx80 gen_x80(void)
{
    uint16_t sign = (rnd32() & 1) << 15;
    uint64_t m = rnd64() | (1ULL << 63);
    uint32_t e;

    switch (rndn(20)) {
    case 0:
        return make_floatx80(rnd32() & 0xffff, rnd64());    /* anything */
    case 1: {
        static const floatx80 sp[] = {
            make_floatx80_init(0, 0), make_floatx80_init(0x8000, 0),
            make_floatx80_init(0x7fff, 0x8000000000000000ULL),
            make_floatx80_init(0xffff, 0x8000000000000000ULL),
            make_floatx80_init(0x7fff, 0xc000000000000000ULL),   /* qnan */
            make_floatx80_init(0x7fff, 0x8000000000000001ULL),   /* snan */
            make_floatx80_init(0x7fff, 0),                       /* pseudo-inf */
            make_floatx80_init(0x0000, 0x0000000000000001ULL),   /* denormal */
            make_floatx80_init(0x0000, 0x8000000000000000ULL),   /* pseudo-den */
            make_floatx80_init(0x3fff, 0x4000000000000000ULL),   /* unnormal */
            make_floatx80_init(0x3fff, 0x8000000000000000ULL),   /* 1 */
            make_floatx80_init(0xbfff, 0x8000000000000000ULL),   /* -1 */
            make_floatx80_init(0x0001, 0x8000000000000000ULL),   /* min norm */
            make_floatx80_init(0x7ffe, 0xffffffffffffffffULL),   /* max */
            make_floatx80_init(0x3c01, 0x8000000000000000ULL),   /* DBL_MIN */
            make_floatx80_init(0x3c02, 0x8000000000000000ULL),   /* 2DBL_MIN */
            make_floatx80_init(0x43fe, 0xfffffffffffff800ULL),   /* DBL_MAX */
            make_floatx80_init(0x3f81, 0x8000000000000000ULL),   /* FLT_MIN */
            make_floatx80_init(0x407e, 0xffffff0000000000ULL),   /* FLT_MAX */
            make_floatx80_init(0x401d, 0xffffffffffffffffULL),
            make_floatx80_init(0x403d, 0x8000000000000000ULL),   /* 2^62 */
            make_floatx80_init(0x403c, 0xffffffffffffffffULL),
            make_floatx80_init(0x3ffe, 0x8000000000000000ULL),   /* 0.5 */
            make_floatx80_init(0x3ffe, 0xc000000000000000ULL),   /* 0.75 */
            make_floatx80_init(0x3fff, 0xc000000000000000ULL),   /* 1.5 */
            make_floatx80_init(0x4000, 0xa000000000000000ULL),   /* 2.5 */
            make_floatx80_init(0x400e, 0xffff000000000000ULL),   /* 65535 */
            make_floatx80_init(0x400d, 0xfffe000000000000ULL),   /* 32767 */
            make_floatx80_init(0xc00e, 0x8000000000000000ULL),   /* -32768 */
            make_floatx80_init(0x401e, 0xfffffffe00000000ULL),   /* 2^31-1 */
            make_floatx80_init(0xc01e, 0x8000000000000000ULL),   /* -2^31 */
            make_floatx80_init(0x401e, 0x8000000000000000ULL),   /* 2^31 */
        };
        return sp[rndn(ARRAY_SIZE(sp))];
    }
    case 2: {                                   /* from float32 */
        floatx80 r;
        if (ffp_f32_to_x80(gen_f32(), &r)) {
            return r;
        }
        return make_floatx80(sign | 0x3fff, m & ~((1ULL << 40) - 1));
    }
    case 3: case 4: case 5: case 6:             /* 24-bit significand */
        e = FFP_X80_BIAS - 40 + rndn(81);
        return make_floatx80(sign | e, m & ~((1ULL << 40) - 1));
    case 7:                                     /* 24-bit, wide exponent */
        e = FFP_X80_BIAS - 1100 + rndn(2200);
        return make_floatx80(sign | e, m & ~((1ULL << 40) - 1));
    case 8: case 9: case 10:                    /* 53-bit significand */
        e = FFP_X80_BIAS - 40 + rndn(81);
        return make_floatx80(sign | e, m & ~0x7ffULL);
    case 11:                                    /* 53-bit near double edge */
        e = rndn(2) ? FFP_X80_BIAS - 1022 + rndn(60)
                    : FFP_X80_BIAS + 1023 - rndn(60);
        return make_floatx80(sign | e, m & ~0x7ffULL);
    case 12:                                    /* few significant bits */
        e = FFP_X80_BIAS - 30 + rndn(61);
        return make_floatx80(sign | e, m & ~((1ULL << (40 + rndn(24))) - 1)
                                       & ~(1ULL << 62 >> rndn(2)));
    case 13: {                                  /* small integer */
        int64_t v = (int64_t)(rnd32() % 200001) - 100000;
        return ffp_int64_to_x80(v);
    }
    case 14:                                    /* full 64-bit */
        e = FFP_X80_BIAS - 40 + rndn(81);
        return make_floatx80(sign | e, m);
    case 15:                                    /* near int32/int64 limits */
        e = FFP_X80_BIAS + 14 + rndn(50);
        return make_floatx80(sign | e, m & ~((1ULL << rndn(64)) - 1));
    default:                                    /* 24/53-bit mix, small */
        e = FFP_X80_BIAS - 3 + rndn(7);
        return make_floatx80(sign | e,
                             m & ~((1ULL << (rndn(2) ? 40 : 11)) - 1));
    }
}

/* Derive a second operand related to the first (cancellation, ties...) */
static floatx80 gen_x80_pair(floatx80 a)
{
    switch (rndn(10)) {
    case 0:
        return a;
    case 1:
        return make_floatx80(a.high ^ 0x8000, a.low);
    case 2:                                     /* a +- small ulps */
        return make_floatx80(a.high ^ (rndn(2) << 15),
                             a.low + ((uint64_t)(rndn(5)) << (rndn(2) ? 40 : 11)));
    case 3:                                     /* exponent shifted */
        return make_floatx80((a.high & 0x8000) |
                             ((a.high & 0x7fff) + rndn(64) - 32), a.low);
    case 4: {                                   /* rounding ties / near */
        static const int k[] = { 23, 24, 25, 52, 53, 54, 63, 64, 65 };
        uint64_t m = rndn(2) ? 0x8000000000000000ULL : 0xc000000000000000ULL;
        return make_floatx80(((rnd32() & 1) << 15) |
                             ((a.high & 0x7fff) - k[rndn(ARRAY_SIZE(k))]),
                             m);
    }
    case 5: {                                   /* results near limits */
        static const int t[] = { -1022, -1021, -1020, 1022, 1023, 1024,
                                 -126, -125, 127, 128, -16382, 16383 };
        int ea = (int)(a.high & 0x7fff) - FFP_X80_BIAS;
        int target = t[rndn(ARRAY_SIZE(t))];
        int eb = rndn(2) ? target - ea : ea - target;   /* mul or div */
        return make_floatx80(((rnd32() & 1) << 15) |
                             ((eb + FFP_X80_BIAS) & 0x7fff),
                             (rnd64() | (1ULL << 63)) &
                             ~((1ULL << (rndn(2) ? 40 : 11)) - 1));
    }
    default:
        return gen_x80();
    }
}

static uint32_t gen_f32_pair(uint32_t a)
{
    switch (rndn(10)) {
    case 0:
        return a;
    case 1:
        return a ^ 0x80000000u;
    case 2:
        return (a ^ (rndn(2) << 31)) + rndn(5) - 2;
    case 3:
        return a + ((rndn(60) - 30) << 23);
    case 4: {                                   /* rounding ties / near */
        int ea = (a >> 23) & 0xff;
        int eb = ea - 24 - (int)rndn(3);
        if (eb < 1) {
            return gen_f32();
        }
        return (rnd32() & 0x80000000u) | (uint32_t)eb << 23 |
               (rndn(2) ? 0 : 0x400000);
    }
    case 5: {                                   /* results near limits */
        static const int t[] = { -127, -126, -125, 127, 128 };
        int ea = (int)((a >> 23) & 0xff) - 127;
        int target = t[rndn(ARRAY_SIZE(t))];
        int eb = (rndn(2) ? target - ea : ea - target) + 127;
        if (eb < 1 || eb > 254) {
            return gen_f32();
        }
        return (rnd32() & 0x80000000u) | (uint32_t)eb << 23 |
               (rnd32() & 0x7fffff);
    }
    default:
        return gen_f32();
    }
}

static bool x80_eq(floatx80 a, floatx80 b)
{
    return a.high == b.high && a.low == b.low;
}

/* ------------------------------------------------------------- x87 tests */

static const char *opname[] = { "add", "sub", "mul", "div", "sqrt" };

static void test_x87_arith(long iters)
{
    static const FloatX80RoundPrec pcs[3] = {
        floatx80_precision_s, floatx80_precision_d, floatx80_precision_x
    };
    static const char *pcname[3] = { "s", "d", "x" };

    for (int op = FFP_ADD; op <= FFP_SQRT; op++) {
        for (int pc = 0; pc < 3; pc++) {
            for (int rm = 0; rm < 4; rm++) {
                unsigned long long fast = 0;
                for (long i = 0; i < iters; i++) {
                    float_status s1, s2;
                    floatx80 a = gen_x80();
                    floatx80 b = gen_x80_pair(a), r1, r2;
                    bool ok;

                    if (rndn(2)) {
                        floatx80 t = a; a = b; b = t;
                    }
                    init_x87_status(&s1, rm, pcs[pc]);
                    init_x87_status(&s2, rm, pcs[pc]);
                    switch (op) {
                    case FFP_ADD: r1 = floatx80_add(a, b, &s1); break;
                    case FFP_SUB: r1 = floatx80_sub(a, b, &s1); break;
                    case FFP_MUL: r1 = floatx80_mul(a, b, &s1); break;
                    case FFP_DIV: r1 = floatx80_div(a, b, &s1); break;
                    default:      r1 = floatx80_sqrt(a, &s1); break;
                    }
                    ok = ffp_x87_arith(op, a, b, &s2, &r2);
                    n_tests++;
                    if (!ok) {
                        if (s2.float_exception_flags) {
                            MISMATCH("x87 %s declined but set flags\n",
                                     opname[op]);
                        }
                        continue;
                    }
                    fast++;
                    if (rm != 0) {
                        n_gate_fail++;
                    }
                    if (!x80_eq(r1, r2) ||
                        s1.float_exception_flags != s2.float_exception_flags) {
                        MISMATCH("x87 %s pc=%s rm=%s a=%04x:%016" PRIx64
                                 " b=%04x:%016" PRIx64 " soft=%04x:%016" PRIx64
                                 "/%x fast=%04x:%016" PRIx64 "/%x\n",
                                 opname[op], pcname[pc], rmname[rm],
                                 a.high, a.low, b.high, b.low, r1.high, r1.low,
                                 s1.float_exception_flags, r2.high, r2.low,
                                 s2.float_exception_flags);
                    }
                }
                n_fast += fast;
                printf("  x87 %-4s pc=%s rm=%-4s: %ld tests, %llu fast\n",
                       opname[op], pcname[pc], rmname[rm], iters, fast);
            }
        }
    }
}

static void test_x87_misc(long iters)
{
    unsigned long long fast_cmp = 0, fast_ld = 0, fast_st = 0, fast_int = 0;

    for (long i = 0; i < iters; i++) {
        int rm = rndn(4);
        float_status s1, s2;
        floatx80 a = gen_x80(), b = gen_x80_pair(a);

        /* compare / compare_quiet */
        for (int q = 0; q < 2; q++) {
            FloatRelation r1, r2;
            init_x87_status(&s1, rm, floatx80_precision_x);
            init_x87_status(&s2, rm, floatx80_precision_x);
            r1 = q ? floatx80_compare_quiet(a, b, &s1)
                   : floatx80_compare(a, b, &s1);
            r2 = q ? ffp_floatx80_compare_quiet(a, b, &s2)
                   : ffp_floatx80_compare(a, b, &s2);
            n_tests++;
            fast_cmp += ffp_x80_cmp_ok(a) && ffp_x80_cmp_ok(b);
            if (r1 != r2 || s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("x87 compare%s a=%04x:%016" PRIx64 " b=%04x:%016"
                         PRIx64 " %d/%x vs %d/%x\n", q ? "_quiet" : "",
                         a.high, a.low, b.high, b.low, r1,
                         s1.float_exception_flags, r2,
                         s2.float_exception_flags);
            }
        }

        /* loads */
        {
            uint32_t f = gen_f32();
            uint64_t d = rndn(4) ? ((uint64_t)rnd64()) :
                         ((uint64_t)(rnd32() & 0x80000000u) << 32 |
                          (uint64_t)rndn(2048) << 52 | (rnd64() >> 12));
            floatx80 r1, r2;
            FloatX80RoundPrec pc = (FloatX80RoundPrec)rndn(3);
            init_x87_status(&s1, rm, pc);
            init_x87_status(&s2, rm, pc);
            r1 = float32_to_floatx80(make_float32(f), &s1);
            fast_ld += ffp_f32_to_x80(f, &r2);
            r2 = ffp_float32_to_floatx80(make_float32(f), &s2);
            n_tests++;
            if (!x80_eq(r1, r2) ||
                s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("flds %08x\n", f);
            }
            if (rndn(8) == 0) {
                d = ((uint64_t)(rnd32() & 0x80000000u) << 32) |
                    (rndn(2) ? 0 : 0x7ff0000000000000ULL) | rndn(3);
            }
            r1 = float64_to_floatx80(make_float64(d), &s1);
            fast_ld += (pc != floatx80_precision_s || !(d & 0x1fffffff)) &&
                       ffp_f64_to_x80_load(d, &r2);
            r2 = ffp_float64_to_floatx80(make_float64(d), &s2);
            n_tests++;
            if (!x80_eq(r1, r2) ||
                s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("fldl %016" PRIx64 "\n", d);
            }
        }

        /* stores */
        {
            float32 f1, f2;
            float64 d1, d2;
            uint64_t tmp;
            init_x87_status(&s1, rm, floatx80_precision_x);
            init_x87_status(&s2, rm, floatx80_precision_x);
            f1 = floatx80_to_float32(a, &s1);
            {
                float_status s3 = s2;
                fast_st += ffp_x80_store(a, &s3, 23, 127, 254, &tmp);
            }
            f2 = ffp_floatx80_to_float32(a, &s2);
            n_tests++;
            if (float32_val(f1) != float32_val(f2) ||
                s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("fsts rm=%s %04x:%016" PRIx64 " soft=%08x/%x "
                         "fast=%08x/%x\n", rmname[rm], a.high, a.low,
                         float32_val(f1), s1.float_exception_flags,
                         float32_val(f2), s2.float_exception_flags);
            }
            init_x87_status(&s1, rm, floatx80_precision_x);
            init_x87_status(&s2, rm, floatx80_precision_x);
            d1 = floatx80_to_float64(a, &s1);
            {
                float_status s3 = s2;
                fast_st += ffp_x80_store(a, &s3, 52, 1023, 2046, &tmp);
            }
            d2 = ffp_floatx80_to_float64(a, &s2);
            n_tests++;
            if (float64_val(d1) != float64_val(d2) ||
                s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("fstl rm=%s %04x:%016" PRIx64 " soft=%016" PRIx64
                         "/%x fast=%016" PRIx64 "/%x\n", rmname[rm], a.high,
                         a.low, float64_val(d1), s1.float_exception_flags,
                         float64_val(d2), s2.float_exception_flags);
            }
        }

        /* float -> int: the fist/fistt wrappers, compared for all inputs */
        for (int rz = 0; rz < 2; rz++) {
            FloatRoundMode mode = rz ? float_round_to_zero : rmodes[rm];
            int64_t v, i64a, i64b;
            int32_t i32a, i32b;
            bool inexact;

            fast_int += ffp_x80_to_int(a, mode, &v, &inexact);
            init_x87_status(&s1, rm, floatx80_precision_x);
            init_x87_status(&s2, rm, floatx80_precision_x);
            i64a = rz ? floatx80_to_int64_round_to_zero(a, &s1)
                      : floatx80_to_int64(a, &s1);
            i64b = rz ? ffp_floatx80_to_int64_round_to_zero(a, &s2)
                      : ffp_floatx80_to_int64(a, &s2);
            n_tests++;
            if (i64a != i64b ||
                s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("fistll rz=%d rm=%s %04x:%016" PRIx64
                         " soft=%" PRId64 "/%x fast=%" PRId64 "/%x\n",
                         rz, rmname[rm], a.high, a.low, i64a,
                         s1.float_exception_flags, i64b,
                         s2.float_exception_flags);
            }
            init_x87_status(&s1, rm, floatx80_precision_x);
            init_x87_status(&s2, rm, floatx80_precision_x);
            i32a = rz ? floatx80_to_int32_round_to_zero(a, &s1)
                      : floatx80_to_int32(a, &s1);
            i32b = rz ? ffp_floatx80_to_int32_round_to_zero(a, &s2)
                      : ffp_floatx80_to_int32(a, &s2);
            n_tests++;
            if (i32a != i32b ||
                s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("fistl rz=%d rm=%s %04x:%016" PRIx64
                         " soft=%d/%x fast=%d/%x\n", rz, rmname[rm],
                         a.high, a.low, i32a, s1.float_exception_flags,
                         i32b, s2.float_exception_flags);
            }
        }

        /* int -> floatx80 */
        {
            int64_t v = rndn(2) ? (int64_t)rnd64() >> rndn(64)
                                : (int32_t)rnd32() >> rndn(32);
            floatx80 r1, r2 = ffp_int64_to_x80(v);
            /* fild{l,ll}_ST0 force PC=extended */
            init_x87_status(&s1, rm, floatx80_precision_x);
            r1 = int64_to_floatx80(v, &s1);
            n_tests++;
            if (!x80_eq(r1, r2) || s1.float_exception_flags) {
                MISMATCH("fild %" PRId64 "\n", v);
            }
            if (v == (int32_t)v) {
                r1 = int32_to_floatx80((int32_t)v, &s1);
                n_tests++;
                if (!x80_eq(r1, r2) || s1.float_exception_flags) {
                    MISMATCH("fildl %" PRId64 "\n", v);
                }
                /* fildl_FT0 uses the current PC */
                for (int pc = 0; pc < 3; pc++) {
                    init_x87_status(&s1, rm, (FloatX80RoundPrec)pc);
                    init_x87_status(&s2, rm, (FloatX80RoundPrec)pc);
                    r1 = int32_to_floatx80((int32_t)v, &s1);
                    r2 = ffp_int32_to_floatx80((int32_t)v, &s2);
                    n_tests++;
                    if (!x80_eq(r1, r2) || s1.float_exception_flags !=
                        s2.float_exception_flags) {
                        MISMATCH("fildl_FT0 pc=%d %" PRId64 "\n", pc, v);
                    }
                }
            }
        }
    }
    n_fast += fast_cmp + fast_ld + fast_st + fast_int;
    printf("  x87 misc: %ld iterations; fast: cmp %llu, load %llu, "
           "store %llu, toint %llu\n", iters, fast_cmp, fast_ld, fast_st,
           fast_int);
}

/* Explicit gate checks: these inputs must never take the fast path. */
static void test_x87_gates(void)
{
    static const floatx80 bad[] = {
        make_floatx80_init(0x7fff, 0xc000000000000000ULL),
        make_floatx80_init(0x7fff, 0x8000000000000001ULL),
        make_floatx80_init(0x7fff, 0x8000000000000000ULL),
        make_floatx80_init(0x7fff, 0),
        make_floatx80_init(0x0000, 1),
        make_floatx80_init(0x0000, 0x8000000000000000ULL),
        make_floatx80_init(0x3fff, 0x4000000000000000ULL),
        make_floatx80_init(0x3fff, 0x8000000000000001ULL),  /* 64-bit sig */
        make_floatx80_init(0x3c00, 0x8000000000000000ULL),  /* < DBL_MIN */
        make_floatx80_init(0x4400, 0x8000000000000000ULL),  /* > DBL_MAX */
    };
    floatx80 one = make_floatx80(0x3fff, 0x8000000000000000ULL), r;
    float_status s;

    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        for (int op = FFP_ADD; op <= FFP_SQRT; op++) {
            for (int pc = 0; pc < 3; pc++) {
                init_x87_status(&s, 0, (FloatX80RoundPrec)pc);
                if (ffp_x87_arith(op, bad[i], one, &s, &r) ||
                    (op != FFP_SQRT && ffp_x87_arith(op, one, bad[i], &s, &r))) {
                    n_gate_fail++;
                    fprintf(stderr, "GATE: x87 %s accepted bad input %u\n",
                            opname[op], i);
                }
            }
        }
    }
    /* non-RNE rounding never takes the arithmetic path */
    for (int rm = 1; rm < 4; rm++) {
        init_x87_status(&s, rm, floatx80_precision_s);
        if (ffp_x87_arith(FFP_ADD, one, one, &s, &r)) {
            n_gate_fail++;
        }
    }
    /* division by zero, sqrt of negative */
    init_x87_status(&s, 0, floatx80_precision_d);
    if (ffp_x87_arith(FFP_DIV, one, make_floatx80(0, 0), &s, &r) ||
        ffp_x87_arith(FFP_SQRT, make_floatx80(0xbfff, 1ULL << 63), one, &s, &r)) {
        n_gate_fail++;
    }
    /* runtime switch off: nothing is accepted */
    xemu_fast_fp_state = 0;
    init_x87_status(&s, 0, floatx80_precision_d);
    if (ffp_x87_arith(FFP_ADD, one, one, &s, &r) ||
        ffp_x87_cmp_ok_dummy()) {
        n_gate_fail++;
    }
    xemu_fast_fp_state = 1;
}

/* compare one x87 op fast vs soft; returns whether the fast path accepted */
static bool check_x87_one(int op, floatx80 a, floatx80 b, int pc, int rm)
{
    float_status s1, s2;
    floatx80 r1, r2;
    bool ok;

    init_x87_status(&s1, rm, (FloatX80RoundPrec)pc);
    init_x87_status(&s2, rm, (FloatX80RoundPrec)pc);
    switch (op) {
    case FFP_ADD: r1 = floatx80_add(a, b, &s1); break;
    case FFP_SUB: r1 = floatx80_sub(a, b, &s1); break;
    case FFP_MUL: r1 = floatx80_mul(a, b, &s1); break;
    case FFP_DIV: r1 = floatx80_div(a, b, &s1); break;
    default:      r1 = floatx80_sqrt(a, &s1); break;
    }
    ok = ffp_x87_arith(op, a, b, &s2, &r2);
    n_tests++;
    if (ok && (!x80_eq(r1, r2) ||
               s1.float_exception_flags != s2.float_exception_flags)) {
        MISMATCH("x87 targeted %s pc=%d a=%04x:%016" PRIx64 " b=%04x:%016"
                 PRIx64 " soft=%04x:%016" PRIx64 "/%x fast=%04x:%016" PRIx64
                 "/%x\n", opname[op], pc, a.high, a.low, b.high, b.low,
                 r1.high, r1.low, s1.float_exception_flags, r2.high, r2.low,
                 s2.float_exception_flags);
    }
    return ok;
}

static floatx80 x80_from_double(double d)
{
    return ffp_f64_to_x80(d);          /* d must be +-0 or normal */
}

/*
 * Hand-made cases that random testing is unlikely to hit:
 *  - PC=single double rounding: a = 1 + 2^-24 (25 bits), b = 2^-60:
 *    RN53(a+b) = 1 + 2^-24 is a 24-bit tie that rounds to 1, but
 *    RN24(a+b) = 1 + 2^-23.  The fast path must decline (a is not 24-bit).
 *  - Exact-looking product just below DBL_MIN that rounds up to DBL_MIN:
 *    6361 * (69431*20394401) = 2^53 - 1, scaled to 2^-1022 - 2^-1075.
 *  - many exact-cancellation / tie cases around 1.0 for every PC.
 */
static void test_x87_targeted(void)
{
    floatx80 a = make_floatx80(0x3fff, 0x8000008000000000ULL);  /* 1+2^-24 */
    floatx80 b = make_floatx80(0x3fff - 60, 0x8000000000000000ULL);
    floatx80 p = x80_from_double(ldexp(6361.0, -500));
    floatx80 q = x80_from_double(ldexp(69431.0 * 20394401.0, -575));

    if (check_x87_one(FFP_ADD, a, b, floatx80_precision_s, 0)) {
        n_gate_fail++;
        fprintf(stderr, "GATE: PC=s double-rounding case accepted\n");
    }
    check_x87_one(FFP_ADD, a, b, floatx80_precision_d, 0);
    for (int pc = 0; pc < 3; pc++) {
        if (check_x87_one(FFP_MUL, p, q, pc, 0)) {
            n_gate_fail++;
            fprintf(stderr, "GATE: near-DBL_MIN product accepted\n");
        }
    }
    /* exact square roots with 26/27-bit roots (square has up to 53 bits) */
    for (int i = 0; i < 200000; i++) {
        uint64_t r = ((1ULL << 26) + rndn(0x2d413cc)) | 1;  /* < 2^26.5 */
        double sq = (double)(r * r);
        if (r * r >= (1ULL << 53)) {
            continue;
        }
        for (int pc = 0; pc < 3; pc++) {
            if (!check_x87_one(FFP_SQRT, x80_from_double(ldexp(sq, -2 * (int)rndn(40))),
                               x80_from_double(1.0), pc, 0) && pc == 1) {
                n_gate_fail++;      /* PC=d exact sqrt must be accepted */
            }
        }
    }
    /* exhaustive small grid: a = 1 + i*2^-k, b = +-j*2^-m */
    for (int k = 20; k <= 56; k++) {
        for (int i = 1; i < 8; i++) {
            for (int m = 0; m <= 66; m += 1) {
                for (int j = 1; j < 4; j++) {
                    double da = 1.0 + ldexp(i, -k);
                    double db = ldexp(j, -m);
                    floatx80 xa = x80_from_double(da), xb;
                    for (int sg = 0; sg < 2; sg++) {
                        xb = x80_from_double(sg ? -db : db);
                        for (int pc = 0; pc < 3; pc++) {
                            for (int op = FFP_ADD; op <= FFP_SQRT; op++) {
                                check_x87_one(op, xa, xb, pc, 0);
                                check_x87_one(op, xb, xa, pc, 0);
                            }
                        }
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------- SSE tests */

static void test_sse_arith(long iters)
{
    for (int op = FFP_ADD; op <= FFP_SQRT; op++) {
        for (int rm = 0; rm < 4; rm++) {
            for (int mode = 0; mode < 8; mode++) {
                bool ftz = mode & 1, daz = mode & 2;
                int flags0 = (mode & 4) ? float_flag_inexact : 0;
                unsigned long long fast = 0;
                for (long i = 0; i < iters; i++) {
                    float_status s1, s2;
                    uint32_t a = gen_f32(), b = gen_f32_pair(a);
                    float32 r1, r2;
                    bool ok;

                    if (rndn(2)) {
                        uint32_t t = a; a = b; b = t;
                    }
                    init_sse_status(&s1, rm, ftz, daz, flags0);
                    init_sse_status(&s2, rm, ftz, daz, flags0);
                    switch (op) {
                    case FFP_ADD: r1 = float32_add(a, b, &s1); break;
                    case FFP_SUB: r1 = float32_sub(a, b, &s1); break;
                    case FFP_MUL: r1 = float32_mul(a, b, &s1); break;
                    case FFP_DIV: r1 = float32_div(a, b, &s1); break;
                    default:      r1 = float32_sqrt(a, &s1); break;
                    }
                    {
                        float_status s3 = s2;
                        ok = ffp_f32_arith(op, a, b, &s3, &r2);
                    }
                    switch (op) {
                    case FFP_ADD: r2 = ffp_float32_add(a, b, &s2); break;
                    case FFP_SUB: r2 = ffp_float32_sub(a, b, &s2); break;
                    case FFP_MUL: r2 = ffp_float32_mul(a, b, &s2); break;
                    case FFP_DIV: r2 = ffp_float32_div(a, b, &s2); break;
                    default:      r2 = ffp_float32_sqrt(a, &s2); break;
                    }
                    n_tests++;
                    fast += ok;
                    if (ok && rm != 0) {
                        n_gate_fail++;
                    }
                    if (float32_val(r1) != float32_val(r2) ||
                        s1.float_exception_flags != s2.float_exception_flags) {
                        MISMATCH("sse %s rm=%s ftz=%d daz=%d a=%08x b=%08x "
                                 "soft=%08x/%x fast=%08x/%x (fast=%d)\n",
                                 opname[op], rmname[rm], ftz, daz, a, b,
                                 float32_val(r1), s1.float_exception_flags,
                                 float32_val(r2), s2.float_exception_flags,
                                 ok);
                    }
                }
                n_fast += fast;
                printf("  sse %-4s rm=%-4s ftz=%d daz=%d PE0=%d: %ld tests, "
                       "%llu fast\n", opname[op], rmname[rm], ftz, daz,
                       !!flags0, iters, fast);
            }
        }
    }
}

static void test_sse_misc(long iters)
{
    unsigned long long fast_cmp = 0, fast_rcp = 0, fast_cvt = 0;
    float32 one = make_float32(0x3f800000);

    for (long i = 0; i < iters; i++) {
        int rm = rndn(4), mode = rndn(4);
        bool ftz = mode & 1, daz = mode & 2;
        int flags0 = rndn(2) ? float_flag_inexact : 0;
        float_status s1, s2;
        uint32_t a = gen_f32(), b = gen_f32_pair(a);

        /* compare, compare_quiet, lt (min/max) */
        for (int q = 0; q < 3; q++) {
            int r1, r2;
            init_sse_status(&s1, rm, ftz, daz, flags0);
            init_sse_status(&s2, rm, ftz, daz, flags0);
            switch (q) {
            case 0:
                r1 = float32_compare(a, b, &s1);
                r2 = ffp_float32_compare(a, b, &s2);
                break;
            case 1:
                r1 = float32_compare_quiet(a, b, &s1);
                r2 = ffp_float32_compare_quiet(a, b, &s2);
                break;
            default:
                r1 = float32_lt(a, b, &s1);
                r2 = ffp_float32_lt(a, b, &s2);
                break;
            }
            n_tests++;
            fast_cmp += ffp_f32_cmp_ok2(a, b);
            if (r1 != r2 || s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("sse cmp%d daz=%d a=%08x b=%08x %d/%x vs %d/%x\n", q,
                         daz, a, b, r1, s1.float_exception_flags, r2,
                         s2.float_exception_flags);
            }
        }

        /* rcp / rsqrt (flags discarded by the helpers) */
        {
            float32 r1, r2;
            init_sse_status(&s1, rm, ftz, daz, flags0);
            init_sse_status(&s2, rm, ftz, daz, flags0);
            r1 = float32_div(one, a, &s1);
            if (ffp_f32_rcp(a, &s2, &r2)) {
                fast_rcp++;
                n_tests++;
                if (float32_val(r1) != float32_val(r2) ||
                    s2.float_exception_flags != flags0) {
                    MISMATCH("rcp rm=%s ftz=%d daz=%d a=%08x soft=%08x "
                             "fast=%08x\n", rmname[rm], ftz, daz, a,
                             float32_val(r1), float32_val(r2));
                }
            }
            init_sse_status(&s1, rm, ftz, daz, flags0);
            r1 = float32_div(one, float32_sqrt(a, &s1), &s1);
            if (ffp_f32_rsqrt(a, &s2, &r2)) {
                fast_rcp++;
                n_tests++;
                if (float32_val(r1) != float32_val(r2) ||
                    s2.float_exception_flags != flags0) {
                    MISMATCH("rsqrt a=%08x soft=%08x fast=%08x\n", a,
                             float32_val(r1), float32_val(r2));
                }
            }
        }

        /* conversions */
        {
            int32_t iv = rndn(2) ? (int32_t)rnd32() : (int32_t)rnd32() >> rndn(32);
            float32 r1, r2;
            init_sse_status(&s1, rm, ftz, daz, flags0);
            init_sse_status(&s2, rm, ftz, daz, flags0);
            r1 = int32_to_float32(iv, &s1);
            r2 = ffp_int32_to_float32(iv, &s2);
            n_tests++;
            fast_cvt += rm == 0;
            if (float32_val(r1) != float32_val(r2) ||
                s1.float_exception_flags != s2.float_exception_flags) {
                MISMATCH("cvtsi2ss %d\n", iv);
            }
            for (int rz = 0; rz < 2; rz++) {
                int32_t o1, o2;
                FloatRoundMode m = rz ? float_round_to_zero : rmodes[rm];
                init_sse_status(&s1, rm, ftz, daz, flags0);
                init_sse_status(&s2, rm, ftz, daz, flags0);
                o1 = rz ? float32_to_int32_round_to_zero(a, &s1)
                        : float32_to_int32(a, &s1);
                if (ffp_f32_to_int32(a, m, &s2, &o2)) {
                    fast_cvt++;
                    n_tests++;
                    if (o1 != o2 ||
                        s1.float_exception_flags != s2.float_exception_flags) {
                        MISMATCH("cvt%sss2si rm=%s daz=%d a=%08x soft=%d/%x "
                                 "fast=%d/%x\n", rz ? "t" : "", rmname[rm],
                                 daz, a, o1, s1.float_exception_flags, o2,
                                 s2.float_exception_flags);
                    }
                }
            }
        }
    }
    n_fast += fast_cmp + fast_rcp + fast_cvt;
    printf("  sse misc: %ld iterations; fast: cmp %llu, rcp/rsqrt %llu, "
           "cvt %llu\n", iters, fast_cmp, fast_rcp, fast_cvt);
}

static void test_sse_gates(void)
{
    static const uint32_t bad[] = {
        0x7fc00000, 0xffc00000, 0x7f800001, 0x7fbfffff, /* NaNs */
        0x00000001, 0x807fffff, 0x00400000,             /* denormals */
        0x7f800000, 0xff800000,                         /* inf (arith) */
    };
    float32 r;
    float_status s;

    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        for (int op = FFP_ADD; op <= FFP_SQRT; op++) {
            init_sse_status(&s, 0, false, false, 0);
            if (ffp_f32_arith(op, bad[i], 0x3f800000, &s, &r) ||
                (op != FFP_SQRT &&
                 ffp_f32_arith(op, 0x3f800000, bad[i], &s, &r))) {
                n_gate_fail++;
                fprintf(stderr, "GATE: sse %s accepted %08x\n", opname[op],
                        bad[i]);
            }
        }
        if (i < 7 && (ffp_f32_cmp_ok2(bad[i], 0x3f800000) ||
                      ffp_f32_rcp(bad[i], &s, &r) ||
                      ffp_f32_rsqrt(bad[i], &s, &r))) {
            n_gate_fail++;
            fprintf(stderr, "GATE: sse cmp/rcp accepted %08x\n", bad[i]);
        }
    }
    for (int rm = 1; rm < 4; rm++) {
        init_sse_status(&s, rm, false, false, 0);
        if (ffp_f32_arith(FFP_ADD, 0x3f800000, 0x3f800000, &s, &r) ||
            ffp_f32_rcp(0x40400000, &s, &r)) {
            n_gate_fail++;
        }
    }
}

int ffp_x87_cmp_ok_dummy(void);
int ffp_x87_cmp_ok_dummy(void)
{
    floatx80 one = make_floatx80(0x3fff, 0x8000000000000000ULL);
    float_status s;
    uint32_t v = 0x3f800000;
    float32 r;
    floatx80 x;

    init_sse_status(&s, 0, false, false, 0);
    /* every wrapper must fall back when the switch is off */
    return ffp_f32_arith(FFP_ADD, v, v, &s, &r) || ffp_f32_cmp_ok2(v, v) ||
           ffp_f32_rcp(v, &s, &r) || ffp_f32_rsqrt(v, &s, &r) ||
           ffp_f32_to_int32(v, float_round_nearest_even, &s, (int32_t *)&v) ||
           (ffp_enabled() && ffp_x80_cmp_ok(one)) ||
           !x80_eq(ffp_float32_to_floatx80(0x3f800000, &s), one) ||
           (ffp_enabled() && ffp_f32_to_x80(v, &x));
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 200000;

    if (argc > 2) {
        rng_s = strtoull(argv[2], NULL, 0) | 1;
    }
    printf("fast_fp differential test, %ld iterations per configuration\n",
           iters);
    test_x87_gates();
    test_sse_gates();
    test_x87_targeted();
    test_x87_arith(iters);
    test_x87_misc(iters * 4);
    test_sse_arith(iters);
    test_sse_misc(iters * 4);
    printf("TOTAL: %llu tests, %llu took the fast path, %llu mismatches, "
           "%llu gate failures\n", n_tests, n_fast, n_mismatch, n_gate_fail);
    return (n_mismatch || n_gate_fail) ? 1 : 0;
}
