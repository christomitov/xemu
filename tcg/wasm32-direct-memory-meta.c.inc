/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Record ORIGINAL instruction identity, operands and live-TLB layout. This
 * enables a later admission check; it does not make a memory TB executable
 * by the direct emitter. No guest reads, extra fetches or runtime calls.
 */
static void wasm_direct_memory_start(const XwdTranslation *t)
{
    if (xemu_wasm_direct_regions_enabled() &&
        xemu_wasm_direct_memory_enabled() && t->capture.valid) {
        wasm_direct_memory = tcg_malloc(sizeof(*wasm_direct_memory));
        *wasm_direct_memory = (XwdMemoryCapture){
            .insn = UINT32_MAX, .valid = true,
        };
    }
}

static void wasm_direct_memory_insn(unsigned index, const uint64_t *data)
{
    XwdMemoryCapture *c = wasm_direct_memory;
    if (!c || !c->valid) {
        return;
    }
    if (index >= XWD_MAX_INSNS || data[0] > UINT32_MAX) {
        c->valid = false;
        return;
    }
    c->insn = index;
    c->restore_pc = data[0];
}

static void wasm_direct_memory_site(TCGContext *s, TCGReg data, TCGReg addr,
                                    MemOpIdx oi, bool store)
{
    XwdMemoryCapture *c = wasm_direct_memory;
    if (!c || !c->valid) {
        return;
    }
    if (c->insn == UINT32_MAX || c->count == XWD_MAX_MEMOPS ||
        (get_memop(oi) & MO_SIZE) > MO_32) {
        c->valid = false;
        return;
    }
    MemOp mop = get_memop(oi);
    TCGAtomAlign aa = atom_and_align_for_opc(s, mop, MO_ATOM_IFALIGN, false);
    int fast = tlb_mask_table_ofs(s, get_mmuidx(oi));
    if (mop & MO_BSWAP) {
        c->valid = false;
        return;
    }
    c->site[c->count++] = (XwdMemorySite){
        .insn = c->insn, .oi = oi, .restore_pc = c->restore_pc,
        .mask_ofs = fast + offsetof(CPUTLBDescFast, mask),
        .table_ofs = fast + offsetof(CPUTLBDescFast, table),
        .cmp_ofs = store ? offsetof(CPUTLBEntry, addr_write) :
                          offsetof(CPUTLBEntry, addr_read),
        .addend_ofs = offsetof(CPUTLBEntry, addend),
        .index_shift = TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS,
        .page_mask = TARGET_PAGE_MASK,
        .a_mask = (1u << MAX(aa.align, mop & MO_SIZE)) - 1,
        .s_mask = (1u << (mop & MO_SIZE)) - 1,
        .data_reg = data, .addr_reg = addr, .store = store,
    };
}

static void wasm_direct_memory_mark(unsigned kind)
{
    if (wasm_direct_memory && wasm_direct_memory->valid &&
        wasm_direct_memory->count) {
        wasm_add_reloc(kind, wasm_direct_memory->count - 1, 0);
    }
}
