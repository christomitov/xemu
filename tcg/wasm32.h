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
} WasmContext;

#define WASM_CTX_ENV_OFF        0
#define WASM_CTX_STACK_OFF      4
#define WASM_CTX_TB_PTR_OFF     8
#define WASM_CTX_TCI_TB_PTR_OFF 12
#define WASM_CTX_DO_INIT_OFF    16

typedef struct WasmTBHeader {
    const uint32_t *tci_ptr;    /* TCI bytecode entry */
    const uint8_t *wasm_ptr;    /* wasm module bytes */
    uint32_t wasm_size;
    const uint32_t *import_ptr; /* table indices of imported helpers */
    uint32_t import_size;       /* in bytes */
    int32_t counter;            /* TCI executions so far */
    void *instance;             /* struct WasmInstance *, or NULL */
    uint32_t pad;
} WasmTBHeader;

QEMU_BUILD_BUG_ON(sizeof(WasmTBHeader) % 8 != 0);

/* shared with generated code that chains TBs (tcg-target.c.inc) */
typedef struct WasmInstance {
    void *tb;       /* the TB header this instance belongs to, or NULL */
    int func_idx;   /* table index of its start function */
    int used;       /* entered since the last eviction sweep */
} WasmInstance;

#define WASM_TB_INSTANCE_OFF    24
#define WASM_INSTANCE_TB_OFF    0
#define WASM_INSTANCE_FIDX_OFF  4
#define WASM_INSTANCE_USED_OFF  8
QEMU_BUILD_BUG_ON(offsetof(WasmTBHeader, instance) != WASM_TB_INSTANCE_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmInstance, func_idx) != WASM_INSTANCE_FIDX_OFF);
QEMU_BUILD_BUG_ON(offsetof(WasmInstance, used) != WASM_INSTANCE_USED_OFF);

/* Blocks run by the interpreter this many times get compiled to wasm. */
extern int wasm32_jit_threshold;

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
