/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Whole selected-group substitution. No ownership or partition decisions here. */
#include "qemu/xemu-wasm-direct.h"
#include "wasm32-direct.c.inc"
#include "wasm32-direct-blueprint.c.inc"
#include "wasm32-direct-bank.c.inc"

#define XWR_EXTRA_LOCALS (XD_TAKEN + 1)

typedef struct XwdRegionMember {
    XwdTranslation t;
    XwdPlan plan;
    TranslationBlock *tb;
    WasmTBHeader *internal[2];
} XwdRegionMember;

typedef struct XwdRegion {
    unsigned n, mode, cc, verify;
    XwdRegionMember member[REGION_MAX];
} XwdRegion;

/* Single vCPU, nonsuspending verifier; never a continuation save area. */
static XwdState wasm_region_expected;

static XwdRegion *xwr_prepare(WasmTBHeader **members, unsigned n)
{
    const char *census = getenv("XEMU_WASM_INSN_CENSUS");
    const char *hp = getenv("XEMU_WASM_HELPER_PROF");
    const char *jp = getenv("XEMU_WASM_JIT_PROFILE");
    XwdRegion *r;

    if (!region_shared || n < 2 || !xemu_wasm_direct_regions_enabled() ||
        !xemu_wasm_direct_mode() || wasm32_ic_for_exit(false) ||
        (census && *census == '1') || (hp && *hp == '1') ||
        (jp && atoi(jp))) {
        return NULL;
    }
    r = g_new0(XwdRegion, 1);
    r->n = n;
    r->mode = xemu_wasm_direct_mode();
    for (unsigned i = 0; i < n; i++) {
        XwdRegionMember *m = &r->member[i];
        WasmTBHeader *h = members[i];
        unsigned begin = 0, slots = 0;
        if (!xwd_blueprint_find(h->wasm_ptr, h->wasm_size, &m->t, &m->plan)) {
            goto decline;
        }
        m->tb = tcg_tb_lookup((uintptr_t)h);
        unsigned mask = m->plan.conditional ? 3 : 1;
        if (!m->tb || m->t.capture.jump_valid != mask) {
            goto decline;
        }
        /* Independent original targets, not the direct emitter's choice. */
        for (unsigned slot = 0; slot < 2; slot++) {
            if ((mask & (1u << slot)) &&
                (m->tb->jmp_reset_offset[slot] == TB_JMP_OFFSET_INVALID ||
                 m->t.capture.jump_pc[slot] != (uint32_t)(m->plan.pc +
                    (slot ? m->plan.fall_delta : m->plan.delta)))) {
                goto decline;
            }
        }
        for (unsigned slot = 0; slot < 2; slot++) {
            WasmTBHeader *next = tb_link_target(m->tb, slot);
            for (unsigned j = 0; j < n; j++) {
                if (next == members[j]) {
                    m->internal[slot] = next;
                }
            }
        }
        XwdLayout a = m->t.layout, b = r->member[0].t.layout;
        a.phase_generated = b.phase_generated = 0;
        if (memcmp(&a, &b, sizeof(a))) {
            goto decline;
        }
        for (unsigned j = 0; j < h->reloc_count; j++) {
            const WasmReloc *rl = &h->reloc_ptr[j];
            if (rl->off > h->body_len ||
                (j && rl->off < h->reloc_ptr[j - 1].off) ||
                rl->kind == WASM_RELOC_DIRECT_SUSPEND) {
                goto decline;
            }
            if (rl->kind == WASM_RELOC_DIRECT_BEGIN) {
                begin++;
            } else if (rl->kind == WASM_RELOC_DIRECT_VERIFY && rl->arg < 2) {
                slots |= 1u << rl->arg;
            } else if (rl->kind == WASM_RELOC_GOTO && rl->arg == 255) {
                goto decline; /* Do not replace lookup/indirect-exit semantics. */
            }
        }
        if (begin != 1 || slots != mask) {
            goto decline;
        }
    }
    return r;
decline:
    g_free(r);
    return NULL;
}

static bool xwr_import(uint32_t fn, const uint8_t *type, unsigned len,
                       uint32_t *hq, const uint8_t **hty, int *htylen,
                       int *nh, unsigned *index)
{
    unsigned u;
    for (u = 0; u < *nh && hq[u] != fn; u++) {
    }
    if (u < *nh) {
        if (htylen[u] != len || memcmp(hty[u], type, len)) {
            return false;
        }
    } else {
        if (u == 255) {
            return false;
        }
        hq[u] = fn;
        hty[u] = type;
        htylen[u] = len;
        (*nh)++;
    }
    *index = REGION_HELPER_START + u;
    return true;
}

static bool xwr_imports(XwdRegion *r, uint32_t *hq, const uint8_t **hty,
                        int *htylen, int *nh)
{
    static const uint8_t cc[] = {0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f};
    static const uint8_t verify[] = {0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 0};
    const XwdLayout *l = &r->member[0].t.layout;

    return xwr_import((uintptr_t)l->cc_all, cc, sizeof(cc), hq, hty, htylen,
                      nh, &r->cc) &&
           (r->mode != 2 || xwr_import((uintptr_t)l->verify, verify,
                sizeof(verify), hq, hty, htylen, nh, &r->verify));
}

static XwdEmitter xwr_emitter(XwdCode *code, XwdRegion *r, unsigned i)
{
    *code = (XwdCode){.valid = true};
    return (XwdEmitter){.code = code, .layout = &r->member[i].t.layout,
        .cc_helper = r->cc, .resident = true, .valid = 0xff,
        .local_bias = REGION_INST_LOCAL};
}

static void xwr_raw_local(XwdEmitter *e, unsigned op, unsigned index)
{
    xwd_byte(e, op);
    xwd_uleb(e, index);
}

static void xwr_ctx_const(XwdEmitter *e, unsigned off, uint32_t value)
{
    xwd_local(e, 0x20, 0);
    xwd_const(e, value);
    xwd_mem(e, 0x36, off);
}

static void xwr_phase(XwdEmitter *e, uint32_t phase)
{
    if (e->layout->phase_ptr) {
        xwd_const(e, e->layout->phase_ptr);
        xwd_const(e, phase);
        xwd_mem(e, 0x36, 0);
    }
}

static bool xwr_flush(ByteBuf *b, XwdEmitter *e)
{
    if (!e->code->valid) {
        return false;
    }
    bb_bytes(b, e->code->bytes, e->code->size);
    e->code->size = 0;
    return true;
}

static bool xwr_init(ByteBuf *b, XwdRegion *r)
{
    XwdCode code;
    XwdEmitter e = xwr_emitter(&code, r, 0);
    /* Register-only admitted reference paths cannot suspend. Reject misuse,
     * rather than reinitialize a lost expected bank during an unproven rewind.
     */
    xwd_local(&e, 0x20, 0);
    xwd_mem(&e, 0x28, WASM_CTX_DO_INIT_OFF);
    xwd_byte(&e, 0x45);
    xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
    xwd_byte(&e, 0x00); xwd_byte(&e, 0x0b);
    xwd_bank_init(&e);
    return xwr_flush(b, &e);
}

static bool xwr_shadow_begin(ByteBuf *b, XwdRegion *r, unsigned i)
{
    XwdCode code;
    XwdEmitter e = xwr_emitter(&code, r, i);
    if (!xwd_bank_step(&e, &r->member[i].plan, true)) {
        return false;
    }
    xwd_const(&e, 1);
    xwd_local(&e, 0x21, XWR_EXTRA_LOCALS);
    return xwr_flush(b, &e);
}

static bool xwr_shadow_verify(ByteBuf *b, XwdRegion *r, unsigned i,
                              unsigned slot)
{
    XwdCode code;
    XwdEmitter e = xwr_emitter(&code, r, i);
    const XwdPlan *p = &r->member[i].plan;
    uint32_t scratch = (uintptr_t)&wasm_region_expected;
    unsigned check = XWD_CHECK_PC;

    xwd_local(&e, 0x20, XWR_EXTRA_LOCALS);
    xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
    xwd_const(&e, 0);
    xwd_local(&e, 0x21, XWR_EXTRA_LOCALS);
    xwd_bank_store(&e, true, scratch);
    xwd_const(&e, scratch); xwd_const(&e, 1);
    xwd_mem(&e, 0x36, offsetof(XwdState, pending));
    xwd_const(&e, scratch);
    xwd_const(&e, p->needs_flags | (p->conditional << 1) | XWD_FEATURE_REGION);
    if (p->conditional) {
        xwd_local(&e, 0x20, XD_TAKEN);
        xwd_const(&e, XWD_FEATURE_TAKEN);
        xwd_byte(&e, 0x6c); xwd_byte(&e, 0x72);
    }
    xwd_mem(&e, 0x36, offsetof(XwdState, features));
    xwd_local(&e, 0x20, XD_ENV); xwd_const(&e, scratch);
    if (slot < 2 && p->conditional) {
        check |= XWD_CHECK_EDGE | (slot == 0 ? XWD_CHECK_TAKEN : 0);
    }
    xwd_const(&e, check);
    if (slot < 2 && !p->pcrel) {
        xwd_const(&e, r->member[i].t.capture.jump_pc[slot]);
    } else {
        xwd_local(&e, 0x20, XD_ENV); xwd_mem(&e, 0x28, e.layout->eip);
    }
    xwr_phase(&e, (uintptr_t)"direct:verify");
    xwd_byte(&e, 0x10); xwd_uleb(&e, r->verify);
    xwr_phase(&e, e.layout->phase_generated);
    xwd_byte(&e, 0x0b);
    return xwr_flush(b, &e);
}

static bool xwr_active_exit(ByteBuf *b, XwdEmitter *e, XwdRegion *r,
                            unsigned i, unsigned slot, unsigned top)
{
    TranslationBlock *tb = r->member[i].tb;
    uint32_t reset = (uintptr_t)tb->tc.ptr + tb->jmp_reset_offset[slot];
    uint32_t exit = (uintptr_t)tcg_splitwx_to_rx(tb) + slot;

    /* Authoritative original slot, loaded live on EVERY execution. */
    xwd_const(e, (uintptr_t)tcg_splitwx_to_rx(&tb->jmp_target_addr[slot]));
    xwd_mem(e, 0x28, 0);
    xwr_raw_local(e, 0x22, REGION_L32_0);
    xwd_const(e, reset); xwd_byte(e, 0x47);
    xwd_byte(e, 0x04); xwd_byte(e, 0x40);
    if (!xwr_flush(b, e)) {
        return false;
    }
    /* Preserve the legacy collector's direct-edge partition as well: self,
     * or the originally selected child. Still validate current ownership.
     */
    xwr_raw_local(e, 0x20, REGION_L32_0);
    xwd_const(e, (uintptr_t)tb->tc.ptr); xwd_byte(e, 0x46);
    if (r->member[i].internal[slot] &&
        r->member[i].internal[slot] != tb->tc.ptr) {
        xwr_raw_local(e, 0x20, REGION_L32_0);
        xwd_const(e, (uintptr_t)r->member[i].internal[slot]);
        xwd_byte(e, 0x46); xwd_byte(e, 0x72);
    }
    xwd_byte(e, 0x04); xwd_byte(e, 0x40);
    if (!xwr_flush(b, e)) {
        return false;
    }
    region_shared_guard(b, r->n, top + 2, true);
    xwd_byte(e, 0x0b);
    /* Foreign/unlinked-to-this-instance target: publish before dispatcher. */
    xwd_bank_store(e, false, 0);
    xwd_local(e, 0x20, 0);
    xwr_raw_local(e, 0x20, REGION_L32_0);
    xwd_mem(e, 0x36, WASM_CTX_TB_PTR_OFF);
    xwr_ctx_const(e, WASM_CTX_DO_INIT_OFF, 1);
    if (!xwr_flush(b, e)) {
        return false;
    }
    bb_count(b, &xemu_wasm_stats.n_jit_exit_direct);
    xwd_const(e, 0); xwd_byte(e, 0x0f); xwd_byte(e, 0x0b);
    /* Original reset-slot exit must still teach cpu-exec the chain slot. */
    xwd_bank_store(e, false, 0);
    xwr_ctx_const(e, WASM_CTX_TB_PTR_OFF, 0);
    if (!xwr_flush(b, e)) {
        return false;
    }
    bb_count(b, slot ? &xemu_wasm_stats.n_jit_exit_chain1 :
                      &xemu_wasm_stats.n_jit_exit_chain0);
    xwd_const(e, exit); xwd_byte(e, 0x0f);
    return xwr_flush(b, e);
}

static bool xwr_active_member(ByteBuf *b, XwdRegion *r, unsigned i,
                              unsigned top)
{
    XwdCode code;
    XwdEmitter e = xwr_emitter(&code, r, i);
    const XwdPlan *p = &r->member[i].plan;

    xwr_ctx_const(&e, WASM_CTX_DO_INIT_OFF, 0);
    xwr_phase(&e, e.layout->phase_generated);
    if (!p->pcrel) {
        xwd_const(&e, p->pc); xwd_local(&e, 0x21, XD_PC);
    }
    /* Preserve gen_tb_start's signed32-bit poll on every member/forward edge. */
    xwd_local(&e, 0x20, XD_ENV); xwd_const(&e, e.layout->icount_decr);
    xwd_byte(&e, 0x6a); xwd_mem(&e, 0x28, 0);
    xwd_const(&e, 0); xwd_byte(&e, 0x48);
    xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
    xwd_bank_store(&e, false, 0);
    xwr_ctx_const(&e, WASM_CTX_TB_PTR_OFF, 0);
    if (!xwr_flush(b, &e)) {
        return false;
    }
    bb_count(b, &xemu_wasm_stats.n_jit_exit_requested);
    xwd_const(&e, (uintptr_t)tcg_splitwx_to_rx(r->member[i].tb) +
                  TB_EXIT_REQUESTED);
    xwd_byte(&e, 0x0f); xwd_byte(&e, 0x0b);
    if (!xwd_bank_step(&e, p, false) || !xwr_flush(b, &e)) {
        return false;
    }
    bb_count(b, &xemu_wasm_stats.n_direct_exec);
    if (p->conditional) {
        xwd_local(&e, 0x20, XD_TAKEN);
        xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
        if (!xwr_flush(b, &e) ||
            !xwr_active_exit(b, &e, r, i, 0, top + 1)) {
            return false;
        }
        xwd_byte(&e, 0x05);
        if (!xwr_flush(b, &e) ||
            !xwr_active_exit(b, &e, r, i, 1, top + 1)) {
            return false;
        }
        xwd_byte(&e, 0x0b);
        return xwr_flush(b, &e);
    }
    return xwr_active_exit(b, &e, r, i, 0, top);
}
