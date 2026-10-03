/*
 * Tiny Code Interpreter for QEMU
 *
 * Copyright (c) 2009, 2011, 2016 Stefan Weil
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/xemu-wasm-stats.h"
#include "tcg/tcg.h"
#include "tcg/helper-info.h"
#include "tcg/tcg-ldst.h"
#include "disas/dis-asm.h"
#include "tcg-has.h"
#include <ffi.h>
#include <math.h>
#include "tci-direct-call.c.inc"
#include "tci-helper-meta.h"

#ifdef EMSCRIPTEN
#include <emscripten.h>
/*
 * Helper-call profile for the page's Stats report: call counts per helper,
 * keyed by name pointer in a small open-addressing table (vCPU thread only
 * writes; the page reads a formatted snapshot).
 */
#define HELPER_PROF_SLOTS 512
static struct { const char *name; uint64_t calls; } helper_prof[HELPER_PROF_SLOTS];

static inline void helper_prof_hit(const char *name)
{
    static int enabled = -1;
    uintptr_t h;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_HELPER_PROF");
        enabled = xemu_wasm_tb_stats_enabled() || (e && *e == '1');
    }
    if (!enabled) {
        return;
    }
    h = ((uintptr_t)name >> 3) * 2654435761u;
    for (unsigned i = 0; i < HELPER_PROF_SLOTS; i++) {
        unsigned slot = (h + i) & (HELPER_PROF_SLOTS - 1);
        if (helper_prof[slot].name == name) {
            helper_prof[slot].calls++;
            return;
        }
        if (!helper_prof[slot].name) {
            helper_prof[slot].name = name;
            helper_prof[slot].calls = 1;
            return;
        }
    }
}

/* "name=calls;..." for the 24 most-called helpers since start */
EMSCRIPTEN_KEEPALIVE const char *xemu_wasm_helper_top(void)
{
    static char buf[2048];
    int idx[24], n = 0;
    for (int k = 0; k < 24; k++) {
        int best = -1;
        for (int i = 0; i < HELPER_PROF_SLOTS; i++) {
            bool taken = false;
            for (int j = 0; j < n; j++) {
                taken |= idx[j] == i;
            }
            if (!taken && helper_prof[i].name &&
                (best < 0 || helper_prof[i].calls > helper_prof[best].calls)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        idx[n++] = best;
    }
    int len = 0;
    buf[0] = 0;
    for (int j = 0; j < n && len < (int)sizeof(buf) - 64; j++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s=%llu;",
                        helper_prof[idx[j]].name,
                        (unsigned long long)helper_prof[idx[j]].calls);
    }
    return buf;
}
#define HELPER_PROF_HIT(name) helper_prof_hit(name)
#else
#define HELPER_PROF_HIT(name) do { } while (0)
#endif
#ifdef CONFIG_TCG_WASM_JIT
#include "wasm32.h"
#endif


#define ctpop_tr    glue(ctpop, TCG_TARGET_REG_BITS)
#define deposit_tr  glue(deposit, TCG_TARGET_REG_BITS)
#define extract_tr  glue(extract, TCG_TARGET_REG_BITS)
#define sextract_tr glue(sextract, TCG_TARGET_REG_BITS)

/*
 * Enable TCI assertions only when debugging TCG (and without NDEBUG defined).
 * Without assertions, the interpreter runs much faster.
 */
#if defined(CONFIG_DEBUG_TCG)
# define tci_assert(cond) assert(cond)
#else
# define tci_assert(cond) ((void)(cond))
#endif

__thread uintptr_t tci_tb_ptr;

static void tci_write_reg64(tcg_target_ulong *regs, uint32_t high_index,
                            uint32_t low_index, uint64_t value)
{
    regs[low_index] = (uint32_t)value;
    regs[high_index] = value >> 32;
}

/* Create a 64 bit value from two 32 bit values. */
static uint64_t tci_uint64(uint32_t high, uint32_t low)
{
    return ((uint64_t)high << 32) + low;
}

/*
 * Load sets of arguments all at once.  The naming convention is:
 *   tci_args_<arguments>
 * where arguments is a sequence of
 *
 *   b = immediate (bit position)
 *   c = condition (TCGCond)
 *   i = immediate (uint32_t)
 *   I = immediate (tcg_target_ulong)
 *   l = label or pointer
 *   m = immediate (MemOpIdx)
 *   n = immediate (call return length)
 *   r = register
 *   s = signed ldst offset
 */

static void tci_args_l(uint32_t insn, const void *tb_ptr, void **l0)
{
    int diff = sextract32(insn, 12, 20);
    *l0 = diff ? (void *)tb_ptr + diff : NULL;
}

static void tci_args_r(uint32_t insn, TCGReg *r0)
{
    *r0 = extract32(insn, 8, 4);
}

static void tci_args_nl(uint32_t insn, const void *tb_ptr,
                        uint8_t *n0, void **l1)
{
    *n0 = extract32(insn, 8, 4);
    *l1 = sextract32(insn, 12, 20) + (void *)tb_ptr;
}

static void tci_args_rl(uint32_t insn, const void *tb_ptr,
                        TCGReg *r0, void **l1)
{
    *r0 = extract32(insn, 8, 4);
    *l1 = sextract32(insn, 12, 20) + (void *)tb_ptr;
}

static void tci_args_rr(uint32_t insn, TCGReg *r0, TCGReg *r1)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
}

static void tci_args_ri(uint32_t insn, TCGReg *r0, tcg_target_ulong *i1)
{
    *r0 = extract32(insn, 8, 4);
    *i1 = sextract32(insn, 12, 20);
}

static void tci_args_rrm(uint32_t insn, TCGReg *r0,
                         TCGReg *r1, MemOpIdx *m2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *m2 = extract32(insn, 16, 16);
}

static void tci_args_rrr(uint32_t insn, TCGReg *r0, TCGReg *r1, TCGReg *r2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
}

static void tci_args_rrs(uint32_t insn, TCGReg *r0, TCGReg *r1, int32_t *i2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *i2 = sextract32(insn, 16, 16);
}

static void tci_args_rrbb(uint32_t insn, TCGReg *r0, TCGReg *r1,
                          uint8_t *i2, uint8_t *i3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *i2 = extract32(insn, 16, 6);
    *i3 = extract32(insn, 22, 6);
}

static void tci_args_rrrc(uint32_t insn,
                          TCGReg *r0, TCGReg *r1, TCGReg *r2, TCGCond *c3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *c3 = extract32(insn, 20, 4);
}

static void tci_args_rrrbb(uint32_t insn, TCGReg *r0, TCGReg *r1,
                           TCGReg *r2, uint8_t *i3, uint8_t *i4)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *i3 = extract32(insn, 20, 6);
    *i4 = extract32(insn, 26, 6);
}

static void tci_args_rrrr(uint32_t insn,
                          TCGReg *r0, TCGReg *r1, TCGReg *r2, TCGReg *r3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *r3 = extract32(insn, 20, 4);
}

static void tci_args_rrrrrc(uint32_t insn, TCGReg *r0, TCGReg *r1,
                            TCGReg *r2, TCGReg *r3, TCGReg *r4, TCGCond *c5)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *r3 = extract32(insn, 20, 4);
    *r4 = extract32(insn, 24, 4);
    *c5 = extract32(insn, 28, 4);
}

static bool tci_compare32(uint32_t u0, uint32_t u1, TCGCond condition)
{
    bool result = false;
    int32_t i0 = u0;
    int32_t i1 = u1;
    switch (condition) {
    case TCG_COND_EQ:
        result = (u0 == u1);
        break;
    case TCG_COND_NE:
        result = (u0 != u1);
        break;
    case TCG_COND_LT:
        result = (i0 < i1);
        break;
    case TCG_COND_GE:
        result = (i0 >= i1);
        break;
    case TCG_COND_LE:
        result = (i0 <= i1);
        break;
    case TCG_COND_GT:
        result = (i0 > i1);
        break;
    case TCG_COND_LTU:
        result = (u0 < u1);
        break;
    case TCG_COND_GEU:
        result = (u0 >= u1);
        break;
    case TCG_COND_LEU:
        result = (u0 <= u1);
        break;
    case TCG_COND_GTU:
        result = (u0 > u1);
        break;
    case TCG_COND_TSTEQ:
        result = (u0 & u1) == 0;
        break;
    case TCG_COND_TSTNE:
        result = (u0 & u1) != 0;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

static bool tci_compare64(uint64_t u0, uint64_t u1, TCGCond condition)
{
    bool result = false;
    int64_t i0 = u0;
    int64_t i1 = u1;
    switch (condition) {
    case TCG_COND_EQ:
        result = (u0 == u1);
        break;
    case TCG_COND_NE:
        result = (u0 != u1);
        break;
    case TCG_COND_LT:
        result = (i0 < i1);
        break;
    case TCG_COND_GE:
        result = (i0 >= i1);
        break;
    case TCG_COND_LE:
        result = (i0 <= i1);
        break;
    case TCG_COND_GT:
        result = (i0 > i1);
        break;
    case TCG_COND_LTU:
        result = (u0 < u1);
        break;
    case TCG_COND_GEU:
        result = (u0 >= u1);
        break;
    case TCG_COND_LEU:
        result = (u0 <= u1);
        break;
    case TCG_COND_GTU:
        result = (u0 > u1);
        break;
    case TCG_COND_TSTEQ:
        result = (u0 & u1) == 0;
        break;
    case TCG_COND_TSTNE:
        result = (u0 & u1) != 0;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

static uint64_t tci_qemu_ld(CPUArchState *env, uint64_t taddr,
                            MemOpIdx oi, const void *tb_ptr)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = (uintptr_t)tb_ptr;

    switch (mop & MO_SSIZE) {
    case MO_UB:
        return helper_ldub_mmu(env, taddr, oi, ra);
    case MO_SB:
        return helper_ldsb_mmu(env, taddr, oi, ra);
    case MO_UW:
        return helper_lduw_mmu(env, taddr, oi, ra);
    case MO_SW:
        return helper_ldsw_mmu(env, taddr, oi, ra);
    case MO_UL:
        return helper_ldul_mmu(env, taddr, oi, ra);
    case MO_SL:
        return helper_ldsl_mmu(env, taddr, oi, ra);
    case MO_UQ:
        return helper_ldq_mmu(env, taddr, oi, ra);
    default:
        g_assert_not_reached();
    }
}

static void tci_qemu_st(CPUArchState *env, uint64_t taddr, uint64_t val,
                        MemOpIdx oi, const void *tb_ptr)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = (uintptr_t)tb_ptr;

    switch (mop & MO_SIZE) {
    case MO_UB:
        helper_stb_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UW:
        helper_stw_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UL:
        helper_stl_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UQ:
        helper_stq_mmu(env, taddr, val, oi, ra);
        break;
    default:
        g_assert_not_reached();
    }
}

#if defined(CONFIG_TCG_WASM_JIT) && TCG_TARGET_HAS_fpu
/*
 * Host floating point support (see wasm32.h). x87 registers are stored in
 * env as floatx80 (sign+15-bit exponent, 64-bit mantissa with explicit
 * integer bit). The common cases (normal numbers in range) are converted
 * inline with round-to-nearest-even, like an x87 host's fld m80/fstp m64;
 * everything else goes through binary128 long double (wasm32's long
 * double), which has the same exponent width and bias as floatx80, so
 * that step is exact and the compiler runtime does the final rounding.
 */
uint32_t tcg_wasm_fpcr = 0x1f80;

typedef union {
    long double ld;
    struct {
        uint64_t lo, hi;
    };
} TciF128;

QEMU_BUILD_BUG_ON(sizeof(long double) != 16);

static long double fx80_to_ld(uint64_t m, uint16_t se)
{
    TciF128 u;
    uint64_t e = se & 0x7fff;
    uint64_t hf = m >> 15;

    if (e == 0 && (hf >> 48)) {
        e = 1;                  /* pseudo-denormal */
    }
    u.lo = m << 49;
    u.hi = ((uint64_t)(se >> 15) << 63) | (e << 48) | (hf & 0xffffffffffffull);
    return u.ld;
}

static void ld_to_fx80(long double v, uint8_t *p)
{
    TciF128 u = { .ld = v };
    uint64_t e = (u.hi >> 48) & 0x7fff;
    uint64_t m = ((u.hi & 0xffffffffffffull) << 15) | (u.lo >> 49);
    uint16_t se = ((u.hi >> 63) << 15) | e;

    if (e) {
        m |= 1ull << 63;
    }
    memcpy(p, &m, 8);
    memcpy(p + 8, &se, 2);
}

uint64_t tcg_wasm_ld80f_f64(uint32_t ptr)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)ptr;
    uint64_t m, r;
    uint16_t se;
    uint32_t e;
    double d;

    memcpy(&m, p, 8);
    memcpy(&se, p + 8, 2);
    e = se & 0x7fff;
    if (likely(e - 15361 < 2046 && (m >> 63))) {
        uint64_t q = m >> 11, rem = m & 0x7ff;

        q += rem > 0x400 || (rem == 0x400 && (q & 1));
        /* a mantissa carry (q == 2^53) bumps the exponent, maybe to inf */
        return ((uint64_t)(se >> 15) << 63) +
               ((uint64_t)(e - 15360) << 52) + (q - (1ull << 52));
    }
    d = (double)fx80_to_ld(m, se);
    memcpy(&r, &d, 8);
    return r;
}

uint32_t tcg_wasm_ld80f_f32(uint32_t ptr)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)ptr;
    uint64_t m;
    uint32_t r, e;
    uint16_t se;
    float f;

    memcpy(&m, p, 8);
    memcpy(&se, p + 8, 2);
    e = se & 0x7fff;
    if (likely(e - 16257 < 254 && (m >> 63))) {
        uint64_t q = m >> 40, rem = m & ((1ull << 40) - 1);

        q += rem > (1ull << 39) || (rem == (1ull << 39) && (q & 1));
        return ((uint32_t)(se >> 15) << 31) +
               ((e - 16256) << 23) + (uint32_t)(q - (1u << 23));
    }
    f = (float)fx80_to_ld(m, se);
    memcpy(&r, &f, 4);
    return r;
}

void tcg_wasm_st80f_f64(uint32_t ptr, uint64_t v)
{
    uint8_t *p = (uint8_t *)(uintptr_t)ptr;
    uint32_t e = (v >> 52) & 0x7ff;
    double d;

    if (likely(e - 1 < 2046)) {
        uint64_t m = (1ull << 63) | (v << 11);
        uint16_t se = ((v >> 63) << 15) | (e + 15360);

        memcpy(p, &m, 8);
        memcpy(p + 8, &se, 2);
        return;
    }
    memcpy(&d, &v, 8);
    ld_to_fx80(d, p);
}

void tcg_wasm_st80f_f32(uint32_t ptr, uint32_t v)
{
    uint8_t *p = (uint8_t *)(uintptr_t)ptr;
    uint32_t e = (v >> 23) & 0xff;
    float f;

    if (likely(e - 1 < 254)) {
        uint64_t m = (1ull << 63) | ((uint64_t)(v & 0x7fffff) << 40);
        uint16_t se = ((v >> 31) << 15) | (e + 16256);

        memcpy(p, &m, 8);
        memcpy(p + 8, &se, 2);
        return;
    }
    memcpy(&f, &v, 4);
    ld_to_fx80(f, p);
}

static inline double tci_f64(uint64_t v)
{
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static inline uint64_t tci_f64_bits(double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    return v;
}

static inline float tci_f32(uint64_t v)
{
    uint32_t u = v;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline uint64_t tci_f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

uint64_t tcg_wasm_sin_f64(uint64_t v)
{
    return tci_f64_bits(sin(tci_f64(v)));
}

uint64_t tcg_wasm_cos_f64(uint64_t v)
{
    return tci_f64_bits(cos(tci_f64(v)));
}

uint32_t tcg_wasm_sin_f32(uint32_t v)
{
    return tci_f32_bits(sinf(tci_f32(v)));
}

uint32_t tcg_wasm_cos_f32(uint32_t v)
{
    return tci_f32_bits(cosf(tci_f32(v)));
}

/* Round to integer per the flcr rounding control (x86 RC encoding). */
static double tci_fp_round(double x)
{
    switch ((tcg_wasm_fpcr >> 13) & 3) {
    case 0:
        return rint(x);         /* nearest-even: the default environment */
    case 1:
        return floor(x);
    case 2:
        return ceil(x);
    default:
        return trunc(x);
    }
}

/* float -> int with x86 "integer indefinite" on NaN/overflow */
static uint32_t tci_fp_to_i32(double x)
{
    double r = tci_fp_round(x);
    return r >= -2147483648.0 && r <= 2147483647.0 ? (int32_t)r : INT32_MIN;
}

static uint64_t tci_fp_to_i64(double x)
{
    double r = tci_fp_round(x);
    return r >= -9223372036854775808.0 && r < 9223372036854775808.0
           ? (int64_t)r : INT64_MIN;
}

/* comisd-style flags: ZF (0x40), PF (0x04), CF (0x01) */
static uint64_t tci_fp_com(double a, double b)
{
    if (a < b) {
        return 0x01;
    } else if (a == b) {
        return 0x40;
    } else if (a > b) {
        return 0;
    }
    return 0x45;
}
#endif

/* Interpret pseudo code in tb. */
/*
 * Disable CFI checks.
 * One possible operation in the pseudo code is a call to binary code.
 * Therefore, disable CFI checks in the interpreter function
 */
#ifdef CONFIG_TCG_WASM_JIT
/*
 * With the wasm32 JIT, tcg_qemu_tb_exec() in wasm32.c dispatches between
 * this interpreter and compiled blocks; v_tb_ptr is the bytecode of a TB
 * (WasmTBHeader.tci_ptr), and chaining hands over to the dispatcher when
 * the next block has a wasm module.
 */
uintptr_t QEMU_DISABLE_CFI tci_exec_tb(CPUArchState *env,
                                       const void *v_tb_ptr)
#else
uintptr_t QEMU_DISABLE_CFI tcg_qemu_tb_exec(CPUArchState *env,
                                            const void *v_tb_ptr)
#endif
{
    const uint32_t *tb_ptr = v_tb_ptr;
    tcg_target_ulong regs[TCG_TARGET_NB_REGS];
    uint64_t stack[(TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE)
                   / sizeof(uint64_t)];
    bool carry = false;

    regs[TCG_AREG0] = (tcg_target_ulong)env;
    regs[TCG_REG_CALL_STACK] = (uintptr_t)stack;
    tci_assert(tb_ptr);

    for (;;) {
        uint32_t insn;
        TCGOpcode opc;
        TCGReg r0, r1, r2, r3, r4;
        tcg_target_ulong t1;
        TCGCond condition;
        uint8_t pos, len;
        uint32_t tmp32;
        uint64_t tmp64, taddr;
        MemOpIdx oi;
        int32_t ofs;
        void *ptr;

        insn = *tb_ptr++;
        opc = extract32(insn, 0, 8);

        switch (opc) {
#ifdef CONFIG_TCG_WASM_JIT
        case INDEX_op_wasm_census:
#ifdef EMSCRIPTEN
            xemu_wasm_census_hit(insn >> 8);
#endif
            break;
#endif
        case INDEX_op_call:
            {
                void *call_slots[MAX_CALL_IARGS];
                ffi_cif *cif;
                void *func;
                unsigned i, s, n;

                tci_args_nl(insn, tb_ptr, &len, &ptr);
                /* pool entries are tcg_target_ulong, maybe wider than ptrs */
                func = (void *)(uintptr_t)((tcg_target_ulong *)ptr)[0];
                cif = (void *)(uintptr_t)((tcg_target_ulong *)ptr)[1];

                n = cif->nargs;
                for (i = s = 0; i < n; ++i) {
                    ffi_type *t = cif->arg_types[i];
                    call_slots[i] = &stack[s];
                    s += DIV_ROUND_UP(t->size, 8);
                }

                /* Helper functions may need to access the "return address" */
                tci_tb_ptr = (uintptr_t)tb_ptr;
                /* direct call when the signature allows (no JS round trip
                 * through libffi on emscripten); sig follows the cif */
                static int use_ffi = -1;
                if (use_ffi < 0) {
                    use_ffi = getenv("XEMU_TCI_FFI") != NULL;
                }
                const TCIHelperMeta *meta = (const TCIHelperMeta *)(cif + 1);
                unsigned sig = use_ffi ? UINT_MAX : meta->direct_sig;
                HELPER_PROF_HIT(meta->name);
                uint64_t rv;
                XTBSTAT_INC(n_helper);
                if (tci_direct_call(sig, func, stack, &rv)) {
                    if (len == 1) {
                        *(uint32_t *)stack = (uint32_t)rv;
                    } else if (len == 2) {
                        memcpy(stack, &rv, 8);
                    }
                } else {
                    ffi_call(cif, func, stack, call_slots);
                }
            }

            switch (len) {
            case 0: /* void */
                break;
            case 1: /* uint32_t */
                /*
                 * The result winds up "left-aligned" in the stack[0] slot.
                 * Note that libffi has an odd special case in that it will
                 * always widen an integral result to ffi_arg.
                 */
                if (sizeof(ffi_arg) == 8) {
                    regs[TCG_REG_R0] = (uint32_t)stack[0];
                } else {
                    regs[TCG_REG_R0] = *(uint32_t *)stack;
                }
                break;
            case 2: /* uint64_t */
                /*
                 * For TCG_TARGET_REG_BITS == 32, the register pair
                 * must stay in host memory order.
                 */
                memcpy(&regs[TCG_REG_R0], stack, 8);
                break;
            case 3: /* Int128 */
                memcpy(&regs[TCG_REG_R0], stack, 16);
                break;
            default:
                g_assert_not_reached();
            }
            break;

        case INDEX_op_br:
            tci_args_l(insn, tb_ptr, &ptr);
            tb_ptr = ptr;
            continue;
#if TCG_TARGET_REG_BITS == 32
        case INDEX_op_setcond2_i32:
            tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &condition);
            regs[r0] = tci_compare64(tci_uint64(regs[r2], regs[r1]),
                                     tci_uint64(regs[r4], regs[r3]),
                                     condition);
            break;
#elif TCG_TARGET_REG_BITS == 64
        case INDEX_op_setcond:
            tci_args_rrrc(insn, &r0, &r1, &r2, &condition);
            regs[r0] = tci_compare64(regs[r1], regs[r2], condition);
            break;
        case INDEX_op_movcond:
            tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &condition);
            tmp32 = tci_compare64(regs[r1], regs[r2], condition);
            regs[r0] = regs[tmp32 ? r3 : r4];
            break;
#endif
        case INDEX_op_mov:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = regs[r1];
            break;
        case INDEX_op_tci_movi:
            tci_args_ri(insn, &r0, &t1);
            regs[r0] = t1;
            break;
        case INDEX_op_tci_movl:
            tci_args_rl(insn, tb_ptr, &r0, &ptr);
            regs[r0] = *(tcg_target_ulong *)ptr;
            break;
        case INDEX_op_tci_setcarry:
            carry = true;
            break;

#if defined(CONFIG_TCG_WASM_JIT) && TCG_TARGET_HAS_fpu
            /* Host floating point (see wasm32.h) */
        case INDEX_op_flcr:
            tci_args_r(insn, &r0);
            tcg_wasm_fpcr = regs[r0];
            break;
        case INDEX_op_ld80f_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tcg_wasm_ld80f_f64(regs[r1]);
            break;
        case INDEX_op_ld80f_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tcg_wasm_ld80f_f32(regs[r1]);
            break;
        case INDEX_op_st80f_f64:
            tci_args_rr(insn, &r0, &r1);
            tcg_wasm_st80f_f64(regs[r1], regs[r0]);
            break;
        case INDEX_op_st80f_f32:
            tci_args_rr(insn, &r0, &r1);
            tcg_wasm_st80f_f32(regs[r1], regs[r0]);
            break;
        case INDEX_op_abs_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = regs[r1] & ~(1ull << 63);
            break;
        case INDEX_op_abs_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (uint32_t)regs[r1] & ~(1u << 31);
            break;
        case INDEX_op_chs_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = regs[r1] ^ (1ull << 63);
            break;
        case INDEX_op_chs_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (uint32_t)regs[r1] ^ (1u << 31);
            break;
        case INDEX_op_add_f64:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f64_bits(tci_f64(regs[r1]) + tci_f64(regs[r2]));
            break;
        case INDEX_op_add_f32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f32_bits(tci_f32(regs[r1]) + tci_f32(regs[r2]));
            break;
        case INDEX_op_sub_f64:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f64_bits(tci_f64(regs[r1]) - tci_f64(regs[r2]));
            break;
        case INDEX_op_sub_f32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f32_bits(tci_f32(regs[r1]) - tci_f32(regs[r2]));
            break;
        case INDEX_op_mul_f64:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f64_bits(tci_f64(regs[r1]) * tci_f64(regs[r2]));
            break;
        case INDEX_op_mul_f32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f32_bits(tci_f32(regs[r1]) * tci_f32(regs[r2]));
            break;
        case INDEX_op_div_f64:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f64_bits(tci_f64(regs[r1]) / tci_f64(regs[r2]));
            break;
        case INDEX_op_div_f32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_f32_bits(tci_f32(regs[r1]) / tci_f32(regs[r2]));
            break;
        case INDEX_op_sqrt_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f64_bits(sqrt(tci_f64(regs[r1])));
            break;
        case INDEX_op_sqrt_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f32_bits(sqrtf(tci_f32(regs[r1])));
            break;
        case INDEX_op_sin_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tcg_wasm_sin_f64(regs[r1]);
            break;
        case INDEX_op_sin_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tcg_wasm_sin_f32(regs[r1]);
            break;
        case INDEX_op_cos_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tcg_wasm_cos_f64(regs[r1]);
            break;
        case INDEX_op_cos_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tcg_wasm_cos_f32(regs[r1]);
            break;
        case INDEX_op_com_f64:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_fp_com(tci_f64(regs[r1]), tci_f64(regs[r2]));
            break;
        case INDEX_op_com_f32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = tci_fp_com(tci_f32(regs[r1]), tci_f32(regs[r2]));
            break;
        case INDEX_op_cvt32f_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f64_bits(tci_f32(regs[r1]));
            break;
        case INDEX_op_cvt64f_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f32_bits(tci_f64(regs[r1]));
            break;
        case INDEX_op_cvt32i_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f64_bits((int32_t)regs[r1]);
            break;
        case INDEX_op_cvt32i_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f32_bits((int32_t)regs[r1]);
            break;
        case INDEX_op_cvt64i_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f64_bits((int64_t)regs[r1]);
            break;
        case INDEX_op_cvt64i_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_f32_bits((int64_t)regs[r1]);
            break;
        case INDEX_op_cvt64f_i32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_fp_to_i32(tci_f64(regs[r1]));
            break;
        case INDEX_op_cvt32f_i32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_fp_to_i32(tci_f32(regs[r1]));
            break;
        case INDEX_op_cvt64f_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_fp_to_i64(tci_f64(regs[r1]));
            break;
        case INDEX_op_cvt32f_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = tci_fp_to_i64(tci_f32(regs[r1]));
            break;
        case INDEX_op_mov32f_i32:
        case INDEX_op_mov32i_f32:
        case INDEX_op_mov_f32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (uint32_t)regs[r1];
            break;
        case INDEX_op_mov64f_i64:
        case INDEX_op_mov64i_f64:
        case INDEX_op_mov_f64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = regs[r1];
            break;
#endif

            /* Load/store operations (32 bit). */

        case INDEX_op_ld8u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint8_t *)ptr;
            break;
        case INDEX_op_ld8s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int8_t *)ptr;
            break;
        case INDEX_op_ld16u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint16_t *)ptr;
            break;
        case INDEX_op_ld16s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int16_t *)ptr;
            break;
        case INDEX_op_ld:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(tcg_target_ulong *)ptr;
            break;
        case INDEX_op_st8:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint8_t *)ptr = regs[r0];
            break;
        case INDEX_op_st16:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint16_t *)ptr = regs[r0];
            break;
        case INDEX_op_st:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(tcg_target_ulong *)ptr = regs[r0];
            break;

            /* Arithmetic operations (mixed 32/64 bit). */

        case INDEX_op_add:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] + regs[r2];
            break;
        case INDEX_op_sub:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] - regs[r2];
            break;
        case INDEX_op_mul:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] * regs[r2];
            break;
        case INDEX_op_and:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] & regs[r2];
            break;
        case INDEX_op_or:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] | regs[r2];
            break;
        case INDEX_op_xor:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ^ regs[r2];
            break;
        case INDEX_op_andc:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] & ~regs[r2];
            break;
        case INDEX_op_orc:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] | ~regs[r2];
            break;
        case INDEX_op_eqv:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] ^ regs[r2]);
            break;
        case INDEX_op_nand:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] & regs[r2]);
            break;
        case INDEX_op_nor:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] | regs[r2]);
            break;
        case INDEX_op_neg:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = -regs[r1];
            break;
        case INDEX_op_not:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = ~regs[r1];
            break;
        case INDEX_op_ctpop:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = ctpop_tr(regs[r1]);
            break;
        case INDEX_op_addco:
            tci_args_rrr(insn, &r0, &r1, &r2);
            t1 = regs[r1] + regs[r2];
            carry = t1 < regs[r1];
            regs[r0] = t1;
            break;
        case INDEX_op_addci:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] + regs[r2] + carry;
            break;
        case INDEX_op_addcio:
            tci_args_rrr(insn, &r0, &r1, &r2);
            if (carry) {
                t1 = regs[r1] + regs[r2] + 1;
                carry = t1 <= regs[r1];
            } else {
                t1 = regs[r1] + regs[r2];
                carry = t1 < regs[r1];
            }
            regs[r0] = t1;
            break;
        case INDEX_op_subbo:
            tci_args_rrr(insn, &r0, &r1, &r2);
            carry = regs[r1] < regs[r2];
            regs[r0] = regs[r1] - regs[r2];
            break;
        case INDEX_op_subbi:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] - regs[r2] - carry;
            break;
        case INDEX_op_subbio:
            tci_args_rrr(insn, &r0, &r1, &r2);
            if (carry) {
                carry = regs[r1] <= regs[r2];
                regs[r0] = regs[r1] - regs[r2] - 1;
            } else {
                carry = regs[r1] < regs[r2];
                regs[r0] = regs[r1] - regs[r2];
            }
            break;
        case INDEX_op_muls2:
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
#if TCG_TARGET_REG_BITS == 32
            tmp64 = (int64_t)(int32_t)regs[r2] * (int32_t)regs[r3];
            tci_write_reg64(regs, r1, r0, tmp64);
#else
            muls64(&regs[r0], &regs[r1], regs[r2], regs[r3]);
#endif
            break;
        case INDEX_op_mulu2:
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
#if TCG_TARGET_REG_BITS == 32
            tmp64 = (uint64_t)(uint32_t)regs[r2] * (uint32_t)regs[r3];
            tci_write_reg64(regs, r1, r0, tmp64);
#else
            mulu64(&regs[r0], &regs[r1], regs[r2], regs[r3]);
#endif
            break;

            /* Arithmetic operations (32 bit). */

        case INDEX_op_tci_divs32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int32_t)regs[r1] / (int32_t)regs[r2];
            break;
        case INDEX_op_tci_divu32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint32_t)regs[r1] / (uint32_t)regs[r2];
            break;
        case INDEX_op_tci_rems32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int32_t)regs[r1] % (int32_t)regs[r2];
            break;
        case INDEX_op_tci_remu32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint32_t)regs[r1] % (uint32_t)regs[r2];
            break;
        case INDEX_op_tci_clz32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            tmp32 = regs[r1];
            regs[r0] = tmp32 ? clz32(tmp32) : regs[r2];
            break;
        case INDEX_op_tci_ctz32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            tmp32 = regs[r1];
            regs[r0] = tmp32 ? ctz32(tmp32) : regs[r2];
            break;
        case INDEX_op_tci_setcond32:
            tci_args_rrrc(insn, &r0, &r1, &r2, &condition);
            regs[r0] = tci_compare32(regs[r1], regs[r2], condition);
            break;
        case INDEX_op_tci_movcond32:
            tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &condition);
            tmp32 = tci_compare32(regs[r1], regs[r2], condition);
            regs[r0] = regs[tmp32 ? r3 : r4];
            break;

            /* Shift/rotate operations. */

        case INDEX_op_shl:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] << (regs[r2] % TCG_TARGET_REG_BITS);
            break;
        case INDEX_op_shr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] >> (regs[r2] % TCG_TARGET_REG_BITS);
            break;
        case INDEX_op_sar:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ((tcg_target_long)regs[r1]
                        >> (regs[r2] % TCG_TARGET_REG_BITS));
            break;
        case INDEX_op_tci_rotl32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = rol32(regs[r1], regs[r2] & 31);
            break;
        case INDEX_op_tci_rotr32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ror32(regs[r1], regs[r2] & 31);
            break;
        case INDEX_op_deposit:
            tci_args_rrrbb(insn, &r0, &r1, &r2, &pos, &len);
            regs[r0] = deposit_tr(regs[r1], pos, len, regs[r2]);
            break;
        case INDEX_op_extract:
            tci_args_rrbb(insn, &r0, &r1, &pos, &len);
            regs[r0] = extract_tr(regs[r1], pos, len);
            break;
        case INDEX_op_sextract:
            tci_args_rrbb(insn, &r0, &r1, &pos, &len);
            regs[r0] = sextract_tr(regs[r1], pos, len);
            break;
        case INDEX_op_brcond:
            tci_args_rl(insn, tb_ptr, &r0, &ptr);
            if (regs[r0]) {
                tb_ptr = ptr;
            }
            break;
        case INDEX_op_bswap16:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap16(regs[r1]);
            break;
        case INDEX_op_bswap32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap32(regs[r1]);
            break;
#if TCG_TARGET_REG_BITS == 64
            /* Load/store operations (64 bit). */

        case INDEX_op_ld32u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint32_t *)ptr;
            break;
        case INDEX_op_ld32s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int32_t *)ptr;
            break;
        case INDEX_op_st32:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint32_t *)ptr = regs[r0];
            break;

            /* Arithmetic operations (64 bit). */

        case INDEX_op_divs:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int64_t)regs[r1] / (int64_t)regs[r2];
            break;
        case INDEX_op_divu:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint64_t)regs[r1] / (uint64_t)regs[r2];
            break;
        case INDEX_op_rems:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int64_t)regs[r1] % (int64_t)regs[r2];
            break;
        case INDEX_op_remu:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint64_t)regs[r1] % (uint64_t)regs[r2];
            break;
        case INDEX_op_clz:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ? clz64(regs[r1]) : regs[r2];
            break;
        case INDEX_op_ctz:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ? ctz64(regs[r1]) : regs[r2];
            break;

            /* Shift/rotate operations (64 bit). */

        case INDEX_op_rotl:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = rol64(regs[r1], regs[r2] & 63);
            break;
        case INDEX_op_rotr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ror64(regs[r1], regs[r2] & 63);
            break;
        case INDEX_op_ext_i32_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (int32_t)regs[r1];
            break;
        case INDEX_op_extu_i32_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (uint32_t)regs[r1];
            break;
        case INDEX_op_bswap64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap64(regs[r1]);
            break;
#endif /* TCG_TARGET_REG_BITS == 64 */

            /* QEMU specific operations. */

#ifdef CONFIG_TCG_WASM_JIT
        case INDEX_op_exit_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            wasm_ctx.tb_ptr = NULL;
            return (uintptr_t)ptr;

        case INDEX_op_goto_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            ptr = *(void **)ptr;
            if (ptr != tb_ptr) {
                /* chained: ptr is the next TB's header */
                tb_ptr = wasm32_tci_chain(ptr);
                if (!tb_ptr) {
                    return 0;
                }
                XTBSTAT_INC(n_tb_exec); /* chained block */
            }
            break;

        case INDEX_op_goto_ptr:
            tci_args_r(insn, &r0);
            ptr = (void *)(uintptr_t)regs[r0];
            if (!ptr) {
                wasm_ctx.tb_ptr = NULL;
                return 0;
            }
            tb_ptr = wasm32_tci_chain(ptr);
            if (!tb_ptr) {
                return 0;
            }
            XTBSTAT_INC(n_tb_exec); /* lookup-and-goto block */
            break;
#else
        case INDEX_op_exit_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            return (uintptr_t)ptr;

        case INDEX_op_goto_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            tb_ptr = *(void **)ptr;
            XSTAT_INC(n_tb_exec); /* chained block */
            break;

        case INDEX_op_goto_ptr:
            tci_args_r(insn, &r0);
            ptr = (void *)regs[r0];
            XSTAT_INC(n_tb_exec); /* lookup-and-goto block */
            if (!ptr) {
                return 0;
            }
            tb_ptr = ptr;
            break;
#endif

        case INDEX_op_qemu_ld:
            tci_args_rrm(insn, &r0, &r1, &oi);
            taddr = regs[r1];
            regs[r0] = tci_qemu_ld(env, taddr, oi, tb_ptr);
            break;
        case INDEX_op_tci_qemu_ld_rrr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            taddr = regs[r1];
            oi = regs[r2];
            regs[r0] = tci_qemu_ld(env, taddr, oi, tb_ptr);
            break;

        case INDEX_op_qemu_st:
            tci_args_rrm(insn, &r0, &r1, &oi);
            taddr = regs[r1];
            tci_qemu_st(env, taddr, regs[r0], oi, tb_ptr);
            break;
        case INDEX_op_tci_qemu_st_rrr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            taddr = regs[r1];
            oi = regs[r2];
            tci_qemu_st(env, taddr, regs[r0], oi, tb_ptr);
            break;

        case INDEX_op_qemu_ld2:
            tcg_debug_assert(TCG_TARGET_REG_BITS == 32);
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
            taddr = regs[r2];
            oi = regs[r3];
            tmp64 = tci_qemu_ld(env, taddr, oi, tb_ptr);
            tci_write_reg64(regs, r1, r0, tmp64);
            break;

        case INDEX_op_qemu_st2:
            tcg_debug_assert(TCG_TARGET_REG_BITS == 32);
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
            tmp64 = tci_uint64(regs[r1], regs[r0]);
            taddr = regs[r2];
            oi = regs[r3];
            tci_qemu_st(env, taddr, tmp64, oi, tb_ptr);
            break;

        case INDEX_op_mb:
            /* Ensure ordering for all kinds */
            smp_mb();
            break;
        default:
            g_assert_not_reached();
        }
    }
}

/*
 * Disassembler that matches the interpreter
 */

static const char *str_r(TCGReg r)
{
    static const char regs[TCG_TARGET_NB_REGS][4] = {
        "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "env", "sp"
    };

    QEMU_BUILD_BUG_ON(TCG_AREG0 != TCG_REG_R14);
    QEMU_BUILD_BUG_ON(TCG_REG_CALL_STACK != TCG_REG_R15);

    assert((unsigned)r < TCG_TARGET_NB_REGS);
    return regs[r];
}

static const char *str_c(TCGCond c)
{
    static const char cond[16][8] = {
        [TCG_COND_NEVER] = "never",
        [TCG_COND_ALWAYS] = "always",
        [TCG_COND_EQ] = "eq",
        [TCG_COND_NE] = "ne",
        [TCG_COND_LT] = "lt",
        [TCG_COND_GE] = "ge",
        [TCG_COND_LE] = "le",
        [TCG_COND_GT] = "gt",
        [TCG_COND_LTU] = "ltu",
        [TCG_COND_GEU] = "geu",
        [TCG_COND_LEU] = "leu",
        [TCG_COND_GTU] = "gtu",
        [TCG_COND_TSTEQ] = "tsteq",
        [TCG_COND_TSTNE] = "tstne",
    };

    assert((unsigned)c < ARRAY_SIZE(cond));
    assert(cond[c][0] != 0);
    return cond[c];
}

/* Disassemble TCI bytecode. */
int print_insn_tci(bfd_vma addr, disassemble_info *info)
{
    const uint32_t *tb_ptr = (const void *)(uintptr_t)addr;
    const TCGOpDef *def;
    const char *op_name;
    uint32_t insn;
    TCGOpcode op;
    TCGReg r0, r1, r2, r3, r4;
    tcg_target_ulong i1;
    int32_t s2;
    TCGCond c;
    MemOpIdx oi;
    uint8_t pos, len;
    void *ptr;

    /* TCI is always the host, so we don't need to load indirect. */
    insn = *tb_ptr++;

    info->fprintf_func(info->stream, "%08x  ", insn);

    op = extract32(insn, 0, 8);
    def = &tcg_op_defs[op];
    op_name = def->name;

    switch (op) {
#ifdef CONFIG_TCG_WASM_JIT
    case INDEX_op_wasm_census:
        info->fprintf_func(info->stream, "%-12s  flags=0x%x insns=%u",
                           op_name, (insn >> 8) & 255, insn >> 16);
        break;
#endif
    case INDEX_op_br:
    case INDEX_op_exit_tb:
    case INDEX_op_goto_tb:
        tci_args_l(insn, tb_ptr, &ptr);
        info->fprintf_func(info->stream, "%-12s  %p", op_name, ptr);
        break;

    case INDEX_op_goto_ptr:
        tci_args_r(insn, &r0);
        info->fprintf_func(info->stream, "%-12s  %s", op_name, str_r(r0));
        break;

    case INDEX_op_call:
        tci_args_nl(insn, tb_ptr, &len, &ptr);
        info->fprintf_func(info->stream, "%-12s  %d, %p", op_name, len, ptr);
        break;

    case INDEX_op_brcond:
        tci_args_rl(insn, tb_ptr, &r0, &ptr);
        info->fprintf_func(info->stream, "%-12s  %s, 0, ne, %p",
                           op_name, str_r(r0), ptr);
        break;

    case INDEX_op_setcond:
    case INDEX_op_tci_setcond32:
        tci_args_rrrc(insn, &r0, &r1, &r2, &c);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2), str_c(c));
        break;

    case INDEX_op_tci_movi:
        tci_args_ri(insn, &r0, &i1);
        info->fprintf_func(info->stream, "%-12s  %s, 0x%" TCG_PRIlx,
                           op_name, str_r(r0), i1);
        break;

    case INDEX_op_tci_movl:
        tci_args_rl(insn, tb_ptr, &r0, &ptr);
        info->fprintf_func(info->stream, "%-12s  %s, %p",
                           op_name, str_r(r0), ptr);
        break;

    case INDEX_op_tci_setcarry:
        info->fprintf_func(info->stream, "%-12s", op_name);
        break;

    case INDEX_op_ld8u:
    case INDEX_op_ld8s:
    case INDEX_op_ld16u:
    case INDEX_op_ld16s:
    case INDEX_op_ld32u:
    case INDEX_op_ld:
    case INDEX_op_st8:
    case INDEX_op_st16:
    case INDEX_op_st32:
    case INDEX_op_st:
        tci_args_rrs(insn, &r0, &r1, &s2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %d",
                           op_name, str_r(r0), str_r(r1), s2);
        break;

    case INDEX_op_bswap16:
    case INDEX_op_bswap32:
    case INDEX_op_ctpop:
    case INDEX_op_mov:
    case INDEX_op_neg:
    case INDEX_op_not:
    case INDEX_op_ext_i32_i64:
    case INDEX_op_extu_i32_i64:
    case INDEX_op_bswap64:
        tci_args_rr(insn, &r0, &r1);
        info->fprintf_func(info->stream, "%-12s  %s, %s",
                           op_name, str_r(r0), str_r(r1));
        break;

    case INDEX_op_add:
    case INDEX_op_addci:
    case INDEX_op_addcio:
    case INDEX_op_addco:
    case INDEX_op_and:
    case INDEX_op_andc:
    case INDEX_op_clz:
    case INDEX_op_ctz:
    case INDEX_op_divs:
    case INDEX_op_divu:
    case INDEX_op_eqv:
    case INDEX_op_mul:
    case INDEX_op_nand:
    case INDEX_op_nor:
    case INDEX_op_or:
    case INDEX_op_orc:
    case INDEX_op_rems:
    case INDEX_op_remu:
    case INDEX_op_rotl:
    case INDEX_op_rotr:
    case INDEX_op_sar:
    case INDEX_op_shl:
    case INDEX_op_shr:
    case INDEX_op_sub:
    case INDEX_op_subbi:
    case INDEX_op_subbio:
    case INDEX_op_subbo:
    case INDEX_op_xor:
    case INDEX_op_tci_ctz32:
    case INDEX_op_tci_clz32:
    case INDEX_op_tci_divs32:
    case INDEX_op_tci_divu32:
    case INDEX_op_tci_rems32:
    case INDEX_op_tci_remu32:
    case INDEX_op_tci_rotl32:
    case INDEX_op_tci_rotr32:
        tci_args_rrr(insn, &r0, &r1, &r2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2));
        break;

    case INDEX_op_deposit:
        tci_args_rrrbb(insn, &r0, &r1, &r2, &pos, &len);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %d, %d",
                           op_name, str_r(r0), str_r(r1), str_r(r2), pos, len);
        break;

    case INDEX_op_extract:
    case INDEX_op_sextract:
        tci_args_rrbb(insn, &r0, &r1, &pos, &len);
        info->fprintf_func(info->stream, "%-12s  %s,%s,%d,%d",
                           op_name, str_r(r0), str_r(r1), pos, len);
        break;

    case INDEX_op_tci_movcond32:
    case INDEX_op_movcond:
    case INDEX_op_setcond2_i32:
        tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &c);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2),
                           str_r(r3), str_r(r4), str_c(c));
        break;

    case INDEX_op_muls2:
    case INDEX_op_mulu2:
        tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1),
                           str_r(r2), str_r(r3));
        break;

    case INDEX_op_qemu_ld:
    case INDEX_op_qemu_st:
        tci_args_rrm(insn, &r0, &r1, &oi);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %x",
                           op_name, str_r(r0), str_r(r1), oi);
        break;

    case INDEX_op_tci_qemu_ld_rrr:
    case INDEX_op_tci_qemu_st_rrr:
        tci_args_rrr(insn, &r0, &r1, &r2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2));
        break;

    case INDEX_op_qemu_ld2:
    case INDEX_op_qemu_st2:
        tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1),
                           str_r(r2), str_r(r3));
        break;

    case 0:
        /* tcg_out_nop_fill uses zeros */
        if (insn == 0) {
            info->fprintf_func(info->stream, "align");
            break;
        }
        /* fall through */

    default:
        info->fprintf_func(info->stream, "illegal opcode %d", op);
        break;
    }

    return sizeof(insn);
}
