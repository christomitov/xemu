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
#include "system/memory.h"

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
static bool jit_entry_profile;
static bool region_shared; /* opt-in stable multi-entry ownership */

/*
 * For the page's hot-code sampler / test harnesses: the TB being run. That
 * is the vCPU thread's wasm_ctx.tb_ptr (region modules update it on every
 * in-region transition); wasm_ctx is thread-local, so its address is
 * published once from the vCPU thread. Before that: the dispatcher's view.
 */
void *volatile wasm32_cur_tb;
static void *volatile *wasm32_vcpu_tb_ptr;
EMSCRIPTEN_KEEPALIVE void *volatile *wasm32_cur_tb_ptr(void)
{
    return wasm32_vcpu_tb_ptr ? wasm32_vcpu_tb_ptr : &wasm32_cur_tb;
}

__thread WasmContext wasm_ctx;

/* XEMU_WASM_IC: 0 off, 1 all exits, 2 (default) validated goto_ptr exits only. */
static int wasm32_ic_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char *e = getenv("XEMU_WASM_IC");
        mode = e && *e == '1' ? 1 : e && *e == '0' ? 0 : 2;
    }
    return mode;
}

bool wasm32_ic_enabled(void)
{
    return wasm32_ic_mode() != 0;
}

bool wasm32_tlb_hint_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_TLB_HINT");
        enabled = e && *e == '1';
    }
    return enabled;
}

bool wasm32_ic_for_exit(bool indirect)
{
    int mode = wasm32_ic_mode();

    return mode == 1 || (mode == 2 && indirect);
}

/*
 * Compile and instantiate a TB module; returns the table index of its start
 * function. helper.u is called by the generated code after every helper
 * call: 0 means Asyncify is unwinding the stack.
 */
EM_JS(int, wasm32_instantiate, (const uint8_t *wasm_begin, int wasm_size,
                                const uint32_t *import_vec, int import_count,
                                int s0, int s1, int ic_count),
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
    return J.install(mod, helper, s0, s1, ic_count);
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
    const f = J.install(rec.mod, rec.helper, s0, s1, rec.links.length);
    J.remove(old);
    return f;
});

EM_JS(void, wasm32_remove_function, (int idx), {
    Module.__wasm32_jit.remove(idx);
});

EM_JS(void, wasm32_js_init, (int *collected), {
    const J = Module.__wasm32_jit = {
        /* counts instances reclaimed by the GC */
        registry: new FinalizationRegistry(() => {
            Atomics.add(HEAP32, collected >> 2, 1);
        }),
        mods: new Map(),
        incoming: new Map(),
        dummy: (ctx) => 0,
        call: (ctx, f) => wasmTable.get(f)(ctx),
    };
    J.unlink = (link) => {
        if (link.target) {
            const incoming = J.incoming.get(link.target);
            incoming.delete(link);
            if (!incoming.size) {
                J.incoming.delete(link.target);
            }
            link.target = 0;
        }
        link.header.value = -1;
        link.table.set(link.slot, null);
    };
    J.remove = (f) => {
        const rec = J.mods.get(f);
        if (rec) {
            /* Clear incoming guards before table-index/instance-slot reuse. */
            const incoming = J.incoming.get(f);
            if (incoming) {
                for (const link of incoming) {
                    J.unlink(link);
                }
            }
            /* Drop outgoing references too: never retain an evicted graph. */
            for (const link of rec.links) {
                J.unlink(link);
            }
            J.mods.delete(f);
        }
        removeFunction(f);
    };
    J.install = (mod, helper, s0, s1, count) => {
        /* Importing the huge C function table into every TB is expensive. */
        const table = count ? new WebAssembly.Table({
            element: 'anyfunc', initial: count, maximum: count,
        }) : undefined;
        const inst = new WebAssembly.Instance(mod, {
            "env": { "buffer": wasmMemory, "ic_table": table },
            "helper": helper,
            "chain": { "s0": s0 ? wasmTable.get(s0) : J.dummy,
                       "s1": s1 ? wasmTable.get(s1) : J.dummy, "call": J.call },
        });
        J.registry.register(inst, 0);
        const f = addFunction(inst.exports.start, 'ii');
        if (inst.exports.ic_self) {
            inst.exports.ic_self.value = f;
        }
        const links = [];
        for (let i = 0; i < count; i++) {
            links.push({ header: inst.exports['ic' + i], table,
                         slot: i, target: 0 });
        }
        J.mods.set(f, { mod, helper, links });
        return f;
    };
});

EM_JS_DEPS(wasm32_jit, "$addFunction,$removeFunction,$wasmTable");

/* Called only by the dispatcher, with a currently owned target instance. */
EM_JS(void, wasm32_ic_fill, (int source, unsigned slot,
                            const WasmTBHeader *header, int target), {
    const J = Module.__wasm32_jit;
    const rec = J.mods.get(source);
    const link = rec && rec.links[slot];
    if (!link) {
        return; /* source was relinked/evicted before its miss was resolved */
    }
    J.unlink(link);
    link.table.set(slot, wasmTable.get(target));
    link.target = target;
    let incoming = J.incoming.get(target);
    if (!incoming) {
        incoming = new Set();
        J.incoming.set(target, incoming);
    }
    incoming.add(link);
    link.header.value = header;
});

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
    static uint32_t out[5];
    TranslationBlock *tb = hdr ? tcg_tb_lookup((uintptr_t)hdr) : NULL;

    memset(out, 0, sizeof(out));
    if (tb) {
        /* CF_PCREL blocks have no vaddr: give the physical address */
        out[0] = (tb_cflags(tb) & CF_PCREL) ? tb_page_addr0(tb) : tb->pc;
        out[1] = tb->cs_base;
        out[2] = tb->size;
        out[3] = tb->icount;
        /* host address of the guest code bytes, for disassembly */
        out[4] = (uint32_t)(uintptr_t)qemu_map_ram_ptr(NULL, tb_page_addr0(tb));
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
    e->members = NULL;
    e->n_members = 0;
    h->instance = e;
    h->instance_member = 0;
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
    if (!wasm_instance_owns(e, h)) {
        /* dropped (or its slot reused): recompile at the next chance */
        XSTAT_INC(n_tci_dropped);
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
        e->n_members = 0;
        g_clear_pointer(&e->members, g_free);
        /* Do not touch old headers: a TB flush may have reused their bytes. */
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
    env = getenv("XEMU_WASM_JIT_ENTRY_PROF");
    jit_entry_profile = env && *env == '1';
    exec_thread = pthread_self();
    wasm32_vcpu_tb_ptr = (void *volatile *)&wasm_ctx.tb_ptr;
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
    int links[2] = { 0 };
    int fidx;

    /* ICs are mutable; don't also retain unused immutable chain imports. */
    if (!wasm32_ic_enabled() && !region_shared) {
        compute_links(h, depth, links);
    }
    XPHASE_SET(XPHASE_VCPU, "jit_compile");
    double t0 = emscripten_get_now();
    fidx = wasm32_instantiate(h->wasm_ptr, h->wasm_size,
                              h->import_ptr, h->import_size / 4,
                              links[0], links[1], h->ic_count);
    double t1 = emscripten_get_now();
    XPHASE_SET(XPHASE_VCPU, "dispatch");
    jit_debt_ms += t1 - t0;
    jit_budget_update(t1);
    XSTAT_INC(n_jit_compile);
    XSTAT_ADD(ns_jit_compile, (t1 - t0) * 1e6);
    if (!h->icount) {
        TranslationBlock *tb = tcg_tb_lookup((uintptr_t)h);
        h->icount = tb ? tb->icount : 0;
    }
    add_instance(h, fidx);
    return fidx;
}

/*
 * Region modules (on by default; XEMU_WASM_REGION=0 disables): a hot TB and
 * its hot, currently chained successors are merged into one wasm function.
 * Each member keeps its own body unchanged (prologue with the gen_tb_start
 * interrupt check,
 * TCI GETPC positions, exits); a br_table loop selects the member to run,
 * and at every goto_tb/goto_ptr exit whose successor header is a member,
 * the exit branches to that member instead of returning to this dispatcher.
 * By default (XEMU_WASM_REGION_SHARE=0: legacy) one stable instance serves
 * every member, so
 * entries from different callers accumulate hotness on the same function.
 * It never steals owned members or grows/rebuilds an existing group. Legacy
 * mode publishes only the root and can copy already-owned successors.
 * Guards still validate live successor headers; unlinked/invalidated targets
 * fall through to the normal exit. tb_flush cannot happen while a region
 * runs or is Asyncify-suspended (single vCPU thread).
 */
#define REGION_MAX          32
#define REGION_LEGACY_MAX   16
#define REGION_MAX_BYTES    (128 * 1024) /* member entry + rewind bodies */
/* Shared groups have bounded entry functions, not arbitrarily wide traces. */
#define REGION_ENTRY_INPUT_MAX (16 * 1024)
#define REGION_ENTRY_MAX    (32 * 1024)
#define REGION_CFG_MAX      1024
#define REGION_DEPTH_MAX    96
#define REGION_NUM_GLOBALS  17      /* TCG regs + BLOCK_PTR (backend) */
#define REGION_HELPER_START 4       /* HELPER_IDX_START (backend) */
/* Optional invocation-local TLB hints precede the region-only locals. */
#define REGION_CUR_LOCAL    (25 + (wasm32_tlb_hint_enabled() ? 2 : 0))
#define REGION_INST_LOCAL   (REGION_CUR_LOCAL + 1)
#define REGION_L32_0        1

static int region_enabled = -1;
static int region_hints = 1;    /* XEMU_WASM_HINTS=0: no branch hints */

typedef struct ByteBuf {
    uint8_t *p;
    size_t len, cap;
} ByteBuf;

static void bb_need(ByteBuf *b, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = MAX(b->cap * 2, b->len + n + 4096);
        b->p = g_realloc(b->p, b->cap);
    }
}
static void bb_u8(ByteBuf *b, uint8_t v)
{
    bb_need(b, 1);
    b->p[b->len++] = v;
}
static void bb_bytes(ByteBuf *b, const void *src, size_t n)
{
    bb_need(b, n);
    memcpy(b->p + b->len, src, n);
    b->len += n;
}
static void bb_uleb(ByteBuf *b, uint32_t v)
{
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        bb_u8(b, c | (v ? 0x80 : 0));
    } while (v);
}
static void bb_sleb(ByteBuf *b, int32_t v)
{
    bool more = true;
    while (more) {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
            more = false;
        } else {
            c |= 0x80;
        }
        bb_u8(b, c);
    }
}
static void bb_name(ByteBuf *b, const char *s)
{
    bb_uleb(b, strlen(s));
    bb_bytes(b, s, strlen(s));
}
static void bb_section(ByteBuf *b, uint8_t id, ByteBuf *content)
{
    bb_u8(b, id);
    bb_uleb(b, content->len);
    bb_bytes(b, content->p, content->len);
    content->len = 0;
}

static uint32_t rd_uleb(const uint8_t **pp)
{
    uint32_t v = 0;
    int sh = 0;
    uint8_t c;
    do {
        c = *(*pp)++;
        v |= (uint32_t)(c & 0x7f) << sh;
        sh += 7;
    } while (c & 0x80);
    return v;
}

/*
 * Type entries of a TB module's helpers (types 3..): pointers into the
 * module bytes and their lengths.
 */
static int tb_helper_types(WasmTBHeader *h, const uint8_t **ty, int *tylen,
                           int max)
{
    const uint8_t *p = h->wasm_ptr + 8, *end = h->wasm_ptr + h->wasm_size;

    while (p < end) {
        uint8_t id = *p++;
        uint32_t size = rd_uleb(&p);
        const uint8_t *next = p + size;
        if (id == 1) {
            uint32_t n = rd_uleb(&p);
            int k = 0;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *start = p;
                uint32_t cnt;
                p++;                            /* 0x60 */
                cnt = rd_uleb(&p);              /* params */
                p += cnt;
                cnt = rd_uleb(&p);              /* results */
                p += cnt;
                if (i >= 3 && k < max) {
                    ty[k] = start;
                    tylen[k] = p - start;
                    k++;
                }
            }
            return k;
        }
        p = next;
    }
    return 0;
}

static WasmTBHeader *tb_link_target(TranslationBlock *tb, int n)
{
    if (tb && tb->jmp_reset_offset[n] != TB_JMP_OFFSET_INVALID) {
        uintptr_t tgt = qatomic_read(&tb->jmp_target_addr[n]);
        uintptr_t reset = (uintptr_t)tb->tc.ptr + tb->jmp_reset_offset[n];
        if (tgt && tgt != reset) {
            return (WasmTBHeader *)tgt;
        }
    }
    return NULL;
}

/*
 * Decode only the scalar instruction set emitted by this backend. Unknown
 * extensions decline aggregation rather than weakening the CFG bound.
 */
static bool region_leb(const uint8_t **p, const uint8_t *end,
                       unsigned max, uint32_t *out)
{
    uint32_t v = 0;

    for (unsigned i = 0; i < max && *p < end; i++) {
        uint8_t c = *(*p)++;
        if (i < 5) {
            v |= (uint32_t)(c & 127) << (7 * i);
        }
        if (!(c & 128)) {
            *out = v;
            return true;
        }
    }
    return false;
}

/*
 * Bodies exclude locals and their final function end. Score counts structured
 * controls, branch edges and returns; depth is actual lexical nesting.
 */
static bool region_cfg(const uint8_t *p, size_t len,
                       unsigned *score_out, unsigned *depth_out)
{
    const uint8_t *end = p + len;
    unsigned depth = 0, peak = 0, score = 0;
    uint32_t v;

    while (p < end) {
        uint8_t op = *p++;
        unsigned imms = 0;
        if (op >= 2 && op <= 4) {
            depth++;
            peak = MAX(peak, depth);
            score++;
            imms = 1;
        } else if (op == 0x0b) {
            if (!depth) {
                return false;
            }
            depth--;
        } else if (op == 0x0c || op == 0x0d || op == 0x12) {
            score++;
            imms = 1;
        } else if (op == 0x0e) {
            if (!region_leb(&p, end, 5, &v) || v >= end - p) {
                return false;
            }
            score += v + 1;
            imms = v + 1;
        } else if (op == 0x0f) {
            score++;
        } else if (op == 0x10 || (op >= 0x20 && op <= 0x24) ||
                   op == 0x3f || op == 0x40 || op == 0x41) {
            imms = 1;
        } else if (op == 0x11 || op == 0x13 ||
                   (op >= 0x28 && op <= 0x3e)) {
            score += op == 0x13;
            imms = 2;
        } else if (op == 0x42) {
            if (!region_leb(&p, end, 10, &v)) {
                return false;
            }
        } else if (op == 0x43 || op == 0x44) {
            unsigned n = op == 0x43 ? 4 : 8;
            if (end - p < n) {
                return false;
            }
            p += n;
        } else if (op == 0xfc) {
            if (!region_leb(&p, end, 5, &v) || v > 7) {
                return false;
            }
        } else if (!(op == 0 || op == 1 || op == 5 || op == 0x1a ||
                     op == 0x1b || (op >= 0x45 && op <= 0xc4))) {
            return false;
        }
        for (unsigned i = 0; i < imms; i++) {
            if (!region_leb(&p, end, 5, &v)) {
                return false;
            }
        }
        if (score > REGION_CFG_MAX || peak > REGION_DEPTH_MAX) {
            return false;
        }
    }
    *score_out = score;
    *depth_out = peak;
    return depth == 0;
}

static bool region_member_ok(WasmTBHeader *c)
{
    if (!c->wasm_ptr || !c->body_len || !c->reloc_ptr || !c->reloc_b_ptr ||
        c->counter == INT32_MIN ||
        (c->counter < wasm32_jit_threshold / 4 && !c->instance)) {
        return false;
    }
    if (region_shared) {
        unsigned score, depth;
        /* Never steal a live member or replace an already warming function. */
        if (wasm_instance_owns(c->instance, c)) {
            return false;
        }
        if (!c->cfg_score) {
            if (!region_cfg(c->wasm_ptr + c->body_off, c->body_len,
                            &score, &depth)) {
                c->cfg_score = UINT16_MAX;
            } else {
                c->cfg_score = MAX(1, score);
                c->cfg_depth = depth;
            }
        }
        return c->cfg_score <= REGION_CFG_MAX;
    }
    return true;
}

/* i32.const addr; i64.load; +1; i64.store (a stats counter) */
static void bb_count(ByteBuf *b, uint64_t *ctr)
{
    int32_t a = (int32_t)(uintptr_t)ctr;
    bb_u8(b, 0x41); bb_sleb(b, a);
    bb_u8(b, 0x41); bb_sleb(b, a);
    bb_u8(b, 0x29); bb_u8(b, 3); bb_u8(b, 0);       /* i64.load */
    bb_u8(b, 0x42); bb_u8(b, 1);                    /* i64.const 1 */
    bb_u8(b, 0x7c);                                 /* i64.add */
    bb_u8(b, 0x37); bb_u8(b, 3); bb_u8(b, 0);       /* i64.store */
}

/* if (L32_0 == hdr[t]) { ctx.tb_ptr = hdr; ctx.do_init = 1; cur = t; br top } */
static void region_guard(ByteBuf *b, WasmTBHeader *t_hdr, int t, int br_depth)
{
    bb_u8(b, 0x20); bb_uleb(b, REGION_L32_0);       /* local.get L32_0 */
    bb_u8(b, 0x41); bb_sleb(b, (int32_t)(uintptr_t)t_hdr);
    bb_u8(b, 0x46);                                 /* i32.eq */
    bb_u8(b, 0x04); bb_u8(b, 0x40);                 /* if */
    bb_u8(b, 0x20); bb_uleb(b, 0);                  /* ctx */
    bb_u8(b, 0x41); bb_sleb(b, (int32_t)(uintptr_t)t_hdr);
    bb_u8(b, 0x36); bb_u8(b, 2); bb_uleb(b, WASM_CTX_TB_PTR_OFF);
    bb_u8(b, 0x20); bb_uleb(b, 0);
    bb_u8(b, 0x41); bb_sleb(b, 1);
    bb_u8(b, 0x36); bb_u8(b, 2); bb_uleb(b, WASM_CTX_DO_INIT_OFF);
    bb_count(b, &xemu_wasm_stats.n_tb_exec);
    bb_count(b, &xemu_wasm_stats.n_tb_region);
    bb_u8(b, 0x41); bb_sleb(b, t);
    bb_u8(b, 0x21); bb_uleb(b, REGION_CUR_LOCAL);   /* local.set cur */
    bb_u8(b, 0x0c); bb_uleb(b, br_depth + 1);       /* br top (+ this if) */
    bb_u8(b, 0x0b);
}

/*
 * A validated indirect target belongs to this function iff its instance has
 * our function ID AND its indexed member pointer matches. The latter rejects
 * old header aliases after eviction/slot reuse. No linear search on hot rets.
 */
static void region_shared_guard(ByteBuf *b, int n, int br_depth)
{
    bb_u8(b, 0x20); bb_uleb(b, REGION_L32_0);
    bb_u8(b, 0x04); bb_u8(b, 0x40);              /* target != NULL */
    bb_u8(b, 0x20); bb_uleb(b, REGION_L32_0);
    bb_u8(b, 0x28); bb_u8(b, 2); bb_uleb(b, WASM_TB_INSTANCE_OFF);
    bb_u8(b, 0x22); bb_uleb(b, REGION_INST_LOCAL);
    bb_u8(b, 0x04); bb_u8(b, 0x40);              /* instance != NULL */
    bb_u8(b, 0x20); bb_uleb(b, REGION_INST_LOCAL);
    bb_u8(b, 0x28); bb_u8(b, 2); bb_uleb(b, WASM_INSTANCE_FIDX_OFF);
    bb_u8(b, 0x23); bb_uleb(b, WASM_IC_SELF_GLOBAL);
    bb_u8(b, 0x46);
    bb_u8(b, 0x04); bb_u8(b, 0x40);              /* this region */
    bb_u8(b, 0x20); bb_uleb(b, REGION_L32_0);
    bb_u8(b, 0x28); bb_u8(b, 2); bb_uleb(b, WASM_TB_MEMBER_OFF);
    bb_u8(b, 0x22); bb_uleb(b, REGION_CUR_LOCAL);
    bb_u8(b, 0x41); bb_sleb(b, n);
    bb_u8(b, 0x49);                              /* i32.lt_u */
    bb_u8(b, 0x04); bb_u8(b, 0x40);
    bb_u8(b, 0x20); bb_uleb(b, REGION_INST_LOCAL);
    bb_u8(b, 0x28); bb_u8(b, 2); bb_uleb(b, WASM_INSTANCE_MEMBERS_OFF);
    bb_u8(b, 0x20); bb_uleb(b, REGION_CUR_LOCAL);
    bb_u8(b, 0x41); bb_sleb(b, 2);
    bb_u8(b, 0x74);                              /* i32.shl */
    bb_u8(b, 0x6a);                              /* i32.add */
    bb_u8(b, 0x28); bb_u8(b, 2); bb_u8(b, 0);
    bb_u8(b, 0x20); bb_uleb(b, REGION_L32_0);
    bb_u8(b, 0x46);
    bb_u8(b, 0x04); bb_u8(b, 0x40);              /* live member slot */
    bb_u8(b, 0x20); bb_u8(b, 0);
    bb_u8(b, 0x20); bb_uleb(b, REGION_L32_0);
    bb_u8(b, 0x36); bb_u8(b, 2); bb_uleb(b, WASM_CTX_TB_PTR_OFF);
    bb_u8(b, 0x20); bb_u8(b, 0);
    bb_u8(b, 0x41); bb_u8(b, 1);
    bb_u8(b, 0x36); bb_u8(b, 2); bb_uleb(b, WASM_CTX_DO_INIT_OFF);
    bb_count(b, &xemu_wasm_stats.n_tb_exec);
    bb_count(b, &xemu_wasm_stats.n_tb_region);
    bb_u8(b, 0x0c); bb_uleb(b, br_depth + 5);
    for (int i = 0; i < 5; i++) {
        bb_u8(b, 0x0b);
    }
}

/* Compile @h as a region with its hot chained successors; 0 if not worth it. */
static int compile_region(WasmTBHeader *h)
{
    WasmTBHeader *m[REGION_MAX];
    int n = 0;
    uint32_t hq[256];                   /* union of helpers (table indices) */
    const uint8_t *hty[256];
    int htylen[256];
    int nh = 0;
    unsigned ic_base[REGION_MAX], ic_count = 0;
    ByteBuf mod = { 0 }, sec = { 0 }, code = { 0 }, hints = { 0 };
    int nhints = 0;
    int fidx;

    double t_start = emscripten_get_now();
    uint32_t bytes = h->body_len + h->body_b_len;
    uint32_t entry_bytes = h->body_len;
    unsigned score = 0, max_depth = 0;
    int limit = region_shared ? REGION_MAX : REGION_LEGACY_MAX;

    if (!region_member_ok(h)) {
        return 0;
    }
    score = h->cfg_score;
    max_depth = h->cfg_depth;
    m[n++] = h;
    for (int i = 0; i < n && n < limit; i++) {
        TranslationBlock *tb = tcg_tb_lookup((uintptr_t)m[i]);
        for (int s = 0; s < 2 && n < limit; s++) {
            WasmTBHeader *c = tb_link_target(tb, s);
            bool dup = false;
            for (int j = 0; j < n; j++) {
                dup |= m[j] == c;
            }
            if (c && !dup && region_member_ok(c) &&
                bytes + c->body_len + c->body_b_len <= REGION_MAX_BYTES &&
                (!region_shared ||
                 (entry_bytes + c->body_len <= REGION_ENTRY_INPUT_MAX &&
                  score + c->cfg_score + 16 * (n + 1) < REGION_CFG_MAX &&
                  MAX(max_depth, c->cfg_depth) + n + 6 < REGION_DEPTH_MAX))) {
                m[n++] = c;
                bytes += c->body_len + c->body_b_len;
                entry_bytes += c->body_len;
                score += c->cfg_score;
                max_depth = MAX(max_depth, c->cfg_depth);
            }
        }
    }
    if (n < 2 || !region_member_ok(h)) {
        return 0;
    }

    /* per member: its helper k -> union index */
    uint8_t map[REGION_MAX][256];
    for (int i = 0; i < n; i++) {
        ic_base[i] = ic_count;
        ic_count += m[i]->ic_count;
        const uint8_t *ty[256];
        int tylen[256];
        int cnt = m[i]->import_size / 4;
        if (cnt > 256 || tb_helper_types(m[i], ty, tylen, 256) != cnt) {
            return 0;
        }
        for (int k = 0; k < cnt; k++) {
            uint32_t q = m[i]->import_ptr[k];
            int u;
            for (u = 0; u < nh && hq[u] != q; u++) {
            }
            if (u == nh) {
                if (nh == 255) {
                    return 0;
                }
                hq[nh] = q;
                hty[nh] = ty[k];
                htylen[nh] = tylen[k];
                nh++;
            }
            map[i][k] = u;
        }
    }

    bb_bytes(&mod, "\0asm\x01\0\0\0", 8);
    /* types: TB function, helper.u, chain.call, helpers */
    bb_uleb(&sec, 3 + nh);
    bb_bytes(&sec, "\x60\x01\x7f\x01\x7f", 5);
    bb_bytes(&sec, "\x60\x00\x01\x7f", 4);
    bb_bytes(&sec, "\x60\x02\x7f\x7f\x01\x7f", 6);
    for (int u = 0; u < nh; u++) {
        bb_bytes(&sec, hty[u], htylen[u]);
    }
    bb_section(&mod, 1, &sec);
    /* imports: same layout as a TB module */
    bb_uleb(&sec, 5 + nh + (ic_count != 0));
    if (ic_count) {
        bb_name(&sec, "env"); bb_name(&sec, "ic_table");
        bb_u8(&sec, 0x01); bb_u8(&sec, 0x70); bb_u8(&sec, 0x01);
        bb_uleb(&sec, ic_count); bb_uleb(&sec, ic_count);
    }
    bb_name(&sec, "env"); bb_name(&sec, "buffer");
    bb_u8(&sec, 0x02); bb_u8(&sec, 0x03); bb_uleb(&sec, 0); bb_uleb(&sec, 65536);
    bb_name(&sec, "helper"); bb_name(&sec, "u"); bb_u8(&sec, 0); bb_uleb(&sec, 1);
    bb_name(&sec, "chain"); bb_name(&sec, "s0"); bb_u8(&sec, 0); bb_uleb(&sec, 0);
    bb_name(&sec, "chain"); bb_name(&sec, "s1"); bb_u8(&sec, 0); bb_uleb(&sec, 0);
    bb_name(&sec, "chain"); bb_name(&sec, "call"); bb_u8(&sec, 0); bb_uleb(&sec, 2);
    for (int u = 0; u < nh; u++) {
        char name[16];
        snprintf(name, sizeof(name), "%d", u);
        bb_name(&sec, "helper"); bb_name(&sec, name);
        bb_u8(&sec, 0); bb_uleb(&sec, 3 + u);
    }
    bb_section(&mod, 2, &sec);
    /* functions: the region, then each member's rewind function (B) */
    bb_uleb(&sec, 1 + n);
    for (int i = 0; i <= n; i++) {
        bb_uleb(&sec, 0);
    }
    bb_section(&mod, 3, &sec);
    unsigned ic_globals = (wasm32_ic_enabled() || region_shared) ?
                          1 + 2 * ic_count : 0;
    QEMU_BUILD_BUG_ON(REGION_NUM_GLOBALS != WASM_IC_SELF_GLOBAL);
    bb_uleb(&sec, REGION_NUM_GLOBALS + ic_globals);
    for (int i = 0; i < REGION_NUM_GLOBALS; i++) {
        bb_bytes(&sec, "\x7e\x01\x42\x00\x0b", 5);
    }
    if (ic_globals) {
        bb_bytes(&sec, "\x7f\x01\x41\x00\x0b", 5);
        for (unsigned i = 0; i < ic_count; i++) {
            bb_bytes(&sec, "\x7f\x01\x41\x7f\x0b", 5);
            bb_bytes(&sec, "\x7f\x01\x41\x00\x0b", 5);
        }
    }
    bb_section(&mod, 6, &sec);
    bb_uleb(&sec, 1 + (ic_globals ? 1 + ic_count : 0));
    bb_name(&sec, "start"); bb_u8(&sec, 0);
    bb_uleb(&sec, REGION_HELPER_START + nh);
    if (ic_globals) {
        bb_name(&sec, "ic_self"); bb_u8(&sec, 3);
        bb_uleb(&sec, WASM_IC_SELF_GLOBAL);
        for (unsigned i = 0; i < ic_count; i++) {
            char name[16];
            snprintf(name, sizeof(name), "ic%u", i);
            bb_name(&sec, name); bb_u8(&sec, 3);
            bb_uleb(&sec, WASM_IC_HDR_GLOBAL + 2 * i);
        }
    }
    bb_section(&mod, 7, &sec);

    /* the function: TB locals + cur (+ ownership scratch for shared groups) */
    bool tlb_hint = wasm32_tlb_hint_enabled();
    bb_bytes(&code, "\x05\x04\x7f\x02\x7e\x01\x7c\x11\x7e", 9);
    bb_u8(&code, (region_shared ? 2 : 1) + (tlb_hint ? 2 : 0));
    bb_u8(&code, 0x7f);
    unsigned code_start = code.len;
    if (region_shared) {
        /*
         * Fresh calls were ownership-validated by the dispatcher/IC. During
         * rewind, the group's membership cannot change (single vCPU).
         */
        bb_u8(&code, 0x20); bb_u8(&code, 0);
        bb_u8(&code, 0x28); bb_u8(&code, 2);
        bb_uleb(&code, WASM_CTX_TB_PTR_OFF);
        bb_u8(&code, 0x28); bb_u8(&code, 2); bb_uleb(&code, WASM_TB_MEMBER_OFF);
        bb_u8(&code, 0x21); bb_uleb(&code, REGION_CUR_LOCAL);
    }
    /* Legacy resume/entry at member j. */
    for (int j = 1; !region_shared && j < n; j++) {
        bb_u8(&code, 0x20); bb_uleb(&code, 0);
        bb_u8(&code, 0x28); bb_u8(&code, 2); bb_uleb(&code, WASM_CTX_TB_PTR_OFF);
        bb_u8(&code, 0x41); bb_sleb(&code, (int32_t)(uintptr_t)m[j]);
        bb_u8(&code, 0x46);
        bb_u8(&code, 0x04); bb_u8(&code, 0x40);
        bb_u8(&code, 0x41); bb_sleb(&code, j);
        bb_u8(&code, 0x21); bb_uleb(&code, REGION_CUR_LOCAL);
        bb_u8(&code, 0x0b);
    }
    bb_u8(&code, 0x03); bb_u8(&code, 0x40);         /* loop top */
    for (int j = 0; j < n; j++) {
        bb_u8(&code, 0x02); bb_u8(&code, 0x40);     /* block */
    }
    bb_u8(&code, 0x20); bb_uleb(&code, REGION_CUR_LOCAL);
    bb_u8(&code, 0x0e); bb_uleb(&code, n);          /* br_table */
    for (int j = 0; j < n; j++) {
        bb_uleb(&code, j);
    }
    bb_uleb(&code, 0);
    for (int i = 0; i < n; i++) {
        const uint8_t *body = m[i]->wasm_ptr + m[i]->body_off;
        uint32_t pos = 0;
        TranslationBlock *tb = tcg_tb_lookup((uintptr_t)m[i]);
        int top = n - 1 - i;        /* blocks between the body and top */

        bb_u8(&code, 0x0b);                         /* end block i */
        for (uint32_t r = 0; r < m[i]->reloc_count; r++) {
            const WasmReloc *rl = &m[i]->reloc_ptr[r];
            bb_bytes(&code, body + pos, rl->off - pos);
            pos = rl->off;
            if (rl->kind == WASM_RELOC_CALL) {
                uint32_t idx = REGION_HELPER_START + map[i][rl->arg];
                for (int k = 0; k < 5; k++) {
                    bb_u8(&code, (idx & 0x7f) | (k < 4 ? 0x80 : 0));
                    idx >>= 7;
                }
                pos += 5;
            } else if (rl->kind == WASM_RELOC_IC) {
                uint32_t idx = ic_base[i] + rl->arg;
                if (rl->depth) {
                    idx = WASM_IC_HDR_GLOBAL + 2 * idx + rl->depth - 1;
                }
                for (int k = 0; k < 5; k++) {
                    bb_u8(&code, (idx & 0x7f) | (k < 4 ? 0x80 : 0));
                    idx >>= 7;
                }
                pos += 5;
            } else if (rl->kind == WASM_RELOC_RETCALL) {
                uint32_t idx = REGION_HELPER_START + nh + 1 + i;
                for (int k = 0; k < 5; k++) {
                    bb_u8(&code, (idx & 0x7f) | (k < 4 ? 0x80 : 0));
                    idx >>= 7;
                }
                pos += 5;
            } else if (rl->kind == WASM_RELOC_HINT) {
                if (!region_hints) {
                    continue;
                }
                /* code starts with the locals: offsets are body-relative */
                bb_uleb(&hints, code.len);
                bb_uleb(&hints, 1);
                bb_u8(&hints, rl->arg);
                nhints++;
            } else if (rl->kind == WASM_RELOC_GOTO) {
                /* depth from here to top: TB loop, body level, blocks */
                int d = rl->depth + 1 + top;
                if (rl->arg == 0xff) {
                    if (region_shared) {
                        region_shared_guard(&code, n, d);
                    } else {
                        for (int t = 0; t < n; t++) {
                            region_guard(&code, m[t], t, d);
                        }
                    }
                } else {
                    WasmTBHeader *c = tb_link_target(tb, rl->arg);
                    for (int t = 0; t < n; t++) {
                        if (m[t] == c) {
                            region_guard(&code, m[t], t, d);
                        }
                    }
                }
            }
        }
        bb_bytes(&code, body + pos, m[i]->body_len - pos);
    }
    bb_u8(&code, 0x0b);                             /* end loop */
    bb_u8(&code, 0x00);                             /* unreachable */
    bb_u8(&code, 0x0b);                             /* end func */
    if (region_shared &&
        (code.len > REGION_ENTRY_MAX ||
         !region_cfg(code.p + code_start, code.len - code_start - 1,
                     &score, &max_depth))) {
        XSTAT_INC(n_region_cap);
        g_free(mod.p);
        g_free(sec.p);
        g_free(code.p);
        g_free(hints.p);
        return 0;
    }
    /* members' rewind functions: B bodies with their helper calls remapped */
    ByteBuf bcode[REGION_MAX] = { 0 }, bhints[REGION_MAX] = { 0 };
    int nbh[REGION_MAX] = { 0 };
    for (int i = 0; i < n; i++) {
        const uint8_t *body = m[i]->wasm_ptr + m[i]->body_b_off;
        uint32_t pos = 0;
        bb_bytes(&bcode[i], tlb_hint ? WASM_TB_HINT_LOCALS : WASM_TB_LOCALS,
                 WASM_TB_LOCALS_LEN + (tlb_hint ? 2 : 0));
        for (uint32_t r = 0; r < m[i]->reloc_b_count; r++) {
            const WasmReloc *rl = &m[i]->reloc_b_ptr[r];
            bb_bytes(&bcode[i], body + pos, rl->off - pos);
            pos = rl->off;
            if (rl->kind == WASM_RELOC_CALL) {
                uint32_t idx = REGION_HELPER_START + map[i][rl->arg];
                for (int k = 0; k < 5; k++) {
                    bb_u8(&bcode[i], (idx & 0x7f) | (k < 4 ? 0x80 : 0));
                    idx >>= 7;
                }
                pos += 5;
            } else if (rl->kind == WASM_RELOC_IC) {
                uint32_t idx = ic_base[i] + rl->arg;
                if (rl->depth) {
                    idx = WASM_IC_HDR_GLOBAL + 2 * idx + rl->depth - 1;
                }
                for (int k = 0; k < 5; k++) {
                    bb_u8(&bcode[i], (idx & 0x7f) | (k < 4 ? 0x80 : 0));
                    idx >>= 7;
                }
                pos += 5;
            } else if (rl->kind == WASM_RELOC_HINT && region_hints) {
                bb_uleb(&bhints[i], bcode[i].len);
                bb_uleb(&bhints[i], 1);
                bb_u8(&bhints[i], rl->arg);
                nbh[i]++;
            }
        }
        bb_bytes(&bcode[i], body + pos, m[i]->body_b_len - pos);
        bb_u8(&bcode[i], 0x0b);                     /* end func */
    }
    int nfh = (nhints > 0);
    for (int i = 0; i < n; i++) {
        nfh += nbh[i] > 0;
    }
    if (nfh) {
        bb_name(&sec, "metadata.code.branch_hint");
        bb_uleb(&sec, nfh);
        if (nhints) {
            bb_uleb(&sec, REGION_HELPER_START + nh);
            bb_uleb(&sec, nhints);
            bb_bytes(&sec, hints.p, hints.len);
        }
        for (int i = 0; i < n; i++) {
            if (nbh[i]) {
                bb_uleb(&sec, REGION_HELPER_START + nh + 1 + i);
                bb_uleb(&sec, nbh[i]);
                bb_bytes(&sec, bhints[i].p, bhints[i].len);
            }
        }
        bb_section(&mod, 0, &sec);
    }
    bb_uleb(&sec, 1 + n);
    bb_uleb(&sec, code.len);
    bb_bytes(&sec, code.p, code.len);
    for (int i = 0; i < n; i++) {
        bb_uleb(&sec, bcode[i].len);
        bb_bytes(&sec, bcode[i].p, bcode[i].len);
        g_free(bcode[i].p);
        g_free(bhints[i].p);
    }
    bb_section(&mod, 10, &sec);

    XPHASE_SET(XPHASE_VCPU, "jit_compile");
    fidx = wasm32_instantiate(mod.p, mod.len, hq, nh, 0, 0, ic_count);
    double t0 = t_start, t1 = emscripten_get_now();
    XPHASE_SET(XPHASE_VCPU, "dispatch");
    jit_debt_ms += t1 - t0;     /* incl. region assembly */
    jit_budget_update(t1);
    XSTAT_INC(n_jit_compile);
    XSTAT_INC(n_region_compile);
    XSTAT_ADD(n_region_members, n);
    if (region_shared) {
        XSTAT_INC(n_region_shared);
        XSTAT_ADD(n_region_aliases, n - 1);
        xemu_wasm_stats.n_region_bytes_max =
            MAX(xemu_wasm_stats.n_region_bytes_max, code.len);
        xemu_wasm_stats.n_region_cfg_max =
            MAX(xemu_wasm_stats.n_region_cfg_max, score);
        xemu_wasm_stats.n_region_depth_max =
            MAX(xemu_wasm_stats.n_region_depth_max, max_depth);
    }
    XSTAT_ADD(ns_jit_compile, (t1 - t0) * 1e6);
    g_free(mod.p);
    g_free(sec.p);
    g_free(code.p);
    g_free(hints.p);
    if (!h->icount) {
        TranslationBlock *tb = tcg_tb_lookup((uintptr_t)h);
        h->icount = tb ? tb->icount : 0;
    }
    add_instance(h, fidx);
    if (region_shared) {
        WasmInstance *e = h->instance;
        /*
         * Check before installing the table: a stale header may still point
         * at this just-reused instance slot and its old index may match.
         */
        for (int i = 1; i < n; i++) {
            g_assert(!wasm_instance_owns(m[i]->instance, m[i]));
        }
        e->members = g_memdup2(m, n * sizeof(m[0]));
        e->n_members = n;
        for (int i = 1; i < n; i++) {
            /*
             * Publish only after successful instantiation. Members become
             * stable aliases, never overlapping independent region roots.
             */
            m[i]->instance_member = i;
            m[i]->instance = e;
            if (!m[i]->icount) {
                TranslationBlock *tb = tcg_tb_lookup((uintptr_t)m[i]);
                m[i]->icount = tb ? tb->icount : 0;
            }
        }
    }
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
    bool profile;

    if (unlikely(!initdone)) {
        wasm32_init();
        initdone = true;
    }
    if (unlikely(!pthread_equal(exec_thread, pthread_self()))) {
        error_report("tcg: the wasm32 JIT supports a single vCPU thread "
                     "(use -accel tcg,thread=single)");
        abort();
    }

    /* Keep the diagnostic gate loop-invariant; counters can be very hot. */
    profile = jit_entry_profile;
    wasm_ctx.env = env;
    wasm_ctx.tb_ptr = (void *)v_tb_ptr;
    wasm_ctx.ic_source = 0;
    for (;;) {
        WasmTBHeader *h = wasm_ctx.tb_ptr;
        uintptr_t res;
        int fidx;
        uint32_t ic_source = wasm_ctx.ic_source;
        uint32_t ic_slot = wasm_ctx.ic_slot;

        wasm_ctx.ic_source = 0;
        trysleep();
        if (!first) {
            XSTAT_INC(n_tb_exec);
        }
        first = false;

        wasm32_cur_tb = h;
        fidx = get_instance(h);
        if (fidx > 0) {
            if (ic_source) {
                XSTAT_INC(n_ic_fill);
                wasm32_ic_fill(ic_source, ic_slot, h, fidx);
            }
            XSTAT_ADD(n_guest_insn, h->icount);
            if (profile) {
                XSTAT_INC(n_jit_entries);
                XSTAT_ADD(n_jit_alias_entries, h->instance_member != 0);
            }
            wasm_ctx.do_init = 1;
            XPHASE_SET(XPHASE_VCPU, NULL);
            res = ((wasm_func_ptr)(uintptr_t)fidx)(&wasm_ctx);
            XPHASE_SET(XPHASE_VCPU, "dispatch");
        } else if (h->counter < wasm32_jit_threshold) {
            h->counter++;
            XSTAT_INC(n_tci_exec);
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
            if (unlikely(region_enabled < 0)) {
                const char *e = getenv("XEMU_WASM_REGION");
                const char *l = getenv("XEMU_WASM_LINK");
                const char *hn = getenv("XEMU_WASM_HINTS");
                region_hints = !(hn && *hn == '0');
                region_enabled = !(e && *e == '0');
                /*
                 * Members' chain.s0/s1 tail calls would share the region's
                 * single pair of chain imports: not supported together.
                 */
                if (region_enabled && l && *l == '1') {
                    warn_report("wasm32 regions disabled by XEMU_WASM_LINK=1");
                    region_enabled = 0;
                }
                const char *sh = getenv("XEMU_WASM_REGION_SHARE");
                region_shared = region_enabled && !(sh && *sh == '0');
            }
            fidx = region_enabled ? compile_region(h) : 0;
            if (!fidx) {
                fidx = compile_tb(h, 0);
            }
            if (unlikely(wasm32_jit_debug > 0)) {
                wasm32_jit_debug--;
                fprintf(stderr, "[jit] instantiate tb=%p size=%u imports=%u "
                        "fidx=%d\n", h, h->wasm_size, h->import_size / 4, fidx);
            }
            if (ic_source) {
                XSTAT_INC(n_ic_fill);
                wasm32_ic_fill(ic_source, ic_slot, h, fidx);
            }
            if (profile) {
                XSTAT_INC(n_jit_entries);
                XSTAT_ADD(n_jit_alias_entries, h->instance_member != 0);
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
