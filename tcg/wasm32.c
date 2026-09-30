/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wasm32 JIT backend: TB dispatcher and WebAssembly module management.
 *
 * Based on tcg/wasm32.c of the WebAssembly TCG backend by Kohei Tokunaga
 * (ktock/qemu-wasm, "tcg: Add a TCG backend for WebAssembly").
 *
 * A TB is interpreted (tci.c) for its first wasm32_jit_threshold runs; after
 * that its module is compiled and instantiated synchronously on the vCPU
 * thread, and the exported function is added to the wasm table so that it
 * can be called like a C function pointer. Compiled blocks chain to each
 * other through this dispatcher (or loop in place when a block jumps to
 * itself).
 *
 * Browsers limit the number of live instances, so at most MAX_INSTANCES are
 * kept: past that, the older half is dropped from the table and forgotten,
 * and their TBs fall back to the interpreter until the JS garbage collector
 * has reclaimed the instances (tracked with a FinalizationRegistry).
 *
 * Only a single thread may execute TBs (TCG round-robin mode, the default
 * for xemu); addFunction() only updates the table of the calling thread.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/xemu-wasm-stats.h"
#include <emscripten.h>
#include "tcg/tcg.h"
#include "accel/tcg/getpc.h"
#include "wasm32.h"
#include "exec/translation-block.h"

/*
 * Adaptive JIT threshold: a TB runs in the interpreter until it has run
 * wasm32_jit_threshold times, then gets its own wasm module. Compiling is
 * costly (~tens of us per module), interpreting is ~10x slower than wasm,
 * so compile early (jit_threshold_low) while compiling stays within
 * jit_budget of the vCPU's time, and fall back to jit_threshold_high during
 * bursts of new code (boot) so compiles can't swamp the vCPU.
 */
int wasm32_jit_threshold = 1000;
static int jit_threshold_low = 64, jit_threshold_high = 1000;
static double jit_budget = 0.10;     /* fraction of wall time */
static double jit_debt_ms, jit_debt_last;
#define JIT_DEBT_MAX_MS 20.0

static void jit_budget_update(double now)
{
    jit_debt_ms -= (now - jit_debt_last) * jit_budget;
    if (jit_debt_ms < 0) {
        jit_debt_ms = 0;
    }
    jit_debt_last = now;
    wasm32_jit_threshold = jit_debt_ms < JIT_DEBT_MAX_MS ?
                           jit_threshold_low : jit_threshold_high;
#ifdef EMSCRIPTEN
    xemu_wasm_stats.n_jit_threshold = wasm32_jit_threshold;
#endif
}
static int wasm32_jit_debug;

/* For debuggers/test harnesses: the TB the dispatcher last entered. */
void *volatile wasm32_cur_tb;
EMSCRIPTEN_KEEPALIVE void *volatile *wasm32_cur_tb_ptr(void)
{
    return &wasm32_cur_tb;
}

__thread WasmContext wasm_ctx;

/*
 * Compile and instantiate a TB module; returns the table index of its start
 * function. helper.u is called by the generated code after every helper
 * call: 0 means Asyncify is unwinding the stack.
 */
EM_JS(int, wasm32_instantiate, (const uint8_t *wasm_begin, int wasm_size,
                                const uint32_t *import_vec, int import_count,
                                int s0, int s1),
{
    const J = Module.__wasm32_jit;
    const helper = {};
    helper.u = () => (Asyncify.state != Asyncify.State.Unwinding) ? 1 : 0;
    for (let i = 0; i < import_count; i++) {
        helper[i] = wasmTable.get(HEAPU32[(import_vec >> 2) + i]);
    }
    /* compile from a copy: the code buffer is shared memory */
    const bytes = HEAPU8.slice(wasm_begin, wasm_begin + wasm_size);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, {
        "env": { "buffer": wasmMemory },
        "helper": helper,
        "chain": { "s0": s0 ? wasmTable.get(s0) : J.dummy,
                   "s1": s1 ? wasmTable.get(s1) : J.dummy, "call": J.call },
    });
    J.registry.register(inst, 0);
    const f = addFunction(inst.exports.start, 'ii');
    J.mods.set(f, { mod, helper });   /* for cheap relinks */
    return f;
});

/*
 * Re-instantiate an already compiled module with fresh successor links;
 * returns the new function's table index (the old one is removed), or 0.
 */
EM_JS(int, wasm32_relink, (int old, int s0, int s1),
{
    const J = Module.__wasm32_jit;
    const rec = J.mods.get(old);
    if (!rec) {
        return 0;
    }
    const inst = new WebAssembly.Instance(rec.mod, {
        "env": { "buffer": wasmMemory },
        "helper": rec.helper,
        "chain": { "s0": s0 ? wasmTable.get(s0) : J.dummy,
                   "s1": s1 ? wasmTable.get(s1) : J.dummy, "call": J.call },
    });
    J.registry.register(inst, 0);
    const f = addFunction(inst.exports.start, 'ii');
    J.mods.delete(old);
    removeFunction(old);
    J.mods.set(f, rec);
    return f;
});

EM_JS(void, wasm32_remove_function, (int idx), {
    Module.__wasm32_jit.mods.delete(idx);
    removeFunction(idx);
});

EM_JS(void, wasm32_js_init, (int *collected), {
    Module.__wasm32_jit = {
        /* counts instances reclaimed by the GC */
        registry: new FinalizationRegistry(() => {
            Atomics.add(HEAP32, collected >> 2, 1);
        }),
        /* table index -> { compiled module, helper imports } */
        mods: new Map(),
        /* placeholder for an unlinked successor (never called: the
         * generated guard checks link_hdr/link_fidx first) */
        dummy: (ctx) => 0,
        /* Asyncify rewind of a linked TB (rare): call it by table index */
        call: (ctx, f) => wasmTable.get(f)(ctx),
    };
});

EM_JS_DEPS(wasm32_jit, "$addFunction,$removeFunction,$wasmTable");

/*
 * Max number of instances alive at the same time. Games' hot code needs more
 * than 15000 TBs (Splinter Cell evicted 7500 modules/s at that cap).
 */
#define MAX_INSTANCES_BUF 65536
static int max_instances = 40000;   /* XEMU_WASM_JIT_MAX */
#define MAX_INSTANCES max_instances

/* WasmInstance: see wasm32.h */

static WasmInstance instances[MAX_INSTANCES_BUF];

/* For the page's "Send hot code" dump: the guest block behind a TB header. */
EMSCRIPTEN_KEEPALIVE uint32_t *wasm32_tb_info(void *hdr)
{
    static uint32_t out[4];
    TranslationBlock *tb = hdr ? tcg_tb_lookup((uintptr_t)hdr) : NULL;

    memset(out, 0, sizeof(out));
    if (tb) {
        out[0] = tb->pc;
        out[1] = tb->cs_base;
        out[2] = tb->size;
        out[3] = tb->icount;
    }
    return out;
}
static int free_slots[MAX_INSTANCES_BUF], n_free = -1;
static int clock_hand;
static int instances_alive;     /* slots in use */
static int instances_collected; /* written by the JS FinalizationRegistry */

static void add_instance(WasmTBHeader *h, int func_idx)
{
    WasmInstance *e;

    if (n_free < 0) {
        for (int i = 0; i < MAX_INSTANCES; i++) {
            free_slots[i] = MAX_INSTANCES - 1 - i;
        }
        n_free = MAX_INSTANCES;
    }
    e = &instances[free_slots[--n_free]];
    e->tb = h;
    e->func_idx = func_idx;
    e->used = 1;
    h->instance = e;
    instances_alive++;

    /*
     * Age the used bits once when the cache is 3/4 full, so that the first
     * eviction sees what ran recently, not everything that ever ran (which
     * made it drop hot code at random: a multi-second fps drop in games).
     */
    if (instances_alive == MAX_INSTANCES * 3 / 4) {
        for (int i = 0; i < MAX_INSTANCES; i++) {
            instances[i].used = 0;
        }
        e->used = 1;
    }
}

static int get_instance(WasmTBHeader *h)
{
    WasmInstance *e = h->instance;

    if (e == NULL) {
        return 0;
    }
    if (e->tb != h) {
        /* dropped (or its slot reused): recompile at the next chance */
        h->instance = NULL;
        h->counter = INT32_MAX / 2;
        return 0;
    }
    e->used = 1;
    return e->func_idx;
}

/*
 * Free 1/16 of the slots, second-chance (clock) order: modules entered
 * since the last sweep are kept, cold ones dropped. Dropped modules count as
 * gone right away: their table slots are freed now and V8 reclaims them
 * whenever it GCs (waiting for the FinalizationRegistry kept the vCPU
 * interpreting everything new for minutes: finalizers only run when this
 * worker returns to its event loop).
 */
static void remove_instances(void)
{
    int target = MAX_INSTANCES / 16, removed = 0;

    for (int scanned = 0; removed < target && scanned < 2 * MAX_INSTANCES;
         scanned++) {
        WasmInstance *e = &instances[clock_hand];
        clock_hand = (clock_hand + 1) % MAX_INSTANCES;
        if (!e->tb) {
            continue;
        }
        if (e->used && scanned < MAX_INSTANCES) {
            e->used = 0;
            continue;
        }
        wasm32_remove_function(e->func_idx);
        e->tb = NULL;
        free_slots[n_free++] = e - instances;
        removed++;
    }
    instances_alive -= removed;
    XSTAT_ADD(n_jit_evict, removed);
}

static void check_instances_collected(void)
{
    /* GC progress: informational only (see remove_instances) */
    qatomic_xchg(&instances_collected, 0);
#ifdef EMSCRIPTEN
    xemu_wasm_stats.n_jit_instances = instances_alive;
#endif
}

static bool can_add_instance(void)
{
    return instances_alive < MAX_INSTANCES;
}

/*
 * While at the instance limit, return to the browser event loop now and
 * then so that the garbage collector can run the finalizers.
 */
#define MAX_EXEC_NUM 50000
static int exec_cnt = MAX_EXEC_NUM;

static inline void trysleep(void)
{
    static unsigned budget_cnt;

    /* let the compile budget recover (lowers the threshold again) */
    if (unlikely((++budget_cnt & 4095) == 0) &&
        wasm32_jit_threshold != jit_threshold_low) {
        jit_budget_update(emscripten_get_now());
    }
    if (unlikely(--exec_cnt == 0)) {
        exec_cnt = MAX_EXEC_NUM;
        check_instances_collected();
        if (!can_add_instance()) {
            emscripten_sleep(0);
            check_instances_collected();
        }
    }
}

static pthread_t exec_thread;

static void wasm32_init(void)
{
    const char *env = getenv("XEMU_WASM_JIT_THRESHOLD");

    if (env) {
        jit_threshold_low = atoi(env);
    }
    env = getenv("XEMU_WASM_JIT_THRESHOLD_HIGH");
    if (env) {
        jit_threshold_high = atoi(env);
    }
    env = getenv("XEMU_WASM_JIT_MAX");
    if (env) {
        max_instances = MAX(64, MIN(atoi(env), MAX_INSTANCES_BUF));
    }
    env = getenv("XEMU_WASM_JIT_BUDGET");   /* percent */
    if (env) {
        jit_budget = atoi(env) / 100.0;
    }
    jit_debt_last = emscripten_get_now();
    jit_budget_update(jit_debt_last);
    env = getenv("XEMU_WASM_JIT_DEBUG");
    if (env) {
        wasm32_jit_debug = atoi(env);
    }
    exec_thread = pthread_self();
    wasm_ctx.stack = g_malloc0(TCG_STATIC_CALL_ARGS_SIZE +
                               TCG_STATIC_FRAME_SIZE);
    wasm_ctx.tci_tb_ptr = &tci_tb_ptr;
    wasm32_js_init(&instances_collected);
    info_report("tcg: wasm32 JIT enabled (threshold %d..%d, budget %.0f%%)",
                jit_threshold_low, jit_threshold_high, jit_budget * 100);
}

typedef uint32_t (*wasm_func_ptr)(WasmContext *);

static int compile_tb(WasmTBHeader *h, int depth);

/*
 * Successor links of @h from its chained goto_tb slots: header and table
 * index of each successor that has a live module (compiling a hot one first
 * when @depth allows). Returns true if they differ from what @h had.
 */
static bool compute_links(WasmTBHeader *h, int depth, int fidx[2])
{
    TranslationBlock *tb = tcg_tb_lookup((uintptr_t)h);
    bool changed = false;

    for (int n = 0; n < 2; n++) {
        WasmTBHeader *b = NULL;
        fidx[n] = 0;
        if (tb && tb->jmp_reset_offset[n] != TB_JMP_OFFSET_INVALID) {
            uintptr_t tgt = qatomic_read(&tb->jmp_target_addr[n]);
            uintptr_t reset = (uintptr_t)tb->tc.ptr + tb->jmp_reset_offset[n];
            if (tgt && tgt != reset) {
                WasmTBHeader *c = (WasmTBHeader *)tgt;
                int f = get_instance(c);
                if (!f && depth == 0 && c != h &&
                    c->counter >= wasm32_jit_threshold / 2 &&
                    can_add_instance()) {
                    f = compile_tb(c, depth + 1);
                }
                if (f > 0) {
                    b = c;
                    fidx[n] = f;
                }
            }
        }
        if (h->link_hdr[n] != b || h->link_fidx[n] != (uint32_t)fidx[n]) {
            changed = true;
        }
        h->link_hdr[n] = b;
        h->link_fidx[n] = fidx[n];
    }
    return changed;
}

static int compile_tb(WasmTBHeader *h, int depth)
{
    int links[2];
    int fidx;

    compute_links(h, depth, links);
    XPHASE_SET(XPHASE_VCPU, "jit_compile");
    double t0 = emscripten_get_now();
    fidx = wasm32_instantiate(h->wasm_ptr, h->wasm_size,
                              h->import_ptr, h->import_size / 4,
                              links[0], links[1]);
    double t1 = emscripten_get_now();
    XPHASE_SET(XPHASE_VCPU, "dispatch");
    jit_debt_ms += t1 - t0;
    jit_budget_update(t1);
    XSTAT_INC(n_jit_compile);
    XSTAT_ADD(ns_jit_compile, (t1 - t0) * 1e6);
    add_instance(h, fidx);
    return fidx;
}

/* A TB whose successor link was missing/stale asked to be relinked. */
static void relink_tb(WasmTBHeader *h)
{
    WasmInstance *e = h->instance;
    int links[2];

    if (!e || e->tb != h || h->relinks >= 16) {
        return;
    }
    h->relinks++;
    if (compute_links(h, 1, links)) {
        int f = wasm32_relink(e->func_idx, links[0], links[1]);
        if (f > 0) {
            e->func_idx = f;
            XSTAT_INC(n_jit_relink);
        }
    }
}

uintptr_t QEMU_DISABLE_CFI tcg_qemu_tb_exec(CPUArchState *env,
                                            const void *v_tb_ptr)
{
    static bool initdone;
    bool first = true;

    if (unlikely(!initdone)) {
        wasm32_init();
        initdone = true;
    }
    if (unlikely(!pthread_equal(exec_thread, pthread_self()))) {
        error_report("tcg: the wasm32 JIT supports a single vCPU thread "
                     "(use -accel tcg,thread=single)");
        abort();
    }

    wasm_ctx.env = env;
    wasm_ctx.tb_ptr = (void *)v_tb_ptr;
    for (;;) {
        WasmTBHeader *h = wasm_ctx.tb_ptr;
        uintptr_t res;
        int fidx;

        trysleep();
        if (!first) {
            XSTAT_INC(n_tb_exec);
        }
        first = false;

        wasm32_cur_tb = h;
        fidx = get_instance(h);
        if (fidx > 0) {
            wasm_ctx.do_init = 1;
            XPHASE_SET(XPHASE_VCPU, NULL);
            res = ((wasm_func_ptr)(uintptr_t)fidx)(&wasm_ctx);
            XPHASE_SET(XPHASE_VCPU, "dispatch");
        } else if (h->counter < wasm32_jit_threshold) {
            h->counter++;
            XPHASE_SET(XPHASE_VCPU, "tci");
            res = tci_exec_tb(env, h->tci_ptr);
            XPHASE_SET(XPHASE_VCPU, "dispatch");
        } else if (!can_add_instance()) {
            XPHASE_SET(XPHASE_VCPU, "jit_evict");
            remove_instances();
            check_instances_collected();
            XPHASE_SET(XPHASE_VCPU, "tci");
            res = tci_exec_tb(env, h->tci_ptr);
            XPHASE_SET(XPHASE_VCPU, "dispatch");
        } else {
            fidx = compile_tb(h, 0);
            if (unlikely(wasm32_jit_debug > 0)) {
                wasm32_jit_debug--;
                fprintf(stderr, "[jit] instantiate tb=%p size=%u imports=%u "
                        "fidx=%d\n", h, h->wasm_size, h->import_size / 4, fidx);
            }
            wasm_ctx.do_init = 1;
            XPHASE_SET(XPHASE_VCPU, NULL);
            res = ((wasm_func_ptr)(uintptr_t)fidx)(&wasm_ctx);
            XPHASE_SET(XPHASE_VCPU, "dispatch");
            if (unlikely(wasm32_jit_debug > 0)) {
                fprintf(stderr, "[jit]  -> res=%lx next=%p\n",
                        (unsigned long)res, wasm_ctx.tb_ptr);
            }
        }
        if (unlikely(wasm_ctx.relink)) {
            WasmTBHeader *rh = wasm_ctx.relink;
            wasm_ctx.relink = NULL;
            relink_tb(rh);
        }
        if (wasm_ctx.tb_ptr == NULL) {
            return res;
        }
    }
}
