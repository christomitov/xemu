/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_XEMU_WASM_DIRECT_H
#define QEMU_XEMU_WASM_DIRECT_H

/* Pool-owned translation metadata; never a guest/TB-header pointer cache. */
#define XWD_VERSION 1
#define XWD_MAX_BYTES 512u
#define XWD_MAX_INSNS 64u
#define XWD_MAX_CODE 16384u

typedef struct XwdCapture {
    uint32_t version, pc;
    uint16_t size, count;
    bool valid, fallthrough, pcrel;
    uint16_t end[XWD_MAX_INSNS];
    uint8_t bytes[XWD_MAX_BYTES];
} XwdCapture;

typedef enum XwdOp {
    XWD_MOV, XWD_MOVZX, XWD_MOVSX, XWD_XCHG, XWD_LEA, XWD_NOP,
    XWD_ADD, XWD_SUB, XWD_AND, XWD_OR, XWD_XOR, XWD_CMP, XWD_TEST,
    XWD_NOT, XWD_NEG, XWD_JMP,
} XwdOp;

typedef struct XwdInsn {
    uint32_t imm;
    uint8_t op, width, src_width, dst, src;
    bool immediate;
    /* LEA only: 0xff means absent, no guest memory access. */
    uint8_t base, index, scale;
} XwdInsn;

typedef struct XwdPlan {
    uint32_t version, pc, delta;
    uint16_t count;
    bool pcrel;
    XwdInsn insn[XWD_MAX_INSNS];
} XwdPlan;

/* Target offsets/constants, supplied by the real x86 frontend. */
struct XwdState;
typedef struct XwdLayout {
    void (*verify)(void *env, struct XwdState *state, uint32_t check_pc);
    uint32_t regs, eip, cc_dst, cc_src, cc_src2, cc_op;
    int32_t can_do_io;
    uint32_t cc_add[3], cc_sub[3], cc_logic[3];
} XwdLayout;

typedef struct XwdState {
    uint32_t regs[8], cc_dst, cc_src, cc_src2, cc_op, eip, pending;
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

#endif
