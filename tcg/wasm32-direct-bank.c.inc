/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Architectural-local bank for homogeneous direct regions. Include after
 * wasm32-direct.c.inc. This is an emitter primitive, NOT a region builder:
 * its caller must implement member selection, live links, entry IRQ guards,
 * boundary publication, and (for mode 2) the single original TCG execution.
 *
 * Every member starts with all architectural locals valid. No compile-time
 * dirty/cache information flows between independently entered member bodies.
 * The runtime lazy-CC opcode is a local too: a join may have any predecessor.
 * No guest data memory or suspending helper is supported by this bank yet.
 */

static void xwd_bank_init(XwdEmitter *e)
{
    const XwdLayout *l = e->layout;
    const unsigned off[] = { l->cc_dst, l->cc_src, l->cc_src2, l->cc_op,
                             l->eip };
    const unsigned reg[] = { XD_DST, XD_SRC, XD_SRC2, XD_CC_OP, XD_PC };

    e->resident = true;
    e->valid = e->dirty = 0;
    e->flags_dirty = e->flags_cached = e->src2_dirty = false;
    xwd_local(e, 0x20, 0);
    xwd_mem(e, 0x28, 0); /* unchanged WasmContext.env */
    xwd_local(e, 0x21, XD_ENV);
    for (unsigned i = 0; i < 8; i++) {
        xwd_load_reg(e, i);
    }
    for (unsigned i = 0; i < 5; i++) {
        xwd_local(e, 0x20, XD_ENV);
        xwd_mem(e, 0x28, off[i]);
        xwd_local(e, 0x21, reg[i]);
    }
}

/* Emit one member, without loading or spilling architectural state. */
static bool xwd_bank_step(XwdEmitter *e, const XwdPlan *p, bool verify)
{
    if (!e->resident || e->valid != 0xff) {
        return false;
    }
    e->dirty = 0;
    e->flags_dirty = e->flags_cached = e->src2_dirty = false;
    if (!xwd_emit_insns(e, p, verify)) {
        return false;
    }
    if (e->flags_dirty) {
        xwd_const(e, e->cc);
        xwd_local(e, 0x21, XD_CC_OP);
    }
    if (p->pcrel) {
        xwd_local(e, 0x20, XD_PC);
    } else {
        xwd_const(e, p->pc);
    }
    if (p->conditional) {
        xwd_local(e, 0x20, XD_DELTA);
    } else {
        xwd_const(e, p->delta);
    }
    xwd_byte(e, 0x6a);
    xwd_local(e, 0x21, XD_PC);
    return e->code->valid;
}

/*
 * At a boundary, publish the whole initialized packet. In mode 2 only the
 * expected scratch is written; the original execution owns architectural env.
 * A caller's IRQ exit must NOT mark an unexecuted member as pending/checked.
 */
static void xwd_bank_store(XwdEmitter *e, bool verify, uint32_t scratch)
{
    const XwdLayout *l = e->layout;
    const unsigned off[] = { l->cc_dst, l->cc_src, l->cc_src2, l->cc_op,
                             l->eip };
    const unsigned check[] = { offsetof(XwdState, cc_dst),
        offsetof(XwdState, cc_src), offsetof(XwdState, cc_src2),
        offsetof(XwdState, cc_op), offsetof(XwdState, eip) };
    const unsigned reg[] = { XD_DST, XD_SRC, XD_SRC2, XD_CC_OP, XD_PC };

    for (unsigned i = 0; i < 8; i++) {
        xwd_address(e, verify, scratch);
        xwd_local(e, 0x20, XD_REG + i);
        xwd_mem(e, 0x36, verify ? offsetof(XwdState, regs) + i * 4 :
                                l->regs + i * 4);
    }
    for (unsigned i = 0; i < 5; i++) {
        xwd_address(e, verify, scratch);
        xwd_local(e, 0x20, reg[i]);
        xwd_mem(e, 0x36, verify ? check[i] : off[i]);
    }
}
