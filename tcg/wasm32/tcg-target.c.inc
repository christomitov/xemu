/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tiny Code Generator for QEMU: WebAssembly JIT backend for wasm32 hosts.
 *
 * Ported to the QEMU 10.2 TCGOutOp backend interface from the WebAssembly
 * backend by Kohei Tokunaga (ktock/qemu-wasm, "tcg: Add a TCG backend for
 * WebAssembly"), which is based on the TCI backend:
 *
 * Copyright (c) 2009, 2011 Stefan Weil
 * Copyright (c) 2025 NTT Corporation
 *
 * Every TB is emitted twice from the same register allocation:
 *  - as TCI bytecode (exactly the encoding of tcg/tci/tcg-target.c.inc,
 *    executed by tcg/tci.c), used while a block is cold, and
 *  - as the body of a WebAssembly function, assembled into a module at the
 *    end of the TB and instantiated by tcg/wasm32.c once the block is hot.
 *
 * TCG registers are mutable i64 globals of the module and the call/spill
 * stack is memory, so a function can return in the middle of a block and be
 * re-entered later. That is what Asyncify needs: when a helper suspends (an
 * emscripten fiber switch for a coroutine, emscripten_sleep), the generated
 * code sees the unwinding state and returns; on rewind it jumps straight
 * back to the call (see BLOCK_PTR below) and calls the helper again, which
 * then resumes.
 *
 * Control flow: the function body is a loop over a chain of
 * "if (BLOCK_PTR <= n) { ... }" regions. TCG labels and helper calls start
 * a new region. A branch sets BLOCK_PTR to the target region and either
 * breaks out of the current region (forward) or continues the loop
 * (backward). Branch targets are patched in when the module is written.
 */

#include "../wasm32.h"
#include "qemu/xemu-wasm-stats.h"
#include "qemu/xemu-wasm-direct.h"
#include "qemu/xemu-wasm-direct-memory.h"

static XwdMemoryCapture *wasm_direct_memory;
static const XwdTranslation *wasm_direct_translation;
static XwdCode *wasm_direct_code;
static XwdCode *wasm_direct_blueprint;
static uint32_t wasm_direct_call_pos;
static XwdState wasm_direct_expected;
static void wasm_direct_verify(TCGContext *s, int slot);
#include "exec/tlb-flags.h"

/* Used for function call generation. */
#define TCG_TARGET_CALL_STACK_OFFSET    0
#define TCG_TARGET_STACK_ALIGN          8
#define TCG_TARGET_CALL_ARG_I32         TCG_CALL_ARG_NORMAL
#define TCG_TARGET_CALL_ARG_I64         TCG_CALL_ARG_NORMAL
#define TCG_TARGET_CALL_ARG_I128        TCG_CALL_ARG_NORMAL
#define TCG_TARGET_CALL_RET_I128        TCG_CALL_RET_NORMAL

/* Only the host FP ops (TCG_OPF_FP) are not in the TCGOutOp tables. */
static TCGConstraintSetIndex
tcg_target_op_def(TCGOpcode op, TCGType type, unsigned flags)
{
    switch (op) {
    case INDEX_op_flcr:
        return C_O0_I1(r);
    case INDEX_op_st80f_f32:
    case INDEX_op_st80f_f64:
        return C_O0_I2(r, r);
    case INDEX_op_add_f32:
    case INDEX_op_add_f64:
    case INDEX_op_sub_f32:
    case INDEX_op_sub_f64:
    case INDEX_op_mul_f32:
    case INDEX_op_mul_f64:
    case INDEX_op_div_f32:
    case INDEX_op_div_f64:
    case INDEX_op_com_f32:
    case INDEX_op_com_f64:
        return C_O1_I2(r, r, r);
    case INDEX_op_ld80f_f32:
    case INDEX_op_ld80f_f64:
    case INDEX_op_abs_f32:
    case INDEX_op_abs_f64:
    case INDEX_op_chs_f32:
    case INDEX_op_chs_f64:
    case INDEX_op_cos_f32:
    case INDEX_op_cos_f64:
    case INDEX_op_sin_f32:
    case INDEX_op_sin_f64:
    case INDEX_op_sqrt_f32:
    case INDEX_op_sqrt_f64:
    case INDEX_op_cvt32f_f64:
    case INDEX_op_cvt32f_i32:
    case INDEX_op_cvt32f_i64:
    case INDEX_op_cvt32i_f32:
    case INDEX_op_cvt32i_f64:
    case INDEX_op_cvt64f_f32:
    case INDEX_op_cvt64f_i32:
    case INDEX_op_cvt64f_i64:
    case INDEX_op_cvt64i_f32:
    case INDEX_op_cvt64i_f64:
    case INDEX_op_mov32f_i32:
    case INDEX_op_mov32i_f32:
    case INDEX_op_mov64f_i64:
    case INDEX_op_mov64i_f64:
    case INDEX_op_mov_f32:
    case INDEX_op_mov_f64:
        return C_O1_I1(r, r);
    default:
        return C_NotImplemented;
    }
}

static const int tcg_target_reg_alloc_order[] = {
    TCG_REG_R4,
    TCG_REG_R5,
    TCG_REG_R6,
    TCG_REG_R7,
    TCG_REG_R8,
    TCG_REG_R9,
    TCG_REG_R10,
    TCG_REG_R11,
    TCG_REG_R12,
    TCG_REG_R13,
    TCG_REG_R14,
    TCG_REG_R15,
    /* Either 2 or 4 of these are call clobbered, so use them last. */
    TCG_REG_R3,
    TCG_REG_R2,
    TCG_REG_R1,
    TCG_REG_R0,
};

/*
 * No call arguments via registers. All go to the "stack" in 64-bit slots,
 * exactly like TCI, so tci.c can call helpers unchanged.
 */
static const int tcg_target_call_iarg_regs[] = { };

static TCGReg tcg_target_call_oarg_reg(TCGCallReturnKind kind, int slot)
{
    tcg_debug_assert(kind == TCG_CALL_RET_NORMAL);
    tcg_debug_assert(slot >= 0 && slot < 128 / TCG_TARGET_REG_BITS);
    return TCG_REG_R0 + slot;
}

#ifdef CONFIG_DEBUG_TCG
static const char *const tcg_target_reg_names[TCG_TARGET_NB_REGS] = {
    "r00", "r01", "r02", "r03", "r04", "r05", "r06", "r07",
    "r08", "r09", "r10", "r11", "r12", "r13", "r14", "r15",
};
#endif

/*
 * ---------------------------------------------------------------------
 * WebAssembly code emission
 * ---------------------------------------------------------------------
 */

/* Globals of the generated module: TCG registers 0..15, then BLOCK_PTR. */
#define BLOCK_PTR_IDX       TCG_TARGET_NB_REGS
#define NUM_GLOBALS         (TCG_TARGET_NB_REGS + 1)

/* Locals: the WasmContext pointer (param), 4 x i32, 2 x i64, 1 x f64. */
#define CTX_IDX             0
#define L32_0               1
#define L32_1               2
#define L32_2               3
#define L32_3               4
#define L64_0               5
#define L64_1               6
#define LF64_0              7
/*
 * TCG registers live in i64 locals (V8 can keep those in machine registers;
 * every global access is a load/store through the instance). The module
 * globals of the same index are only their backing store across an Asyncify
 * unwind: saved before any call that may suspend, reloaded on a rewind.
 */
#define LREG(r)             (8 + (r))
/* BLOCK_PTR (the resumable region to run) as a local, too: saved to its
 * global with the registers before a call that may unwind */
#define LBLOCK              LREG(TCG_TARGET_NB_REGS)

/* Function indices: imports first, then the TB function itself. */
#define CHECK_UNWINDING_IDX 0   /* helper.u: 1 unless Asyncify unwinds */
#define CHAIN_S0_IDX        1   /* chain.s0/s1: linked goto_tb successors */
#define CHAIN_CALL_IDX      3   /* chain.call(ctx, fidx): rewind forwarding */
#define HELPER_IDX_START    4   /* imported TCG helpers */

/* Wasm opcodes */
#define W_UNREACHABLE   0x00
#define W_BLOCK         0x02
#define W_LOOP          0x03
#define W_IF            0x04
#define W_ELSE          0x05
#define W_END           0x0b
#define W_BR_IF         0x0d
#define W_BR            0x0c
#define W_RETURN        0x0f
#define W_CALL          0x10
#define W_RETURN_CALL_INDIRECT 0x13
#define W_RETURN_CALL   0x12

/*
 * Successor linking (off by default, XEMU_WASM_LINK=1 enables: measured 2-3% slower): each module imports its two
 * goto_tb successors as functions (chain.s0/s1), bound when it is
 * instantiated, and tail-calls them while the link is still valid, instead
 * of returning to the C dispatcher for every block transition. Unlike
 * importing the whole function table this keeps instantiation cheap.
 */
static int wasm_link = -1;
static bool wasm_link_enabled(void)
{
    if (wasm_link < 0) {
        const char *e = getenv("XEMU_WASM_LINK");
        wasm_link = e && *e == '1';
    }
    return wasm_link;
}
static WasmTBHeader *cur_hdr;   /* header of the TB being generated */
#define W_SELECT        0x1b
#define W_LOCAL_GET     0x20
#define W_LOCAL_SET     0x21
#define W_LOCAL_TEE     0x22
#define W_GLOBAL_GET    0x23
#define W_GLOBAL_SET    0x24
#define W_I32_LOAD      0x28
#define W_I64_LOAD      0x29
#define W_I64_LOAD8_S   0x30
#define W_I64_LOAD8_U   0x31
#define W_I64_LOAD16_S  0x32
#define W_I64_LOAD16_U  0x33
#define W_I64_LOAD32_S  0x34
#define W_I64_LOAD32_U  0x35
#define W_I32_STORE     0x36
#define W_I64_STORE     0x37
#define W_I64_STORE8    0x3c
#define W_I64_STORE16   0x3d
#define W_I64_STORE32   0x3e
#define W_I32_CONST     0x41
#define W_I64_CONST     0x42
#define W_I32_EQZ       0x45
#define W_I32_EQ        0x46
#define W_I32_LT_U      0x49
#define W_I32_NE        0x47
#define W_I64_EQZ       0x50
#define W_I64_LE_U      0x58
#define W_I32_CLZ       0x67
#define W_I32_CTZ       0x68
#define W_I32_ADD       0x6a
#define W_I32_MUL       0x6c
#define W_I32_DIV_S     0x6d
#define W_I32_DIV_U     0x6e
#define W_I32_REM_S     0x6f
#define W_I32_REM_U     0x70
#define W_I32_AND       0x71
#define W_I32_XOR       0x73
#define W_I32_OR        0x72
#define W_I32_SHL       0x74
#define W_I32_SHR_U     0x76
#define W_I32_ROTL      0x77
#define W_I32_ROTR      0x78
#define W_I64_CLZ       0x79
#define W_I64_CTZ       0x7a
#define W_I64_POPCNT    0x7b
#define W_I64_ADD       0x7c
#define W_I64_SUB       0x7d
#define W_I64_MUL       0x7e
#define W_I64_DIV_S     0x7f
#define W_I64_DIV_U     0x80
#define W_I64_REM_S     0x81
#define W_I64_REM_U     0x82
#define W_I64_AND       0x83
#define W_I64_OR        0x84
#define W_I64_XOR       0x85
#define W_I64_SHL       0x86
#define W_I64_SHR_S     0x87
#define W_I64_SHR_U     0x88
#define W_I64_ROTL      0x89
#define W_I64_ROTR      0x8a
#define W_I32_WRAP_I64  0xa7
#define W_I64_EXTEND_I32_S 0xac
#define W_I64_EXTEND_I32_U 0xad
#define W_I32_EXTEND16_S 0xc1
#define W_I64_EXTEND32_S 0xc4

#define W_BLOCKTYPE_VOID 0x40
#define W_TYPE_I32       0x7f
#define W_TYPE_I64       0x7e

/*
 * Wasm code is generated into a list of tcg_malloc()ed chunks (freed with
 * the rest of the per-TB pool) and copied behind the TCI code at the end.
 */
#define BUF_MAX 4096
typedef struct LinkedBuf {
    struct LinkedBuf *next;
    uint32_t size;
    uint8_t data[BUF_MAX];
} LinkedBuf;

static LinkedBuf *new_linked_buf(void)
{
    LinkedBuf *p = tcg_malloc(sizeof(LinkedBuf));
    p->size = 0;
    p->next = NULL;
    return p;
}

static inline LinkedBuf *linked_buf_out8(LinkedBuf *buf, uint8_t v)
{
    if (buf->size == BUF_MAX) {
        buf->next = new_linked_buf();
        buf = buf->next;
    }
    buf->data[buf->size++] = v;
    return buf;
}

static int linked_buf_len(LinkedBuf *buf)
{
    int total = 0;
    for (LinkedBuf *p = buf; p; p = p->next) {
        total += p->size;
    }
    return total;
}

static void linked_buf_write(LinkedBuf *buf, uint8_t *dst)
{
    for (LinkedBuf *p = buf; p; p = p->next) {
        memcpy(dst, p->data, p->size);
        dst += p->size;
    }
}

/* Code of the TB function. */
static __thread LinkedBuf *code_buf_root, *code_buf_cur;

/* Relocations of the current TB (see WasmReloc), in emission order. */
typedef struct RelocNode {
    struct RelocNode *next;
    WasmReloc r;
    int region;     /* GOTO: region of the site; r.depth is local until end */
} RelocNode;
static __thread RelocNode *relocs, *relocs_last;
static __thread int relocs_num;

static int wasm_code_len(void);
static int block_idx_cur(void);
static void wasm_add_reloc(uint8_t kind, uint8_t arg, uint16_t depth)
{
    RelocNode *e = tcg_malloc(sizeof(RelocNode));
    e->r.off = wasm_code_len();
    e->r.kind = kind;
    e->r.arg = arg;
    e->r.depth = depth;
    e->region = block_idx_cur();
    e->next = NULL;
    if (relocs_last) {
        relocs_last->next = e;
    } else {
        relocs = e;
    }
    relocs_last = e;
    relocs_num++;
}
/* Function types of the imported helpers. */
static __thread LinkedBuf *types_buf_root, *types_buf_cur;

static inline void wasm8(TCGContext *s, uint8_t v)
{
    code_buf_cur = linked_buf_out8(code_buf_cur, v);
}

static int wasm_code_len(void)
{
    return linked_buf_len(code_buf_root);
}

static void wasm_uleb(TCGContext *s, uint32_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        wasm8(s, b | (v ? 0x80 : 0));
    } while (v);
}

/* No hint survives a C boundary, including nonsuspending helper calls. */
static void wasm_tlb_hint_drop(TCGContext *s)
{
    if (wasm32_tlb_hint_enabled()) {
        wasm8(s, W_I32_CONST);
        wasm8(s, 0);
        wasm8(s, W_LOCAL_SET);
        wasm_uleb(s, WASM_TLB_HINT_MMU_LOCAL);
    }
}

/* All ordinary imported calls use this path; helper LEBs are relocatable. */
static void wasm_call_idx(TCGContext *s, int idx)
{
    wasm_tlb_hint_drop(s);
    wasm8(s, 0x10);             /* call */
    if (idx < HELPER_IDX_START) {
        wasm_uleb(s, idx);
        return;
    }
    wasm_add_reloc(WASM_RELOC_CALL, idx - HELPER_IDX_START, 0);
    for (int i = 0; i < 5; i++) {
        wasm8(s, (idx & 0x7f) | (i < 4 ? 0x80 : 0));
        idx >>= 7;
    }
}

static void wasm_sleb(TCGContext *s, int64_t v)
{
    bool more = true;
    while (more) {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
            more = false;
        } else {
            b |= 0x80;
        }
        wasm8(s, b);
    }
}

static inline void wasm_i32_const(TCGContext *s, int32_t v)
{
    wasm8(s, W_I32_CONST);
    wasm_sleb(s, v);
}

static inline void wasm_i64_const(TCGContext *s, int64_t v)
{
    wasm8(s, W_I64_CONST);
    wasm_sleb(s, v);
}

/*
 * TCG registers this TB really uses (outside the Asyncify register snapshot
 * code). Save/restore pairs of the others are replaced by nops at the end
 * of the TB, so V8 does not see them as live (see wasm_snapshot_site).
 */
static __thread uint32_t used_regs;
static __thread bool in_snapshot;

static inline void wasm_var(TCGContext *s, uint8_t op, uint32_t idx)
{
    if (!in_snapshot && (op == 0x20 || op == 0x21 || op == 0x22) &&
        idx >= LREG(0) && idx < LREG(TCG_TARGET_NB_REGS)) {
        used_regs |= 1u << (idx - LREG(0));
    }
    wasm8(s, op);
    wasm_uleb(s, idx);
}

/* A 4-byte snapshot pair (local.get/global.set or global.get/local.set). */
typedef struct SnapshotSite {
    struct SnapshotSite *next;
    uint32_t pos;
    uint8_t reg;
} SnapshotSite;
static __thread SnapshotSite *snapshot_sites;

static int wasm_code_len(void);
static void wasm_snapshot_site(int reg)
{
    SnapshotSite *e = tcg_malloc(sizeof(*e));
    e->pos = wasm_code_len();
    e->reg = reg;
    e->next = snapshot_sites;
    snapshot_sites = e;
}

/* Memory access with alignment hint 0 (byte) and a static offset. */
static inline void wasm_mem(TCGContext *s, uint8_t op, uint32_t offset)
{
    wasm8(s, op);
    wasm8(s, 0);
    wasm_uleb(s, offset);
}

static inline void wasm_if(TCGContext *s, uint8_t blocktype)
{
    wasm8(s, W_IF);
    wasm8(s, blocktype);
}

/*
 * "if" with a branch hint: V8 compiles the unlikely side as deferred code,
 * moving spills and register shuffles off the hot path.
 */
static int wasm_hints = -1;
static bool wasm_hints_enabled(void)
{
    if (wasm_hints < 0) {
        const char *e = getenv("XEMU_WASM_HINTS");
        wasm_hints = !(e && *e == '0');
    }
    return wasm_hints;
}

static void wasm_if_hint(TCGContext *s, uint8_t blocktype, bool likely)
{
    wasm_add_reloc(WASM_RELOC_HINT, likely, 0);
    wasm_if(s, blocktype);
}

static inline void wasm_br(TCGContext *s, uint32_t depth)
{
    wasm8(s, W_BR);
    wasm_uleb(s, depth);
}

/* Push TCG register @r as i64. */
static inline void wasm_get_r(TCGContext *s, TCGReg r)
{
    wasm_var(s, W_LOCAL_GET, LREG(r));
}

/* Push the low 32 bits of TCG register @r as i32. */
static inline void wasm_get_r32(TCGContext *s, TCGReg r)
{
    wasm_get_r(s, r);
    wasm8(s, W_I32_WRAP_I64);
}

/* Pop i64 into TCG register @r. */
static inline void wasm_set_r(TCGContext *s, TCGReg r)
{
    wasm_var(s, W_LOCAL_SET, LREG(r));
}

/* Registers -> their globals, before a call that may unwind (Asyncify). */
static void wasm_save_regs(TCGContext *s)
{
    in_snapshot = true;
    for (int r = 0; r < TCG_TARGET_NB_REGS; r++) {
        wasm_snapshot_site(r);
        wasm_var(s, W_LOCAL_GET, LREG(r));
        wasm_var(s, W_GLOBAL_SET, r);
    }
    in_snapshot = false;
    wasm_var(s, W_LOCAL_GET, LBLOCK);
    wasm_var(s, W_GLOBAL_SET, BLOCK_PTR_IDX);
    if (wasm32_ic_enabled()) {
        /* A region member's standalone instance need not own these globals. */
        wasm_var(s, W_LOCAL_GET, CTX_IDX);
        wasm_var(s, W_GLOBAL_GET, WASM_IC_SELF_GLOBAL);
        wasm_mem(s, W_I32_STORE, WASM_CTX_REWIND_FUNC_OFF);
    }
}

/* Push a host address: register @base (i32) plus @offset. */
static int wasm_addr(TCGContext *s, TCGReg base, intptr_t offset)
{
    wasm_get_r32(s, base);
    if ((int32_t)offset < 0) {
        wasm_i32_const(s, (int32_t)offset);
        wasm8(s, W_I32_ADD);
        return 0;
    }
    return (int32_t)offset;
}

static void wasm_ctx_load_i32(TCGContext *s, int off)
{
    wasm_var(s, W_LOCAL_GET, CTX_IDX);
    wasm_mem(s, W_I32_LOAD, off);
}

static void wasm_ctx_store_i32_const(TCGContext *s, int off, int32_t v)
{
    wasm_var(s, W_LOCAL_GET, CTX_IDX);
    wasm_i32_const(s, v);
    wasm_mem(s, W_I32_STORE, off);
}

/* XSTAT: increment a 64-bit counter (plain load/add/store, one writer). */
static void wasm_count(TCGContext *s, uint64_t *ctr)
{
    uint32_t addr = (uint32_t)(uintptr_t)ctr;

    wasm_i32_const(s, addr);
    wasm_i32_const(s, addr);
    wasm_mem(s, W_I64_LOAD, 0);
    wasm_i64_const(s, 1);
    wasm8(s, W_I64_ADD);
    wasm_mem(s, W_I64_STORE, 0);
}

/* Resolved while generating code: lean TBs gain no runtime policy branch. */
static void wasm_count_tb_stat(TCGContext *s, uint64_t *ctr)
{
#ifdef EMSCRIPTEN
    if (xemu_wasm_tb_stats_enabled()) {
        wasm_count(s, ctr);
    }
#endif
}

/* One entry marker survives self-loops, IC tails and copied region bodies. */
static void wasm_census(TCGContext *s, uint32_t word)
{
    tcg_debug_assert(!(word & ~(XWC_COUNTER_MASK | XWC_SELFLOOP | XWC_NOCOUNT)));
    if (word & XWC_NOCOUNT) {
        return;
    }
    word &= XWC_COUNTER_MASK;
    tcg_out32(s, INDEX_op_wasm_census | (word << 8));
#ifdef EMSCRIPTEN
    unsigned buckets[3] = { word & XWC_MASK, 0x40, 0x41 };
    unsigned count = !(word & XWC_ELIGIBLE) ? 1 :
                     (word & XWC_REGONLY) ? 3 : 2;

    for (unsigned i = 0; i < count; i++) {
        uint32_t addr = (uintptr_t)xemu_wasm_census_counter(buckets[i], true);

        wasm_count(s, xemu_wasm_census_counter(buckets[i], false));
        wasm_i32_const(s, addr);
        wasm_i32_const(s, addr);
        wasm_mem(s, W_I64_LOAD, 0);
        wasm_i64_const(s, word >> XWC_INSN_SHIFT);
        wasm8(s, W_I64_ADD);
        wasm_mem(s, W_I64_STORE, 0);
    }
#endif
}

/* No extra generated instructions unless entry/exit profiling is enabled. */
static bool wasm_profile_exits(void)
{
#ifdef EMSCRIPTEN
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_JIT_ENTRY_PROF");
        enabled = e && *e == '1';
    }
    return enabled;
#else
    return false;
#endif
}

/* Normal returns only; Asyncify unwind/forward returns are not C exits. */
static void wasm_count_exit_tb(TCGContext *s, uintptr_t arg)
{
#ifdef EMSCRIPTEN
    uint64_t *counter;

    if (!wasm_profile_exits()) {
        return;
    }
    if (!arg) {
        counter = &xemu_wasm_stats.n_jit_exit_plain;
    } else {
        switch (arg & TB_EXIT_MASK) {
        case TB_EXIT_IDX0:
            counter = &xemu_wasm_stats.n_jit_exit_chain0;
            break;
        case TB_EXIT_IDX1:
            counter = &xemu_wasm_stats.n_jit_exit_chain1;
            break;
        case TB_EXIT_REQUESTED:
            counter = &xemu_wasm_stats.n_jit_exit_requested;
            break;
        default:
            counter = &xemu_wasm_stats.n_jit_exit_other;
            break;
        }
    }
    wasm_count(s, counter);
#endif
}

/* Insert after all self/region/IC/link guards, including IC throttle exits. */
static void wasm_count_goto_exit(TCGContext *s, bool indirect)
{
#ifdef EMSCRIPTEN
    if (!wasm_profile_exits()) {
        return;
    }
    if (indirect) {
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_if(s, W_BLOCKTYPE_VOID);
        wasm_count(s, &xemu_wasm_stats.n_jit_exit_ptr);
        wasm8(s, W_ELSE);
        wasm_count(s, &xemu_wasm_stats.n_jit_exit_ptr_null);
        wasm8(s, W_END);
    } else {
        wasm_count(s, &xemu_wasm_stats.n_jit_exit_direct);
    }
#endif
}

#ifdef EMSCRIPTEN
/*
 * XEMU_WASM_HELPER_PROF=1: count calls per helper (by function pointer);
 * xemu_wasm_helper_prof_dump() prints the top ones.
 */
#include <emscripten.h>
#define HPROF_N 4096
static struct { uintptr_t fn; const char *name; uint64_t n; } hprof[HPROF_N];

static void wasm_count_helper(TCGContext *s, const void *func,
                              const TCGHelperInfo *info)
{
    static int on = -1;
    uint32_t i;

    if (on < 0) {
        const char *e = getenv("XEMU_WASM_HELPER_PROF");
        on = e && *e == '1';
    }
    if (!on) {
        return;
    }
    for (i = ((uintptr_t)func * 2654435761u) % HPROF_N;
         hprof[i].fn && hprof[i].fn != (uintptr_t)func;
         i = (i + 1) % HPROF_N) {
    }
    hprof[i].fn = (uintptr_t)func;
    hprof[i].name = info->name;
    wasm_count(s, &hprof[i].n);
}

EMSCRIPTEN_KEEPALIVE void xemu_wasm_helper_prof_dump(void)
{
    for (int k = 0; k < 25; k++) {
        int best = -1;
        for (int i = 0; i < HPROF_N; i++) {
            if (hprof[i].n && (best < 0 || hprof[i].n > hprof[best].n)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        fprintf(stderr, "[hprof] %-24s %llu\n",
                hprof[best].name ? hprof[best].name : "?",
                (unsigned long long)hprof[best].n);
        hprof[best].n = 0;
    }
    for (int i = 0; i < HPROF_N; i++) {
        hprof[i].n = 0;
    }
}
#endif

/*
 * Group helper bodies without adding per-call probes or JS wrappers.
 * Keep individual names in the separate helper-count profiler.
 */
static const char *wasm_helper_phase_name(const TCGHelperInfo *info)
{
    const char *n = info->name;

    if (!n) {
        return xemu_wasm_insn_profile_enabled() ? "helper:unknown" : NULL;
    }
    if (g_str_has_prefix(n, "gvec_")) {
        return "gvec";
    }
#ifdef XBOX
    if (g_str_has_suffix(n, "_mmx") || !strcmp(n, "emms") ||
        !strcmp(n, "femms")) {
        return "mmx";
    }
    if (!strcmp(n, "fxsave") || !strcmp(n, "fxrstor") ||
        g_str_has_prefix(n, "xsave") || g_str_has_prefix(n, "xrstor")) {
        return "fpu_state";
    }
    /* Other current i386 f* helpers are x87, except the TLB flush helper. */
    if (n[0] == 'f' && !g_str_has_prefix(n, "flush_")) {
        return "x87";
    }
#endif
    return n;
}

static XemuWasmInsnPhase wasm_tb_insn_class = XWIP_UNKNOWN;

/* Final frontend metadata is available before emitting even the prologue. */
static void wasm_insn_profile_start(TCGContext *s)
{
    TCGOp *op;

    wasm_tb_insn_class = XWIP_UNKNOWN;
    if (!xemu_wasm_insn_profile_enabled()) {
        return;
    }
    QTAILQ_FOREACH(op, &s->ops, link) {
        if (op->opc == INDEX_op_wasm_census) {
            uint32_t word = *(const uint32_t *)op->args[0];
            bool loop = word & XWC_SELFLOOP;

            wasm_tb_insn_class = (word & XWC_ELIGIBLE) ?
                (loop ? XWIP_ELIGIBLE_LOOP : XWIP_ELIGIBLE) :
                (loop ? XWIP_OTHER_LOOP : XWIP_OTHER);
            break;
        }
    }
}

/* Rename generated scopes only; C helpers/slow paths stay separate. */
static const char *wasm_insn_phase_name(const char *name)
{
#ifdef EMSCRIPTEN
    if (xemu_wasm_insn_profile_enabled()) {
        bool self_edge = name && !strcmp(name, "spin");
        XemuWasmInsnPhase c = wasm_tb_insn_class;

        if (!name || self_edge || !strncmp(name, "jit:", 4)) {
            /* A runtime self-edge can additionally reveal an indirect loop. */
            if (self_edge && c == XWIP_ELIGIBLE) {
                c = XWIP_ELIGIBLE_LOOP;
            } else if (self_edge && c == XWIP_OTHER) {
                c = XWIP_OTHER_LOOP;
            }
            return xemu_wasm_insn_phase_names[c];
        }
    }
#endif
    return name;
}

/* publish what the vCPU is doing for the sampling profile (NULL = jit) */
static void wasm_phase(TCGContext *s, const char *name)
{
#ifdef EMSCRIPTEN
    if (!xemu_wasm_profile_enabled()) {
        return;
    }
    name = wasm_insn_phase_name(name);
    wasm_i32_const(s, (int32_t)(uintptr_t)&xemu_wasm_phase[XPHASE_VCPU]);
    wasm_i32_const(s, (int32_t)(uintptr_t)name);
    wasm_mem(s, W_I32_STORE, 0);
#endif
}

/*
 * Opt-in generated-code attribution: 1 = inline helpers/call-site glue,
 * 2 = inline SoftMMU, 8 = frontend TB lookup (bit 4 selects C notdirty
 * subphases in accel/tcg). None adds generated code when disabled. Extra stores
 * can perturb optimization and region grouping: compare an uninstrumented run.
 * Scopes also cover work inside self loops, replacing the heuristic "spin"
 * tag. For overhead, compare throughput, not spin-subtracted phase busy time.
 */
static unsigned wasm_jit_profile(void)
{
#ifdef EMSCRIPTEN
    static int flags = -1;

    if (flags < 0) {
        const char *e = getenv("XEMU_WASM_JIT_PROFILE");
        flags = e ? atoi(e) & 11 : 0;
    }
    return flags;
#else
    return 0;
#endif
}

static void wasm_jit_phase(TCGContext *s, unsigned mask, const char *name)
{
    if (wasm_jit_profile() & mask) {
        wasm_phase(s, name);
    }
}

static const char *wasm_callsite_phase_name(const TCGHelperInfo *info)
{
    const char *name = wasm_helper_phase_name(info);

    if (name) {
        if (!strcmp(name, "gvec")) {
            return "jit:gvec";
        }
        if (!strcmp(name, "x87")) {
            return "jit:x87";
        }
        if (!strcmp(name, "mmx")) {
            return "jit:mmx";
        }
        if (!strcmp(name, "fpu_state")) {
            return "jit:fpu_state";
        }
    }
    return "jit:call";
}

/* @reset replaces an existing post-helper NULL store, even when disabled. */
static void wasm_callsite_phase(TCGContext *s, const TCGHelperInfo *info,
                                bool reset)
{
    if (wasm_jit_profile() & 1) {
        wasm_phase(s, wasm_callsite_phase_name(info));
    } else if (reset) {
        wasm_phase(s, NULL);
    }
}

/* a TB execution that never leaves generated code (self loop) */
static void wasm_count_tb_exec(TCGContext *s)
{
#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_tb_exec);
#endif
}

static const struct {
    uint8_t i32;
    uint8_t i64;
} tcg_cond_to_inst[] = {
    [TCG_COND_EQ] =  { 0x46 /* i32.eq */   , 0x51 /* i64.eq */},
    [TCG_COND_NE] =  { 0x47 /* i32.ne */   , 0x52 /* i64.ne */},
    [TCG_COND_LT] =  { 0x48 /* i32.lt_s */ , 0x53 /* i64.lt_s */},
    [TCG_COND_GE] =  { 0x4e /* i32.ge_s */ , 0x59 /* i64.ge_s */},
    [TCG_COND_LE] =  { 0x4c /* i32.le_s */ , 0x57 /* i64.le_s */},
    [TCG_COND_GT] =  { 0x4a /* i32.gt_s */ , 0x55 /* i64.gt_s */},
    [TCG_COND_LTU] = { 0x49 /* i32.lt_u */ , 0x54 /* i64.lt_u */},
    [TCG_COND_GEU] = { 0x4f /* i32.ge_u */ , 0x5a /* i64.ge_u */},
    [TCG_COND_LEU] = { 0x4d /* i32.le_u */ , 0x58 /* i64.le_u */},
    [TCG_COND_GTU] = { 0x4b /* i32.gt_u */ , 0x56 /* i64.gt_u */},
};

/* Push the i32 truth value of (a1 cond a2). */
static void wasm_cond(TCGContext *s, TCGType type, TCGCond cond,
                      TCGReg a1, TCGReg a2)
{
    tcg_debug_assert(cond < ARRAY_SIZE(tcg_cond_to_inst) &&
                     tcg_cond_to_inst[cond].i32 != 0);
    if (type == TCG_TYPE_I32) {
        wasm_get_r32(s, a1);
        wasm_get_r32(s, a2);
        wasm8(s, tcg_cond_to_inst[cond].i32);
    } else {
        wasm_get_r(s, a1);
        wasm_get_r(s, a2);
        wasm8(s, tcg_cond_to_inst[cond].i64);
    }
}

/* d = a1 <op64> a2 */
static void wasm_binop64(TCGContext *s, uint8_t op,
                         TCGReg d, TCGReg a1, TCGReg a2)
{
    wasm_get_r(s, a1);
    wasm_get_r(s, a2);
    wasm8(s, op);
    wasm_set_r(s, d);
}

/* d = (i64)(i32)(a1 <op32> a2), sign- or zero-extended */
static void wasm_binop32(TCGContext *s, uint8_t op, bool sext,
                         TCGReg d, TCGReg a1, TCGReg a2)
{
    wasm_get_r32(s, a1);
    wasm_get_r32(s, a2);
    wasm8(s, op);
    wasm8(s, sext ? W_I64_EXTEND_I32_S : W_I64_EXTEND_I32_U);
    wasm_set_r(s, d);
}

/* d = ~(a1 <op> a2) or a1 <op> ~a2 */
static void wasm_binop_not(TCGContext *s, uint8_t op, bool not_result,
                           TCGReg d, TCGReg a1, TCGReg a2)
{
    wasm_get_r(s, a1);
    wasm_get_r(s, a2);
    if (!not_result) {
        wasm_i64_const(s, -1);
        wasm8(s, W_I64_XOR);
    }
    wasm8(s, op);
    if (not_result) {
        wasm_i64_const(s, -1);
        wasm8(s, W_I64_XOR);
    }
    wasm_set_r(s, d);
}

static void wasm_mov(TCGContext *s, TCGReg d, TCGReg a)
{
    if (d != a) {
        wasm_get_r(s, a);
        wasm_set_r(s, d);
    }
}

static void wasm_movi(TCGContext *s, TCGType type, TCGReg d, int64_t v)
{
    wasm_i64_const(s, type == TCG_TYPE_I32 ? (int32_t)v : v);
    wasm_set_r(s, d);
}

static void wasm_extract(TCGContext *s, TCGReg d, TCGReg a,
                         unsigned pos, unsigned len)
{
    wasm_get_r(s, a);
    if (pos) {
        wasm_i64_const(s, pos);
        wasm8(s, W_I64_SHR_U);
    }
    if (pos + len < 64) {
        wasm_i64_const(s, (int64_t)MAKE_64BIT_MASK(0, len));
        wasm8(s, W_I64_AND);
    }
    wasm_set_r(s, d);
}

static void wasm_sextract(TCGContext *s, TCGReg d, TCGReg a,
                          unsigned pos, unsigned len)
{
    wasm_get_r(s, a);
    if (pos == 0 && len == 32) {
        wasm8(s, W_I64_EXTEND32_S);
    } else {
        if (64 - pos - len) {
            wasm_i64_const(s, 64 - pos - len);
            wasm8(s, W_I64_SHL);
        }
        if (64 - len) {
            wasm_i64_const(s, 64 - len);
            wasm8(s, W_I64_SHR_S);
        }
    }
    wasm_set_r(s, d);
}

static void wasm_deposit(TCGContext *s, TCGReg d, TCGReg a1, TCGReg a2,
                         unsigned pos, unsigned len)
{
    uint64_t mask = MAKE_64BIT_MASK(pos, len);

    wasm_get_r(s, a1);
    wasm_i64_const(s, (int64_t)~mask);
    wasm8(s, W_I64_AND);
    wasm_get_r(s, a2);
    if (pos) {
        wasm_i64_const(s, pos);
        wasm8(s, W_I64_SHL);
    }
    wasm_i64_const(s, (int64_t)mask);
    wasm8(s, W_I64_AND);
    wasm8(s, W_I64_OR);
    wasm_set_r(s, d);
}

/* clz/ctz with a2 as the result for a zero input */
static void wasm_clz_ctz(TCGContext *s, TCGType type, bool ctz,
                         TCGReg d, TCGReg a1, TCGReg a2)
{
    wasm_get_r(s, a2);
    if (type == TCG_TYPE_I32) {
        wasm_get_r32(s, a1);
        wasm8(s, ctz ? W_I32_CTZ : W_I32_CLZ);
        wasm8(s, W_I64_EXTEND_I32_U);
        wasm_get_r32(s, a1);
        wasm8(s, W_I32_EQZ);
    } else {
        wasm_get_r(s, a1);
        wasm8(s, ctz ? W_I64_CTZ : W_I64_CLZ);
        wasm_get_r(s, a1);
        wasm8(s, W_I64_EQZ);
    }
    wasm8(s, W_SELECT);
    wasm_set_r(s, d);
}

static void wasm_shift(TCGContext *s, TCGType type, uint8_t op64,
                       TCGReg d, TCGReg a1, TCGReg a2)
{
    wasm_get_r(s, a1);
    if (type == TCG_TYPE_I32) {
        if (op64 == W_I64_SHR_U) {
            wasm_i64_const(s, 0xffffffff);
            wasm8(s, W_I64_AND);
        } else if (op64 == W_I64_SHR_S) {
            wasm8(s, W_I64_EXTEND32_S);
        }
        wasm_get_r(s, a2);
        wasm_i64_const(s, 31);
        wasm8(s, W_I64_AND);
    } else {
        wasm_get_r(s, a2);
    }
    wasm8(s, op64);
    wasm_set_r(s, d);
}

static void wasm_bswap16(TCGContext *s, TCGReg d, TCGReg a, unsigned flags)
{
    /* (x >> 8 & 0xff) | (x & 0xff) << 8, on the low 16 bits */
    wasm_get_r32(s, a);
    wasm_var(s, W_LOCAL_TEE, L32_0);
    wasm_i32_const(s, 8);
    wasm8(s, W_I32_SHR_U);
    wasm_i32_const(s, 0xff);
    wasm8(s, W_I32_AND);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_i32_const(s, 0xff);
    wasm8(s, W_I32_AND);
    wasm_i32_const(s, 8);
    wasm8(s, W_I32_SHL);
    wasm8(s, W_I32_OR);
    if (flags & TCG_BSWAP_OS) {
        wasm8(s, W_I32_EXTEND16_S);
        wasm8(s, W_I64_EXTEND_I32_S);
    } else {
        wasm8(s, W_I64_EXTEND_I32_U);
    }
    wasm_set_r(s, d);
}

static void wasm_bswap32(TCGContext *s, TCGReg d, TCGReg a, unsigned flags)
{
    /* (rotr(x, 8) & 0xff00ff00) | (rotl(x, 8) & 0x00ff00ff) */
    wasm_get_r32(s, a);
    wasm_var(s, W_LOCAL_TEE, L32_0);
    wasm_i32_const(s, 8);
    wasm8(s, W_I32_ROTR);
    wasm_i32_const(s, 0xff00ff00);
    wasm8(s, W_I32_AND);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_i32_const(s, 8);
    wasm8(s, W_I32_ROTL);
    wasm_i32_const(s, 0x00ff00ff);
    wasm8(s, W_I32_AND);
    wasm8(s, W_I32_OR);
    wasm8(s, (flags & TCG_BSWAP_OS) ? W_I64_EXTEND_I32_S : W_I64_EXTEND_I32_U);
    wasm_set_r(s, d);
}

static void wasm_bswap64(TCGContext *s, TCGReg d, TCGReg a)
{
    /* swap 32-bit halves, then bytes within 16-bit, then 16-bit halves */
    wasm_get_r(s, a);
    wasm_i64_const(s, 32);
    wasm8(s, W_I64_ROTR);
    wasm_var(s, W_LOCAL_TEE, L64_0);
    wasm_i64_const(s, 0xff00ff00ff00ff00ull);
    wasm8(s, W_I64_AND);
    wasm_i64_const(s, 8);
    wasm8(s, W_I64_SHR_U);
    wasm_var(s, W_LOCAL_GET, L64_0);
    wasm_i64_const(s, 0x00ff00ff00ff00ffull);
    wasm8(s, W_I64_AND);
    wasm_i64_const(s, 8);
    wasm8(s, W_I64_SHL);
    wasm8(s, W_I64_OR);
    wasm_var(s, W_LOCAL_TEE, L64_0);
    wasm_i64_const(s, 0xffff0000ffff0000ull);
    wasm8(s, W_I64_AND);
    wasm_i64_const(s, 16);
    wasm8(s, W_I64_SHR_U);
    wasm_var(s, W_LOCAL_GET, L64_0);
    wasm_i64_const(s, 0x0000ffff0000ffffull);
    wasm8(s, W_I64_AND);
    wasm_i64_const(s, 16);
    wasm8(s, W_I64_SHL);
    wasm8(s, W_I64_OR);
    wasm_set_r(s, d);
}

static void wasm_ldst(TCGContext *s, uint8_t op, bool is_store,
                      TCGReg val, TCGReg base, intptr_t offset)
{
    int ofs = wasm_addr(s, base, offset);

    if (is_store) {
        wasm_get_r(s, val);
        wasm_mem(s, op, ofs);
    } else {
        wasm_mem(s, op, ofs);
        wasm_set_r(s, val);
    }
}

/*
 * Debug aid: XEMU_WASM_JIT_DENY=<bitmask> keeps TBs that use any of the
 * given operation classes (see WASM_CLASS() uses) in the interpreter.
 */
static __thread unsigned wasm_used_classes;
static int wasm_deny_classes = -1;
#define WASM_CLASS(n) (wasm_used_classes |= 1u << (n))

/*
 * Blocks and labels.
 */
typedef struct LabelInfo {
    struct LabelInfo *next;
    int label;
    int block;
} LabelInfo;

typedef struct BlockPlaceholder {
    struct BlockPlaceholder *next;
    int label;
    int pos;
} BlockPlaceholder;

static __thread LabelInfo *label_info;
static __thread BlockPlaceholder *block_placeholder;
static __thread int block_idx;

static void add_label(int label, int block)
{
    LabelInfo *e = tcg_malloc(sizeof(LabelInfo));
    e->label = label;
    e->block = block;
    e->next = label_info;
    label_info = e;
}

static int get_block_of_label(int label)
{
    for (LabelInfo *p = label_info; p; p = p->next) {
        if (p->label == label) {
            return p->block;
        }
    }
    return -1;
}

static void add_block_placeholder(int label, int pos)
{
    BlockPlaceholder *e = tcg_malloc(sizeof(BlockPlaceholder));
    e->label = label;
    e->pos = pos;
    e->next = block_placeholder;
    block_placeholder = e;
}

static int block_idx_cur(void)
{
    return block_idx;
}

/*
 * Code layout of a TB function (straight-line regions):
 *
 *   prologue (fresh entry / Asyncify rewind arm, sets LBLOCK)
 *   loop $top
 *     block B[n-1] ... block B[1]              (inserted at the TB end,
 *       block D                                 when n is known)
 *         br_if D (LBLOCK == 0)        likely: region 0
 *         br_table D B[1] .. B[n-1] (LBLOCK)   rewind / backward branch
 *       end
 *       region 0
 *     end B[1]
 *     region 1 ...
 *   end $top
 *
 * Regions follow each other without any test, so V8 sees straight-line
 * code (the old "if (LBLOCK <= k)" chain merged all locals at every
 * region). Forward branches "br" to the block ending at the target region;
 * backward branches and self loops set LBLOCK and continue $top. Their br
 * depths are patched at the TB end (BrPatch).
 */
typedef struct BrPatch {
    struct BrPatch *next;
    uint32_t pos;       /* of the 5-byte br depth LEB */
    int region, d;      /* site region, constructs between site and region */
    int label;          /* target label id, or -1 for $top */
} BrPatch;
static __thread BrPatch *br_patches;
static __thread uint32_t region_ins_pos;
/* prologue: positions of its "else" and closing "end" (entry function) */
static __thread uint32_t pro_else_pos, pro_end_pos;
static __thread bool has_backward_label;

static void wasm_br_patch(TCGContext *s, int d, int label)
{
    BrPatch *e = tcg_malloc(sizeof(*e));

    wasm8(s, W_BR);
    e->pos = wasm_code_len();
    e->region = block_idx;
    e->d = d;
    e->label = label;
    e->next = br_patches;
    br_patches = e;
    for (int i = 0; i < 5; i++) {
        wasm8(s, i < 4 ? 0x80 : 0);
    }
}

/* Close the current region (block B[block_idx + 1]) and start the next. */
static void wasm_new_block(TCGContext *s)
{
    wasm8(s, W_END);
    ++block_idx;
}

/*
 * Make a possible Asyncify rewind resume at the region that is opened
 * next: the code emitted after this must be safe to run again.
 */
static void wasm_new_resumable_block(TCGContext *s)
{
    if (wasm_direct_blueprint) {
        wasm_add_reloc(WASM_RELOC_DIRECT_SUSPEND, 0, 0);
    }
    wasm_i64_const(s, block_idx + 1);
    wasm_var(s, W_LOCAL_SET, LBLOCK);
    wasm_new_block(s);
}

/* Called by tcg_out_label() in tcg.c. */
static void tcg_out_label_cb(TCGContext *s, TCGLabel *l)
{
    wasm_new_block(s);
    add_label(l->id, block_idx);
}

/*
 * Branch to @l; with @br_if, only if the i32 on the stack is non-zero.
 * Depth: region "if" = 0, loop = 1 (+1 inside the br_if "if").
 */
static void wasm_br_to_label(TCGContext *s, TCGLabel *l, bool br_if)
{
    int depth = 0;

    if (br_if) {
        wasm_if(s, W_BLOCKTYPE_VOID);
        depth++;
    }
    int target = get_block_of_label(l->id);
    if (target != -1) {
        /* backward: continue $top at the label's region */
        has_backward_label |= target > 0;
        wasm_i64_const(s, target);
        wasm_var(s, W_LOCAL_SET, LBLOCK);
        wasm_br_patch(s, depth, -1);
    } else {
        /* forward: to the end of the block preceding the label's region */
        wasm_br_patch(s, depth, l->id);
    }
    if (br_if) {
        wasm8(s, W_END);
    }
}

/*
 * Helpers imported by the module.
 */
typedef struct HelperInfo {
    struct HelperInfo *next;
    uint32_t idx_on_qemu;   /* function pointer == wasm table index */
} HelperInfo;

static __thread HelperInfo *helpers, *helpers_last;
static __thread int helpers_num;

static void types_out8(uint8_t v)
{
    types_buf_cur = linked_buf_out8(types_buf_cur, v);
}

static void types_out_uleb(uint32_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        types_out8(b | (v ? 0x80 : 0));
    } while (v);
}

static int get_helper_idx(const void *func)
{
    int idx = HELPER_IDX_START;

    for (HelperInfo *p = helpers; p; p = p->next) {
        if (p->idx_on_qemu == (uint32_t)(uintptr_t)func) {
            return idx;
        }
        idx++;
    }
    return -1;
}

/* Register an import; the caller must then emit its type to types_buf. */
static int register_helper(const void *func)
{
    HelperInfo *e = tcg_malloc(sizeof(HelperInfo));

    e->idx_on_qemu = (uint32_t)(uintptr_t)func;
    e->next = NULL;
    if (helpers_last) {
        helpers_last->next = e;
    } else {
        helpers = e;
    }
    helpers_last = e;
    return HELPER_IDX_START + helpers_num++;
}

static uint8_t typecode_to_wasm(int typecode)
{
    switch (typecode) {
    case dh_typecode_i32:
    case dh_typecode_s32:
    case dh_typecode_ptr:
    case dh_typecode_i128:      /* passed by reference */
        return W_TYPE_I32;
    case dh_typecode_i64:
    case dh_typecode_s64:
        return W_TYPE_I64;
    default:
        g_assert_not_reached();
    }
}

static int helper_nargs(unsigned typemask)
{
    int nargs = 32 - clz32(typemask >> 3);
    return DIV_ROUND_UP(nargs, 3);
}

/* Wasm function type of a TCG helper, per the C ABI of wasm32 clang. */
static void gen_func_type(const TCGHelperInfo *info)
{
    unsigned typemask = info->typemask;
    int rettype = typemask & 7;
    int nargs = helper_nargs(typemask);
    int nparams = 0;

    for (int j = 0; j < nargs; ++j) {
        if (extract32(typemask, (j + 1) * 3, 3) != dh_typecode_void) {
            nparams++;
        }
    }
    if (rettype == dh_typecode_i128) {
        nparams++;      /* sret pointer */
    }

    types_out8(0x60);
    types_out_uleb(nparams);
    if (rettype == dh_typecode_i128) {
        types_out8(W_TYPE_I32);
    }
    for (int j = 0; j < nargs; ++j) {
        int typecode = extract32(typemask, (j + 1) * 3, 3);
        if (typecode != dh_typecode_void) {
            types_out8(typecode_to_wasm(typecode));
        }
    }
    if (rettype == dh_typecode_void || rettype == dh_typecode_i128) {
        types_out8(0);
    } else {
        types_out8(1);
        types_out8(typecode_to_wasm(rettype));
    }
}

/* If Asyncify is unwinding the stack, return from the TB function now. */
static void wasm_handle_unwinding(TCGContext *s)
{
    wasm_call_idx(s, CHECK_UNWINDING_IDX);
    wasm8(s, W_I32_EQZ);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
    wasm_jit_phase(s, 1, NULL);
    wasm_i32_const(s, 0);
    wasm8(s, W_RETURN);
    wasm8(s, W_END);
}

/* Store the helper return address (GETPC) = current TCI position. */
static void wasm_set_tci_tb_ptr(TCGContext *s)
{
    wasm_ctx_load_i32(s, WASM_CTX_TCI_TB_PTR_OFF);
    wasm_i32_const(s, (int32_t)(uintptr_t)s->code_ptr);
    wasm_mem(s, W_I32_STORE, 0);
}

/*
 * Can this helper suspend via Asyncify (reach device I/O -> coroutine/fiber
 * switch)? Only then does the call need a resumable block and the
 * "is Asyncify unwinding?" JS check afterwards, which otherwise costs a JS
 * round trip per helper call (tens of millions/s for x87/SSE-heavy code).
 * Conservative: anything not known to be pure register/flag/lookup work
 * keeps the check.
 */
static bool wasm_helper_may_suspend(const TCGHelperInfo *info)
{
    const char *n = info->name;
    static const char *const touches_memory[] = {
        "save", "rstor", "env", "fldt", "fstt", "fbld", "fbst", "maskmov",
        "ld_", "st_", "cmpxchg", "atomic", "in", "out",
    };

    if (!n) {
        return true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(touches_memory); i++) {
        if (strstr(n, touches_memory[i])) {
            return true;
        }
    }
    if (g_str_has_prefix(n, "cc_compute") || !strcmp(n, "lookup_tb_ptr")) {
        return false;
    }
    /* x87 register ops (fadd_ST0_FT0, fmov_*, flds_ST0 (value arg), ...) */
    if (n[0] == 'f') {
        return false;
    }
    /* SSE/MMX register ops and conversions */
    if (g_str_has_suffix(n, "_xmm") || g_str_has_suffix(n, "_mmx") ||
        g_str_has_suffix(n, "ss") || g_str_has_suffix(n, "ps") ||
        g_str_has_suffix(n, "sd") || g_str_has_suffix(n, "pd") ||
        g_str_has_prefix(n, "cvt") || g_str_has_prefix(n, "comis") ||
        g_str_has_prefix(n, "ucomis") || g_str_has_prefix(n, "rcp") ||
        g_str_has_prefix(n, "rsqrt")) {
        return false;
    }
    return true;
}

static void wasm_push_call_args(TCGContext *s, unsigned typemask, int nargs);
static void wasm_call_results(TCGContext *s, int rettype);

#ifdef EMSCRIPTEN
uint32_t xemu_wasm_ld_i128_fast(Int128 *ret, CPUArchState *env,
                                uint64_t addr, uint32_t oi);
uint32_t xemu_wasm_st_i128_fast(CPUArchState *env, uint64_t addr,
                                const Int128 *val, uint32_t oi);

/*
 * ld/st_i128: try xemu_wasm_{ld,st}_i128_fast (plain non-suspending call;
 * it succeeds only for an aligned single-page clean-TLB RAM access) and
 * run the original resumable helper call only when it declines. L32_1
 * holds the fast result across the resumable region boundary; a rewind
 * starts with L32_1 == 0 and so replays the original helper.
 */
static bool wasm_call_i128_fast(TCGContext *s, const tcg_insn_unit *func,
                                const TCGHelperInfo *info)
{
    static int enabled = -1;
    bool ld;
    const void *fast;
    unsigned typemask = info->typemask;
    int nargs = helper_nargs(typemask);
    int func_idx, fast_idx;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_I128_FAST");
        enabled = !(e && *e == '0');
    }
    if (!enabled || !info->name ||
        (tcg_ctx->gen_tb->cflags & CF_PARALLEL)) {
        return false;
    }
    if (!strcmp(info->name, "ld_i128")) {
        ld = true;
        fast = xemu_wasm_ld_i128_fast;
    } else if (!strcmp(info->name, "st_i128")) {
        ld = false;
        fast = xemu_wasm_st_i128_fast;
    } else {
        return false;
    }

    func_idx = get_helper_idx(func);
    if (func_idx < 0) {
        func_idx = register_helper(func);
        gen_func_type(info);
    }
    fast_idx = get_helper_idx(fast);
    if (fast_idx < 0) {
        static const uint8_t ty_ld[] = { 0x60, 4, W_TYPE_I32, W_TYPE_I32,
                                         W_TYPE_I64, W_TYPE_I32,
                                         1, W_TYPE_I32 };
        static const uint8_t ty_st[] = { 0x60, 4, W_TYPE_I32, W_TYPE_I64,
                                         W_TYPE_I32, W_TYPE_I32,
                                         1, W_TYPE_I32 };
        fast_idx = register_helper(fast);
        for (int i = 0; i < sizeof(ty_ld); i++) {
            types_out8(ld ? ty_ld[i] : ty_st[i]);
        }
    }

#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_helper);
    wasm_count_helper(s, func, info);
#endif
    wasm_jit_phase(s, 1, "jit:i128");
    /* fast attempt */
    wasm_push_call_args(s, typemask, nargs);
    wasm_call_idx(s, fast_idx);
    wasm_var(s, W_LOCAL_SET, L32_1);

    /* fallback: the original helper call, resumable */
    wasm_new_resumable_block(s);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm8(s, W_I32_EQZ);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
    wasm_callsite_phase(s, info, false);
    wasm_set_tci_tb_ptr(s);
    wasm_push_call_args(s, typemask, nargs);
    wasm_save_regs(s);
    wasm_phase(s, wasm_helper_phase_name(info));
    wasm_call_idx(s, func_idx);
    wasm_callsite_phase(s, info, true);
    if (!ld) {
        /* st_i128 returns nothing */
    }
    wasm_handle_unwinding(s);
    wasm8(s, W_END);

    /* both paths left the result at the call stack slots */
    wasm_call_results(s, typemask & 7);
    wasm_jit_phase(s, 1, NULL);
    return true;
}
#endif

/* Used only by the next marked helper call during code generation. */
static __thread const WasmLookupLayout *wasm_lookup_layout;
static __thread uint32_t wasm_lookup_cflags;

static void wasm_lookup_env(TCGContext *s, int32_t offset)
{
    int ofs = wasm_addr(s, TCG_AREG0, offset);
    wasm_mem(s, W_I32_LOAD, ofs);
}

static void wasm_lookup_fail_if(TCGContext *s)
{
    wasm8(s, W_BR_IF);
    wasm_uleb(s, 0);
}

/*
 * Exact counterpart of the frontend jump-cache probe, expressed in i32
 * rather than lowered through i64 TCG temporaries. No translation, tag,
 * permission, epoch or pointer is cached between calls. L32_1 is zero on
 * every decline, or the live TB's code pointer on success.
 */
static void wasm_lookup_probe(TCGContext *s, const WasmLookupLayout *d,
                              uint32_t cflags)
{
    wasm_jit_phase(s, 8, "jit:lookup");
    wasm_i32_const(s, 0);
    wasm_var(s, W_LOCAL_SET, L32_1);
    wasm8(s, W_BLOCK);
    wasm8(s, W_BLOCKTYPE_VOID);

    wasm_lookup_env(s, d->env_bps);
    wasm_lookup_fail_if(s);

    wasm_lookup_env(s, d->env_cs);
    wasm_lookup_env(s, d->env_eip);
    wasm8(s, W_I32_ADD);
    wasm_var(s, W_LOCAL_SET, L32_2);
    wasm_lookup_env(s, d->env_jc);
    wasm_var(s, W_LOCAL_TEE, L32_3);
    wasm_mem(s, W_I32_LOAD, d->jc_gen);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm_var(s, W_LOCAL_SET, L64_0);

    /* The same tb_jmp_cache_hash_func(), with 32-bit wrapping. */
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm_i32_const(s, d->hash_shift);
    wasm8(s, W_I32_SHR_U);
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm8(s, W_I32_XOR);
    wasm_var(s, W_LOCAL_TEE, L32_0);
    wasm_i32_const(s, d->hash_shift);
    wasm8(s, W_I32_SHR_U);
    wasm_i32_const(s, d->hash_page_mask);
    wasm8(s, W_I32_AND);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_i32_const(s, d->hash_addr_mask);
    wasm8(s, W_I32_AND);
    wasm8(s, W_I32_OR);
    wasm_i32_const(s, d->entry_size);
    wasm8(s, W_I32_MUL);
    wasm_var(s, W_LOCAL_GET, L32_3);
    wasm8(s, W_I32_ADD);
    wasm_var(s, W_LOCAL_SET, L32_3);

    wasm_var(s, W_LOCAL_GET, L32_3);
    wasm_mem(s, W_I32_LOAD, d->entry_tb);
    wasm_var(s, W_LOCAL_TEE, L32_0);
    wasm8(s, W_I32_EQZ);
    wasm_lookup_fail_if(s);
    wasm_var(s, W_LOCAL_GET, L32_3);
    wasm_mem(s, W_I32_LOAD, d->entry_pc);
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm8(s, W_I32_NE);
    wasm_lookup_fail_if(s);
    wasm_var(s, W_LOCAL_GET, L32_3);
    wasm_mem(s, W_I32_LOAD, d->entry_gen);
    wasm_var(s, W_LOCAL_GET, L64_0);
    wasm8(s, W_I32_WRAP_I64);
    wasm8(s, W_I32_NE);
    wasm_lookup_fail_if(s);

    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_LOAD, d->tb_cs);
    wasm_lookup_env(s, d->env_cs);
    wasm8(s, W_I32_NE);
    wasm_lookup_fail_if(s);
    wasm_lookup_env(s, d->env_eflags);
    wasm_i32_const(s, d->flags_mask);
    wasm8(s, W_I32_AND);
    wasm_lookup_env(s, d->env_hflags);
    wasm8(s, W_I32_OR);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_LOAD, d->tb_flags);
    wasm8(s, W_I32_NE);
    wasm_lookup_fail_if(s);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_LOAD, d->tb_cflags);
    wasm_i32_const(s, cflags);
    wasm8(s, W_I32_NE);
    wasm_lookup_fail_if(s);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_LOAD, d->tb_ptr);
    wasm_var(s, W_LOCAL_SET, L32_1);
    wasm8(s, W_END);
    wasm_jit_phase(s, 8, NULL);
}

static void wasm_call(TCGContext *s, const tcg_insn_unit *func,
                      const TCGHelperInfo *info);

static bool wasm_call_lookup_fast(TCGContext *s, const tcg_insn_unit *func,
                                  const TCGHelperInfo *info)
{
    const WasmLookupLayout *d = wasm_lookup_layout;

    wasm_lookup_layout = NULL;
    if (!d || !info->name || strcmp(info->name, "lookup_tb_ptr") ||
        wasm_helper_may_suspend(info)) {
        return false;
    }
    /* A marker is emitted only for the frontend's original eligible sites. */
    wasm_lookup_probe(s, d, wasm_lookup_cflags);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm8(s, W_I32_EQZ);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
    /* Generator recursion only: unchanged helper ABI/GETPC/fallback body. */
    wasm_call(s, func, info);
    wasm8(s, W_ELSE);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm_call_results(s, info->typemask & 7);
    wasm8(s, W_END);
    return true;
}

#ifdef EMSCRIPTEN
static bool wasm_call_sse_fast(TCGContext *s, const tcg_insn_unit *func,
                               const TCGHelperInfo *info);
static bool wasm_call_cc_fast(TCGContext *s, const tcg_insn_unit *func,
                              const TCGHelperInfo *info);
static bool wasm_call_shufps_inline(TCGContext *s, const tcg_insn_unit *func,
                                    const TCGHelperInfo *info);
#endif

static void wasm_call(TCGContext *s, const tcg_insn_unit *func,
                      const TCGHelperInfo *info)
{
    if (wasm_call_lookup_fast(s, func, info)) {
        return;
    }
#ifdef EMSCRIPTEN
    if (wasm_call_i128_fast(s, func, info)) {
        return;
    }
    if (wasm_call_sse_fast(s, func, info)) {
        return;
    }
    if (wasm_call_cc_fast(s, func, info)) {
        return;
    }
    if (wasm_call_shufps_inline(s, func, info)) {
        return;
    }
#endif
    bool may_suspend = wasm_helper_may_suspend(info);
    unsigned typemask = info->typemask;
    int rettype = typemask & 7;
    int nargs = helper_nargs(typemask);
    int func_idx = get_helper_idx(func);
    int slot = 0;

    if (func_idx < 0) {
        func_idx = register_helper(func);
        gen_func_type(info);
    }

#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_helper);
    wasm_count_helper(s, func, info);
#endif
    if (may_suspend) {
        wasm_new_resumable_block(s);
    }
    wasm_callsite_phase(s, info, false);
    wasm_set_tci_tb_ptr(s);

    wasm_push_call_args(s, typemask, nargs);

    if (may_suspend) {
        wasm_save_regs(s);
    }
#ifdef EMSCRIPTEN
    if (wasm_profile_exits() && (info->flags & TCG_CALL_NO_RETURN)) {
        /* Auxiliary attempts: faults inside other helpers can also longjmp. */
        wasm_count(s, &xemu_wasm_stats.n_jit_noreturn);
    }
#endif
    wasm_phase(s, wasm_helper_phase_name(info));
    wasm_call_idx(s, func_idx);
    wasm_callsite_phase(s, info, true);
    wasm_call_results(s, rettype);
    if (may_suspend) {
        wasm_handle_unwinding(s);
    }
    wasm_jit_phase(s, 1, NULL);
}

/* Push a helper call's arguments (and the i128 result pointer) from the
 * TCG call stack slots. */
static void wasm_push_call_args(TCGContext *s, unsigned typemask, int nargs)
{
    int rettype = typemask & 7;
    int slot = 0;

    if (rettype == dh_typecode_i128) {
        /* the result is returned via memory, at the arg slots */
        wasm_get_r32(s, TCG_REG_CALL_STACK);
    }
    for (int j = 0; j < nargs; ++j) {
        int typecode = extract32(typemask, (j + 1) * 3, 3);
        int ofs = TCG_TARGET_CALL_STACK_OFFSET + slot * 8;

        switch (typecode) {
        case dh_typecode_void:
            continue;
        case dh_typecode_i32:
        case dh_typecode_s32:
        case dh_typecode_ptr:
            wasm_get_r32(s, TCG_REG_CALL_STACK);
            wasm_mem(s, W_I32_LOAD, ofs);
            slot++;
            break;
        case dh_typecode_i64:
        case dh_typecode_s64:
            wasm_get_r32(s, TCG_REG_CALL_STACK);
            wasm_mem(s, W_I64_LOAD, ofs);
            slot++;
            break;
        case dh_typecode_i128:
            /* byval: pass the address of the two slots */
            wasm_get_r32(s, TCG_REG_CALL_STACK);
            if (ofs) {
                wasm_i32_const(s, ofs);
                wasm8(s, W_I32_ADD);
            }
            slot += 2;
            break;
        default:
            g_assert_not_reached();
        }
    }
}

/* Move a helper call's result into R0 (/R1). */
static void wasm_call_results(TCGContext *s, int rettype)
{
    switch (rettype) {
    case dh_typecode_void:
        break;
    case dh_typecode_i32:
    case dh_typecode_s32:
    case dh_typecode_ptr:
        wasm8(s, W_I64_EXTEND_I32_U);
        wasm_set_r(s, TCG_REG_R0);
        break;
    case dh_typecode_i64:
    case dh_typecode_s64:
        wasm_set_r(s, TCG_REG_R0);
        break;
    case dh_typecode_i128:
        wasm_get_r32(s, TCG_REG_CALL_STACK);
        wasm_mem(s, W_I64_LOAD, 0);
        wasm_set_r(s, TCG_REG_R0);
        wasm_get_r32(s, TCG_REG_CALL_STACK);
        wasm_mem(s, W_I64_LOAD, 8);
        wasm_set_r(s, TCG_REG_R1);
        break;
    default:
        g_assert_not_reached();
    }
}

/* Opt-in smaller lookup; no cached translations or mutable TLB fields. */
static bool wasm_tlb_fast(void)
{
#ifdef EMSCRIPTEN
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_TLB_FAST");
        enabled = e && *e == '1';
    }
    return enabled;
#else
    return false;
#endif
}

/* Expected tag, retaining the full original span/alignment check. */
static void wasm_tlb_compare(TCGContext *s, unsigned a_mask, unsigned s_mask)
{
    wasm_var(s, W_LOCAL_GET, L32_2);
    if (a_mask < s_mask) {
        wasm_i32_const(s, s_mask - a_mask);
        wasm8(s, W_I32_ADD);
    }
    wasm_i32_const(s, (uint32_t)TARGET_PAGE_MASK | a_mask);
    wasm8(s, W_I32_AND);
}

static void wasm_tlb_host(TCGContext *s)
{
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_LOAD, offsetof(CPUTLBEntry, addend));
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm8(s, W_I32_ADD);
}

/*
 * Emit only in the existing store-TLB miss arm: plain RAM hits gain no
 * instructions. The leaf can neither suspend nor fault, and it commits no
 * store. A zero result still selects the original resumable store helper.
 */
static bool wasm_notdirty_inline(TCGContext *s, MemOpIdx oi)
{
#if defined(XBOX) && defined(EMSCRIPTEN) && defined(CONFIG_SOFTMMU) && \
    !defined(CONFIG_DEBUG_TCG) && !defined(CONFIG_PLUGIN) && \
    !defined(CONFIG_TSAN)
    static int enabled = -1;
    MemOp mop = get_memop(oi);
    unsigned atom = mop & MO_ATOM_MASK;
    unsigned align = MAX(memop_alignment_bits(mop), mop & MO_SIZE);
    bool profile;
    int idx;

    if (enabled < 0) {
        /*
         * Default on (Mac A/B Oct 4: GR +1.6/+2.2 fps, SC/R6 neutral);
         * any explicit value other than "1" (e.g. =0) disables it.
         */
        const char *e = getenv("XEMU_WASM_NOTDIRTY_INLINE");
        enabled = (!e || *e == '1') && !getenv("XEMU_WASM_SMC_DUMP");
    }
    if (!enabled || (s->gen_tb->cflags & CF_PARALLEL) ||
        (mop & MO_SIZE) > MO_32 ||
        (mop & ~(MO_SIZE | MO_AMASK | MO_ATOM_MASK)) ||
        (atom != MO_ATOM_IFALIGN && atom != MO_ATOM_NONE)) {
        return false;
    }
    /* Configure profiling here, never initialize configuration in the leaf. */
    profile = xemu_wasm_smc_profile_enabled();
    idx = get_helper_idx(helper_wasm_notdirty);
    if (idx < 0) {
        idx = register_helper(helper_wasm_notdirty);
        types_out8(0x60);
        types_out8(3); /* env, addr, MemOpIdx */
        types_out8(W_TYPE_I32);
        types_out8(W_TYPE_I32);
        types_out8(W_TYPE_I32);
        types_out8(1);
        types_out8(W_TYPE_I32); /* admitted host address, or zero */
    }

    /* Exact live tag: reject FORCE_SLOW (GPU/watch callbacks), invalid, etc. */
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_LOAD, offsetof(CPUTLBEntry, addr_write));
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm_i32_const(s, TARGET_PAGE_MASK);
    wasm8(s, W_I32_AND);
    wasm_i32_const(s, TLB_NOTDIRTY);
    wasm8(s, W_I32_OR);
    wasm8(s, W_I32_EQ);
    /* Natural alignment implies a same-page span for these 1/2/4-byte ops. */
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm_i32_const(s, (1u << align) - 1);
    wasm8(s, W_I32_AND);
    wasm8(s, W_I32_EQZ);
    wasm8(s, W_I32_AND);
    wasm_if_hint(s, W_TYPE_I32, true);
    wasm_get_r32(s, TCG_AREG0);
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm_i32_const(s, oi);
    if (profile) {
        wasm_phase(s, "notdirty_inline");
    }
    wasm_call_idx(s, idx);
    if (profile) {
        wasm_phase(s, (wasm_jit_profile() & 2) ? "jit:st" : NULL);
    }
    wasm8(s, W_ELSE);
    wasm_i32_const(s, 0);
    wasm8(s, W_END);
    return true;
#else
    return false;
#endif
}

/*
 * Guest memory access: inline TLB lookup, helper call on a miss.
 *
 * On wasm32 guest addresses (vaddr) and TLB comparators are 32 bits.
 * Leaves the host address, or 0 on a TLB miss, in local L32_1.
 */
static void wasm_tlb_load(TCGContext *s, TCGReg addr, MemOpIdx oi,
                          bool is_ld)
{
    MemOp opc = get_memop(oi);
    TCGAtomAlign aa = atom_and_align_for_opc(s, opc, MO_ATOM_IFALIGN, false);
    unsigned a_mask = (1u << aa.align) - 1;
    unsigned s_mask = (1u << (opc & MO_SIZE)) - 1;
    int fast_ofs = tlb_mask_table_ofs(s, get_mmuidx(oi));
    int mask_ofs = fast_ofs + offsetof(CPUTLBDescFast, mask);
    int table_ofs = fast_ofs + offsetof(CPUTLBDescFast, table);
    int cmp_ofs = is_ld ? offsetof(CPUTLBEntry, addr_read)
                        : offsetof(CPUTLBEntry, addr_write);
    bool fast = wasm_tlb_fast();
    bool hint = wasm32_tlb_hint_enabled();
    unsigned mmu_key = get_mmuidx(oi) + 1;

    QEMU_BUILD_BUG_ON(sizeof(((CPUTLBEntry *)0)->addr_read) != 4);
    QEMU_BUILD_BUG_ON(sizeof(vaddr) != 4);

    if (opc & MO_BSWAP) {
        /* let the helper swap */
        wasm_i32_const(s, 0);
        wasm_var(s, W_LOCAL_SET, L32_1);
        return;
    }

    if (hint) {
        /*
         * Cache a location, never permissions or a translation. Table
         * allocation/replacement is owner-vCPU work; every imported call
         * drops the hint before that work could execute. Other threads
         * can still revoke write permission, so the tag is always live.
         */
        wasm_get_r32(s, addr);
        wasm_var(s, W_LOCAL_SET, L32_2);
        wasm8(s, W_BLOCK);
        wasm8(s, W_BLOCKTYPE_VOID);
        wasm_var(s, W_LOCAL_GET, WASM_TLB_HINT_MMU_LOCAL);
        wasm_i32_const(s, mmu_key);
        wasm8(s, W_I32_EQ);
        if (a_mask < s_mask) {
            /*
             * The full lookup hashes the START page, but compares against
             * the adjusted END page. A hint might instead name that next
             * page: require the entire access to fit before trying it.
             */
            wasm_var(s, W_LOCAL_GET, L32_2);
            wasm_i32_const(s, ~(uint32_t)TARGET_PAGE_MASK);
            wasm8(s, W_I32_AND);
            wasm_i32_const(s, (1u << TARGET_PAGE_BITS) - s_mask);
            wasm8(s, W_I32_LT_U);
            wasm8(s, W_I32_AND);
        }
        wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
        wasm_var(s, W_LOCAL_GET, WASM_TLB_HINT_PTR_LOCAL);
        wasm_var(s, W_LOCAL_TEE, L32_0);
        wasm_mem(s, W_I32_LOAD, cmp_ofs);
        /* The span already fits; compare the start page and alignment. */
        wasm_tlb_compare(s, a_mask, a_mask);
        wasm8(s, W_I32_EQ);
        wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
        wasm_tlb_host(s);
        wasm_var(s, W_LOCAL_SET, L32_1);
        wasm_br(s, 2); /* both ifs, then the lookup block */
        wasm8(s, W_END);
        wasm8(s, W_END);
    }

    if (fast) {
        /*
         * One descriptor address, not two env-relative addresses. Both
         * fields are still loaded each time: helpers may resize the TLB.
         * The descriptor is within the CPU object, so its field offsets
         * cannot wrap the wasm32 address space.
         */
        wasm_get_r32(s, TCG_AREG0);
        wasm_i32_const(s, fast_ofs);
        wasm8(s, W_I32_ADD);
        wasm_var(s, W_LOCAL_SET, L32_3);
    }

    /* L32_2 = addr; L32_0 = entry */
    wasm_get_r32(s, addr);
    wasm_var(s, W_LOCAL_TEE, L32_2);
    /* addr ^ ((addr >> 12) & 0xff000): the index hash of tlb_index() */
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm_i32_const(s, 12);
    wasm8(s, W_I32_SHR_U);
    /*
     * For a 32-bit address the fold has at most 20 bits. After shifting,
     * the omitted low 12 bits fall entirely below CPU_TLB_ENTRY_BITS when
     * PAGE_BITS >= 12. The live mask (entries - 1) << ENTRY_BITS removes
     * them anyway. Keep the original expression on smaller-page targets.
     */
    if (!fast || TARGET_PAGE_BITS < 12) {
        wasm_i32_const(s, 0xff000);
        wasm8(s, W_I32_AND);
    }
    wasm8(s, W_I32_XOR);
    wasm_i32_const(s, TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS);
    wasm8(s, W_I32_SHR_U);
    if (fast) {
        wasm_var(s, W_LOCAL_GET, L32_3);
        wasm_mem(s, W_I32_LOAD, offsetof(CPUTLBDescFast, mask));
    } else {
        wasm_get_r32(s, TCG_AREG0);
        wasm_i32_const(s, mask_ofs);
        wasm8(s, W_I32_ADD);
        wasm_mem(s, W_I32_LOAD, 0);
    }
    wasm8(s, W_I32_AND);
    if (fast) {
        wasm_var(s, W_LOCAL_GET, L32_3);
        wasm_mem(s, W_I32_LOAD, offsetof(CPUTLBDescFast, table));
    } else {
        wasm_get_r32(s, TCG_AREG0);
        wasm_i32_const(s, table_ofs);
        wasm8(s, W_I32_ADD);
        wasm_mem(s, W_I32_LOAD, 0);
    }
    wasm8(s, W_I32_ADD);
    wasm_var(s, W_LOCAL_TEE, L32_0);
    wasm_mem(s, W_I32_LOAD, cmp_ofs);

    wasm_tlb_compare(s, a_mask, s_mask);
    wasm8(s, W_I32_EQ);

    wasm_if_hint(s, W_TYPE_I32, true);    /* TLB hit */
    if (hint) {
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_var(s, W_LOCAL_SET, WASM_TLB_HINT_PTR_LOCAL);
        wasm_i32_const(s, mmu_key);
        wasm_var(s, W_LOCAL_SET, WASM_TLB_HINT_MMU_LOCAL);
    }
    wasm_tlb_host(s);
    wasm8(s, W_ELSE);
    if (is_ld || !wasm_notdirty_inline(s, oi)) {
        wasm_i32_const(s, 0);
    }
    wasm8(s, W_END);
    wasm_var(s, W_LOCAL_SET, L32_1);
    if (hint) {
        wasm8(s, W_END);
    }
}

static const void *qemu_ld_helper_ptr(MemOp mop)
{
    switch (mop & MO_SSIZE) {
    case MO_UB:
        return helper_ldub_mmu;
    case MO_SB:
        return helper_ldsb_mmu;
    case MO_UW:
        return helper_lduw_mmu;
    case MO_SW:
        return helper_ldsw_mmu;
    case MO_UL:
        return helper_ldul_mmu;
    case MO_SL:
        return helper_ldsl_mmu;
    case MO_UQ:
        return helper_ldq_mmu;
    default:
        g_assert_not_reached();
    }
}

static const void *qemu_st_helper_ptr(MemOp mop)
{
    switch (mop & MO_SIZE) {
    case MO_8:
        return helper_stb_mmu;
    case MO_16:
        return helper_stw_mmu;
    case MO_32:
        return helper_stl_mmu;
    case MO_64:
        return helper_stq_mmu;
    default:
        g_assert_not_reached();
    }
}

static int wasm_ldst_helper_idx(MemOp mop, bool is_ld)
{
    const void *func = is_ld ? qemu_ld_helper_ptr(mop) : qemu_st_helper_ptr(mop);
    int idx = get_helper_idx(func);

    if (idx >= 0) {
        return idx;
    }
    idx = register_helper(func);
    types_out8(0x60);
    if (is_ld) {
        /* (env, addr, oi, ra) -> tcg_target_ulong */
        types_out8(4);
        types_out8(W_TYPE_I32);
        types_out8(W_TYPE_I64);
        types_out8(W_TYPE_I32);
        types_out8(W_TYPE_I32);
        types_out8(1);
        types_out8(W_TYPE_I64);
    } else {
        /* (env, addr, val, oi, ra) */
        types_out8(5);
        types_out8(W_TYPE_I32);
        types_out8(W_TYPE_I64);
        types_out8((mop & MO_SIZE) == MO_64 ? W_TYPE_I64 : W_TYPE_I32);
        types_out8(W_TYPE_I32);
        types_out8(W_TYPE_I32);
        types_out8(0);
    }
    return idx;
}

#include "../wasm32-direct-memory-meta.c.inc"

static void wasm_qemu_ld(TCGContext *s, TCGReg data, TCGReg addr, MemOpIdx oi)
{
    MemOp mop = get_memop(oi);
    int func_idx = wasm_ldst_helper_idx(mop, true);
    static const uint8_t ld_op[MO_SSIZE + 1] = {
        [MO_UB] = W_I64_LOAD8_U,
        [MO_SB] = W_I64_LOAD8_S,
        [MO_UW] = W_I64_LOAD16_U,
        [MO_SW] = W_I64_LOAD16_S,
        [MO_UL] = W_I64_LOAD32_U,
        [MO_SL] = W_I64_LOAD32_S,
        [MO_UQ] = W_I64_LOAD,
    };

    wasm_direct_memory_mark(WASM_RELOC_MEM_BEGIN);
    wasm_jit_phase(s, 2, "jit:ld");
    wasm_tlb_load(s, addr, oi, true);

    /* fast path */
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm_mem(s, ld_op[mop & MO_SSIZE], 0);
    wasm_set_r(s, data);
    wasm8(s, W_END);
    wasm_jit_phase(s, 2, NULL);

    /*
     * slow path, in its own region: a rewind re-enters here with the
     * locals cleared, which again selects the helper call.
     */
    wasm_new_resumable_block(s);
    wasm_direct_memory_mark(WASM_RELOC_MEM_RESUME);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm8(s, W_I32_EQZ);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
    wasm_get_r32(s, TCG_AREG0);
    wasm_get_r32(s, addr);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm_i32_const(s, oi);
    wasm_i32_const(s, (int32_t)(uintptr_t)s->code_ptr);
    wasm_save_regs(s);
    wasm_phase(s, "mem_slow_ld");
    wasm_call_idx(s, func_idx);
    wasm_phase(s, NULL);
    wasm_set_r(s, data);
    wasm_handle_unwinding(s);
    wasm8(s, W_END);
    wasm_direct_memory_mark(WASM_RELOC_MEM_END);
}

static void wasm_qemu_st(TCGContext *s, TCGReg data, TCGReg addr, MemOpIdx oi)
{
    MemOp mop = get_memop(oi);
    int func_idx = wasm_ldst_helper_idx(mop, false);
    static const uint8_t st_op[MO_64 + 1] = {
        [MO_8] = W_I64_STORE8,
        [MO_16] = W_I64_STORE16,
        [MO_32] = W_I64_STORE32,
        [MO_64] = W_I64_STORE,
    };

    wasm_direct_memory_mark(WASM_RELOC_MEM_BEGIN);
    wasm_jit_phase(s, 2, "jit:st");
    wasm_tlb_load(s, addr, oi, false);

    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm_get_r(s, data);
    wasm_mem(s, st_op[mop & MO_SIZE], 0);
    wasm8(s, W_END);
    wasm_jit_phase(s, 2, NULL);

    wasm_new_resumable_block(s);
    wasm_direct_memory_mark(WASM_RELOC_MEM_RESUME);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm8(s, W_I32_EQZ);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
    wasm_get_r32(s, TCG_AREG0);
    wasm_get_r32(s, addr);
    wasm8(s, W_I64_EXTEND_I32_U);
    if ((mop & MO_SIZE) == MO_64) {
        wasm_get_r(s, data);
    } else {
        wasm_get_r32(s, data);
    }
    wasm_i32_const(s, oi);
    wasm_i32_const(s, (int32_t)(uintptr_t)s->code_ptr);
    wasm_save_regs(s);
    wasm_phase(s, "mem_slow_st");
    wasm_call_idx(s, func_idx);
    wasm_phase(s, NULL);
    wasm_handle_unwinding(s);
    wasm8(s, W_END);
    wasm_direct_memory_mark(WASM_RELOC_MEM_END);
}

static void wasm_exit_tb(TCGContext *s, uintptr_t arg)
{
    wasm_count_exit_tb(s, arg);
    wasm_ctx_store_i32_const(s, WASM_CTX_TB_PTR_OFF, 0);
    wasm_i32_const(s, (int32_t)arg);
    wasm8(s, W_RETURN);
}

/* Patchable positive i32 constant/global index for a module-local IC site. */
static void wasm_ic_operand(TCGContext *s, uint8_t op, unsigned site,
                            unsigned part)
{
    unsigned value = part ? WASM_IC_HDR_GLOBAL + 2 * site + part - 1 : site;

    wasm8(s, op);
    wasm_add_reloc(WASM_RELOC_IC, site, part);
    for (int i = 0; i < 5; i++) {
        wasm8(s, (value & 0x7f) | (i < 4 ? 0x80 : 0));
        value >>= 7;
    }
}

/*
 * Cache the already validated successor, not an unchecked guest PC. Direct
 * exits still read their live chain slot; indirect exits still run the full
 * frontend lookup (PC/CS/flags/generation/breakpoints, including CF_PCREL).
 * Eviction/relink clears incoming caches before recycling a function index.
 * The dispatcher alone fills misses, after checking instance ownership.
 */
static void wasm_ic_exit(TCGContext *s, bool indirect)
{
    unsigned site = cur_hdr->ic_count++;

    tcg_debug_assert(site < 256); /* WasmReloc.arg */
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_ic_operand(s, W_GLOBAL_GET, site, 1);
    wasm8(s, W_I32_EQ);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_LOAD, WASM_TB_INSTANCE_OFF);
    wasm_i32_const(s, 1);
    wasm_mem(s, W_I32_STORE, WASM_INSTANCE_USED_OFF);
    wasm_count_tb_exec(s);
#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, indirect ? &xemu_wasm_stats.n_ic_indirect_hit :
                                    &xemu_wasm_stats.n_ic_direct_hit);
#endif
    wasm_var(s, W_LOCAL_GET, CTX_IDX);
    wasm_ic_operand(s, W_I32_CONST, site, 0);
    wasm8(s, W_RETURN_CALL_INDIRECT);
    wasm_uleb(s, 0); /* (ctx) -> i32 */
    wasm_uleb(s, 0); /* this module's small cache table, not the C table */
    wasm8(s, W_END);
#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, indirect ? &xemu_wasm_stats.n_ic_indirect_miss :
                                    &xemu_wasm_stats.n_ic_direct_miss);
#endif
    if (indirect) {
        /*
         * Polymorphic sites must not pay a JS/table update on every miss.
         * Retry once per 256 misses; stable targets still get an immediate
         * initial fill. Direct chain targets need no replacement throttle.
         */
        wasm_ic_operand(s, W_GLOBAL_GET, site, 2);
        wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
        wasm_ic_operand(s, W_GLOBAL_GET, site, 2);
        wasm_i32_const(s, -1);
        wasm8(s, W_I32_ADD);
        wasm_ic_operand(s, W_GLOBAL_SET, site, 2);
        wasm_count_goto_exit(s, true);
        wasm_i32_const(s, 0);
        wasm8(s, W_RETURN);
        wasm8(s, W_END);
        wasm_i32_const(s, 255);
        wasm_ic_operand(s, W_GLOBAL_SET, site, 2);
    }
    wasm_var(s, W_LOCAL_GET, CTX_IDX);
    wasm_var(s, W_GLOBAL_GET, WASM_IC_SELF_GLOBAL);
    wasm_mem(s, W_I32_STORE, WASM_CTX_IC_SOURCE_OFF);
    wasm_var(s, W_LOCAL_GET, CTX_IDX);
    wasm_ic_operand(s, W_I32_CONST, site, 0);
    wasm_mem(s, W_I32_STORE, WASM_CTX_IC_SLOT_OFF);
}

/*
 * Continue with the TB whose header is in L32_0: loop if it is this one,
 * else try the module-local cache before returning to the dispatcher.
 * Nested inside @depth "if"s of the region.
 */
static void wasm_goto_tb_in_l32_0(TCGContext *s, int depth, int slot)
{
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_ctx_load_i32(s, WASM_CTX_TB_PTR_OFF);
    wasm8(s, W_I32_EQ);
    wasm_if(s, W_BLOCKTYPE_VOID);
    wasm_count_tb_exec(s);
#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_tb_selfloop);
    wasm_phase(s, "spin");
#endif
    wasm_i64_const(s, 0);
    wasm_var(s, W_LOCAL_SET, LBLOCK);
    wasm_br_patch(s, depth + 1, -1);     /* $top, region 0 */
    wasm8(s, W_END);

    /* a region module may branch to a member TB here (tcg/wasm32.c);
     * the depth to $top is completed at the TB end */
    wasm_add_reloc(WASM_RELOC_GOTO, slot < 0 ? 0xff : slot, depth);

    wasm_var(s, W_LOCAL_GET, CTX_IDX);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_mem(s, W_I32_STORE, WASM_CTX_TB_PTR_OFF);
    wasm_ctx_store_i32_const(s, WASM_CTX_DO_INIT_OFF, 1);

    if (wasm32_ic_for_exit(slot < 0)) {
        wasm_ic_exit(s, slot < 0);
    }
    if (slot >= 0 && wasm_link_enabled() && !wasm32_ic_enabled()) {
        uint32_t lh = (uint32_t)(uintptr_t)cur_hdr + WASM_TB_LINK_HDR_OFF + 4 * slot;
        uint32_t lf = (uint32_t)(uintptr_t)cur_hdr + WASM_TB_LINK_FIDX_OFF + 4 * slot;
        /*
         * if (next == link_hdr[slot]) {
         *     inst = next->instance;
         *     if (inst && inst->tb == next && inst->func_idx == link_fidx[slot])
         *         return_call chain.s<slot>(ctx);
         * }
         * ctx.relink = this TB   (link missing or stale: dispatcher relinks)
         */
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_i32_const(s, lh);
        wasm_mem(s, W_I32_LOAD, 0);
        wasm8(s, W_I32_EQ);
        wasm_if(s, W_BLOCKTYPE_VOID);
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_mem(s, W_I32_LOAD, WASM_TB_INSTANCE_OFF);
        wasm_var(s, W_LOCAL_TEE, L32_1);
        wasm_if(s, W_BLOCKTYPE_VOID);
        wasm_var(s, W_LOCAL_GET, L32_1);
        wasm_mem(s, W_I32_LOAD, WASM_INSTANCE_TB_OFF);
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm8(s, W_I32_EQ);
        wasm_var(s, W_LOCAL_GET, L32_1);
        wasm_mem(s, W_I32_LOAD, WASM_INSTANCE_FIDX_OFF);
        wasm_i32_const(s, lf);
        wasm_mem(s, W_I32_LOAD, 0);
        wasm8(s, W_I32_EQ);
        wasm8(s, W_I32_AND);
        wasm_if(s, W_BLOCKTYPE_VOID);
        wasm_var(s, W_LOCAL_GET, L32_1);
        wasm_i32_const(s, 1);
        wasm_mem(s, W_I32_STORE, WASM_INSTANCE_USED_OFF);
        wasm_count_tb_exec(s);
#ifdef EMSCRIPTEN
        wasm_count_tb_stat(s, &xemu_wasm_stats.n_tb_linked);
#endif
        wasm_tlb_hint_drop(s);
        wasm_var(s, W_LOCAL_GET, CTX_IDX);
        wasm8(s, W_RETURN_CALL);
        wasm_uleb(s, CHAIN_S0_IDX + slot);
        wasm8(s, W_END);
        wasm8(s, W_END);
        wasm8(s, W_END);
        wasm_var(s, W_LOCAL_GET, CTX_IDX);
        wasm_i32_const(s, (int32_t)(uintptr_t)cur_hdr);
        wasm_mem(s, W_I32_STORE, WASM_CTX_RELINK_OFF);
#ifdef EMSCRIPTEN
        wasm_count_tb_stat(s, &xemu_wasm_stats.n_tb_linkmiss);
#endif
    }
    wasm_count_goto_exit(s, slot < 0);
    wasm_i32_const(s, 0);
    wasm8(s, W_RETURN);
}

static void wasm_goto_ptr(TCGContext *s, TCGReg arg)
{
#ifdef EMSCRIPTEN
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_tb_gotoptr);
#endif
    wasm_get_r32(s, arg);
    wasm_var(s, W_LOCAL_SET, L32_0);
    wasm_goto_tb_in_l32_0(s, 0, -1);
}

static void wasm_goto_tb(TCGContext *s, int which, uintptr_t reset_ptr)
{
    /* chained iff jmp_target_addr[which] no longer points past goto_tb */
    wasm_i32_const(s, (int32_t)get_jmp_target_addr(s, which));
    wasm_mem(s, W_I32_LOAD, 0);
    wasm_var(s, W_LOCAL_TEE, L32_0);
    wasm_i32_const(s, (int32_t)reset_ptr);
    wasm8(s, W_I32_NE);
    wasm_if(s, W_BLOCKTYPE_VOID);
    wasm_goto_tb_in_l32_0(s, 1, which);
    wasm8(s, W_END);
}

#include "../wasm32-direct-glue.c.inc"
#include "../wasm32-direct-memory-codec.c.inc"

/*
 * ---------------------------------------------------------------------
 * TCI bytecode emission (as tcg/tci/tcg-target.c.inc)
 * ---------------------------------------------------------------------
 */

static bool patch_reloc(tcg_insn_unit *code_ptr, int type,
                        intptr_t value, intptr_t addend)
{
    intptr_t diff = value - (intptr_t)(code_ptr + 1);

    tcg_debug_assert(addend == 0);
    tcg_debug_assert(type == 20);

    if (diff == sextract32(diff, 0, type)) {
        tcg_patch32(code_ptr, deposit32(*code_ptr, 32 - type, type, diff));
        return true;
    }
    return false;
}

static void stack_bounds_check(TCGReg base, intptr_t offset)
{
    if (base == TCG_REG_CALL_STACK) {
        tcg_debug_assert(offset >= 0);
        tcg_debug_assert(offset < (TCG_STATIC_CALL_ARGS_SIZE +
                                   TCG_STATIC_FRAME_SIZE));
    }
}

static void tcg_out_op_l(TCGContext *s, TCGOpcode op, TCGLabel *l0)
{
    tcg_insn_unit insn = 0;

    tcg_out_reloc(s, s->code_ptr, 20, l0, 0);
    insn = deposit32(insn, 0, 8, op);
    tcg_out32(s, insn);
}

static void tcg_out_op_p(TCGContext *s, TCGOpcode op, void *p0)
{
    tcg_insn_unit insn = 0;
    intptr_t diff;

    /* Special case for exit_tb: map null -> 0. */
    if (p0 == NULL) {
        diff = 0;
    } else {
        diff = p0 - (void *)(s->code_ptr + 1);
        tcg_debug_assert(diff != 0);
        if (diff != sextract32(diff, 0, 20)) {
            tcg_raise_tb_overflow(s);
        }
    }
    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 12, 20, diff);
    tcg_out32(s, insn);
}

static void tcg_out_op_r(TCGContext *s, TCGOpcode op, TCGReg r0)
{
    tcg_insn_unit insn = 0;

    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    tcg_out32(s, insn);
}

static void tcg_out_op_v(TCGContext *s, TCGOpcode op)
{
    tcg_out32(s, (uint8_t)op);
}

static void tcg_out_op_ri(TCGContext *s, TCGOpcode op, TCGReg r0, int32_t i1)
{
    tcg_insn_unit insn = 0;

    tcg_debug_assert(i1 == sextract32(i1, 0, 20));
    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 20, i1);
    tcg_out32(s, insn);
}

static void tcg_out_op_rl(TCGContext *s, TCGOpcode op, TCGReg r0, TCGLabel *l1)
{
    tcg_insn_unit insn = 0;

    tcg_out_reloc(s, s->code_ptr, 20, l1, 0);
    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    tcg_out32(s, insn);
}

static void tcg_out_op_rr(TCGContext *s, TCGOpcode op, TCGReg r0, TCGReg r1)
{
    tcg_insn_unit insn = 0;

    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    tcg_out32(s, insn);
}

static void tcg_out_op_rrm(TCGContext *s, TCGOpcode op,
                           TCGReg r0, TCGReg r1, TCGArg m2)
{
    tcg_insn_unit insn = 0;

    tcg_debug_assert(m2 == extract32(m2, 0, 16));
    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    insn = deposit32(insn, 16, 16, m2);
    tcg_out32(s, insn);
}

static void tcg_out_op_rrr(TCGContext *s, TCGOpcode op,
                           TCGReg r0, TCGReg r1, TCGReg r2)
{
    tcg_insn_unit insn = 0;

    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    insn = deposit32(insn, 16, 4, r2);
    tcg_out32(s, insn);
}

static void tcg_out_op_rrs(TCGContext *s, TCGOpcode op,
                           TCGReg r0, TCGReg r1, intptr_t i2)
{
    tcg_insn_unit insn = 0;

    tcg_debug_assert(i2 == sextract32(i2, 0, 16));
    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    insn = deposit32(insn, 16, 16, i2);
    tcg_out32(s, insn);
}

static void tcg_out_op_rrbb(TCGContext *s, TCGOpcode op, TCGReg r0,
                            TCGReg r1, uint8_t b2, uint8_t b3)
{
    tcg_insn_unit insn = 0;

    tcg_debug_assert(b2 == extract32(b2, 0, 6));
    tcg_debug_assert(b3 == extract32(b3, 0, 6));
    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    insn = deposit32(insn, 16, 6, b2);
    insn = deposit32(insn, 22, 6, b3);
    tcg_out32(s, insn);
}

static void tcg_out_op_rrrc(TCGContext *s, TCGOpcode op,
                            TCGReg r0, TCGReg r1, TCGReg r2, TCGCond c3)
{
    tcg_insn_unit insn = 0;

    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    insn = deposit32(insn, 16, 4, r2);
    insn = deposit32(insn, 20, 4, c3);
    tcg_out32(s, insn);
}

static void tcg_out_op_rrrbb(TCGContext *s, TCGOpcode op, TCGReg r0,
                             TCGReg r1, TCGReg r2, uint8_t b3, uint8_t b4)
{
    tcg_insn_unit insn = 0;

    tcg_debug_assert(b3 == extract32(b3, 0, 6));
    tcg_debug_assert(b4 == extract32(b4, 0, 6));
    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    insn = deposit32(insn, 16, 4, r2);
    insn = deposit32(insn, 20, 6, b3);
    insn = deposit32(insn, 26, 6, b4);
    tcg_out32(s, insn);
}

static void tcg_out_op_rrrrrc(TCGContext *s, TCGOpcode op,
                              TCGReg r0, TCGReg r1, TCGReg r2,
                              TCGReg r3, TCGReg r4, TCGCond c5)
{
    tcg_insn_unit insn = 0;

    insn = deposit32(insn, 0, 8, op);
    insn = deposit32(insn, 8, 4, r0);
    insn = deposit32(insn, 12, 4, r1);
    insn = deposit32(insn, 16, 4, r2);
    insn = deposit32(insn, 20, 4, r3);
    insn = deposit32(insn, 24, 4, r4);
    insn = deposit32(insn, 28, 4, c5);
    tcg_out32(s, insn);
}

/* TCI-only helpers (no wasm), for the internal uses of TCG_REG_TMP */
static void tci_out_movi(TCGContext *s, TCGType type,
                         TCGReg ret, tcg_target_long arg)
{
    if (type == TCG_TYPE_I32) {
        arg = (int32_t)arg;
    }
    if (arg == sextract32(arg, 0, 20)) {
        tcg_out_op_ri(s, INDEX_op_tci_movi, ret, arg);
    } else {
        tcg_insn_unit insn = 0;

        new_pool_label(s, arg, 20, s->code_ptr, 0);
        insn = deposit32(insn, 0, 8, INDEX_op_tci_movl);
        insn = deposit32(insn, 8, 4, ret);
        tcg_out32(s, insn);
    }
}

static void tci_out_ldst(TCGContext *s, TCGOpcode op, TCGReg val,
                         TCGReg base, intptr_t offset)
{
    stack_bounds_check(base, offset);
    if (offset != sextract32(offset, 0, 16)) {
        tci_out_movi(s, TCG_TYPE_PTR, TCG_REG_TMP, offset);
        tcg_out_op_rrr(s, INDEX_op_add, TCG_REG_TMP, TCG_REG_TMP, base);
        base = TCG_REG_TMP;
        offset = 0;
    }
    tcg_out_op_rrs(s, op, val, base, offset);
}

/*
 * ---------------------------------------------------------------------
 * Backend interface: every operation emits TCI, then wasm
 * ---------------------------------------------------------------------
 */

static void tcg_out_ld(TCGContext *s, TCGType type, TCGReg val, TCGReg base,
                       intptr_t offset)
{
    WASM_CLASS(11);
    bool i32 = type == TCG_TYPE_I32 || type == TCG_TYPE_F32;

    tci_out_ldst(s, i32 ? INDEX_op_ld32u : INDEX_op_ld, val, base, offset);
    wasm_ldst(s, i32 ? W_I64_LOAD32_U : W_I64_LOAD, false, val, base, offset);
}

static void tcg_out_st(TCGContext *s, TCGType type, TCGReg val, TCGReg base,
                       intptr_t offset)
{
    WASM_CLASS(11);
    bool i32 = type == TCG_TYPE_I32 || type == TCG_TYPE_F32;

    tci_out_ldst(s, i32 ? INDEX_op_st32 : INDEX_op_st, val, base, offset);
    wasm_ldst(s, i32 ? W_I64_STORE32 : W_I64_STORE, true, val, base, offset);
}

static bool tcg_out_mov(TCGContext *s, TCGType type, TCGReg ret, TCGReg arg)
{
    WASM_CLASS(15);
    tcg_out_op_rr(s, INDEX_op_mov, ret, arg);
    wasm_mov(s, ret, arg);
    return true;
}

static void tcg_out_movi(TCGContext *s, TCGType type,
                         TCGReg ret, tcg_target_long arg)
{
    WASM_CLASS(15);
    switch (type) {
    case TCG_TYPE_I32:
    case TCG_TYPE_I64:
        break;
    default:
        g_assert_not_reached();
    }
    tci_out_movi(s, type, ret, arg);
    wasm_movi(s, type, ret, arg);
}

static void tcg_out_extract(TCGContext *s, TCGType type, TCGReg rd,
                            TCGReg rs, unsigned pos, unsigned len)
{
    WASM_CLASS(7);
    tcg_out_op_rrbb(s, INDEX_op_extract, rd, rs, pos, len);
    wasm_extract(s, rd, rs, pos, len);
}

static const TCGOutOpExtract outop_extract = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tcg_out_extract,
};

static void tcg_out_sextract(TCGContext *s, TCGType type, TCGReg rd,
                             TCGReg rs, unsigned pos, unsigned len)
{
    WASM_CLASS(7);
    tcg_out_op_rrbb(s, INDEX_op_sextract, rd, rs, pos, len);
    wasm_sextract(s, rd, rs, pos, len);
}

static const TCGOutOpExtract outop_sextract = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tcg_out_sextract,
};

static const TCGOutOpExtract2 outop_extract2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tcg_out_ext8s(TCGContext *s, TCGType type, TCGReg rd, TCGReg rs)
{
    tcg_out_sextract(s, type, rd, rs, 0, 8);
}

static void tcg_out_ext8u(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_extract(s, TCG_TYPE_REG, rd, rs, 0, 8);
}

static void tcg_out_ext16s(TCGContext *s, TCGType type, TCGReg rd, TCGReg rs)
{
    tcg_out_sextract(s, type, rd, rs, 0, 16);
}

static void tcg_out_ext16u(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_extract(s, TCG_TYPE_REG, rd, rs, 0, 16);
}

static void tcg_out_ext32s(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_sextract(s, TCG_TYPE_I64, rd, rs, 0, 32);
}

static void tcg_out_ext32u(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_extract(s, TCG_TYPE_I64, rd, rs, 0, 32);
}

static void tcg_out_exts_i32_i64(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_ext32s(s, rd, rs);
}

static void tcg_out_extu_i32_i64(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_ext32u(s, rd, rs);
}

static void tcg_out_extrl_i64_i32(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_mov(s, TCG_TYPE_I32, rd, rs);
}

static bool tcg_out_xchg(TCGContext *s, TCGType type, TCGReg r1, TCGReg r2)
{
    return false;
}

static void tcg_out_addi_ptr(TCGContext *s, TCGReg rd, TCGReg rs,
                             tcg_target_long imm)
{
    /* This function is only used for passing structs by reference. */
    g_assert_not_reached();
}

static void tcg_out_call(TCGContext *s, const tcg_insn_unit *func,
                         const TCGHelperInfo *info)
{
    WASM_CLASS(2);
    ffi_cif *cif = info->cif;
    tcg_insn_unit insn = 0;
    uint8_t which;

    if (cif->rtype == &ffi_type_void) {
        which = 0;
    } else {
        tcg_debug_assert(cif->rtype->size == 4 ||
                         cif->rtype->size == 8 ||
                         cif->rtype->size == 16);
        which = ctz32(cif->rtype->size) - 1;
    }
    new_pool_l2(s, 20, s->code_ptr, 0, (uintptr_t)func, (uintptr_t)cif);
    insn = deposit32(insn, 0, 8, INDEX_op_call);
    insn = deposit32(insn, 8, 4, which);
    tcg_out32(s, insn);

    /* after the TCI insn: s->code_ptr is the TCI return address */
    wasm_call(s, func, info);
}

static void tcg_out_exit_tb(TCGContext *s, uintptr_t arg)
{
    WASM_CLASS(14);
    tcg_out_op_p(s, INDEX_op_exit_tb, (void *)arg);
    wasm_direct_verify(s, -1);
    wasm_exit_tb(s, arg);
}

static void tcg_out_goto_tb(TCGContext *s, int which)
{
    WASM_CLASS(13);
    /* indirect jump method. */
    tcg_out_op_p(s, INDEX_op_goto_tb, (void *)get_jmp_target_addr(s, which));
    set_jmp_reset_offset(s, which);
    wasm_direct_verify(s, which);
    wasm_goto_tb(s, which, (uintptr_t)s->code_ptr);
}

static void tcg_out_goto_ptr(TCGContext *s, TCGReg a0)
{
    WASM_CLASS(12);
    tcg_out_op_r(s, INDEX_op_goto_ptr, a0);
    wasm_direct_verify(s, -1);
    wasm_goto_ptr(s, a0);
}

void tb_target_set_jmp_target(const TranslationBlock *tb, int n,
                              uintptr_t jmp_rx, uintptr_t jmp_rw)
{
    /* Always indirect, nothing to do */
}

static void tgen_add(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(15);
    tcg_out_op_rrr(s, INDEX_op_add, a0, a1, a2);
    wasm_binop64(s, W_I64_ADD, a0, a1, a2);
}

static const TCGOutOpBinary outop_add = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_add,
};

/*
 * Carry ops are not implemented; TCG expands add2/sub2 generically.
 */
static const TCGOutOpBinary outop_addco = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpAddSubCarry outop_addci = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpBinary outop_addcio = {
    .base.static_constraint = C_NotImplemented,
};

static void tcg_out_set_carry(TCGContext *s)
{
    g_assert_not_reached();
}

static void tgen_and(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(15);
    tcg_out_op_rrr(s, INDEX_op_and, a0, a1, a2);
    wasm_binop64(s, W_I64_AND, a0, a1, a2);
}

static const TCGOutOpBinary outop_and = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_and,
};

static void tgen_andc(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(10);
    tcg_out_op_rrr(s, INDEX_op_andc, a0, a1, a2);
    wasm_binop_not(s, W_I64_AND, false, a0, a1, a2);
}

static const TCGOutOpBinary outop_andc = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_andc,
};

static void tgen_clz(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(9);
    TCGOpcode opc = (type == TCG_TYPE_I32
                     ? INDEX_op_tci_clz32
                     : INDEX_op_clz);
    tcg_out_op_rrr(s, opc, a0, a1, a2);
    wasm_clz_ctz(s, type, false, a0, a1, a2);
}

static const TCGOutOpBinary outop_clz = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_clz,
};

static void tgen_ctz(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(9);
    TCGOpcode opc = (type == TCG_TYPE_I32
                     ? INDEX_op_tci_ctz32
                     : INDEX_op_ctz);
    tcg_out_op_rrr(s, opc, a0, a1, a2);
    wasm_clz_ctz(s, type, true, a0, a1, a2);
}

static const TCGOutOpBinary outop_ctz = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_ctz,
};

static void tgen_deposit(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1,
                         TCGReg a2, unsigned ofs, unsigned len)
{
    WASM_CLASS(7);
    tcg_out_op_rrrbb(s, INDEX_op_deposit, a0, a1, a2, ofs, len);
    wasm_deposit(s, a0, a1, a2, ofs, len);
}

static const TCGOutOpDeposit outop_deposit = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_deposit,
};

static void tgen_divs(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(8);
    if (type == TCG_TYPE_I32) {
        tcg_out_op_rrr(s, INDEX_op_tci_divs32, a0, a1, a2);
        wasm_binop32(s, W_I32_DIV_S, true, a0, a1, a2);
    } else {
        tcg_out_op_rrr(s, INDEX_op_divs, a0, a1, a2);
        wasm_binop64(s, W_I64_DIV_S, a0, a1, a2);
    }
}

static const TCGOutOpBinary outop_divs = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_divs,
};

static const TCGOutOpDivRem outop_divs2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_divu(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(8);
    if (type == TCG_TYPE_I32) {
        tcg_out_op_rrr(s, INDEX_op_tci_divu32, a0, a1, a2);
        wasm_binop32(s, W_I32_DIV_U, false, a0, a1, a2);
    } else {
        tcg_out_op_rrr(s, INDEX_op_divu, a0, a1, a2);
        wasm_binop64(s, W_I64_DIV_U, a0, a1, a2);
    }
}

static const TCGOutOpBinary outop_divu = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_divu,
};

static const TCGOutOpDivRem outop_divu2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_eqv(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(10);
    tcg_out_op_rrr(s, INDEX_op_eqv, a0, a1, a2);
    wasm_binop_not(s, W_I64_XOR, true, a0, a1, a2);
}

static const TCGOutOpBinary outop_eqv = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_eqv,
};

static void tgen_extrh_i64_i32(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1)
{
    tcg_out_extract(s, TCG_TYPE_I64, a0, a1, 32, 32);
}

static const TCGOutOpUnary outop_extrh_i64_i32 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_extrh_i64_i32,
};

static void tgen_mul(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(8);
    tcg_out_op_rrr(s, INDEX_op_mul, a0, a1, a2);
    wasm_binop64(s, W_I64_MUL, a0, a1, a2);
}

static const TCGOutOpBinary outop_mul = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_mul,
};

static const TCGOutOpMul2 outop_muls2 = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpBinary outop_mulsh = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpMul2 outop_mulu2 = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpBinary outop_muluh = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_nand(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(10);
    tcg_out_op_rrr(s, INDEX_op_nand, a0, a1, a2);
    wasm_binop_not(s, W_I64_AND, true, a0, a1, a2);
}

static const TCGOutOpBinary outop_nand = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_nand,
};

static void tgen_nor(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(10);
    tcg_out_op_rrr(s, INDEX_op_nor, a0, a1, a2);
    wasm_binop_not(s, W_I64_OR, true, a0, a1, a2);
}

static const TCGOutOpBinary outop_nor = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_nor,
};

static void tgen_or(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(15);
    tcg_out_op_rrr(s, INDEX_op_or, a0, a1, a2);
    wasm_binop64(s, W_I64_OR, a0, a1, a2);
}

static const TCGOutOpBinary outop_or = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_or,
};

static void tgen_orc(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(10);
    tcg_out_op_rrr(s, INDEX_op_orc, a0, a1, a2);
    wasm_binop_not(s, W_I64_OR, false, a0, a1, a2);
}

static const TCGOutOpBinary outop_orc = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_orc,
};

static void tgen_rems(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(8);
    if (type == TCG_TYPE_I32) {
        tcg_out_op_rrr(s, INDEX_op_tci_rems32, a0, a1, a2);
        wasm_binop32(s, W_I32_REM_S, true, a0, a1, a2);
    } else {
        tcg_out_op_rrr(s, INDEX_op_rems, a0, a1, a2);
        wasm_binop64(s, W_I64_REM_S, a0, a1, a2);
    }
}

static const TCGOutOpBinary outop_rems = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_rems,
};

static void tgen_remu(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(8);
    if (type == TCG_TYPE_I32) {
        tcg_out_op_rrr(s, INDEX_op_tci_remu32, a0, a1, a2);
        wasm_binop32(s, W_I32_REM_U, false, a0, a1, a2);
    } else {
        tcg_out_op_rrr(s, INDEX_op_remu, a0, a1, a2);
        wasm_binop64(s, W_I64_REM_U, a0, a1, a2);
    }
}

static const TCGOutOpBinary outop_remu = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_remu,
};

static void tgen_rotl(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(3);
    if (type == TCG_TYPE_I32) {
        tcg_out_op_rrr(s, INDEX_op_tci_rotl32, a0, a1, a2);
        wasm_binop32(s, W_I32_ROTL, false, a0, a1, a2);
    } else {
        tcg_out_op_rrr(s, INDEX_op_rotl, a0, a1, a2);
        wasm_binop64(s, W_I64_ROTL, a0, a1, a2);
    }
}

static const TCGOutOpBinary outop_rotl = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_rotl,
};

static void tgen_rotr(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(3);
    if (type == TCG_TYPE_I32) {
        tcg_out_op_rrr(s, INDEX_op_tci_rotr32, a0, a1, a2);
        wasm_binop32(s, W_I32_ROTR, false, a0, a1, a2);
    } else {
        tcg_out_op_rrr(s, INDEX_op_rotr, a0, a1, a2);
        wasm_binop64(s, W_I64_ROTR, a0, a1, a2);
    }
}

static const TCGOutOpBinary outop_rotr = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_rotr,
};

static void tgen_sar(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(3);
    TCGReg t = a1;

    /* TCI: sign-extend a 32-bit input into TMP first */
    if (type < TCG_TYPE_REG) {
        tcg_out_op_rrbb(s, INDEX_op_sextract, TCG_REG_TMP, a1, 0, 32);
        t = TCG_REG_TMP;
    }
    tcg_out_op_rrr(s, INDEX_op_sar, a0, t, a2);
    wasm_shift(s, type, W_I64_SHR_S, a0, a1, a2);
}

static const TCGOutOpBinary outop_sar = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_sar,
};

static void tgen_shl(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(3);
    tcg_out_op_rrr(s, INDEX_op_shl, a0, a1, a2);
    wasm_shift(s, type, W_I64_SHL, a0, a1, a2);
}

static const TCGOutOpBinary outop_shl = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_shl,
};

static void tgen_shr(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(3);
    TCGReg t = a1;

    if (type < TCG_TYPE_REG) {
        tcg_out_op_rrbb(s, INDEX_op_extract, TCG_REG_TMP, a1, 0, 32);
        t = TCG_REG_TMP;
    }
    tcg_out_op_rrr(s, INDEX_op_shr, a0, t, a2);
    wasm_shift(s, type, W_I64_SHR_U, a0, a1, a2);
}

static const TCGOutOpBinary outop_shr = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_shr,
};

static void tgen_sub(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(15);
    tcg_out_op_rrr(s, INDEX_op_sub, a0, a1, a2);
    wasm_binop64(s, W_I64_SUB, a0, a1, a2);
}

static const TCGOutOpSubtract outop_sub = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_sub,
};

static const TCGOutOpAddSubCarry outop_subbo = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpAddSubCarry outop_subbi = {
    .base.static_constraint = C_NotImplemented,
};

static const TCGOutOpAddSubCarry outop_subbio = {
    .base.static_constraint = C_NotImplemented,
};

static void tcg_out_set_borrow(TCGContext *s)
{
    g_assert_not_reached();
}

static void tgen_xor(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    WASM_CLASS(15);
    tcg_out_op_rrr(s, INDEX_op_xor, a0, a1, a2);
    wasm_binop64(s, W_I64_XOR, a0, a1, a2);
}

static const TCGOutOpBinary outop_xor = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_xor,
};

static void tgen_ctpop(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    WASM_CLASS(9);
    tcg_out_op_rr(s, INDEX_op_ctpop, a0, a1);
    wasm_get_r(s, a1);
    wasm8(s, W_I64_POPCNT);
    wasm_set_r(s, a0);
}

static TCGConstraintSetIndex cset_ctpop(TCGType type, unsigned flags)
{
    return type == TCG_TYPE_REG ? C_O1_I1(r, r) : C_NotImplemented;
}

static const TCGOutOpUnary outop_ctpop = {
    .base.static_constraint = C_Dynamic,
    .base.dynamic_constraint = cset_ctpop,
    .out_rr = tgen_ctpop,
};

static void tgen_bswap16(TCGContext *s, TCGType type,
                         TCGReg a0, TCGReg a1, unsigned flags)
{
    WASM_CLASS(9);
    tcg_out_op_rr(s, INDEX_op_bswap16, a0, a1);
    if (flags & TCG_BSWAP_OS) {
        tcg_out_op_rrbb(s, INDEX_op_sextract, a0, a0, 0, 16);
    }
    wasm_bswap16(s, a0, a1, flags);
}

static const TCGOutOpBswap outop_bswap16 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap16,
};

static void tgen_bswap32(TCGContext *s, TCGType type,
                         TCGReg a0, TCGReg a1, unsigned flags)
{
    WASM_CLASS(9);
    tcg_out_op_rr(s, INDEX_op_bswap32, a0, a1);
    if (flags & TCG_BSWAP_OS) {
        tcg_out_op_rrbb(s, INDEX_op_sextract, a0, a0, 0, 32);
    }
    wasm_bswap32(s, a0, a1, flags);
}

static const TCGOutOpBswap outop_bswap32 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap32,
};

static void tgen_bswap64(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    WASM_CLASS(9);
    tcg_out_op_rr(s, INDEX_op_bswap64, a0, a1);
    wasm_bswap64(s, a0, a1);
}

static const TCGOutOpUnary outop_bswap64 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap64,
};

static void tgen_neg(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    WASM_CLASS(10);
    tcg_out_op_rr(s, INDEX_op_neg, a0, a1);
    wasm_i64_const(s, 0);
    wasm_get_r(s, a1);
    wasm8(s, W_I64_SUB);
    wasm_set_r(s, a0);
}

static const TCGOutOpUnary outop_neg = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_neg,
};

static void tgen_not(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    WASM_CLASS(10);
    tcg_out_op_rr(s, INDEX_op_not, a0, a1);
    wasm_get_r(s, a1);
    wasm_i64_const(s, -1);
    wasm8(s, W_I64_XOR);
    wasm_set_r(s, a0);
}

static const TCGOutOpUnary outop_not = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_not,
};

static void tgen_setcond(TCGContext *s, TCGType type, TCGCond cond,
                         TCGReg dest, TCGReg arg1, TCGReg arg2)
{
    WASM_CLASS(4);
    TCGOpcode opc = (type == TCG_TYPE_I32
                     ? INDEX_op_tci_setcond32
                     : INDEX_op_setcond);
    tcg_out_op_rrrc(s, opc, dest, arg1, arg2, cond);
    wasm_cond(s, type, cond, arg1, arg2);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm_set_r(s, dest);
}

static const TCGOutOpSetcond outop_setcond = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_setcond,
};

static void tgen_negsetcond(TCGContext *s, TCGType type, TCGCond cond,
                            TCGReg dest, TCGReg arg1, TCGReg arg2)
{
    WASM_CLASS(4);
    TCGOpcode opc = (type == TCG_TYPE_I32
                     ? INDEX_op_tci_setcond32
                     : INDEX_op_setcond);
    tcg_out_op_rrrc(s, opc, dest, arg1, arg2, cond);
    tcg_out_op_rr(s, INDEX_op_neg, dest, dest);
    wasm_i64_const(s, 0);
    wasm_cond(s, type, cond, arg1, arg2);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm8(s, W_I64_SUB);
    wasm_set_r(s, dest);
}

static const TCGOutOpSetcond outop_negsetcond = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_negsetcond,
};

static void tgen_brcond(TCGContext *s, TCGType type, TCGCond cond,
                        TCGReg arg0, TCGReg arg1, TCGLabel *l)
{
    WASM_CLASS(5);
    TCGOpcode opc = (type == TCG_TYPE_I32
                     ? INDEX_op_tci_setcond32
                     : INDEX_op_setcond);
    tcg_out_op_rrrc(s, opc, TCG_REG_TMP, arg0, arg1, cond);
    tcg_out_op_rl(s, INDEX_op_brcond, TCG_REG_TMP, l);
    wasm_cond(s, type, cond, arg0, arg1);
    wasm_br_to_label(s, l, true);
}

static const TCGOutOpBrcond outop_brcond = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_rr = tgen_brcond,
};

static void tgen_movcond(TCGContext *s, TCGType type, TCGCond cond,
                         TCGReg ret, TCGReg c1, TCGArg c2, bool const_c2,
                         TCGArg vt, bool const_vt, TCGArg vf, bool consf_vf)
{
    WASM_CLASS(4);
    TCGOpcode opc = (type == TCG_TYPE_I32
                     ? INDEX_op_tci_movcond32
                     : INDEX_op_movcond);
    tcg_out_op_rrrrrc(s, opc, ret, c1, c2, vt, vf, cond);
    wasm_get_r(s, vt);
    wasm_get_r(s, vf);
    wasm_cond(s, type, cond, c1, c2);
    wasm8(s, W_SELECT);
    wasm_set_r(s, ret);
}

static const TCGOutOpMovcond outop_movcond = {
    .base.static_constraint = C_O1_I4(r, r, r, r, r),
    .out = tgen_movcond,
};

/* 32-bit hosts only */
__attribute__((unused))
static const TCGOutOpBrcond2 outop_brcond2 = {
    .base.static_constraint = C_NotImplemented,
};

__attribute__((unused))
static const TCGOutOpSetcond2 outop_setcond2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tcg_out_mb(TCGContext *s, unsigned a0)
{
    tcg_out_op_v(s, INDEX_op_mb);
    /* atomic.fence */
    wasm8(s, 0xfe);
    wasm8(s, 0x03);
    wasm8(s, 0x00);
}

static void tcg_out_br(TCGContext *s, TCGLabel *l)
{
    WASM_CLASS(6);
    tcg_out_op_l(s, INDEX_op_br, l);
    wasm_br_to_label(s, l, false);
}

static void tgen_ld8u(TCGContext *s, TCGType type, TCGReg dest,
                      TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_ld8u, dest, base, offset);
    wasm_ldst(s, W_I64_LOAD8_U, false, dest, base, offset);
}

static const TCGOutOpLoad outop_ld8u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld8u,
};

static void tgen_ld8s(TCGContext *s, TCGType type, TCGReg dest,
                      TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_ld8s, dest, base, offset);
    wasm_ldst(s, W_I64_LOAD8_S, false, dest, base, offset);
}

static const TCGOutOpLoad outop_ld8s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld8s,
};

static void tgen_ld16u(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_ld16u, dest, base, offset);
    wasm_ldst(s, W_I64_LOAD16_U, false, dest, base, offset);
}

static const TCGOutOpLoad outop_ld16u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld16u,
};

static void tgen_ld16s(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_ld16s, dest, base, offset);
    wasm_ldst(s, W_I64_LOAD16_S, false, dest, base, offset);
}

static const TCGOutOpLoad outop_ld16s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld16s,
};

static void tgen_ld32u(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_ld32u, dest, base, offset);
    wasm_ldst(s, W_I64_LOAD32_U, false, dest, base, offset);
}

static const TCGOutOpLoad outop_ld32u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld32u,
};

static void tgen_ld32s(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_ld32s, dest, base, offset);
    wasm_ldst(s, W_I64_LOAD32_S, false, dest, base, offset);
}

static const TCGOutOpLoad outop_ld32s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld32s,
};

static void tgen_st8(TCGContext *s, TCGType type, TCGReg data,
                     TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_st8, data, base, offset);
    wasm_ldst(s, W_I64_STORE8, true, data, base, offset);
}

static const TCGOutOpStore outop_st8 = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tgen_st8,
};

static void tgen_st16(TCGContext *s, TCGType type, TCGReg data,
                      TCGReg base, ptrdiff_t offset)
{
    WASM_CLASS(11);
    tci_out_ldst(s, INDEX_op_st16, data, base, offset);
    wasm_ldst(s, W_I64_STORE16, true, data, base, offset);
}

static const TCGOutOpStore outop_st16 = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tgen_st16,
};

static const TCGOutOpStore outop_st = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tcg_out_st,
};

static void tgen_qemu_ld(TCGContext *s, TCGType type, TCGReg data,
                         TCGReg addr, MemOpIdx oi)
{
    WASM_CLASS(1);
    if (oi & ~0xffff) {
        tci_out_movi(s, TCG_TYPE_I32, TCG_REG_TMP, oi);
        tcg_out_op_rrr(s, INDEX_op_tci_qemu_ld_rrr, data, addr, TCG_REG_TMP);
    } else {
        tcg_out_op_rrm(s, INDEX_op_qemu_ld, data, addr, oi);
    }
    wasm_direct_memory_site(s, data, addr, oi, false);
    wasm_qemu_ld(s, data, addr, oi);
}

static const TCGOutOpQemuLdSt outop_qemu_ld = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_qemu_ld,
};

static const TCGOutOpQemuLdSt2 outop_qemu_ld2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_qemu_st(TCGContext *s, TCGType type, TCGReg data,
                         TCGReg addr, MemOpIdx oi)
{
    WASM_CLASS(1);
    if (oi & ~0xffff) {
        tci_out_movi(s, TCG_TYPE_I32, TCG_REG_TMP, oi);
        tcg_out_op_rrr(s, INDEX_op_tci_qemu_st_rrr, data, addr, TCG_REG_TMP);
    } else {
        tcg_out_op_rrm(s, INDEX_op_qemu_st, data, addr, oi);
    }
    wasm_direct_memory_site(s, data, addr, oi, true);
    wasm_qemu_st(s, data, addr, oi);
}

static const TCGOutOpQemuLdSt outop_qemu_st = {
    .base.static_constraint = C_O0_I2(r, r),
    .out = tgen_qemu_st,
};

static const TCGOutOpQemuLdSt2 outop_qemu_st2 = {
    .base.static_constraint = C_NotImplemented,
};

static inline bool tcg_out_sti(TCGContext *s, TCGType type, TCGArg val,
                               TCGReg base, intptr_t ofs)
{
    return false;
}

/* Test if a constant matches the constraint. */
static bool tcg_target_const_match(int64_t val, int ct,
                                   TCGType type, TCGCond cond, int vece)
{
    return ct & TCG_CT_CONST;
}

static void tcg_out_nop_fill(tcg_insn_unit *p, int count)
{
    memset(p, 0, sizeof(*p) * count);
}

static void tcg_target_init(TCGContext *s)
{
    /* The current code uses uint8_t for tcg operations. */
    tcg_debug_assert(tcg_op_defs_max <= UINT8_MAX);

    /* Registers available for 32 bit operations. */
    tcg_target_available_regs[TCG_TYPE_I32] = BIT(TCG_TARGET_NB_REGS) - 1;
    /* Registers available for 64 bit operations. */
    tcg_target_available_regs[TCG_TYPE_I64] = BIT(TCG_TARGET_NB_REGS) - 1;
    /* Host FP values are bit patterns in the same registers. */
    tcg_target_available_regs[TCG_TYPE_F32] = BIT(TCG_TARGET_NB_REGS) - 1;
    tcg_target_available_regs[TCG_TYPE_F64] = BIT(TCG_TARGET_NB_REGS) - 1;
    /*
     * The "registers" are interpreter locals / module globals and cannot
     * be clobbered by helpers, but calls return into R0 (and R1).
     */
    tcg_target_call_clobber_regs =
        MAKE_64BIT_MASK(TCG_REG_R0, 128 / TCG_TARGET_REG_BITS);

    s->reserved_regs = 0;
    tcg_regset_set_reg(s->reserved_regs, TCG_REG_TMP);
    tcg_regset_set_reg(s->reserved_regs, TCG_REG_CALL_STACK);

    /* The call arguments come first, followed by the temp storage. */
    tcg_set_frame(s, TCG_REG_CALL_STACK, TCG_STATIC_CALL_ARGS_SIZE,
                  TCG_STATIC_FRAME_SIZE);
}

/* Generate global QEMU prologue and epilogue code. */
static inline void tcg_target_qemu_prologue(TCGContext *s)
{
}

bool tcg_target_has_memory_bswap(MemOp memop)
{
    return true;
}

static bool tcg_out_qemu_ld_slow_path(TCGContext *s, TCGLabelQemuLdst *l)
{
    g_assert_not_reached();
}

static bool tcg_out_qemu_st_slow_path(TCGContext *s, TCGLabelQemuLdst *l)
{
    g_assert_not_reached();
}

/*
 * Host floating point (TCG_TARGET_HAS_fpu), for the i386 hard-FPU x87 path.
 * F32/F64 values are bit patterns in the i64 registers (F32 zero-extended),
 * reinterpreted around native wasm f32/f64 instructions. See wasm32.h.
 */
#define W_F32_EQ            0x5b
#define W_F32_NE            0x5c
#define W_F64_EQ            0x61
#define W_F64_NE            0x62
#define W_F64_LT            0x63
#define W_F64_LE            0x65
#define W_F64_GE            0x66
#define W_F32_ABS           0x8b
#define W_F32_NEG           0x8c
#define W_F32_SQRT          0x91
#define W_F32_ADD           0x92
#define W_F32_SUB           0x93
#define W_F32_MUL           0x94
#define W_F32_DIV           0x95
#define W_F64_ABS           0x99
#define W_F64_NEG           0x9a
#define W_F64_CEIL          0x9b
#define W_F64_FLOOR         0x9c
#define W_F64_TRUNC         0x9d
#define W_F64_NEAREST       0x9e
#define W_F64_SQRT          0x9f
#define W_F64_ADD           0xa0
#define W_F64_SUB           0xa1
#define W_F64_MUL           0xa2
#define W_F64_DIV           0xa3
#define W_F32_CONVERT_I32_S 0xb2
#define W_F32_CONVERT_I64_S 0xb4
#define W_F32_DEMOTE_F64    0xb6
#define W_F64_CONVERT_I32_S 0xb7
#define W_F64_CONVERT_I64_S 0xb9
#define W_F64_PROMOTE_F32   0xbb
#define W_I32_REINTERPRET_F32 0xbc
#define W_I64_REINTERPRET_F64 0xbd
#define W_F32_REINTERPRET_I32 0xbe
#define W_F64_REINTERPRET_I64 0xbf
#define W_F64_CONST         0x44

#ifdef EMSCRIPTEN
#define W_I32_LOAD8_U_   0x2d
#define W_I32_LOAD16_U_  0x2f
#define W_I32_STORE16_   0x3b
#define W_I32_SUB_       0x6b

int xemu_wasm_sse_status_ofs(int which) __attribute__((weak));

/* push call argument slot j (a pointer) */
static void sse_arg(TCGContext *s, int j)
{
    wasm_get_r32(s, TCG_REG_CALL_STACK);
    wasm_mem(s, W_I32_LOAD, TCG_TARGET_CALL_STACK_OFFSET + j * 8);
}

/* push (v is +-0 or a normal float), v a local holding the bits */
static void sse_zon(TCGContext *s, int v)
{
    wasm_var(s, W_LOCAL_GET, v);
    wasm_i32_const(s, 23);
    wasm8(s, W_I32_SHR_U);
    wasm_i32_const(s, 0xff);
    wasm8(s, W_I32_AND);
    wasm_i32_const(s, 1);
    wasm8(s, W_I32_SUB_);
    wasm_i32_const(s, 254);
    wasm8(s, W_I32_LT_U);
    wasm_var(s, W_LOCAL_GET, v);
    wasm_i32_const(s, 1);
    wasm8(s, W_I32_SHL);
    wasm8(s, W_I32_EQZ);
    wasm8(s, W_I32_OR);
}

/* push ((v << 1) != 0): v is not +-0 */
static void sse_nonzero(TCGContext *s, int v)
{
    wasm_var(s, W_LOCAL_GET, v);
    wasm_i32_const(s, 1);
    wasm8(s, W_I32_SHL);
    wasm_i32_const(s, 0);
    wasm8(s, W_I32_NE);
}

static void sse_f32(TCGContext *s, int v)
{
    wasm_var(s, W_LOCAL_GET, v);
    wasm8(s, W_F32_REINTERPRET_I32);
}

static void sse_f64(TCGContext *s, int v)
{
    sse_f32(s, v);
    wasm8(s, W_F64_PROMOTE_F32);
}

int xemu_wasm_ccop_val(int which) __attribute__((weak));

#define W_I32_POPCNT_ 0x69

/*
 * cc_compute_c / cc_compute_all(dst, src1, src2, op) inline for the CC_OP
 * values that dominate in practice (the dashboard intro: cc_compute_all
 * with LOGICB 1.7M/s, cc_compute_c with EFLAGS 1.4M/s), with exactly the
 * helper's result (target/i386/tcg/cc_helper.c); other ops call it.
 *   c:   EFLAGS -> src1 & 1;  LOGICx -> 0;  INCx/DECx -> src1
 *   all: EFLAGS -> src1;  LOGICB/LOGICL -> P|Z|S of the result
 * XEMU_WASM_CC_FAST=0 disables, =2 verifies against the helper.
 * Locals: L32_0 = op, L32_2 = dst, L32_3 = src1.
 */
static void cc_logic_flags(TCGContext *s, bool byte)
{
    /* PF: even parity of the low byte */
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm_i32_const(s, 0xff);
    wasm8(s, W_I32_AND);
    wasm8(s, W_I32_POPCNT_);
    wasm_i32_const(s, 1);
    wasm8(s, W_I32_AND);
    wasm_i32_const(s, 1);
    wasm8(s, W_I32_XOR);
    wasm_i32_const(s, 2);
    wasm8(s, W_I32_SHL);
    /* ZF */
    wasm_var(s, W_LOCAL_GET, L32_2);
    if (byte) {
        wasm_i32_const(s, 0xff);
        wasm8(s, W_I32_AND);
    }
    wasm8(s, W_I32_EQZ);
    wasm_i32_const(s, 6);
    wasm8(s, W_I32_SHL);
    wasm8(s, W_I32_OR);
    /* SF */
    wasm_var(s, W_LOCAL_GET, L32_2);
    if (!byte) {
        wasm_i32_const(s, 24);
        wasm8(s, W_I32_SHR_U);
    }
    wasm_i32_const(s, 0x80);
    wasm8(s, W_I32_AND);
    wasm8(s, W_I32_OR);
}

/* push the address of 32-bit lane ((order >> sh) & 3) of argument @arg */
static void shufps_lane(TCGContext *s, int arg, int sh)
{
    sse_arg(s, arg);
    wasm_var(s, W_LOCAL_GET, L32_0);
    if (sh) {
        wasm_i32_const(s, sh);
        wasm8(s, W_I32_SHR_U);
    }
    wasm_i32_const(s, 3);
    wasm8(s, W_I32_AND);
    wasm_i32_const(s, 2);
    wasm8(s, W_I32_SHL);
    wasm8(s, W_I32_ADD);
    wasm_mem(s, W_I32_LOAD, 0);
}

/*
 * shufps_xmm(d, v, s, order) inline: a pure lane shuffle (SHUFFLE4 in
 * target/i386/ops_sse.h). All four lanes are read before d is written,
 * as d may be v or s. XEMU_WASM_SHUFPS_INLINE=0 disables.
 */
static bool wasm_call_shufps_inline(TCGContext *s, const tcg_insn_unit *func,
                                    const TCGHelperInfo *info)
{
    static int enabled = -1;

    if (enabled < 0) {
        /* 0: off, 1: on, 2: verify against the helper (testing) */
        const char *e = getenv("XEMU_WASM_SHUFPS_INLINE");
        enabled = e ? atoi(e) : 1;
    }
    if (!enabled || !info->name || strcmp(info->name, "shufps_xmm") ||
        helper_nargs(info->typemask) != 4 || (info->typemask & 7) !=
        dh_typecode_void) {
        return false;
    }
    wasm_jit_phase(s, 1, "jit:shufps");
    sse_arg(s, 3);
    wasm_var(s, W_LOCAL_SET, L32_0);
    shufps_lane(s, 1, 0);
    wasm_var(s, W_LOCAL_SET, L32_1);
    shufps_lane(s, 1, 2);
    wasm_var(s, W_LOCAL_SET, L32_2);
    shufps_lane(s, 2, 4);
    wasm_var(s, W_LOCAL_SET, L32_3);
    shufps_lane(s, 2, 6);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm_var(s, W_LOCAL_SET, L64_0);
    if (enabled == 2) {
        int func_idx = get_helper_idx(func);
        if (func_idx < 0) {
            func_idx = register_helper(func);
            gen_func_type(info);
        }
        wasm_push_call_args(s, info->typemask, 4);
        wasm_jit_phase(s, 1, wasm_helper_phase_name(info));
        wasm_call_idx(s, func_idx);
        wasm_jit_phase(s, 1, "jit:shufps");
        wasm_count(s, &xemu_wasm_stats.n_sse_checked);
        for (int k = 0; k < 4; k++) {
            sse_arg(s, 0);
            wasm_mem(s, W_I32_LOAD, 4 * k);
            if (k < 3) {
                wasm_var(s, W_LOCAL_GET, L32_1 + k);
            } else {
                wasm_var(s, W_LOCAL_GET, L64_0);
                wasm8(s, W_I32_WRAP_I64);
            }
            wasm8(s, W_I32_NE);
            if (k) {
                wasm8(s, W_I32_OR);
            }
        }
        wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
        wasm_count(s, &xemu_wasm_stats.n_sse_mismatch);
        wasm8(s, W_END);
        wasm_jit_phase(s, 1, NULL);
        return true;
    }
    sse_arg(s, 0);
    wasm_var(s, W_LOCAL_GET, L32_1);
    wasm_mem(s, W_I32_STORE, 0);
    sse_arg(s, 0);
    wasm_var(s, W_LOCAL_GET, L32_2);
    wasm_mem(s, W_I32_STORE, 4);
    sse_arg(s, 0);
    wasm_var(s, W_LOCAL_GET, L32_3);
    wasm_mem(s, W_I32_STORE, 8);
    sse_arg(s, 0);
    wasm_var(s, W_LOCAL_GET, L64_0);
    wasm8(s, W_I32_WRAP_I64);
    wasm_mem(s, W_I32_STORE, 12);
    wasm_jit_phase(s, 1, NULL);
    return true;
}

static bool wasm_call_cc_fast(TCGContext *s, const tcg_insn_unit *func,
                              const TCGHelperInfo *info)
{
    static int enabled = -1;
    unsigned typemask = info->typemask;
    int nargs = helper_nargs(typemask);
    int func_idx, nest = 0;
    bool all;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_CC_FAST");
        enabled = e ? atoi(e) : 1;
        if (!xemu_wasm_ccop_val) {
            enabled = 0;
        }
    }
    if (!enabled || !info->name || nargs != 4) {
        return false;
    }
    if (!strcmp(info->name, "cc_compute_all")) {
        all = true;
    } else if (!strcmp(info->name, "cc_compute_c")) {
        all = false;
    } else {
        return false;
    }
    /* target_ulong must be i32 (32-bit guest), op an int */
    for (int j = 1; j <= 4; j++) {
        int tc = extract32(typemask, j * 3, 3);
        if (tc != dh_typecode_i32 && tc != dh_typecode_s32) {
            return false;
        }
    }
    func_idx = get_helper_idx(func);
    if (func_idx < 0) {
        func_idx = register_helper(func);
        gen_func_type(info);
    }

    wasm_jit_phase(s, 1, "jit:cc");
    sse_arg(s, 3);
    wasm_var(s, W_LOCAL_SET, L32_0);
    sse_arg(s, 0);
    wasm_var(s, W_LOCAL_SET, L32_2);
    sse_arg(s, 1);
    wasm_var(s, W_LOCAL_SET, L32_3);

    /* op == EFLAGS */
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_i32_const(s, xemu_wasm_ccop_val(0));
    wasm8(s, W_I32_EQ);
    wasm_if_hint(s, W_TYPE_I32, true);
    wasm_var(s, W_LOCAL_GET, L32_3);
    if (!all) {
        wasm_i32_const(s, 1);
        wasm8(s, W_I32_AND);
    }
    wasm8(s, W_ELSE);
    nest++;
    if (all) {
        for (int k = 0; k < 2; k++) {
            /* op == LOGICB / LOGICL */
            wasm_var(s, W_LOCAL_GET, L32_0);
            wasm_i32_const(s, xemu_wasm_ccop_val(1 + k));
            wasm8(s, W_I32_EQ);
            wasm_if_hint(s, W_TYPE_I32, true);
            cc_logic_flags(s, k == 0);
            wasm8(s, W_ELSE);
            nest++;
        }
    } else {
        /* (op - LOGICB) <u 4: LOGICB..LOGICQ */
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_i32_const(s, xemu_wasm_ccop_val(1));
        wasm8(s, W_I32_SUB_);
        wasm_i32_const(s, 4);
        wasm8(s, W_I32_LT_U);
        wasm_if_hint(s, W_TYPE_I32, true);
        wasm_i32_const(s, 0);
        wasm8(s, W_ELSE);
        nest++;
        /* (op - INCB) <u 8: INCB..DECQ */
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_i32_const(s, xemu_wasm_ccop_val(3));
        wasm8(s, W_I32_SUB_);
        wasm_i32_const(s, 8);
        wasm8(s, W_I32_LT_U);
        wasm_if_hint(s, W_TYPE_I32, true);
        wasm_var(s, W_LOCAL_GET, L32_3);
        wasm8(s, W_ELSE);
        nest++;
    }
    /* any other op: the helper */
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_helper);
    wasm_count_helper(s, func, info);
    wasm_set_tci_tb_ptr(s);
    wasm_push_call_args(s, typemask, nargs);
    wasm_phase(s, wasm_helper_phase_name(info));
    wasm_call_idx(s, func_idx);
    wasm_phase(s, (wasm_jit_profile() & 1) ? "jit:cc" : NULL);
    while (nest--) {
        wasm8(s, W_END);
    }

    if (enabled == 2) {
        /* verify: the helper's result must equal the inline one */
        wasm_var(s, W_LOCAL_SET, L32_1);
        wasm_count(s, &xemu_wasm_stats.n_sse_checked);
        wasm_push_call_args(s, typemask, nargs);
        wasm_jit_phase(s, 1, wasm_helper_phase_name(info));
        wasm_call_idx(s, func_idx);
        wasm_jit_phase(s, 1, "jit:cc");
        wasm_var(s, W_LOCAL_GET, L32_1);
        wasm8(s, W_I32_NE);
        wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
        wasm_count(s, &xemu_wasm_stats.n_sse_mismatch);
        wasm8(s, W_END);
        wasm_var(s, W_LOCAL_GET, L32_1);
    }
    wasm_call_results(s, typemask & 7);
    wasm_jit_phase(s, 1, NULL);
    return true;
}

/*
 * addss/subss/mulss/divss inline: the exact fast path of ffp_f32_arith()
 * (target/i386/tcg/fast_fp.h) emitted as wasm, so the common case needs
 * no helper call. Same gates: round-to-nearest-even, operands +-0 or
 * normal, result finite with r == 0 or |r| > FLT_MIN, no zero product or
 * quotient from nonzero inputs, no division by zero; the inexact flag is
 * computed exactly (TwoSum for add/sub, exact double products for mul/div).
 * Anything else, and a rewind, runs the original helper (L32_1 == 0).
 *
 * Locals: L32_2 = a bits, L32_3 = b bits (negated for sub), L32_0 = r bits,
 * L32_1 = done.
 */
static bool wasm_call_sse_fast(TCGContext *s, const tcg_insn_unit *func,
                               const TCGHelperInfo *info)
{
    static int enabled = -1;
    unsigned typemask = info->typemask;
    int nargs = helper_nargs(typemask);
    int op, rm_ofs, fl_ofs, func_idx;

    if (enabled < 0) {
        /* 0: off, 1: on, 2: verify against the helper (testing) */
        const char *e = getenv("XEMU_WASM_SSE_FAST");
        enabled = e ? atoi(e) : 1;
        if (!xemu_wasm_sse_status_ofs || xemu_wasm_sse_status_ofs(0) < 0) {
            enabled = 0;
        }
    }
    if (!enabled || !info->name) {
        return false;
    }
    if (!strcmp(info->name, "addss")) {
        op = W_F32_ADD;
    } else if (!strcmp(info->name, "subss")) {
        op = W_F32_SUB;
    } else if (!strcmp(info->name, "mulss")) {
        op = W_F32_MUL;
    } else if (!strcmp(info->name, "divss")) {
        op = W_F32_DIV;
    } else {
        return false;
    }
    /* helper_xxxss(env, Reg *d, Reg *v, Reg *s): void, four pointers */
    if (nargs != 4 || (typemask & 7) != dh_typecode_void) {
        return false;
    }
    for (int j = 1; j <= 4; j++) {
        if (extract32(typemask, j * 3, 3) != dh_typecode_ptr) {
            return false;
        }
    }
    rm_ofs = xemu_wasm_sse_status_ofs(0);
    fl_ofs = xemu_wasm_sse_status_ofs(1);

    func_idx = get_helper_idx(func);
    if (func_idx < 0) {
        func_idx = register_helper(func);
        gen_func_type(info);
    }
    wasm_count_tb_stat(s, &xemu_wasm_stats.n_helper);
    wasm_count_helper(s, func, info);

    wasm_jit_phase(s, 1, "jit:sse");
    /* helper(env, d, v, s): d.s0 = v.s0 OP s.s0; d.s1..3 = v.s1..3 */
    wasm_i32_const(s, 0);
    wasm_var(s, W_LOCAL_SET, L32_1);
    sse_arg(s, 2);
    wasm_mem(s, W_I32_LOAD, 0);
    wasm_var(s, W_LOCAL_SET, L32_2);
    sse_arg(s, 3);
    wasm_mem(s, W_I32_LOAD, 0);
    wasm_var(s, W_LOCAL_SET, L32_3);

    /* rounding nearest-even && zon(a) && zon(b) [&& b != 0 for div] */
    wasm_get_r32(s, TCG_AREG0);
    wasm_mem(s, W_I32_LOAD8_U_, rm_ofs);
    wasm8(s, W_I32_EQZ);
    sse_zon(s, L32_2);
    wasm8(s, W_I32_AND);
    sse_zon(s, L32_3);
    wasm8(s, W_I32_AND);
    if (op == W_F32_DIV) {
        sse_nonzero(s, L32_3);
        wasm8(s, W_I32_AND);
    }
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
    {
        if (op == W_F32_SUB) {
            /* a - b == a + (-b), exactly */
            wasm_var(s, W_LOCAL_GET, L32_3);
            wasm_i32_const(s, 0x80000000);
            wasm8(s, W_I32_XOR);
            wasm_var(s, W_LOCAL_SET, L32_3);
            op = W_F32_ADD;
        }
        sse_f32(s, L32_2);
        sse_f32(s, L32_3);
        wasm8(s, op);
        wasm8(s, W_I32_REINTERPRET_F32);
        wasm_var(s, W_LOCAL_SET, L32_0);

        /* res_ok(r): (e-2) <u 253 || r == +-0 || (e == 1 && frac != 0) */
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_i32_const(s, 23);
        wasm8(s, W_I32_SHR_U);
        wasm_i32_const(s, 0xff);
        wasm8(s, W_I32_AND);
        wasm_i32_const(s, 2);
        wasm8(s, W_I32_SUB_);
        wasm_i32_const(s, 253);
        wasm8(s, W_I32_LT_U);
        sse_nonzero(s, L32_0);
        wasm8(s, W_I32_EQZ);
        wasm8(s, W_I32_OR);
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_i32_const(s, 23);
        wasm8(s, W_I32_SHR_U);
        wasm_i32_const(s, 0xff);
        wasm8(s, W_I32_AND);
        wasm_i32_const(s, 1);
        wasm8(s, W_I32_EQ);
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm_i32_const(s, 0x7fffff);
        wasm8(s, W_I32_AND);
        wasm_i32_const(s, 0);
        wasm8(s, W_I32_NE);
        wasm8(s, W_I32_AND);
        wasm8(s, W_I32_OR);
        if (op == W_F32_MUL || op == W_F32_DIV) {
            /* not (r == 0 from a nonzero product / dividend): underflow */
            sse_nonzero(s, L32_0);
            sse_nonzero(s, L32_2);
            wasm8(s, W_I32_EQZ);
            wasm8(s, W_I32_OR);
            if (op == W_F32_MUL) {
                sse_nonzero(s, L32_3);
                wasm8(s, W_I32_EQZ);
                wasm8(s, W_I32_OR);
            }
            wasm8(s, W_I32_AND);
        }
        wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
        {
            /* push inexact */
            switch (op) {
            case W_F32_ADD:
                /* bb = r - a; err = (a - (r - bb)) + (b - bb) */
                sse_f32(s, L32_2);
                sse_f32(s, L32_0);
                sse_f32(s, L32_0);
                sse_f32(s, L32_2);
                wasm8(s, W_F32_SUB);            /* bb */
                wasm8(s, W_F32_SUB);            /* r - bb */
                wasm8(s, W_F32_SUB);            /* a - (r - bb) */
                sse_f32(s, L32_3);
                sse_f32(s, L32_0);
                sse_f32(s, L32_2);
                wasm8(s, W_F32_SUB);            /* bb */
                wasm8(s, W_F32_SUB);            /* b - bb */
                wasm8(s, W_F32_ADD);
                wasm8(s, W_I32_REINTERPRET_F32);
                wasm_i32_const(s, 1);
                wasm8(s, W_I32_SHL);            /* err != +-0 */
                wasm_i32_const(s, 0);
                wasm8(s, W_I32_NE);
                break;
            case W_F32_MUL:
                sse_f64(s, L32_0);
                sse_f64(s, L32_2);
                sse_f64(s, L32_3);
                wasm8(s, W_F64_MUL);
                wasm8(s, W_F64_NE);
                break;
            default: /* W_F32_DIV */
                sse_f64(s, L32_0);
                sse_f64(s, L32_3);
                wasm8(s, W_F64_MUL);
                sse_f64(s, L32_2);
                wasm8(s, W_F64_NE);
                break;
            }
            if (enabled == 2) {
                wasm8(s, W_I64_EXTEND_I32_U);
                wasm_var(s, W_LOCAL_SET, L64_1);
                wasm_i32_const(s, 1);
                wasm_var(s, W_LOCAL_SET, L32_1);
                goto verify_done;
            }
            wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
            wasm_get_r32(s, TCG_AREG0);
            wasm_get_r32(s, TCG_AREG0);
            wasm_mem(s, W_I32_LOAD16_U_, fl_ofs);
            wasm_i32_const(s, 0x10);            /* float_flag_inexact */
            wasm8(s, W_I32_OR);
            wasm_mem(s, W_I32_STORE16_, fl_ofs);
            wasm8(s, W_END);

            /* d.s1..3 = v.s1..3 (read before d.s0 is written: d may be v) */
            sse_arg(s, 1);
            sse_arg(s, 2);
            wasm_mem(s, W_I32_LOAD, 4);
            wasm_mem(s, W_I32_STORE, 4);
            sse_arg(s, 1);
            sse_arg(s, 2);
            wasm_mem(s, W_I64_LOAD, 8);
            wasm_mem(s, W_I64_STORE, 8);
            sse_arg(s, 1);
            wasm_var(s, W_LOCAL_GET, L32_0);
            wasm_mem(s, W_I32_STORE, 0);
            wasm_i32_const(s, 1);
            wasm_var(s, W_LOCAL_SET, L32_1);
        }
    verify_done:
        wasm8(s, W_END);
    }
    wasm8(s, W_END);

    wasm_jit_phase(s, 1, NULL);
    /* fallback: the original helper call */
    wasm_new_resumable_block(s);
    if (enabled == 2) {
        /* run the helper anyway, with the inexact flag cleared */
        wasm_get_r32(s, TCG_AREG0);
        wasm_mem(s, W_I32_LOAD16_U_, fl_ofs);
        wasm8(s, W_I64_EXTEND_I32_U);
        wasm_var(s, W_LOCAL_SET, L64_0);
        wasm_get_r32(s, TCG_AREG0);
        wasm_var(s, W_LOCAL_GET, L64_0);
        wasm8(s, W_I32_WRAP_I64);
        wasm_i32_const(s, ~0x10);
        wasm8(s, W_I32_AND);
        wasm_mem(s, W_I32_STORE16_, fl_ofs);
        wasm_i32_const(s, 1);
    } else {
        wasm_var(s, W_LOCAL_GET, L32_1);
        wasm8(s, W_I32_EQZ);
    }
    wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
    wasm_callsite_phase(s, info, false);
    wasm_set_tci_tb_ptr(s);
    wasm_push_call_args(s, typemask, nargs);
    wasm_save_regs(s);
    wasm_phase(s, wasm_helper_phase_name(info));
    wasm_call_idx(s, func_idx);
    wasm_callsite_phase(s, info, true);
    wasm_handle_unwinding(s);
    wasm_jit_phase(s, 1, NULL);
    wasm8(s, W_END);
    if (enabled == 2) {
        wasm_var(s, W_LOCAL_GET, L32_1);
        wasm_if_hint(s, W_BLOCKTYPE_VOID, true);
        wasm_count(s, &xemu_wasm_stats.n_sse_checked);
        /* d.s0 != r || inexact(helper) != inexact(fast) */
        sse_arg(s, 1);
        wasm_mem(s, W_I32_LOAD, 0);
        wasm_var(s, W_LOCAL_GET, L32_0);
        wasm8(s, W_I32_NE);
        wasm_get_r32(s, TCG_AREG0);
        wasm_mem(s, W_I32_LOAD16_U_, fl_ofs);
        wasm_i32_const(s, 4);
        wasm8(s, W_I32_SHR_U);
        wasm_i32_const(s, 1);
        wasm8(s, W_I32_AND);
        wasm_var(s, W_LOCAL_GET, L64_1);
        wasm8(s, W_I32_WRAP_I64);
        wasm8(s, W_I32_NE);
        wasm8(s, W_I32_OR);
        wasm_if_hint(s, W_BLOCKTYPE_VOID, false);
        wasm_count(s, &xemu_wasm_stats.n_sse_mismatch);
        wasm8(s, W_END);
        wasm8(s, W_END);
        /* restore the flags the guest had, plus what the helper raised */
        wasm_get_r32(s, TCG_AREG0);
        wasm_get_r32(s, TCG_AREG0);
        wasm_mem(s, W_I32_LOAD16_U_, fl_ofs);
        wasm_var(s, W_LOCAL_GET, L64_0);
        wasm8(s, W_I32_WRAP_I64);
        wasm8(s, W_I32_OR);
        wasm_mem(s, W_I32_STORE16_, fl_ofs);
    }
    wasm_call_results(s, typemask & 7);
    return true;
}
#endif
#define W_PREFIX_FC         0xfc
#define W_FC_I32_TRUNC_SAT_F64_S 0x02
#define W_FC_I64_TRUNC_SAT_F64_S 0x06

static void wasm_get_f64(TCGContext *s, TCGReg r)
{
    wasm_get_r(s, r);
    wasm8(s, W_F64_REINTERPRET_I64);
}

static void wasm_get_f32(TCGContext *s, TCGReg r)
{
    wasm_get_r32(s, r);
    wasm8(s, W_F32_REINTERPRET_I32);
}

static void wasm_get_fp(TCGContext *s, bool dp, TCGReg r)
{
    if (dp) {
        wasm_get_f64(s, r);
    } else {
        wasm_get_f32(s, r);
    }
}

static void wasm_set_fp(TCGContext *s, bool dp, TCGReg r)
{
    if (dp) {
        wasm8(s, W_I64_REINTERPRET_F64);
    } else {
        wasm8(s, W_I32_REINTERPRET_F32);
        wasm8(s, W_I64_EXTEND_I32_U);
    }
    wasm_set_r(s, r);
}

static void wasm_f64_const(TCGContext *s, double d)
{
    uint64_t v;

    memcpy(&v, &d, 8);
    wasm8(s, W_F64_CONST);
    for (int i = 0; i < 8; i++) {
        wasm8(s, v >> (i * 8));
    }
}

/*
 * Call a plain C function (not a TCG helper) imported by table index;
 * @type is its wasm function type, e.g. "\x60\x01\x7f\x01\x7e".
 * The arguments are already on the wasm stack. It must not suspend.
 */
static void wasm_call_c(TCGContext *s, const void *func,
                        const char *type, int type_len)
{
    int idx = get_helper_idx(func);

    if (idx < 0) {
        idx = register_helper(func);
        for (int i = 0; i < type_len; i++) {
            types_out8(type[i]);
        }
    }
    wasm_call_idx(s, idx);
}

/*
 * float -> int: round the f64 on the stack per the flcr rounding control,
 * then convert, giving the x86 "integer indefinite" on NaN/overflow.
 */
static void wasm_fp_to_int(TCGContext *s, bool i64, TCGReg d)
{
    wasm_var(s, W_LOCAL_SET, LF64_0);
    /* L32_0 = RC */
    wasm_i32_const(s, (int32_t)(uintptr_t)&tcg_wasm_fpcr);
    wasm_mem(s, W_I32_LOAD, 0);
    wasm_i32_const(s, 13);
    wasm8(s, W_I32_SHR_U);
    wasm_i32_const(s, 3);
    wasm8(s, W_I32_AND);
    wasm_var(s, W_LOCAL_SET, L32_0);
    /* RC == 1 ? floor : nearest */
    wasm_var(s, W_LOCAL_GET, LF64_0);
    wasm8(s, W_F64_FLOOR);
    wasm_var(s, W_LOCAL_GET, LF64_0);
    wasm8(s, W_F64_NEAREST);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_i32_const(s, 1);
    wasm8(s, W_I32_EQ);
    wasm8(s, W_SELECT);
    /* RC == 2 ? ceil : trunc */
    wasm_var(s, W_LOCAL_GET, LF64_0);
    wasm8(s, W_F64_CEIL);
    wasm_var(s, W_LOCAL_GET, LF64_0);
    wasm8(s, W_F64_TRUNC);
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_i32_const(s, 2);
    wasm8(s, W_I32_EQ);
    wasm8(s, W_SELECT);
    /* RC < 2 ? first : second */
    wasm_var(s, W_LOCAL_GET, L32_0);
    wasm_i32_const(s, 2);
    wasm8(s, W_I32_LT_U);
    wasm8(s, W_SELECT);
    wasm_var(s, W_LOCAL_SET, LF64_0);

    /* in range ? trunc_sat(r) : INT_MIN */
    wasm_var(s, W_LOCAL_GET, LF64_0);
    wasm8(s, W_PREFIX_FC);
    wasm_uleb(s, i64 ? W_FC_I64_TRUNC_SAT_F64_S : W_FC_I32_TRUNC_SAT_F64_S);
    if (i64) {
        wasm_i64_const(s, INT64_MIN);
    } else {
        wasm_i32_const(s, INT32_MIN);
    }
    wasm_var(s, W_LOCAL_GET, LF64_0);
    wasm_f64_const(s, i64 ? -9223372036854775808.0 : -2147483648.0);
    wasm8(s, W_F64_GE);
    wasm_var(s, W_LOCAL_GET, LF64_0);
    if (i64) {
        wasm_f64_const(s, 9223372036854775808.0);
        wasm8(s, W_F64_LT);
    } else {
        wasm_f64_const(s, 2147483647.0);
        wasm8(s, W_F64_LE);
    }
    wasm8(s, W_I32_AND);
    wasm8(s, W_SELECT);
    if (!i64) {
        wasm8(s, W_I64_EXTEND_I32_U);
    }
    wasm_set_r(s, d);
}

/* d = comisd-style flags of (a1, a2): ZF 0x40, PF 0x04, CF 0x01 */
static void wasm_fp_com(TCGContext *s, bool dp, TCGReg d, TCGReg a1, TCGReg a2)
{
    uint8_t op_lt = dp ? W_F64_LT : 0x5d /* f32.lt */;
    uint8_t op_eq = dp ? W_F64_EQ : W_F32_EQ;
    uint8_t op_ne = dp ? W_F64_NE : W_F32_NE;

    wasm_i64_const(s, 0x45);                /* unordered */
    wasm_get_fp(s, dp, a1);
    wasm_get_fp(s, dp, a2);
    wasm8(s, op_lt);
    wasm8(s, W_I64_EXTEND_I32_U);           /* CF */
    wasm_get_fp(s, dp, a1);
    wasm_get_fp(s, dp, a2);
    wasm8(s, op_eq);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm_i64_const(s, 6);
    wasm8(s, W_I64_SHL);                    /* ZF */
    wasm8(s, W_I64_OR);
    wasm_get_fp(s, dp, a1);
    wasm_get_fp(s, dp, a1);
    wasm8(s, op_ne);
    wasm_get_fp(s, dp, a2);
    wasm_get_fp(s, dp, a2);
    wasm8(s, op_ne);
    wasm8(s, W_I32_OR);                     /* either is NaN */
    wasm8(s, W_SELECT);
    wasm_set_r(s, d);
}

static void tcg_out_op(TCGContext *s, TCGOpcode opc, TCGType type,
                       const TCGArg args[TCG_MAX_OP_ARGS],
                       const int const_args[TCG_MAX_OP_ARGS])
{
    TCGReg a0 = args[0], a1 = args[1], a2 = args[2];
    bool dp = false;
    uint8_t wop;

    WASM_CLASS(16);

#define FP_CASE(x) \
    case glue(glue(INDEX_op_, x), _f64): \
        dp = true; /* fall through */ \
    case glue(glue(INDEX_op_, x), _f32)

    switch (opc) {
    case INDEX_op_flcr:
        tcg_out_op_r(s, opc, a0);
        wasm_i32_const(s, (int32_t)(uintptr_t)&tcg_wasm_fpcr);
        wasm_get_r32(s, a0);
        wasm_mem(s, W_I32_STORE, 0);
        break;

    case INDEX_op_ld80f_f64:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r32(s, a1);
        wasm_call_c(s, tcg_wasm_ld80f_f64, "\x60\x01\x7f\x01\x7e", 5);
        wasm_set_r(s, a0);
        break;
    case INDEX_op_ld80f_f32:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r32(s, a1);
        wasm_call_c(s, tcg_wasm_ld80f_f32, "\x60\x01\x7f\x01\x7f", 5);
        wasm8(s, W_I64_EXTEND_I32_U);
        wasm_set_r(s, a0);
        break;
    case INDEX_op_st80f_f64:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r32(s, a1);
        wasm_get_r(s, a0);
        wasm_call_c(s, tcg_wasm_st80f_f64, "\x60\x02\x7f\x7e\x00", 5);
        break;
    case INDEX_op_st80f_f32:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r32(s, a1);
        wasm_get_r32(s, a0);
        wasm_call_c(s, tcg_wasm_st80f_f32, "\x60\x02\x7f\x7f\x00", 5);
        break;

    case INDEX_op_sin_f64:
    case INDEX_op_cos_f64:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r(s, a1);
        wasm_call_c(s, opc == INDEX_op_sin_f64 ? (void *)tcg_wasm_sin_f64
                                               : (void *)tcg_wasm_cos_f64,
                    "\x60\x01\x7e\x01\x7e", 5);
        wasm_set_r(s, a0);
        break;
    case INDEX_op_sin_f32:
    case INDEX_op_cos_f32:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r32(s, a1);
        wasm_call_c(s, opc == INDEX_op_sin_f32 ? (void *)tcg_wasm_sin_f32
                                               : (void *)tcg_wasm_cos_f32,
                    "\x60\x01\x7f\x01\x7f", 5);
        wasm8(s, W_I64_EXTEND_I32_U);
        wasm_set_r(s, a0);
        break;

    FP_CASE(add):
        wop = dp ? W_F64_ADD : W_F32_ADD;
        goto do_binary;
    FP_CASE(sub):
        wop = dp ? W_F64_SUB : W_F32_SUB;
        goto do_binary;
    FP_CASE(mul):
        wop = dp ? W_F64_MUL : W_F32_MUL;
        goto do_binary;
    FP_CASE(div):
        wop = dp ? W_F64_DIV : W_F32_DIV;
    do_binary:
        tcg_out_op_rrr(s, opc, a0, a1, a2);
        wasm_get_fp(s, dp, a1);
        wasm_get_fp(s, dp, a2);
        wasm8(s, wop);
        wasm_set_fp(s, dp, a0);
        break;

    FP_CASE(abs):
        wop = dp ? W_F64_ABS : W_F32_ABS;
        goto do_unary;
    FP_CASE(chs):
        wop = dp ? W_F64_NEG : W_F32_NEG;
        goto do_unary;
    FP_CASE(sqrt):
        wop = dp ? W_F64_SQRT : W_F32_SQRT;
    do_unary:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_fp(s, dp, a1);
        wasm8(s, wop);
        wasm_set_fp(s, dp, a0);
        break;

    FP_CASE(com):
        tcg_out_op_rrr(s, opc, a0, a1, a2);
        wasm_fp_com(s, dp, a0, a1, a2);
        break;

    case INDEX_op_cvt32f_f64:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_f32(s, a1);
        wasm8(s, W_F64_PROMOTE_F32);
        wasm_set_fp(s, true, a0);
        break;
    case INDEX_op_cvt64f_f32:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_f64(s, a1);
        wasm8(s, W_F32_DEMOTE_F64);
        wasm_set_fp(s, false, a0);
        break;
    FP_CASE(cvt32i):
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r32(s, a1);
        wasm8(s, dp ? W_F64_CONVERT_I32_S : W_F32_CONVERT_I32_S);
        wasm_set_fp(s, dp, a0);
        break;
    FP_CASE(cvt64i):
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_r(s, a1);
        wasm8(s, dp ? W_F64_CONVERT_I64_S : W_F32_CONVERT_I64_S);
        wasm_set_fp(s, dp, a0);
        break;
    case INDEX_op_cvt64f_i32:
    case INDEX_op_cvt64f_i64:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_f64(s, a1);
        wasm_fp_to_int(s, opc == INDEX_op_cvt64f_i64, a0);
        break;
    case INDEX_op_cvt32f_i32:
    case INDEX_op_cvt32f_i64:
        tcg_out_op_rr(s, opc, a0, a1);
        wasm_get_f32(s, a1);
        wasm8(s, W_F64_PROMOTE_F32);
        wasm_fp_to_int(s, opc == INDEX_op_cvt32f_i64, a0);
        break;

    case INDEX_op_mov32f_i32:
    case INDEX_op_mov32i_f32:
    case INDEX_op_mov_f32:
        tcg_out_op_rr(s, INDEX_op_mov_f32, a0, a1);
        wasm_get_r32(s, a1);
        wasm8(s, W_I64_EXTEND_I32_U);
        wasm_set_r(s, a0);
        break;
    case INDEX_op_mov64f_i64:
    case INDEX_op_mov64i_f64:
    case INDEX_op_mov_f64:
        tcg_out_op_rr(s, INDEX_op_mov, a0, a1);
        wasm_mov(s, a0, a1);
        break;

    default:
        g_assert_not_reached();
    }
#undef FP_CASE
}

/*
 * ---------------------------------------------------------------------
 * TB start / end: the header, and the module around the wasm code
 * ---------------------------------------------------------------------
 */

static void tcg_out_tb_start(TCGContext *s)
{
    WasmTBHeader *h = (WasmTBHeader *)s->code_buf;

    wasm_insn_profile_start(s);
    wasm_direct_translation = NULL;
    wasm_direct_code = NULL;
    wasm_direct_blueprint = NULL;
    wasm_direct_memory = NULL;
    code_buf_root = code_buf_cur = new_linked_buf();
    types_buf_root = types_buf_cur = new_linked_buf();
    label_info = NULL;
    block_placeholder = NULL;
    relocs = relocs_last = NULL;
    relocs_num = 0;
    br_patches = NULL;
    has_backward_label = false;
    used_regs = 0;
    snapshot_sites = NULL;
    wasm_lookup_layout = NULL;
    block_idx = 0;
    helpers = helpers_last = NULL;
    helpers_num = 0;
    wasm_used_classes = 0;
    if (unlikely(wasm_deny_classes < 0)) {
        const char *e = getenv("XEMU_WASM_JIT_DENY");
        wasm_deny_classes = e ? strtol(e, NULL, 0) : 0;
    }

    memset(h, 0, sizeof(*h));
    cur_hdr = h;
    s->code_ptr += sizeof(*h) / sizeof(tcg_insn_unit);
    h->tci_ptr = s->code_ptr;

    /*
     * Fresh entry (do_init): set up env and the stack pointer on the first
     * run of this instance, and start at region 0. On an Asyncify rewind
     * do_init is 0 and BLOCK_PTR still selects the interrupted region.
     */
    wasm_ctx_load_i32(s, WASM_CTX_DO_INIT_OFF);
    wasm_if_hint(s, W_BLOCKTYPE_VOID, true);     /* rewind: rare */

    wasm_ctx_load_i32(s, WASM_CTX_ENV_OFF);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm_set_r(s, TCG_AREG0);
    wasm_ctx_load_i32(s, WASM_CTX_STACK_OFF);
    wasm8(s, W_I64_EXTEND_I32_U);
    wasm_set_r(s, TCG_REG_CALL_STACK);

    wasm_ctx_store_i32_const(s, WASM_CTX_DO_INIT_OFF, 0);
    wasm_i64_const(s, 0);
    wasm_var(s, W_LOCAL_SET, LBLOCK);

    /* Asyncify rewind: the registers saved before the unwinding call */
    pro_else_pos = wasm_code_len();
    wasm8(s, W_ELSE);
#ifdef EMSCRIPTEN
    wasm_count(s, &xemu_wasm_stats.n_jit_rewind);
#endif
    if (wasm32_ic_enabled()) {
        /*
         * Rewind the module that actually saved the registers, even when
         * its interrupted TB also occurs in this module (region overlap).
         * No instance management occurs while generated code is suspended.
         */
        wasm_ctx_load_i32(s, WASM_CTX_REWIND_FUNC_OFF);
        wasm_var(s, W_GLOBAL_GET, WASM_IC_SELF_GLOBAL);
        wasm8(s, W_I32_NE);
        wasm_if(s, W_BLOCKTYPE_VOID);
#ifdef EMSCRIPTEN
        wasm_count(s, &xemu_wasm_stats.n_ic_rewind);
#endif
        wasm_var(s, W_LOCAL_GET, CTX_IDX);
        wasm_ctx_load_i32(s, WASM_CTX_REWIND_FUNC_OFF);
        wasm_call_idx(s, CHAIN_CALL_IDX);
        wasm8(s, W_RETURN);
        wasm8(s, W_END);
    } else if (wasm_link_enabled()) {
        /*
         * The dispatcher re-calls the first TB of a chain; if a TB it linked
         * to (ctx.tb_ptr) is the one that unwound, rewind that one instead.
         */
        wasm_ctx_load_i32(s, WASM_CTX_TB_PTR_OFF);
        wasm_i32_const(s, (int32_t)(uintptr_t)h);
        wasm8(s, W_I32_NE);
        wasm_if(s, W_BLOCKTYPE_VOID);
        wasm_var(s, W_LOCAL_GET, CTX_IDX);
        wasm_ctx_load_i32(s, WASM_CTX_TB_PTR_OFF);
        wasm_mem(s, W_I32_LOAD, WASM_TB_INSTANCE_OFF);
        wasm_mem(s, W_I32_LOAD, WASM_INSTANCE_FIDX_OFF);
        wasm_call_idx(s, CHAIN_CALL_IDX);
        wasm8(s, W_RETURN);
        wasm8(s, W_END);
    }
    in_snapshot = true;
    for (int r = 0; r < TCG_TARGET_NB_REGS; r++) {
        wasm_snapshot_site(r);
        wasm_var(s, W_GLOBAL_GET, r);
        wasm_var(s, W_LOCAL_SET, LREG(r));
    }
    in_snapshot = false;
    wasm_var(s, W_GLOBAL_GET, BLOCK_PTR_IDX);
    wasm_var(s, W_LOCAL_SET, LBLOCK);
    pro_end_pos = wasm_code_len();
    wasm8(s, W_END);

    /* Also runs for copied region-member entries and actual B rewinds. */
    wasm_tlb_hint_drop(s);

    /* Entering a TB/member or rewind: its own class, or legacy jit/spin. */
    wasm_phase(s, NULL);

    /* loop $top; the region blocks and dispatch go here at the TB end */
    wasm8(s, W_LOOP);
    wasm8(s, W_BLOCKTYPE_VOID);
    region_ins_pos = wasm_code_len();
}

/* Byte writer for the module, with overflow detection. */
typedef struct ModWriter {
    uint8_t *p;
    uint8_t *end;
} ModWriter;

static inline void mod8(ModWriter *w, uint8_t v)
{
    if (w->p < w->end) {
        *w->p = v;
    }
    w->p++;
}

static void mod_uleb(ModWriter *w, uint32_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        mod8(w, b | (v ? 0x80 : 0));
    } while (v);
}

/* Fixed-size (5 byte) LEB, patched in when the length is known. */
static uint8_t *mod_uleb5_placeholder(ModWriter *w)
{
    uint8_t *at = w->p;
    for (int i = 0; i < 5; i++) {
        mod8(w, 0);
    }
    return at;
}

static void mod_uleb5_patch(ModWriter *w, uint8_t *at, uint32_t v)
{
    if (at + 5 > w->end) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        at[i] = (v & 0x7f) | (i < 4 ? 0x80 : 0);
        v >>= 7;
    }
}

static void mod_bytes(ModWriter *w, const void *src, size_t n)
{
    if (w->p + n <= w->end) {
        memcpy(w->p, src, n);
    }
    w->p += n;
}

static void mod_name(ModWriter *w, const char *name)
{
    size_t n = strlen(name);
    mod_uleb(w, n);
    mod_bytes(w, name, n);
}

static uint8_t *mod_section_begin(ModWriter *w, uint8_t id)
{
    mod8(w, id);
    return mod_uleb5_placeholder(w);
}

static void mod_section_end(ModWriter *w, uint8_t *size_at)
{
    mod_uleb5_patch(w, size_at, w->p - (size_at + 5));
}

static int tcg_out_tb_end(TCGContext *s)
{
    WasmTBHeader *h = (WasmTBHeader *)s->code_buf;
    ModWriter w = {
        .p = (uint8_t *)s->code_ptr,
        .end = (uint8_t *)s->code_gen_highwater + TCG_HIGHWATER,
    };
    uint8_t *sec, *body;
    int code_len;
    bool tlb_hint = wasm32_tlb_hint_enabled();
    const char *locals = tlb_hint ? WASM_TB_HINT_LOCALS : WASM_TB_LOCALS;
    unsigned locals_len = WASM_TB_LOCALS_LEN + (tlb_hint ? 2 : 0);

    wasm8(s, W_END);            /* end $top */
    wasm8(s, W_UNREACHABLE);
    wasm8(s, W_END);            /* end func */
    code_len = wasm_code_len();

    /*
     * Region blocks and dispatch, inserted after "loop $top" (see the
     * layout comment at wasm_new_block); every recorded position after
     * the insertion point moves by its length.
     */
    int nregions = block_idx + 1;
    uint8_t op[64 + 8 * 1024];
    int L = 0, brif_at = -1;
    if (nregions > 1) {
        if (nregions > 1000) {
            return -2;      /* too many regions: retranslate with fewer insns */
        }
        for (int k = nregions - 1; k >= 1; k--) {
            op[L++] = 0x02;
            op[L++] = 0x40;
        }
        op[L++] = 0x02;                 /* block D */
        op[L++] = 0x40;
        op[L++] = 0x20;                 /* local.get LBLOCK */
        op[L++] = LBLOCK;
        op[L++] = 0x50;                 /* i64.eqz */
        brif_at = L;
        op[L++] = 0x0d;                 /* br_if D */
        op[L++] = 0x00;
        op[L++] = 0x20;
        op[L++] = LBLOCK;
        op[L++] = 0xa7;                 /* i32.wrap_i64 */
        op[L++] = 0x0e;                 /* br_table D B[1] .. B[n-1] */
        for (uint32_t v = nregions, more = 1; more;) {
            op[L++] = (v & 0x7f) | (v > 0x7f ? 0x80 : 0);
            more = v > 0x7f;
            v >>= 7;
        }
        for (int k = 0; k <= nregions; k++) {
            uint32_t v = k < nregions ? k : 0;      /* default: D */
            do {
                op[L++] = (v & 0x7f) | (v > 0x7f ? 0x80 : 0);
                v >>= 7;
            } while (v);
        }
        op[L++] = 0x0b;                 /* end D */
    }
    QEMU_BUILD_BUG_ON(LBLOCK > 0x7f);
    for (RelocNode *p = relocs; p; p = p->next) {
        if (p->r.off >= region_ins_pos) {
            p->r.off += L;
        }
        if (p->r.kind == WASM_RELOC_GOTO) {
            p->r.depth += nregions - 1 - p->region;
        }
    }
    if (brif_at >= 0) {
        /* hint the dispatch: falling into region 0 is the normal case */
        RelocNode *e = tcg_malloc(sizeof(*e)), **pp = &relocs;
        e->r.off = region_ins_pos + brif_at;
        e->r.kind = WASM_RELOC_HINT;
        e->r.arg = 1;
        e->r.depth = 0;
        e->region = 0;
        while (*pp && (*pp)->r.off < e->r.off) {
            pp = &(*pp)->next;
        }
        e->next = *pp;
        *pp = e;
        relocs_num++;
    }
    for (SnapshotSite *p = snapshot_sites; p; p = p->next) {
        if (p->pos >= region_ins_pos) {
            p->pos += L;
        }
    }

    h->wasm_ptr = w.p;

    /* magic, version */
    mod_bytes(&w, "\0asm\x01\0\0\0", 8);

    /* types: 0 = TB function, 1 = helper.u, 2 = chain.call, 3.. = helpers */
    sec = mod_section_begin(&w, 1);
    mod_uleb(&w, 3 + helpers_num);
    mod_bytes(&w, "\x60\x01\x7f\x01\x7f", 5);   /* (i32) -> i32 */
    mod_bytes(&w, "\x60\x00\x01\x7f", 4);       /* () -> i32 */
    mod_bytes(&w, "\x60\x02\x7f\x7f\x01\x7f", 6);   /* (i32, i32) -> i32 */
    for (LinkedBuf *p = types_buf_root; p; p = p->next) {
        mod_bytes(&w, p->data, p->size);
    }
    mod_section_end(&w, sec);

    /* import section: memory, helper.u, chain.s0/s1/call, helpers */
    sec = mod_section_begin(&w, 2);
    mod_uleb(&w, 5 + helpers_num + (h->ic_count != 0));
    if (h->ic_count) {
        mod_name(&w, "env");
        mod_name(&w, "ic_table");
        mod8(&w, 0x01); /* table */
        mod8(&w, 0x70); /* funcref */
        mod8(&w, 0x01); /* minimum and maximum */
        mod_uleb(&w, h->ic_count);
        mod_uleb(&w, h->ic_count);
    }
    mod_name(&w, "env");
    mod_name(&w, "buffer");
    mod8(&w, 0x02);             /* memory */
    mod8(&w, 0x03);             /* shared, with maximum */
    mod_uleb(&w, 0);
    mod_uleb(&w, 65536);
    mod_name(&w, "helper");
    mod_name(&w, "u");
    mod8(&w, 0x00);             /* func */
    mod_uleb(&w, 1);
    mod_name(&w, "chain");
    mod_name(&w, "s0");
    mod8(&w, 0x00);
    mod_uleb(&w, 0);
    mod_name(&w, "chain");
    mod_name(&w, "s1");
    mod8(&w, 0x00);
    mod_uleb(&w, 0);
    mod_name(&w, "chain");
    mod_name(&w, "call");
    mod8(&w, 0x00);
    mod_uleb(&w, 2);
    for (int i = 0; i < helpers_num; i++) {
        char name[16];
        snprintf(name, sizeof(name), "%d", i);
        mod_name(&w, "helper");
        mod_name(&w, name);
        mod8(&w, 0x00);
        mod_uleb(&w, 3 + i);
    }
    mod_section_end(&w, sec);

    /*
     * Function B (rewind-capable): the code as emitted, with the region
     * blocks/dispatch inserted and all patches applied.
     */
    uint32_t lenB = code_len + L;
    uint8_t *cb = g_malloc(lenB);
    linked_buf_write(code_buf_root, cb);
    memmove(cb + region_ins_pos + L, cb + region_ins_pos,
            code_len - region_ins_pos);
    memcpy(cb + region_ins_pos, op, L);
    for (BrPatch *p = br_patches; p; p = p->next) {
        uint32_t v;
        if (p->label < 0) {
            v = p->d + (nregions - 1 - p->region);
        } else {
            int j = get_block_of_label(p->label);
            tcg_debug_assert(j > p->region);
            v = p->d + (j - p->region - 1);
        }
        uint8_t *ph = cb + p->pos + L;
        for (int i = 0; i < 5; i++) {
            ph[i] = (v & 0x7f) | (i < 4 ? 0x80 : 0);
            v >>= 7;
        }
    }
    {
        /* snapshot pairs of registers this TB never uses: nops */
        static int snapnop = -1;
        if (snapnop < 0) {
            const char *e = getenv("XEMU_WASM_SNAPNOP");
            snapnop = !(e && *e == '0');
        }
        if (snapnop) {
            uint32_t keep = used_regs | (1u << TCG_AREG0) |
                            (1u << TCG_REG_CALL_STACK);
            for (SnapshotSite *p = snapshot_sites; p; p = p->next) {
                if (!(keep & (1u << p->reg))) {
                    memset(cb + p->pos, 0x01, 4);
                }
            }
        }
    }

    /*
     * Function A (entry): B without the Asyncify rewind arm (it tail-calls
     * B instead) and, unless a branch goes back to a later region, without
     * the region dispatch. Then nothing reaches the loop head except fresh
     * entry and self loops to region 0, and V8 needs no phis for the TCG
     * register locals (B's merges cost ~40 spill/shuffle instructions per
     * TB entry).
     */
    const uint32_t func_a = HELPER_IDX_START + helpers_num;
    const uint32_t func_b = func_a + 1;
    if (wasm_direct_code) {
        uint32_t v = func_b + 1;
        uint8_t *at = cb + wasm_direct_call_pos + L;
        tcg_debug_assert(wasm_direct_call_pos >= region_ins_pos);
        for (int i = 0; i < 5; i++) {
            at[i] = (v & 127) | (i < 4 ? 128 : 0);
            v >>= 7;
        }
    }
    uint8_t new_else[10];
    int ne = 0;
    new_else[ne++] = W_ELSE;
    new_else[ne++] = 0x20;              /* local.get ctx */
    new_else[ne++] = CTX_IDX;
    new_else[ne++] = W_RETURN_CALL;
    uint32_t retcall_at = pro_else_pos + ne;
    for (uint32_t v = func_b, i = 0; i < 5; i++) {
        new_else[ne++] = (v & 0x7f) | (i < 4 ? 0x80 : 0);
        v >>= 7;
    }
    new_else[ne++] = W_END;
    const uint32_t LA = nregions > 1 ?
                        (has_backward_label ? L : 2 * (nregions - 1)) : 0;
    const uint32_t dP = (pro_end_pos + 1 - pro_else_pos) - ne;
    const uint32_t lenA = lenB - dP - (L - LA);
    uint8_t *ca = g_malloc(lenA);
    {
        uint8_t *q = ca;
        memcpy(q, cb, pro_else_pos);
        q += pro_else_pos;
        memcpy(q, new_else, ne);
        q += ne;
        memcpy(q, cb + pro_end_pos + 1, region_ins_pos - (pro_end_pos + 1));
        q += region_ins_pos - (pro_end_pos + 1);
        memcpy(q, cb + region_ins_pos, LA);
        q += LA;
        memcpy(q, cb + region_ins_pos + L, lenB - (region_ins_pos + L));
        q += lenB - (region_ins_pos + L);
        tcg_debug_assert(q == ca + lenA);
    }
    /* relocations of A: B's, moved; plus the tail call */
    int nrel_a = 0;
    WasmReloc *rel_a = g_new(WasmReloc, relocs_num + 1);
    bool retcall_done = false;
    for (RelocNode *p = relocs; p; p = p->next) {
        uint32_t o = p->r.off;
        if (!retcall_done && o > retcall_at) {
            rel_a[nrel_a++] = (WasmReloc){ .off = retcall_at,
                                           .kind = WASM_RELOC_RETCALL };
            retcall_done = true;
        }
        if (o < pro_else_pos) {
            /* before the rewind arm: unchanged */
        } else if (o <= pro_end_pos) {
            continue;                   /* inside the rewind arm */
        } else if (o < region_ins_pos) {
            o -= dP;
        } else if (o < region_ins_pos + L) {
            if (o - region_ins_pos >= LA) {
                continue;               /* in the dropped dispatch */
            }
            o -= dP;
        } else {
            o -= dP + (L - LA);
        }
        rel_a[nrel_a] = p->r;
        rel_a[nrel_a].off = o;
        nrel_a++;
    }
    if (!retcall_done) {
        rel_a[nrel_a++] = (WasmReloc){ .off = retcall_at,
                                       .kind = WASM_RELOC_RETCALL };
    }

    /* Function section: A, B, optional private direct register function. */
    sec = mod_section_begin(&w, 3);
    mod_uleb(&w, 2 + !!wasm_direct_code);
    mod_uleb(&w, 0);
    mod_uleb(&w, 0);
    if (wasm_direct_code) {
        mod_uleb(&w, 0); /* same (ctx)->i32 ABI, PRIVATE i32 locals */
    }
    mod_section_end(&w, sec);

    /* global section: mutable i64 = 0 */
    sec = mod_section_begin(&w, 6);
    unsigned ic_globals = wasm32_ic_enabled() ? 1 + 2 * h->ic_count : 0;
    QEMU_BUILD_BUG_ON(NUM_GLOBALS != WASM_IC_SELF_GLOBAL);
    mod_uleb(&w, NUM_GLOBALS + ic_globals);
    for (int i = 0; i < NUM_GLOBALS; i++) {
        mod_bytes(&w, "\x7e\x01\x42\x00\x0b", 5);
    }
    if (ic_globals) {
        mod_bytes(&w, "\x7f\x01\x41\x00\x0b", 5); /* self function ID */
        for (unsigned i = 0; i < h->ic_count; i++) {
            /* -1 cannot equal a TB header (nor the NULL lookup result). */
            mod_bytes(&w, "\x7f\x01\x41\x7f\x0b", 5);
            mod_bytes(&w, "\x7f\x01\x41\x00\x0b", 5); /* miss countdown */
        }
    }
    mod_section_end(&w, sec);

    /* export section */
    sec = mod_section_begin(&w, 7);
    mod_uleb(&w, 1 + (ic_globals ? 1 + h->ic_count : 0));
    mod_name(&w, "start");
    mod8(&w, 0x00);
    mod_uleb(&w, func_a);
    if (ic_globals) {
        mod_name(&w, "ic_self");
        mod8(&w, 0x03); /* global */
        mod_uleb(&w, WASM_IC_SELF_GLOBAL);
        for (unsigned i = 0; i < h->ic_count; i++) {
            char name[16];
            snprintf(name, sizeof(name), "ic%u", i);
            mod_name(&w, name);
            mod8(&w, 0x03);
            mod_uleb(&w, WASM_IC_HDR_GLOBAL + 2 * i);
        }
    }
    mod_section_end(&w, sec);

    /* branch hints (custom section, before the code section) */
    if (wasm_hints_enabled()) {
        int nha = 0, nhb = 0;
        for (int i = 0; i < nrel_a; i++) {
            nha += rel_a[i].kind == WASM_RELOC_HINT;
        }
        for (RelocNode *p = relocs; p; p = p->next) {
            nhb += p->r.kind == WASM_RELOC_HINT;
        }
        if (nha || nhb) {
            sec = mod_section_begin(&w, 0);
            mod_name(&w, "metadata.code.branch_hint");
            mod_uleb(&w, (nha > 0) + (nhb > 0));
            if (nha) {
                mod_uleb(&w, func_a);
                mod_uleb(&w, nha);
                for (int i = 0; i < nrel_a; i++) {
                    if (rel_a[i].kind == WASM_RELOC_HINT) {
                        mod_uleb(&w, locals_len + rel_a[i].off);
                        mod_uleb(&w, 1);
                        mod8(&w, rel_a[i].arg);
                    }
                }
            }
            if (nhb) {
                mod_uleb(&w, func_b);
                mod_uleb(&w, nhb);
                for (RelocNode *p = relocs; p; p = p->next) {
                    if (p->r.kind == WASM_RELOC_HINT) {
                        mod_uleb(&w, locals_len + p->r.off);
                        mod_uleb(&w, 1);
                        mod8(&w, p->r.arg);
                    }
                }
            }
            mod_section_end(&w, sec);
        }
    }

    if (wasm_direct_blueprint) {
        sec = mod_section_begin(&w, 0);
        mod_name(&w, XWD_BLUEPRINT_NAME);
        mod_bytes(&w, wasm_direct_blueprint->bytes,
                  wasm_direct_blueprint->size);
        mod_section_end(&w, sec);
        if (wasm_direct_memory && wasm_direct_memory->valid) {
            XwdCode *memory = tcg_malloc(sizeof(*memory));
            if (xwd_memory_write(wasm_direct_memory, memory)) {
                sec = mod_section_begin(&w, 0);
                mod_name(&w, XWD_MEMORY_NAME);
                mod_bytes(&w, memory->bytes, memory->size);
                mod_section_end(&w, sec);
            }
        }
    }
    if (wasm_direct_code) {
        sec = mod_section_begin(&w, 0);
        mod_name(&w, "xemu.direct-x86");
        mod_uleb(&w, XWD_VERSION);
        mod_uleb(&w, xemu_wasm_direct_mode());
        mod_section_end(&w, sec);
    }

    /* Code section: A, B, optional private direct body. */
    sec = mod_section_begin(&w, 10);
    mod_uleb(&w, 2 + !!wasm_direct_code);
    /* locals: 4 x i32, 2 x i64, 1 x f64, 16+1 x i64 (TCG regs, BLOCK_PTR) */
    QEMU_BUILD_BUG_ON(TCG_TARGET_NB_REGS != 16);
    QEMU_BUILD_BUG_ON(sizeof(WASM_TB_LOCALS) - 1 != WASM_TB_LOCALS_LEN);
    QEMU_BUILD_BUG_ON(sizeof(WASM_TB_HINT_LOCALS) - 1 !=
                      WASM_TB_LOCALS_LEN + 2);
    QEMU_BUILD_BUG_ON(WASM_TLB_HINT_PTR_LOCAL != LBLOCK + 1);
    QEMU_BUILD_BUG_ON(WASM_TLB_HINT_MMU_LOCAL != LBLOCK + 2);
    body = mod_uleb5_placeholder(&w);
    mod_bytes(&w, locals, locals_len);
    h->body_off = w.p - h->wasm_ptr;
    h->body_len = lenA - 1;             /* without the function's "end" */
    mod_bytes(&w, ca, lenA);
    mod_uleb5_patch(&w, body, w.p - (body + 5));
    body = mod_uleb5_placeholder(&w);
    mod_bytes(&w, locals, locals_len);
    h->body_b_off = w.p - h->wasm_ptr;
    h->body_b_len = lenB - 1;
    mod_bytes(&w, cb, lenB);
    mod_uleb5_patch(&w, body, w.p - (body + 5));
    if (wasm_direct_code) {
        mod_uleb(&w, wasm_direct_code->size);
        mod_bytes(&w, wasm_direct_code->bytes, wasm_direct_code->size);
        /* Existing non-fusible contract: never feed these to legacy regions. */
        h->body_len = h->body_b_len = 0;
    }
    mod_section_end(&w, sec);
    g_free(ca);
    g_free(cb);

    h->wasm_size = w.p - h->wasm_ptr;

    /* imported helpers: function pointers == table indices */
    w.p = (uint8_t *)ROUND_UP((uintptr_t)w.p, 4);
    h->import_ptr = (const uint32_t *)w.p;
    for (HelperInfo *p = helpers; p; p = p->next) {
        mod_bytes(&w, &p->idx_on_qemu, 4);
    }
    h->import_size = w.p - (uint8_t *)h->import_ptr;

    h->reloc_ptr = (const WasmReloc *)w.p;
    h->reloc_count = nrel_a;
    mod_bytes(&w, rel_a, nrel_a * sizeof(WasmReloc));
    g_free(rel_a);
    h->reloc_b_ptr = (const WasmReloc *)w.p;
    h->reloc_b_count = relocs_num;
    for (RelocNode *p = relocs; p; p = p->next) {
        mod_bytes(&w, &p->r, sizeof(p->r));
    }

    if (w.p > w.end) {
        return -1;
    }
    if (unlikely(wasm_used_classes & wasm_deny_classes)) {
        h->counter = INT32_MIN;     /* never compile */
    }
    s->code_ptr = (tcg_insn_unit *)ROUND_UP((uintptr_t)w.p,
                                            sizeof(tcg_insn_unit));
    if (unlikely((void *)s->code_ptr > s->code_gen_highwater)) {
        return -1;
    }
    return 0;
}
