/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Canonical boundaries inside the ordinary selected region, not a new ABI. */

static void xwr_mixed_load(XwdEmitter *e, XwdRegion *r)
{
    if (!r->mixed) {
        return;
    }
    xwd_local(e, 0x20, r->bank_live);
    xwd_byte(e, 0x45);
    xwd_byte(e, 0x04); xwd_byte(e, 0x40);
    if (r->mode == 1 && xemu_wasm_direct_profile_enabled()) {
        xwr_phase(e, (uintptr_t)"direct:reload");
    }
    xwd_bank_init(e);
    xwd_const(e, 1); xwd_local(e, 0x21, r->bank_live);
    xwd_byte(e, 0x0b);
}

/* Called only on root do_init=0, before interpreting the member index. */
static void xwr_mixed_rewind(XwdEmitter *e, XwdRegion *r)
{
    if (r->mode == 2 && r->memory_members) {
        xwd_byte(e, 0x10); xwd_uleb(e, r->mem_cancel);
    }
    if (wasm32_ic_enabled()) {
        /*
         * The interrupted header may belong to a DIFFERENT module reached
         * by a legacy IC tail call. Its member index could name a direct
         * member here! Preserve the original owner forwarding before the
         * br_table, not in a possibly replaced member's A prologue.
         */
        xwd_local(e, 0x20, 0);
        xwd_mem(e, 0x28, WASM_CTX_REWIND_FUNC_OFF);
        xwr_raw_local(e, 0x23, WASM_IC_SELF_GLOBAL);
        xwd_byte(e, 0x47);
        xwd_byte(e, 0x04); xwd_byte(e, 0x40);
        xwd_local(e, 0x20, 0);
        xwd_local(e, 0x20, 0);
        xwd_mem(e, 0x28, WASM_CTX_REWIND_FUNC_OFF);
        /* Existing chain.call import, exactly as in the original prologue. */
        xwd_byte(e, 0x10); xwd_uleb(e, REGION_HELPER_START - 1);
        xwd_byte(e, 0x0f); xwd_byte(e, 0x0b);
    }
    uint32_t mask = 0;
    for (unsigned i = 0; i < r->n; i++) {
        mask |= (uint32_t)r->member[i].direct << i;
    }
    xwd_const(e, mask); xwr_raw_local(e, 0x20, REGION_CUR_LOCAL);
    xwd_byte(e, 0x76); xwd_const(e, 1); xwd_byte(e, 0x71);
    xwd_byte(e, 0x45);
    xwd_byte(e, 0x04); xwd_byte(e, 0x40);
    /* Adapter entries, NOT completed B work or after-effect replay proof. */
    uint32_t counter = (uintptr_t)&xemu_wasm_stats.n_direct_mixed_legacy_rewind;
    xwd_const(e, counter); xwd_const(e, counter);
    xwd_byte(e, 0x29); xwd_byte(e, 3); xwd_byte(e, 0);
    xwd_byte(e, 0x42); xwd_byte(e, 1); xwd_byte(e, 0x7c);
    xwd_byte(e, 0x37); xwd_byte(e, 3); xwd_byte(e, 0);
    if (r->mode == 1) {
        /* An owned active direct member cannot be the suspension point. */
        xwd_byte(e, 0x05); xwd_byte(e, 0x00);
    }
    xwd_byte(e, 0x0b);
}

static bool xwr_legacy_member(ByteBuf *b, XwdRegion *r)
{
    XwdCode code;
    XwdEmitter e = xwr_emitter(&code, r, r->anchor);

    assert(r->mixed);
    if (r->mode == 1) {
        xwd_local(&e, 0x20, r->bank_live);
        xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
        if (xemu_wasm_direct_profile_enabled()) {
            xwr_phase(&e, (uintptr_t)"direct:spill");
        }
        /*
         * Per-member dirtiness forgets earlier writes in a direct run.
         * Publish the FULL initialized packet, including runtime CC_OP/PC.
         */
        xwd_bank_store(&e, false, 0);
        xwd_byte(&e, 0x0b);
    }
    /*
     * Reference mode never publishes expected state into architectural env.
     * A legacy member may fault, exit or suspend: nothing is bank-resident
     * across it. Its unmodified prologue/IRQ poll/A/B bodies stay authoritative.
     */
    xwd_const(&e, 0); xwd_local(&e, 0x21, r->bank_live);
    xwd_const(&e, 0); xwd_local(&e, 0x21, r->pending);
    return xwr_flush(b, &e);
}
