/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Capture only bytes already fetched by the authoritative x86 decoder. */

#if defined(EMSCRIPTEN) && defined(CONFIG_TCG_WASM_JIT) && \
    !defined(TARGET_X86_64)
static void x86_wasm_direct_verify(void *opaque, XwdState *state,
                                   uint32_t check_pc, uint32_t actual_pc)
{
    CPUX86State *env = opaque;
    bool equal = true;
    unsigned bad_regs = 0;

    if (!state->pending) {
        return; /* Entry IRQ exit or the unlinked arm after a verified goto. */
    }
    state->pending = 0;
    for (int i = 0; i < 8; i++) {
        if (env->regs[i] != state->regs[i]) {
            bad_regs |= 1u << i;
        }
    }
    equal &= !bad_regs;
    equal &= helper_cc_compute_all(env->cc_dst, env->cc_src, env->cc_src2,
                                   env->cc_op) ==
             helper_cc_compute_all(state->cc_dst, state->cc_src, state->cc_src2,
                                   state->cc_op);
    equal &= !(check_pc & XWD_CHECK_PC) || actual_pc == state->eip;
    equal &= !(check_pc & XWD_CHECK_EDGE) ||
             !!(state->features & XWD_FEATURE_TAKEN) ==
             !!(check_pc & XWD_CHECK_TAKEN);
    equal &= env_cpu(env)->neg.can_do_io;
    XSTAT_INC(n_direct_checked);
    if (state->features & 1) {
        XSTAT_INC(n_direct_flags_checked);
    }
    if (state->features & 2) {
        XSTAT_INC(n_direct_jcc_checked);
    }
    if (state->features & XWD_FEATURE_REGION) {
        XSTAT_INC(n_direct_region_checked);
    }
    if (state->features & XWD_FEATURE_MIXED) {
        XSTAT_INC(n_direct_mixed_checked);
        if (state->features & XWD_FEATURE_MEMORY) {
            XSTAT_INC(n_direct_mixed_memory_tb_checked);
        }
    }
    if (!equal) {
        XSTAT_INC(n_direct_mismatch);
        fprintf(stderr, "direct-x86 register differential mismatch "
                "eip=%08x expected=%08x check_pc=%u regs=%02x io=%u\n",
                actual_pc, state->eip, check_pc, bad_regs,
                env_cpu(env)->neg.can_do_io);
        for (int i = 0; i < 8; i++) {
            if (bad_regs & (1u << i)) {
                fprintf(stderr, " r%d actual=%08x expected=%08x\n", i,
                        env->regs[i], state->regs[i]);
            }
        }
        fprintf(stderr, " cc actual=%u/%08x/%08x/%08x "
                "expected=%u/%08x/%08x/%08x\n", env->cc_op, env->cc_dst,
                env->cc_src, env->cc_src2, state->cc_op, state->cc_dst,
                state->cc_src, state->cc_src2);
        abort();
    }
}
#endif

static void x86_wasm_direct_start(DisasContext *s)
{
    s->wasm_direct = NULL;
#if defined(EMSCRIPTEN) && defined(CONFIG_TCG_WASM_JIT) && \
    !defined(TARGET_X86_64) && !defined(CONFIG_PLUGIN)
    uint32_t cf = tb_cflags(s->base.tb);

    if (!xemu_wasm_direct_mode() || !PE(s) || !CODE32(s) || !SS32(s) ||
        CODE64(s) || ADDSEG(s) || VM86(s) || GUEST(s) || SVME(s) ||
        s->cs_base || s->cpu_has_bps ||
        (s->flags & (HF_TF_MASK | HF_RF_MASK | HF_INHIBIT_IRQ_MASK |
                     HF_MPX_EN_MASK | HF_MPX_IU_MASK)) ||
        (cf & (CF_COUNT_MASK | CF_NOIRQ | CF_SINGLE_STEP | CF_BP_PAGE |
               CF_MEMI_ONLY | CF_PARALLEL | CF_USE_ICOUNT))) {
        return;
    }
    XwdTranslation *t = tcg_malloc(sizeof(*t));
    memset(t, 0, sizeof(*t));
    t->capture.version = XWD_VERSION;
    t->capture.valid = true;
    t->capture.pc = s->base.pc_first;
    t->capture.pcrel = cf & CF_PCREL;
    t->layout = (XwdLayout) {
        .verify = x86_wasm_direct_verify,
        .cc_all = helper_cc_compute_all,
        .regs = offsetof(CPUX86State, regs),
        .eip = offsetof(CPUX86State, eip),
        .cc_dst = offsetof(CPUX86State, cc_dst),
        .cc_src = offsetof(CPUX86State, cc_src),
        .cc_src2 = offsetof(CPUX86State, cc_src2),
        .cc_op = offsetof(CPUX86State, cc_op),
        .can_do_io = offsetof(CPUState, neg.can_do_io) - sizeof(CPUState),
        .icount_decr = offsetof(CPUState, neg.icount_decr) - sizeof(CPUState),
        .cc_add = { CC_OP_ADDB, CC_OP_ADDW, CC_OP_ADDL },
        .cc_sub = { CC_OP_SUBB, CC_OP_SUBW, CC_OP_SUBL },
        .cc_logic = { CC_OP_LOGICB, CC_OP_LOGICW, CC_OP_LOGICL },
        .cc_adc = { CC_OP_ADCB, CC_OP_ADCW, CC_OP_ADCL },
        .cc_sbb = { CC_OP_SBBB, CC_OP_SBBW, CC_OP_SBBL },
        .cc_inc = { CC_OP_INCB, CC_OP_INCW, CC_OP_INCL },
        .cc_dec = { CC_OP_DECB, CC_OP_DECW, CC_OP_DECL },
        .cc_shl = { CC_OP_SHLB, CC_OP_SHLW, CC_OP_SHLL },
        .cc_sar = { CC_OP_SARB, CC_OP_SARW, CC_OP_SARL },
        .cc_eflags = CC_OP_EFLAGS,
    };
    s->wasm_direct = t;
    tcg_gen_wasm_direct(t);
#endif
}

static void x86_wasm_direct_bytes(DisasContext *s, uint64_t value, unsigned n)
{
    XwdCapture *c = s->wasm_direct ? &s->wasm_direct->capture : NULL;
    if (!c || !c->valid) {
        return;
    }
    if (!n || n > sizeof(value) || c->size > XWD_MAX_BYTES ||
        (uint32_t)(s->pc - n - c->pc) != c->size ||
        n > XWD_MAX_BYTES - c->size) {
        c->valid = false;
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        c->bytes[c->size++] = value >> (8 * i);
    }
}

static void x86_wasm_direct_insn(DisasContext *s)
{
    XwdCapture *c = s->wasm_direct ? &s->wasm_direct->capture : NULL;
    if (c && c->valid) {
        if (c->count == XWD_MAX_INSNS ||
            c->size != (uint32_t)(s->pc - c->pc)) {
            c->valid = false;
        } else {
            c->end[c->count++] = c->size;
        }
    }
}

static void x86_wasm_direct_jump(DisasContext *s, unsigned slot, uint32_t pc)
{
    if (s->wasm_direct) {
        XwdCapture *c = &s->wasm_direct->capture;
        assert(slot < 2);
        if ((c->jump_valid & (1u << slot)) && c->jump_pc[slot] != pc) {
            c->valid = false;
        }
        c->jump_valid |= 1u << slot;
        c->jump_pc[slot] = pc;
    }
}

static void x86_wasm_direct_stop(DisasContext *s)
{
    if (s->wasm_direct) {
        XwdCapture *c = &s->wasm_direct->capture;
        c->valid &= !s->base.plugin_enabled &&
                    c->count == s->base.num_insns &&
                    (s->base.is_jmp == DISAS_TOO_MANY ||
                     s->base.is_jmp == DISAS_NORETURN);
        c->fallthrough = s->base.is_jmp == DISAS_TOO_MANY;
    }
}
