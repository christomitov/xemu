/* SPDX-License-Identifier: GPL-2.0-or-later */
/* This stage deliberately does not fuse direct/legacy register layouts. */
#include "wasm32-direct.c.inc"
#include "wasm32-direct-blueprint.c.inc"

static void wasm_direct_verify(TCGContext *s, int slot)
{
    int idx;
    if (wasm_direct_blueprint) {
        /*
         * Original TCG is still authoritative; a region builder may inject
         * a differential check at this independently selected exit later.
         */
        wasm_add_reloc(WASM_RELOC_DIRECT_VERIFY, slot < 0 ? 0xff : slot, 0);
        return;
    }
    if (!wasm_direct_translation || xemu_wasm_direct_mode() != 2) {
        return;
    }
    const void *fn = wasm_direct_translation->layout.verify;
    idx = get_helper_idx(fn);
    if (idx < 0) {
        idx = register_helper(fn);
        types_out8(0x60);
        types_out8(4);
        for (int i = 0; i < 4; i++) {
            types_out8(W_TYPE_I32);
        }
        types_out8(0);
    }
    wasm_get_r32(s, TCG_AREG0);
    wasm_i32_const(s, (uintptr_t)&wasm_direct_expected);
    wasm_i32_const(s, 1); /* Every completed path now checks its chosen PC. */
    if (slot >= 0 && !wasm_direct_translation->capture.pcrel) {
        tcg_debug_assert(wasm_direct_translation->capture.jump_valid &
                         (1u << slot));
        wasm_i32_const(s, wasm_direct_translation->capture.jump_pc[slot]);
    } else {
        wasm_get_r32(s, TCG_AREG0);
        wasm_mem(s, W_I32_LOAD, wasm_direct_translation->layout.eip);
    }
    wasm_phase(s, "direct:verify");
    /* Pure register/flag comparison; no guest effects or successful yield. */
    wasm_call_idx(s, idx);
    wasm_phase(s, NULL);
}

static void wasm_direct_entry(TCGContext *s, const XwdTranslation *t)
{
    XwdPlan plan;
    XwdLayout layout = t->layout;
    XwdCode *code;
    int mode = xemu_wasm_direct_mode();
    uint32_t cc_idx = UINT32_MAX;

    if (!mode || !xwd_decode(&t->capture, &plan) ||
        (plan.needs_flags && !xemu_wasm_direct_flags_enabled())) {
        XSTAT_INC(n_direct_decline);
        return;
    }
#ifdef EMSCRIPTEN
    if (xemu_wasm_profile_enabled()) {
        layout.phase_ptr = (uintptr_t)&xemu_wasm_phase[XPHASE_VCPU];
        layout.phase_cc = (uintptr_t)"helper:direct-cc";
        layout.phase_generated = (uintptr_t)wasm_insn_phase_name(NULL);
    }
#endif
    if (xemu_wasm_direct_regions_enabled()) {
        XwdTranslation value = *t;
        value.layout = layout;
        code = tcg_malloc(sizeof(*code));
        if (!xwd_blueprint_write(&value, code)) {
            XSTAT_INC(n_direct_decline);
            return;
        }
        wasm_direct_blueprint = code;
        wasm_add_reloc(WASM_RELOC_DIRECT_BEGIN, 0, 0);
        /*
         * No private function/imports/counters: retain original TCG A/B for
         * singletons and for the entire group if substitution declines.
         */
        return;
    }
    if (plan.needs_flags) {
        int idx = get_helper_idx(t->layout.cc_all);
        if (idx < 0) {
            idx = register_helper(t->layout.cc_all);
            types_out8(0x60);
            types_out8(4);
            for (int i = 0; i < 4; i++) {
                types_out8(W_TYPE_I32);
            }
            types_out8(1);
            types_out8(W_TYPE_I32);
        }
        cc_idx = idx;
    }
    code = tcg_malloc(sizeof(*code));
    if (!xwd_emit(&plan, &layout, mode == 2,
                  (uintptr_t)&wasm_direct_expected, cc_idx, code)) {
        XSTAT_INC(n_direct_decline);
        return;
    }
    wasm_direct_translation = t;
    wasm_direct_code = code;
    XSTAT_INC(n_direct_build);
    if (plan.needs_flags) {
        XSTAT_INC(n_direct_flags_build);
    }
    if (plan.conditional) {
        XSTAT_INC(n_direct_jcc_build);
    }
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
