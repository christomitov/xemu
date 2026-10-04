/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Direct x86 memory stage: guest loads/stores of a direct body.
 *
 * Each memory instruction performs exactly one guest access, in this shape
 * (required by the integration's suspend/resume):
 *
 *   compute address (and store value) into private locals
 *   inline softmmu TLB lookup, identical to the backend's wasm_tlb_load()
 *   if hit: one host load/store           (cannot fault: the TLB said so)
 *   end
 *   resume_boundary                       (empty operand stack)
 *   if miss: slow(...)                    (original helper path)
 *   end
 *   commit architectural results          (only after the access)
 *
 * The tag compare keeps the full alignment/span check and the TLB flag bits
 * (NOTDIRTY, MMIO, watchpoints, invalid), so every access the fast path must
 * not take misses and goes through slow(). In verify mode the whole access
 * is replaced by shadow(), which never touches guest memory.
 *
 * Segments: addresses are flat (offset only). The integration admits memory
 * plans only for code whose DS/ES/SS bases are zero (no HF_ADDSEG).
 */

/* i32 load/store with an explicit alignment exponent */
static void xwd_memop(XwdEmitter *e, uint8_t op, unsigned align, unsigned off)
{
    xwd_byte(e, op);
    xwd_byte(e, align);
    xwd_uleb(e, off);
}

/* XD_MADDR = disp + base + (index << scale) */
static void xwd_mem_address(XwdEmitter *e, const XwdInsn *o)
{
    if (o->op == XWD_PUSH || o->op == XWD_POP || o->op == XWD_CALL) {
        xwd_get(e, 4, 32);                 /* ESP */
        if (o->op != XWD_POP) {
            xwd_const(e, 4);
            xwd_byte(e, 0x6b);             /* i32.sub */
        }
    } else {
        xwd_const(e, o->disp);
        if (o->base != 0xff) {
            xwd_get(e, o->base, 32);
            xwd_byte(e, 0x6a);
        }
        if (o->index != 0xff) {
            xwd_get(e, o->index, 32);
            if (o->scale) {
                xwd_const(e, o->scale);
                xwd_byte(e, 0x74);
            }
            xwd_byte(e, 0x6a);
        }
    }
    xwd_local(e, 0x21, XD_MADDR);
}

/* env-relative i32 load (offsets may be negative: CPUNegativeOffsetState) */
static void xwd_env_load(XwdEmitter *e, int32_t ofs)
{
    xwd_local(e, 0x20, XD_ENV);
    xwd_const(e, (uint32_t)ofs);
    xwd_byte(e, 0x6a);
    xwd_memop(e, 0x28, 2, 0);
}

/*
 * One access of instruction @insn: a load leaves its zero-extended value in
 * XD_MVAL; a store writes the low @width bits of XD_MVAL.
 */
static bool xwd_mem_access(XwdEmitter *e, unsigned insn, bool verify,
                           bool store, unsigned width)
{
    const XwdMemHooks *h = e->mem;
    XwdMemAccess a = { .insn = insn, .index = e->access++, .store = store,
                       .width = width };

    if (!h || !h->resolve || !h->resume_boundary || !h->slow || !h->shadow ||
        !h->resolve(e, insn, a.index, store, width, &a) ||
        a.insn != insn || a.store != store || a.width != width) {
        return false;
    }
    if (verify) {
        return h->shadow(e, &a, XD_MADDR, XD_MVAL);
    }

    /* entry = table + ((a ^ ((a >> fold_shift) & fold_mask)) >> shift & mask) */
    xwd_local(e, 0x20, XD_MADDR);
    xwd_local(e, 0x20, XD_MADDR);
    xwd_const(e, a.fold_shift);
    xwd_byte(e, 0x76);                     /* i32.shr_u */
    xwd_const(e, a.fold_mask);
    xwd_byte(e, 0x71);
    xwd_byte(e, 0x73);                     /* i32.xor */
    xwd_const(e, a.index_shift);
    xwd_byte(e, 0x76);
    xwd_env_load(e, a.mask_ofs);
    xwd_byte(e, 0x71);
    xwd_env_load(e, a.table_ofs);
    xwd_byte(e, 0x6a);
    xwd_local(e, 0x22, XD_MENTRY);
    xwd_memop(e, 0x28, 2, a.cmp_ofs);
    /* expected tag: the access's end page with alignment bits */
    xwd_local(e, 0x20, XD_MADDR);
    if (a.a_mask < a.s_mask) {
        xwd_const(e, a.s_mask - a.a_mask);
        xwd_byte(e, 0x6a);
    }
    xwd_const(e, a.page_mask | a.a_mask);
    xwd_byte(e, 0x71);
    xwd_byte(e, 0x46);                     /* i32.eq */
    xwd_local(e, 0x22, XD_MHIT);

    xwd_byte(e, 0x04);                     /* if hit */
    xwd_byte(e, 0x40);
    xwd_local(e, 0x20, XD_MENTRY);
    xwd_memop(e, 0x28, 2, a.addend_ofs);
    xwd_local(e, 0x20, XD_MADDR);
    xwd_byte(e, 0x6a);                     /* host address */
    if (store) {
        xwd_local(e, 0x20, XD_MVAL);
        xwd_memop(e, width == 8 ? 0x3a : width == 16 ? 0x3b : 0x36,
                  width == 8 ? 0 : width == 16 ? 1 : 2, 0);
    } else {
        xwd_memop(e, width == 8 ? 0x2d : width == 16 ? 0x2f : 0x28,
                  width == 8 ? 0 : width == 16 ? 1 : 2, 0);
        xwd_local(e, 0x21, XD_MVAL);
    }
    xwd_byte(e, 0x0b);                     /* end */

    if (!h->resume_boundary(e, &a)) {
        return false;
    }
    xwd_local(e, 0x20, XD_MHIT);
    xwd_byte(e, 0x45);                     /* i32.eqz */
    xwd_byte(e, 0x04);                     /* if miss */
    xwd_byte(e, 0x40);
    if (!h->slow(e, &a, XD_MADDR, XD_MVAL)) {
        return false;
    }
    xwd_byte(e, 0x0b);
    return e->code->valid;
}

/*
 * The access of memory instruction @o, before its arithmetic: loads (and
 * POP) leave the value in XD_MVAL for xwd_get(XWD_MEMREG); a store's value
 * is produced by the caller first. Returns false to decline the body.
 */
static bool xwd_mem_insn(XwdEmitter *e, const XwdInsn *o, unsigned insn,
                         const XwdPlan *p, bool verify)
{
    xwd_mem_address(e, o);
    if (o->mem_store) {
        if (o->op == XWD_CALL) {
            /*
             * Return address: the EIP after the CALL (the plan's last
             * instruction). PC-relative code takes the entry EIP at run
             * time, like the exit's EIP update: the region's XD_PC, or
             * env->eip in a standalone body.
             */
            if (p->pcrel) {
                if (e->resident) {
                    xwd_local(e, 0x20, XD_PC);
                } else {
                    xwd_local(e, 0x20, XD_ENV);
                    xwd_mem(e, 0x28, e->layout->eip);
                }
                xwd_const(e, p->fall_delta);
                xwd_byte(e, 0x6a);
            } else {
                xwd_const(e, p->pc + p->fall_delta);
            }
        } else if (o->op == XWD_PUSH && !o->immediate) {
            xwd_get(e, o->src, 32);
        } else if (o->immediate) {
            xwd_const(e, o->imm);
        } else {
            xwd_get(e, o->src, o->width);
        }
        xwd_local(e, 0x21, XD_MVAL);
    }
    return xwd_mem_access(e, insn, verify, o->mem_store, o->mem_width);
}
