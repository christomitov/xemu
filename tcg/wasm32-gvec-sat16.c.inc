/*
 * Opt-in packed signed-word saturation in the ordinary TCG helper path.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "tcg/tcg-gvec-desc.h"

/* L64_0 holds the descriptor with operation-specific data removed. */
static void wasm_gvec_sat16_size(TCGContext *s, unsigned bytes)
{
    unsigned max = bytes / 8 - 1;

    /* Both encodings of oprsz == maxsz: explicit size, or the maxsz tag. */
    wasm_var(s, W_LOCAL_GET, L64_0);
    wasm8(s, W_I32_WRAP_I64);
    wasm_i32_const(s, max | (max << SIMD_OPRSZ_SHIFT));
    wasm8(s, W_I32_EQ);
    wasm_var(s, W_LOCAL_GET, L64_0);
    wasm8(s, W_I32_WRAP_I64);
    wasm_i32_const(s, max | (2 << SIMD_OPRSZ_SHIFT));
    wasm8(s, W_I32_EQ);
    wasm8(s, W_I32_OR);
}

static void wasm_gvec_sat16_body(TCGContext *s, unsigned bytes, bool sub)
{
    /*
     * Equally sized, naturally aligned spans are identical or disjoint.
     * This cheap conservative guard rejects every partial output overlap;
     * unaligned (even otherwise safe) operands retain the original helper.
     */
    wasm_var(s, W_LOCAL_GET, L32_0);       /* d */
    wasm_var(s, W_LOCAL_GET, L32_2);       /* a */
    wasm8(s, W_I32_OR);
    wasm_var(s, W_LOCAL_GET, L32_3);       /* b */
    wasm8(s, W_I32_OR);
    wasm_i32_const(s, bytes - 1);
    wasm8(s, W_I32_AND);
    wasm8(s, W_I32_EQZ);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);

    /* No v128 locals or ABI changes: load both inputs before any store. */
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm8(s, 0xfd);                      /* SIMD prefix */
    wasm_mem(s, bytes == 8 ? 0x5d : 0x00, 0); /* load64_zero / v128.load */
    wasm_var(s, W_LOCAL_GET, L32_3);
    wasm8(s, 0xfd);
    wasm_mem(s, bytes == 8 ? 0x5d : 0x00, 0);
    wasm8(s, 0xfd);
    wasm_uleb(s, sub ? 0x92 : 0x8f);     /* i16x8.sub_sat_s / add_sat_s */
    wasm8(s, 0xfd);
    wasm_mem(s, bytes == 8 ? 0x5b : 0x0b, 0); /* store64_lane / v128.store */
    if (bytes == 8) {
        wasm8(s, 0);                    /* lane zero, exactly eight bytes */
    }
    wasm_i32_const(s, 1);
    wasm_var(s, W_LOCAL_SET, L32_1);      /* done */
    wasm8(s, W_END);
}

/*
 * Emit a fresh-entry attempt, before the original helper's resumable block.
 * L32_1 is zero on decline and in a new rewind invocation, so the unchanged
 * helper path still handles all other descriptors, overlaps, and rewinds.
 */
static bool wasm_gvec_sat16_inline(TCGContext *s, const TCGHelperInfo *info)
{
    static int enabled = -1;
    bool sub;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_GVEC_SSAT16");
        enabled = e && *e == '1';
    }
    if (!enabled || !info->name) {
        return false;
    }
    if (!strcmp(info->name, "gvec_ssadd16")) {
        sub = false;
    } else if (!strcmp(info->name, "gvec_sssub16")) {
        sub = true;
    } else {
        return false;
    }
    if (helper_nargs(info->typemask) != 4 ||
        (info->typemask & 7) != dh_typecode_void ||
        extract32(info->typemask, 3, 3) != dh_typecode_ptr ||
        extract32(info->typemask, 6, 3) != dh_typecode_ptr ||
        extract32(info->typemask, 9, 3) != dh_typecode_ptr ||
        extract32(info->typemask, 12, 3) != dh_typecode_i32) {
        return false;
    }

    wasm_jit_phase(s, 1, "jit:gvec");
    wasm_i32_const(s, 0);
    wasm_var(s, W_LOCAL_SET, L32_1);
    for (unsigned i = 0; i < 4; i++) {
        static const unsigned locals[] = { L32_0, L32_2, L32_3 };

        wasm_get_r32(s, TCG_REG_CALL_STACK);
        wasm_mem(s, W_I32_LOAD, TCG_TARGET_CALL_STACK_OFFSET + i * 8);
        if (i < 3) {
            wasm_var(s, W_LOCAL_SET, locals[i]);
        } else {
            wasm_i32_const(s, (1u << SIMD_DATA_SHIFT) - 1);
            wasm8(s, W_I32_AND);
            wasm8(s, W_I64_EXTEND_I32_U);
            wasm_var(s, W_LOCAL_SET, L64_0);
        }
    }
    wasm_gvec_sat16_size(s, 8);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
    wasm_gvec_sat16_body(s, 8, sub);
    wasm8(s, W_ELSE);
    wasm_gvec_sat16_size(s, 16);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
    wasm_gvec_sat16_body(s, 16, sub);
    wasm8(s, W_END);
    wasm8(s, W_END);
    wasm_jit_phase(s, 1, NULL);
    return true;
}
