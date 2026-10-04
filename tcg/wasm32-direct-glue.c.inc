/* SPDX-License-Identifier: GPL-2.0-or-later */
/* This stage deliberately does not fuse direct/legacy register layouts. */
#include "wasm32-direct.c.inc"

static void wasm_direct_verify(TCGContext *s, bool pc)
{
    int idx;
    if (!wasm_direct_translation || xemu_wasm_direct_mode() != 2) {
        return;
    }
    const void *fn = wasm_direct_translation->layout.verify;
    idx = get_helper_idx(fn);
    if (idx < 0) {
        idx = register_helper(fn);
        types_out8(0x60);
        types_out8(3);
        types_out8(W_TYPE_I32); types_out8(W_TYPE_I32); types_out8(W_TYPE_I32);
        types_out8(0);
    }
    wasm_get_r32(s, TCG_AREG0);
    wasm_i32_const(s, (uintptr_t)&wasm_direct_expected);
    wasm_i32_const(s, pc);
    wasm_phase(s, "direct:verify");
    /* Pure register/flag comparison; no guest effects or successful yield. */
    wasm_call_idx(s, idx);
    wasm_phase(s, NULL);
}

static void wasm_direct_entry(TCGContext *s, const XwdTranslation *t)
{
    XwdPlan plan;
    XwdCode *code;
    int mode = xemu_wasm_direct_mode();

    if (!mode || !xwd_decode(&t->capture, &plan)) {
        XSTAT_INC(n_direct_decline);
        return;
    }
    code = tcg_malloc(sizeof(*code));
    if (!xwd_emit(&plan, &t->layout, mode == 2,
                  (uintptr_t)&wasm_direct_expected, code)) {
        XSTAT_INC(n_direct_decline);
        return;
    }
    wasm_direct_translation = t;
    wasm_direct_code = code;
    XSTAT_INC(n_direct_build);
#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_direct_exec);
#endif
    wasm_var(s, W_LOCAL_GET, CTX_IDX);
    wasm8(s, W_CALL);
    /* Internal direct function index is known only after legacy imports. */
    wasm_direct_call_pos = wasm_code_len();
    for (int i = 0; i < 5; i++) {
        wasm8(s, i < 4 ? 0x80 : 0);
    }
    wasm8(s, 0x1a); /* drop internal function result */
    if (mode == 1) {
        /* Canonical architectural state. No chain/fusion speedup claimed. */
        wasm_exit_tb(s, 0);
    }
    /* Mode 2 leaves guest state untouched; original TCG executes it once. */
}
