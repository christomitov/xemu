/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Direct emission in the unchanged selected group; no ownership decisions. */
#include "qemu/xemu-wasm-direct.h"
#include "qemu/xemu-wasm-direct-memory.h"
#include "wasm32-direct.c.inc"
#include "wasm32-direct-blueprint.c.inc"
#include "wasm32-direct-memory-codec.c.inc"
#include "wasm32-direct-bank.c.inc"

typedef struct XwdRegionMember {
    XwdTranslation t;
    XwdPlan plan;
    TranslationBlock *tb;
    WasmTBHeader *internal[2];
    XwdMemoryCapture memory;
    XwdMemHooks hooks;
    struct XwdRegion *region;
    bool direct;
} XwdRegionMember;

typedef struct XwdRegion {
    unsigned n, mode, cc, verify, memory_members, locals, pending, address;
    unsigned mem_begin, mem_observe, mem_load, mem_store, mem_end;
    unsigned mem_cancel, mem_miss;
    unsigned direct_members, anchor, bank_live, miss_tag;
    bool mixed;
    XwdRegionMember member[REGION_MAX];
} XwdRegion;

static bool xwr_mem_resolve(XwdEmitter *, unsigned, unsigned, bool, unsigned,
                            XwdMemAccess *);
static bool xwr_mem_resume(XwdEmitter *, const XwdMemAccess *);
static bool xwr_mem_miss(XwdEmitter *, const XwdMemAccess *, unsigned, unsigned);
static bool xwr_mem_shadow(XwdEmitter *, const XwdMemAccess *, unsigned,
                           unsigned);

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
        unsigned begin = 0, slots = 0, access = 0, phase = 0;
        if (!xwd_blueprint_find_ex(h->wasm_ptr, h->wasm_size, &m->t, &m->plan,
                                   xemu_wasm_direct_memory_enabled())) {
            goto member_declined;
        }
        m->region = r;
        if (m->plan.accesses) {
            if (!xwd_memory_find(h->wasm_ptr, h->wasm_size, &m->memory) ||
                m->memory.count != m->plan.accesses) {
                goto member_declined;
            }
            unsigned k = 0;
            for (unsigned insn = 0; insn < m->plan.count; insn++) {
                const XwdInsn *op = &m->plan.insn[insn];
                if (!op->mem) {
                    continue;
                }
                const XwdMemorySite *site = &m->memory.site[k++];
                uint32_t pc = m->plan.pc +
                    (insn ? m->t.capture.end[insn - 1] : 0);
                if (site->insn != insn || site->store != op->mem_store ||
                    8u << ((site->oi >> 5) & 7) != op->mem_width ||
                    site->restore_pc != (m->plan.pcrel ? pc & 0xfff : pc)) {
                    goto member_declined;
                }
            }
            m->hooks = (XwdMemHooks){ .opaque = m, .resolve = xwr_mem_resolve,
                .resume_boundary = xwr_mem_resume, .slow = xwr_mem_miss,
                .shadow = xwr_mem_shadow };
        }
        m->tb = tcg_tb_lookup((uintptr_t)h);
        unsigned mask = m->plan.conditional ? 3 : 1;
        if (!m->tb || m->t.capture.jump_valid != mask) {
            goto member_declined;
        }
        /* Independent original targets, not the direct emitter's choice. */
        for (unsigned slot = 0; slot < 2; slot++) {
            if ((mask & (1u << slot)) &&
                (m->tb->jmp_reset_offset[slot] == TB_JMP_OFFSET_INVALID ||
                 m->t.capture.jump_pc[slot] != (uint32_t)(m->plan.pc +
                    (slot ? m->plan.fall_delta : m->plan.delta)))) {
                goto member_declined;
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
        XwdLayout a = m->t.layout;
        XwdLayout b = r->direct_members ? r->member[r->anchor].t.layout : a;
        a.phase_generated = b.phase_generated = 0;
        if (memcmp(&a, &b, sizeof(a))) {
            goto member_declined;
        }
        for (unsigned j = 0; j < h->reloc_count; j++) {
            const WasmReloc *rl = &h->reloc_ptr[j];
            if (rl->off > h->body_len ||
                (j && rl->off < h->reloc_ptr[j - 1].off)) {
                goto member_declined;
            }
            if (rl->kind == WASM_RELOC_MEM_BEGIN) {
                if (phase || access >= m->memory.count || rl->arg != access) {
                    goto member_declined;
                }
                phase = 1;
            } else if (rl->kind == WASM_RELOC_DIRECT_SUSPEND) {
                if (phase != 1) {
                    goto member_declined;
                }
                phase = 2;
            } else if (rl->kind == WASM_RELOC_MEM_RESUME) {
                if (phase != 2 || rl->arg != access) {
                    goto member_declined;
                }
                phase = 3;
            } else if (rl->kind == WASM_RELOC_MEM_END) {
                if (phase != 3 || rl->arg != access) {
                    goto member_declined;
                }
                phase = 0;
                access++;
            } else if (rl->kind == WASM_RELOC_DIRECT_BEGIN) {
                begin++;
            } else if (rl->kind == WASM_RELOC_DIRECT_VERIFY && rl->arg < 2) {
                slots |= 1u << rl->arg;
            } else if (rl->kind == WASM_RELOC_GOTO && rl->arg == 255) {
                /* Keep lookup/indirect-exit members entirely original. */
                goto member_declined;
            }
        }
        if (begin != 1 || slots != mask || phase || access != m->memory.count) {
            goto member_declined;
        }
        m->direct = true;
        if (!r->direct_members++) {
            r->anchor = i;
        }
        r->memory_members += m->plan.accesses != 0;
        continue;
member_declined:
        *m = (XwdRegionMember){0};
        if (!xemu_wasm_direct_mixed_enabled()) {
            goto decline;
        }
    }
    if (!r->direct_members) {
        goto decline;
    }
    r->mixed = r->direct_members != n;
    /*
     * Change only sampled scope identities, after layout compatibility was
     * checked. Reference/mode2 bodies must never be called active direct time.
     */
    if (r->mode == 1 && xemu_wasm_direct_profile_enabled()) {
        for (unsigned i = 0; i < n; i++) {
            if (!r->member[i].direct) {
                continue;
            }
            XwdLayout *l = &r->member[i].t.layout;
            bool memory = r->member[i].plan.accesses != 0;
            l->phase_generated = (uintptr_t)(memory ? "direct:mem" :
                                                       "direct:reg");
            l->phase_cc = (uintptr_t)(memory ? "direct:mem-cc" :
                                                "direct:reg-cc");
        }
    }
    r->pending = r->memory_members ? XD_NLOCALS : XD_TAKEN + 1;
    r->address = r->pending + 1;
    r->locals = r->memory_members ? r->address : r->pending;
    if (r->mixed) {
        r->bank_live = ++r->locals;
    }
    if (r->mode == 1 && r->memory_members &&
        xemu_wasm_direct_miss_classify_enabled()) {
        r->miss_tag = ++r->locals;
        for (unsigned i = 0; i < n; i++) {
            r->member[i].hooks.tag_local = r->miss_tag;
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
    const XwdLayout *l = &r->member[r->anchor].t.layout;

    static const uint8_t one[] = {0x60, 1, 0x7f, 0};
    static const uint8_t two[] = {0x60, 2, 0x7f, 0x7f, 0};
    static const uint8_t five[] = {0x60, 5, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0};
    static const uint8_t six[] = {0x60, 6, 0x7f, 0x7f, 0x7f, 0x7f,
                                0x7f, 0x7f, 0};
    static const uint8_t observe[] = {0x60, 6, 0x7f, 0x7f, 0x7f, 0x7f,
                                    0x7f, 0x7e, 0};
    static const uint8_t none[] = {0x60, 0, 0};
    if (!xwr_import((uintptr_t)l->cc_all, cc, sizeof(cc), hq, hty, htylen,
                     nh, &r->cc) ||
        (r->mode == 2 && !xwr_import((uintptr_t)l->verify, verify,
                sizeof(verify), hq, hty, htylen, nh, &r->verify))) {
        return false;
    }
    if (!r->memory_members) {
        return true;
    }
#define XWR_MEM_IMPORT(fn, ty, field) \
    xwr_import((uintptr_t)wasm32_direct_mem_##fn, ty, sizeof(ty), \
               hq, hty, htylen, nh, &r->field)
    if (r->mode == 1) {
        return r->miss_tag ? XWR_MEM_IMPORT(miss_classify, six, mem_miss) :
                            XWR_MEM_IMPORT(miss, one, mem_miss);
    }
    return XWR_MEM_IMPORT(begin, two, mem_begin) &&
           XWR_MEM_IMPORT(observe, observe, mem_observe) &&
           XWR_MEM_IMPORT(load, cc, mem_load) &&
           XWR_MEM_IMPORT(store, five, mem_store) &&
           XWR_MEM_IMPORT(end, one, mem_end) &&
           XWR_MEM_IMPORT(cancel, none, mem_cancel);
#undef XWR_MEM_IMPORT
}

static XwdEmitter xwr_emitter(XwdCode *code, XwdRegion *r, unsigned i)
{
    *code = (XwdCode){.valid = true};
    return (XwdEmitter){.code = code, .layout = &r->member[i].t.layout,
        .cc_helper = r->cc, .resident = true, .valid = 0xff,
        .local_bias = REGION_INST_LOCAL,
        .mem = r->member[i].plan.accesses ? &r->member[i].hooks : NULL};
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

#include "wasm32-direct-memory-region.c.inc"
#include "wasm32-direct-mixed.c.inc"

static bool xwr_init(ByteBuf *b, XwdRegion *r)
{
    XwdCode code;
    XwdEmitter e = xwr_emitter(&code, r, r->anchor);
    if (r->mixed) {
        /*
         * Fresh and rewind entries both start without a resident bank.
         * In particular, don't touch the original saved TCG locals or make
         * a legacy B continuation depend on these unsaved direct locals.
         */
        xwd_const(&e, 0); xwd_local(&e, 0x21, r->bank_live);
        xwd_const(&e, 0); xwd_local(&e, 0x21, r->pending);
        xwd_local(&e, 0x20, 0);
        xwd_mem(&e, 0x28, WASM_CTX_DO_INIT_OFF);
        xwd_byte(&e, 0x45);
        xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
        xwr_mixed_rewind(&e, r);
        xwd_byte(&e, 0x0b);
        return xwr_flush(b, &e);
    }
    if (r->mode == 1 && xemu_wasm_direct_profile_enabled()) {
        xwr_phase(&e, (uintptr_t)"direct:entry");
    }
    if (r->memory_members && r->mode == 2) {
        xwd_local(&e, 0x20, 0);
        xwd_mem(&e, 0x28, WASM_CTX_DO_INIT_OFF);
        xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
        xwd_bank_init(&e);
        xwd_byte(&e, 0x05);
        /* Original B resumes normally, but this lost-bank invocation is
         * explicitly unverified. Do not fabricate a new expected prefix.
         */
        xwd_byte(&e, 0x10); xwd_uleb(&e, r->mem_cancel);
        xwd_byte(&e, 0x0b);
        return xwr_flush(b, &e);
    }
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
    const XwdPlan *p = &r->member[i].plan;
    /*
     * BEGIN is after the original IRQ poll: skipped members stay unchecked.
     * Reanchor only after legacy/fresh entry, retain independent expected
     * state across consecutive supported members.
     */
    xwr_mixed_load(&e, r);
    if (p->accesses) {
        if (!p->pcrel) {
            xwd_const(&e, p->pc); xwd_local(&e, 0x21, XD_PC);
        }
        xwd_local(&e, 0x20, XD_ENV);
        if (p->pcrel) {
            xwd_local(&e, 0x20, XD_ENV);
            xwd_mem(&e, 0x28, e.layout->eip);
        } else {
            xwd_const(&e, r->member[i].t.capture.pc);
        }
        xwd_byte(&e, 0x10); xwd_uleb(&e, r->mem_begin);
    } else if (!xwd_bank_step(&e, p, true)) {
        return false;
    }
    xwd_const(&e, 1);
    xwd_local(&e, 0x21, r->pending);
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

    xwd_local(&e, 0x20, r->pending);
    xwd_byte(&e, 0x04); xwd_byte(&e, 0x40);
    xwd_const(&e, 0);
    xwd_local(&e, 0x21, r->pending);
    if (p->accesses) {
        if (!xwd_bank_step(&e, p, true)) {
            return false;
        }
        xwd_local(&e, 0x20, XD_ENV);
        xwd_byte(&e, 0x10); xwd_uleb(&e, r->mem_end);
    }
    xwd_bank_store(&e, true, scratch);
    xwd_const(&e, scratch); xwd_const(&e, 1);
    xwd_mem(&e, 0x36, offsetof(XwdState, pending));
    xwd_const(&e, scratch);
    xwd_const(&e, p->needs_flags | (p->conditional << 1) | XWD_FEATURE_REGION |
                   (r->mixed ? XWD_FEATURE_MIXED : 0) |
                   (r->mixed && p->accesses ? XWD_FEATURE_MEMORY : 0));
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

    xwr_mixed_load(&e, r);
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
