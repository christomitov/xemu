/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wasm32 JIT backend: data shared between the code generator
 * (tcg/wasm32/tcg-target.c.inc), the TB dispatcher (tcg/wasm32.c) and the
 * bytecode interpreter used for cold blocks (tcg/tci.c).
 *
 * Based on the WebAssembly TCG backend by Kohei Tokunaga (ktock/qemu-wasm).
 *
 * Every TB starts with a WasmTBHeader. It is followed by TCI bytecode (which
 * tci.c interprets while the block is cold), the TCI constant pool, and a
 * complete WebAssembly module whose exported "start" function executes the
 * same block. Both encodings are produced from one register allocation, so
 * TCG registers, the call/spill stack layout and the helper return address
 * (tci_tb_ptr, an offset in the TCI stream) are identical: execution may
 * switch between them at every TB boundary.
 */
#ifndef TCG_WASM32_H
#define TCG_WASM32_H

/*
 * State passed to generated code (by address, as the only parameter of the
 * module's start function). Offsets are hardcoded in the code generator.
 */
typedef struct WasmContext {
    CPUArchState *env;      /* 0: initial value of TCG_AREG0 */
    uint64_t *stack;        /* 4: initial value of TCG_REG_CALL_STACK */
    void *tb_ptr;           /* 8: current TB header; NULL once we exit */
    uintptr_t *tci_tb_ptr;  /* 12: &tci_tb_ptr, the helper return address */
    uint32_t do_init;       /* 16: 1 = start the block from its entry */
    void *relink;           /* 20: TB header whose successor links went stale */
    uint32_t rewind_func;   /* 24: module that saved the Asyncify registers */
    uint32_t ic_source;     /* 28: pending miss: source module's function */
    uint32_t ic_slot;       /* 32: pending miss: table slot in that module */
} WasmContext;

#define WASM_CTX_ENV_OFF        0
#define WASM_CTX_STACK_OFF      4
#define WASM_CTX_TB_PTR_OFF     8
#define WASM_CTX_TCI_TB_PTR_OFF 12
#define WASM_CTX_DO_INIT_OFF    16
#define WASM_CTX_RELINK_OFF     20
#define WASM_CTX_REWIND_FUNC_OFF 24
#define WASM_CTX_IC_SOURCE_OFF 28
#define WASM_CTX_IC_SLOT_OFF   32

typedef struct WasmTBHeader {
    const uint32_t *tci_ptr;    /* TCI bytecode entry */
    const uint8_t *wasm_ptr;    /* wasm module bytes */
    uint32_t wasm_size;
    const uint32_t *import_ptr; /* table indices of imported helpers */
    uint32_t import_size;       /* in bytes */
    int32_t counter;            /* TCI executions so far */
    void *instance;             /* struct WasmInstance *, or NULL */
    uint32_t relinks;           /* times re-instantiated with fresh links */
    /*
     * Successor links (goto_tb slots 0/1), filled when the module is
     * instantiated: the successor's header and the table index of its
     * function, which the module imports as chain.s0/s1 and tail-calls
     * while both still match (see wasm_goto_tb_in_l32_0).
     */
    void *link_hdr[2];
    uint32_t link_fidx[2];
    uint32_t icount;            /* guest insns (stats), set when compiled */
    /*
     * For merging into region modules (tcg/wasm32.c): the function body
     * (after the locals, without the final "end") inside the module bytes,
     * and relocations into it (WasmReloc, ascending offsets).
     */
    uint32_t body_off;
    uint32_t body_len;
    const struct WasmReloc *reloc_ptr;
    uint32_t reloc_count;
    /*
     * The module has two functions: the entry function above (exported;
     * no Asyncify rewind arm: on a rewind it tail-calls the second one)
     * and the full rewind-capable function below, which V8 compiles only
     * if a rewind ever happens.
     */
    uint32_t body_b_off;
    uint32_t body_b_len;
    const struct WasmReloc *reloc_b_ptr;
    uint32_t reloc_b_count;
    uint32_t ic_count;          /* direct/indirect exit sites in this TB */
    uint32_t instance_member;   /* index in the owning shared region */
    uint16_t cfg_score;         /* cached entry-body control-flow size */
    uint16_t cfg_depth;         /* cached entry-body nesting depth */
} WasmTBHeader;

/*
 * WASM_RELOC_CALL: 5-byte padded ULEB call index of imported helper #arg
 *   (0-based, in the TB's import_ptr order) at body offset @off.
 * WASM_RELOC_GOTO: exit of goto_tb slot @arg (0/1) or goto_ptr (arg = 0xff)
 *   with the successor's header in L32_0; code may be inserted at @off.
 *   @depth: br depth from @off to the TB's own loop.
 */
/*
 * WASM_RELOC_HINT: an "if" at @off with a branch hint (@arg 1 = likely,
 * 0 = unlikely) for the metadata.code.branch_hint section.
 */
/*
 * WASM_RELOC_RETCALL: 5-byte padded ULEB function index of the entry
 * function's tail call to its rewind function (entry body only).
 */
enum { WASM_RELOC_CALL = 1, WASM_RELOC_GOTO = 2, WASM_RELOC_HINT = 3,
       WASM_RELOC_RETCALL = 4,
       /* 5-byte IC operand: depth=0 slot, 1 header global, 2 countdown. */
       WASM_RELOC_IC = 5 };
#define WASM_TB_LOCALS_LEN 9    /* without the optional TLB hint locals */
#define WASM_TB_LOCALS "\x04\x04\x7f\x02\x7e\x01\x7c\x11\x7e"
#define WASM_TB_HINT_LOCALS "\x05\x04\x7f\x02\x7e\x01\x7c\x11\x7e\x02\x7f"
#define WASM_TLB_HINT_PTR_LOCAL 25
#define WASM_TLB_HINT_MMU_LOCAL 26
#define WASM_IC_SELF_GLOBAL 17 /* after registers and BLOCK_PTR */
#define WASM_IC_HDR_GLOBAL  18 /* pairs of header/miss-countdown globals */
typedef struct WasmReloc {
    uint32_t off;
    uint8_t kind;
    uint8_t arg;
    uint16_t depth;
} WasmReloc;

#define WASM_TB_LINK_HDR_OFF    32
#define WASM_TB_LINK_FIDX_OFF   40

QEMU_BUILD_BUG_ON(sizeof(WasmTBHeader) % 8 != 0);

/* shared with generated code that chains TBs (tcg-target.c.inc) */
typedef struct WasmInstance {
    void *tb;       /* root TB header of this instance, or NULL */
    int func_idx;   /* table index of its start function */
    int used;       /* entered since the last eviction sweep */
    WasmTBHeader **members; /* shared-region headers, or NULL for one TB */
    unsigned n_members;
} WasmInstance;

/* Header pointers can survive eviction; validate their member slot too. */
static inline bool wasm_instance_owns(WasmInstance *e, WasmTBHeader *h)
{
    return e && (e->tb == h ||
                 (h->instance_member < e->n_members && e->members &&
                  e->members[h->instance_member] == h));
}

#define WASM_TB_INSTANCE_OFF    24
#define WASM_INSTANCE_TB_OFF    0
#define WASM_INSTANCE_FIDX_OFF  4
#define WASM_INSTANCE_USED_OFF  8
#define WASM_INSTANCE_MEMBERS_OFF 12
#define WASM_TB_MEMBER_OFF      88
QEMU_BUILD_BUG_ON(offsetof(WasmTBHeader, instance_member) !=
                  WASM_TB_MEMBER_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmInstance, members) != WASM_INSTANCE_MEMBERS_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmTBHeader, instance) != WASM_TB_INSTANCE_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmTBHeader, link_hdr) != WASM_TB_LINK_HDR_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmTBHeader, link_fidx) != WASM_TB_LINK_FIDX_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmContext, relink) != WASM_CTX_RELINK_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmContext, rewind_func) !=
                  WASM_CTX_REWIND_FUNC_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmContext, ic_source) != WASM_CTX_IC_SOURCE_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmContext, ic_slot) != WASM_CTX_IC_SLOT_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmInstance, func_idx) != WASM_INSTANCE_FIDX_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmInstance, used) != WASM_INSTANCE_USED_OFF);

/* Blocks run by the interpreter this many times get compiled to wasm. */
extern int wasm32_jit_threshold;
bool wasm32_ic_enabled(void);
bool wasm32_ic_for_exit(bool indirect);
bool wasm32_tlb_hint_enabled(void);

extern __thread WasmContext wasm_ctx;

/*
 * Called by the interpreter when a TB chains to @next (a TB header).
 * Returns the bytecode to continue with, or NULL if the dispatcher in
 * wasm32.c must take over (the block has, or is due for, a wasm module).
 */
static inline const uint32_t *wasm32_tci_chain(void *next)
{
    WasmTBHeader *h = next;

    wasm_ctx.tb_ptr = next;
    if (h->instance == NULL && h->counter < wasm32_jit_threshold) {
        h->counter++;
        return h->tci_ptr;
    }
    return NULL;
}

/* The TCI entry point, in tci.c. */
uintptr_t tci_exec_tb(CPUArchState *env, const void *tci_ptr);

/*
 * Host floating point ops (TCG_TARGET_HAS_fpu, used by the i386 "hard FPU"
 * x87 path). F32/F64 temps live in the ordinary 64-bit registers as bit
 * patterns (F32 zero-extended); both encodings reinterpret them.
 *
 * tcg_wasm_fpcr is the value of the last flcr op, in MXCSR format: its
 * rounding control (bits 13-14) applies to float->int conversions. Wasm
 * arithmetic always rounds to nearest-even.
 *
 * The floatx80 <-> binary32/64 conversions (ld80f/st80f) and sin/cos are
 * out-of-line C functions, called directly from both TCI and wasm code.
 */
extern uint32_t tcg_wasm_fpcr;
uint64_t tcg_wasm_ld80f_f64(uint32_t ptr);
uint32_t tcg_wasm_ld80f_f32(uint32_t ptr);
void tcg_wasm_st80f_f64(uint32_t ptr, uint64_t v);
void tcg_wasm_st80f_f32(uint32_t ptr, uint32_t v);
uint64_t tcg_wasm_sin_f64(uint64_t v);
uint64_t tcg_wasm_cos_f64(uint64_t v);
uint32_t tcg_wasm_sin_f32(uint32_t v);
uint32_t tcg_wasm_cos_f32(uint32_t v);

#endif
