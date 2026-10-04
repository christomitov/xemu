/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included after gen_* definitions, before the decoder. No extra fetches. */
#ifdef CONFIG_TCG_WASM_JIT
static bool wasm_census_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_INSN_CENSUS");
        enabled = e && *e == '1';
    }
    return enabled;
}

static bool wasm_census_gen_in(X86GenFunc gen, const X86GenFunc *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (gen == list[i]) {
            return true;
        }
    }
    return false;
}

static void wasm_census_record(DisasContext *s, const X86DecodedInsn *d)
{
    /* These are proposal-v1 admission rules, not an implemented translator. */
    static const X86GenFunc candidate[] = {
        gen_MOV, gen_LEA, gen_ADD, gen_ADC, gen_SUB, gen_SBB,
        gen_AND, gen_OR, gen_XOR, gen_INC, gen_DEC, gen_NEG, gen_NOT,
        gen_SHL, gen_SHR, gen_SAR, gen_ROL, gen_ROR, gen_IMUL3,
        gen_Jcc, gen_JMP, gen_JMP_m, gen_CMOVcc, gen_SETcc, gen_NOP,
        gen_PUSH, gen_POP, gen_CALL, gen_CALL_m, gen_RET, gen_LEAVE,
        gen_CBW, gen_CWD, gen_CLC, gen_STC, gen_CMC,
    };
    static const X86GenFunc integer[] = {
        gen_AAA, gen_AAD, gen_AAM, gen_AAS, gen_DAA, gen_DAS,
        gen_ADCX, gen_ADOX, gen_ANDN, gen_BEXTR, gen_BLSI, gen_BLSMSK,
        gen_BLSR, gen_BSF, gen_BSR, gen_BSWAP, gen_BT, gen_BTC,
        gen_BTR, gen_BTS, gen_BZHI, gen_CMPXCHG, gen_CMPXCHG8B,
        gen_CMPXCHG16B, gen_CMPccXADD, gen_CRC32, gen_DIV, gen_IDIV,
        gen_IMUL, gen_MUL, gen_MULX, gen_MOVBE, gen_POPCNT,
        gen_LZCNT, gen_TZCNT, gen_PDEP, gen_PEXT, gen_RCL, gen_RCR,
        gen_RORX, gen_SARX, gen_SHLD, gen_SHRD, gen_SHLX, gen_SHRX,
        gen_XADD, gen_XCHG, gen_XLAT, gen_JCXZ, gen_LOOP, gen_LOOPE,
        gen_LOOPNE, gen_ENTER, gen_PUSHA, gen_POPA, gen_LAHF, gen_SAHF,
        gen_CLD, gen_STD,
    };
    static const X86GenFunc system[] = {
        gen_multi0F, gen_ARPL, gen_CALLF, gen_CALLF_m, gen_JMPF,
        gen_JMPF_m, gen_RETF, gen_INT, gen_INT1, gen_INT3, gen_INTO,
        gen_IRET, gen_IN, gen_OUT, gen_CLI, gen_STI, gen_HLT,
        gen_CLTS, gen_CPUID, gen_RDMSR, gen_RDPMC, gen_RDTSC,
        gen_WRMSR, gen_RSM, gen_SYSCALL, gen_SYSENTER, gen_SYSEXIT,
        gen_SYSRET, gen_PUSHF, gen_POPF, gen_LDS, gen_LES, gen_LFS,
        gen_LGS, gen_LSS, gen_LFENCE, gen_MFENCE, gen_SFENCE, gen_PAUSE,
    };
    static const X86GenFunc strings[] = {
        gen_MOVS, gen_CMPS, gen_STOS, gen_LODS, gen_SCAS, gen_INS, gen_OUTS,
    };
    static const X86GenFunc stack[] = {
        gen_PUSH, gen_POP, gen_CALL, gen_CALL_m, gen_RET, gen_LEAVE,
    };
    X86GenFunc gen = d->e.gen;
    unsigned mask = 0;
    bool memory = false;
    bool admit;

    if (!s->wasm_census_word) {
        return;
    }
    s->wasm_census_classified = true;
    for (unsigned i = 0; i < ARRAY_SIZE(d->op); i++) {
        switch (d->op[i].unit) {
        case X86_OP_SSE:
            mask |= XWC_SSE;
            break;
        case X86_OP_MMX:
            mask |= XWC_MMX;
            break;
        case X86_OP_SEG:
        case X86_OP_CR:
        case X86_OP_DR:
            mask |= XWC_SYSTEM;
            break;
        default:
            break;
        }
        memory |= d->op[i].has_ea;
    }
    if (gen == gen_x87 || gen == gen_WAIT) {
        mask |= XWC_X87;
    }
    if (gen == gen_EMMS || gen == gen_3dnow) {
        mask |= XWC_MMX;
    }
    if (gen == gen_LDMXCSR || gen == gen_STMXCSR ||
        gen == gen_VZEROALL || gen == gen_VZEROUPPER) {
        mask |= XWC_SSE;
    }
    if (gen == gen_FXSAVE || gen == gen_FXRSTOR || gen == gen_XSAVE ||
        gen == gen_XSAVEOPT || gen == gen_XRSTOR) {
        mask |= XWC_SSE | XWC_X87 | XWC_MMX | XWC_SYSTEM;
    }
#define IN(list) wasm_census_gen_in(gen, list, ARRAY_SIZE(list))
    if (IN(system) || gen == gen_INS || gen == gen_OUTS ||
        (d->e.check & (X86_CHECK_cpl0 | X86_CHECK_iopl | X86_CHECK_smm))) {
        mask |= XWC_SYSTEM;
    }
    if (IN(strings)) {
        mask |= XWC_STRING;
    }
    admit = IN(candidate);
    if (!mask && !admit && !IN(integer)) {
        mask = XWC_OTHER;
    }
    *s->wasm_census_word |= mask;
    /*
     * Conservatively withhold direct self-loop TBs from expected savings.
     * Includes the load/compare fence-poll shapes without any PC/game list.
     * Productive self-loops also land here: this is NOT proof of idleness,
     * nor a detector for all multi-TB/indirect wait loops. No extra fetches.
     */
    if (xemu_wasm_insn_profile_enabled() &&
        (gen == gen_Jcc || gen == gen_JMP) &&
        CODE32(s) && !CODE64(s) && s->dflag == MO_32 && !s->cs_base &&
        (uint32_t)(s->pc + d->immediate) == (uint32_t)s->base.pc_first) {
        *s->wasm_census_word |= XWC_SELFLOOP;
    }
    if (mask || !admit || s->aflag != MO_32 || s->dflag != MO_32 ||
        s->override >= 0 ||
        (s->prefix & (PREFIX_LOCK | PREFIX_REPZ | PREFIX_REPNZ | PREFIX_VEX))) {
        *s->wasm_census_word &= ~XWC_ELIGIBLE;
    }
    if ((memory && gen != gen_LEA) || IN(stack)) {
        *s->wasm_census_word &= ~XWC_REGONLY;
    }
#undef IN
}
#endif
