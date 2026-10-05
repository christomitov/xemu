/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Memory integration for the hit-or-canonical-exit stage. */
static unsigned xwr_mem_offset(XwdRegionMember *m, unsigned insn)
{
    return insn ? m->t.capture.end[insn - 1] : 0;
}

static bool xwr_mem_resolve(XwdEmitter *e, unsigned insn, unsigned access,
                            bool store, unsigned width, XwdMemAccess *out)
{
    XwdRegionMember *m = e->mem->opaque;
    if (access >= m->memory.count) {
        return false;
    }
    XwdMemorySite *s = &m->memory.site[access];
    if (s->insn != insn || s->store != store ||
        (8u << ((s->oi >> 5) & 7)) != width) {
        return false;
    }
    out->oi = s->oi;
    out->mask_ofs = s->mask_ofs;
    out->table_ofs = s->table_ofs;
    out->cmp_ofs = s->cmp_ofs;
    out->addend_ofs = s->addend_ofs;
    out->index_shift = s->index_shift;
    out->fold_shift = 12;
    out->fold_mask = 0xff000;
    out->page_mask = s->page_mask;
    out->a_mask = s->a_mask;
    out->s_mask = s->s_mask;
    out->opaque = s;
    return true;
}

static bool xwr_mem_resume(XwdEmitter *e, const XwdMemAccess *a)
{
    /* No active helper/suspension. The first candidate exits on every miss. */
    return true;
}

static bool xwr_mem_miss(XwdEmitter *e, const XwdMemAccess *a,
                         unsigned addr_local, unsigned value_local)
{
    XwdRegionMember *m = e->mem->opaque;
    if (xemu_wasm_direct_profile_enabled()) {
        /* Ends at dispatcher return. Original retry/TB lookup is outside
         * this scope; the sample count is NOT total miss-associated cost.
         */
        xwr_phase(e, (uintptr_t)"direct:miss");
    }
    if (e->flags_dirty) {
        /* bank_step normally publishes this opcode at the end of the TB. */
        xwd_const(e, e->cc);
        xwd_local(e, 0x21, XD_CC_OP);
    }
    xwd_bank_store(e, false, 0);
    xwd_local(e, 0x20, XD_ENV);
    xwd_local(e, 0x20, XD_PC);
    xwd_const(e, xwr_mem_offset(m, a->insn));
    xwd_byte(e, 0x6a);
    xwd_mem(e, 0x36, e->layout->eip);
    xwr_ctx_const(e, WASM_CTX_TB_PTR_OFF, 0);
    xwd_local(e, 0x20, XD_ENV);
    if (m->region->miss_tag) {
        xwd_local(e, 0x20, addr_local);
        xwd_local(e, 0x20, m->region->miss_tag);
        xwd_const(e, a->page_mask);
        xwd_const(e, a->a_mask);
        xwd_const(e, a->store);
    }
    xwd_byte(e, 0x10); xwd_uleb(e, m->region->mem_miss);
    xwd_const(e, 0); xwd_byte(e, 0x0f);
    return e->code->valid;
}

static bool xwr_mem_shadow(XwdEmitter *e, const XwdMemAccess *a,
                           unsigned addr_local, unsigned value_local)
{
    XwdRegionMember *m = e->mem->opaque;
    xwd_local(e, 0x20, XD_ENV);
    xwd_local(e, 0x20, XD_PC);
    xwd_const(e, xwr_mem_offset(m, a->insn));
    xwd_byte(e, 0x6a);
    xwd_local(e, 0x20, addr_local);
    xwd_const(e, a->oi);
    if (a->store) {
        xwd_local(e, 0x20, value_local);
    }
    xwd_byte(e, 0x10);
    xwd_uleb(e, a->store ? m->region->mem_store : m->region->mem_load);
    if (!a->store) {
        xwd_local(e, 0x21, value_local);
    }
    return e->code->valid;
}

static bool xwr_observation(ByteBuf *b, XwdRegion *r, unsigned member,
                            unsigned access, unsigned kind)
{
    XwdRegionMember *m = &r->member[member];
    XwdCode code;
    XwdEmitter e = xwr_emitter(&code, r, member);
    if (access >= m->memory.count) {
        return false;
    }
    const XwdMemorySite *s = &m->memory.site[access];
    if (kind == WASM_RELOC_MEM_RESUME) {
        /* Slow entry: original address still exists even if the load's
         * output aliases it. No snapshot/replay of a completed fast load.
         */
        xwr_raw_local(&e, 0x20, REGION_L32_0 + 1);
        xwd_byte(&e, 0x45);
        xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
    }
    if (kind == WASM_RELOC_MEM_BEGIN || kind == WASM_RELOC_MEM_RESUME) {
        xwr_raw_local(&e, 0x20, 8 + s->addr_reg);
        xwd_byte(&e, 0xa7);
        xwd_local(&e, 0x21, r->address);
        if (kind == WASM_RELOC_MEM_RESUME) {
            xwd_byte(&e, 0x0b);
        }
    } else {
        xwd_local(&e, 0x20, XD_ENV);
        xwd_const(&e, xwr_mem_offset(m, s->insn));
        xwd_local(&e, 0x20, r->address);
        xwd_const(&e, s->oi);
        xwd_const(&e, s->store);
        xwr_raw_local(&e, 0x20, 8 + s->data_reg);
        xwd_byte(&e, 0x10); xwd_uleb(&e, r->mem_observe);
    }
    return xwr_flush(b, &e);
}
