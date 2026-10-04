/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_XEMU_WASM_DIRECT_H
#define QEMU_XEMU_WASM_DIRECT_H

/* Pool-owned translation metadata; never a guest/TB-header pointer cache. */
#define XWD_VERSION 3
#define XWD_MAX_BYTES 512u
#define XWD_MAX_INSNS 64u
#define XWD_MAX_CODE 16384u

typedef struct XwdCapture {
    uint32_t version, pc;
    uint16_t size, count;
    bool valid, fallthrough, pcrel;
    /* Original frontend's canonical non-PCREL goto targets for checking. */
    uint8_t jump_valid;
    uint32_t jump_pc[2];
    uint16_t end[XWD_MAX_INSNS];
    uint8_t bytes[XWD_MAX_BYTES];
} XwdCapture;

typedef enum XwdOp {
    XWD_MOV, XWD_MOVZX, XWD_MOVSX, XWD_XCHG, XWD_LEA, XWD_NOP,
    XWD_ADD, XWD_SUB, XWD_AND, XWD_OR, XWD_XOR, XWD_CMP, XWD_TEST,
    XWD_NOT, XWD_NEG, XWD_JMP,
    XWD_ADC, XWD_SBB, XWD_INC, XWD_DEC, XWD_CLC, XWD_STC, XWD_CMC,
    XWD_JCC, XWD_CMOV, XWD_SETCC,
    /* Memory stage (xwd_decode_ex with allow_memory). */
    XWD_PUSH, XWD_POP,
    /* CALL rel32: PUSH of the return address, then a direct jump */
    XWD_CALL,
} XwdOp;

typedef struct XwdInsn {
    uint32_t imm;
    uint8_t op, width, src_width, dst, src, cond;
    bool immediate;
    /*
     * LEA and memory operands: 0xff means absent. LEA computes the address
     * (displacement in imm) without accessing memory; a memory operand
     * (mem) keeps its displacement in disp so imm stays the immediate.
     */
    uint8_t base, index, scale;
    /*
     * Memory operand (rm side of ModRM): mem_store is the one access's
     * direction, mem_width its size. MOV/MOVZX/MOVSX/ALU/CMP/TEST read it,
     * MOV [m] writes it; PUSH/POP access [ESP-4]/[ESP] (no ModRM).
     */
    bool mem, mem_store;
    uint8_t mem_width;
    uint32_t disp;
} XwdInsn;

typedef struct XwdPlan {
    uint32_t version, pc, delta, fall_delta;
    uint16_t count;
    bool pcrel, conditional, needs_flags;
    uint16_t accesses;      /* guest memory accesses (one per mem insn) */
    XwdInsn insn[XWD_MAX_INSNS];
} XwdPlan;

/* Target offsets/constants, supplied by the real x86 frontend. */
struct XwdState;
typedef struct XwdLayout {
    void (*verify)(void *env, struct XwdState *state, uint32_t check_pc,
                   uint32_t actual_pc);
    uint32_t (*cc_all)(uint32_t dst, uint32_t src, uint32_t src2, int op);
    uint32_t regs, eip, cc_dst, cc_src, cc_src2, cc_op;
    int32_t can_do_io, icount_decr;
    uint32_t cc_add[3], cc_sub[3], cc_logic[3];
    uint32_t cc_adc[3], cc_sbb[3], cc_inc[3], cc_dec[3], cc_eflags;
    /* Optional sampler scope, resolved at code generation; zero disables. */
    uint32_t phase_ptr, phase_cc, phase_generated;
} XwdLayout;

#define XWD_CHECK_PC 1u
#define XWD_CHECK_EDGE 2u
#define XWD_CHECK_TAKEN 4u
#define XWD_FEATURE_REGION 4u
#define XWD_FEATURE_TAKEN 8u

typedef struct XwdState {
    uint32_t regs[8], cc_dst, cc_src, cc_src2, cc_op, eip, pending, features;
} XwdState;

typedef struct XwdTranslation {
    XwdCapture capture;
    XwdLayout layout;
} XwdTranslation;

typedef struct XwdCode {
    uint32_t size;
    bool valid;
    uint8_t bytes[XWD_MAX_CODE];
} XwdCode;

static inline int xemu_wasm_direct_mode(void)
{
#if defined(EMSCRIPTEN) && defined(CONFIG_TCG_WASM_JIT)
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("XEMU_WASM_DIRECT_X86");
        mode = e && (*e == '1' || *e == '2') ? *e - '0' : 0;
    }
    return mode;
#else
    return 0;
#endif
}

/* Independently gate the carry/condition stage over the register scaffold. */
static inline bool xemu_wasm_direct_flags_enabled(void)
{
#if defined(EMSCRIPTEN) && defined(CONFIG_TCG_WASM_JIT)
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_DIRECT_FLAGS");
        enabled = e && *e == '1';
    }
    return enabled;
#else
    return false;
#endif
}

/* Region substitution is a separate, default-off correctness stage. */
static inline bool xemu_wasm_direct_regions_enabled(void)
{
#if defined(EMSCRIPTEN) && defined(CONFIG_TCG_WASM_JIT)
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_DIRECT_REGIONS");
        enabled = e && *e == '1';
    }
    return enabled;
#else
    return false;
#endif
}

#endif
