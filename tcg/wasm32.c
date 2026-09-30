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

int wasm32_jit_threshold = 1000;
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
                                const uint32_t *import_vec, int import_count),
{
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
    });
    Module.__wasm32_jit.registry.register(inst, 0);
    return addFunction(inst.exports.start, 'ii');
});

EM_JS(void, wasm32_remove_function, (int idx), {
    removeFunction(idx);
});

EM_JS(void, wasm32_js_init, (int *collected), {
    Module.__wasm32_jit = {
        /* counts instances reclaimed by the GC */
        registry: new FinalizationRegistry(() => {
            Atomics.add(HEAP32, collected >> 2, 1);
        }),
    };
});

EM_JS_DEPS(wasm32_jit, "$addFunction,$removeFunction,$wasmTable");

/*
 * Max number of instances alive at the same time.
 */
#define MAX_INSTANCES 15000
#define INSTANCES_BUF_MAX (MAX_INSTANCES + 1)

typedef struct WasmInstance {
    void *tb;       /* the TB header this instance belongs to, or NULL */
    int func_idx;   /* table index of its start function */
} WasmInstance;

static WasmInstance instances[INSTANCES_BUF_MAX];
static int instances_begin, instances_end;
static int instances_alive;     /* created and not yet collected */
static int instances_pending_gc;
static int instances_collected; /* written by the JS FinalizationRegistry */

static int instances_count(void)
{
    if (instances_begin <= instances_end) {
        return instances_end - instances_begin;
    }
    return instances_end + INSTANCES_BUF_MAX - instances_begin;
}

static void add_instance(WasmTBHeader *h, int func_idx)
{
    WasmInstance *e = &instances[instances_end];

    e->tb = h;
    e->func_idx = func_idx;
    h->instance = e;
    instances_end = (instances_end + 1) % INSTANCES_BUF_MAX;
    instances_alive++;
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
        h->counter = wasm32_jit_threshold;
        return 0;
    }
    return e->func_idx;
}

/* Drop the older half of the instances. */
static void remove_instances(void)
{
    int num;

    if (instances_pending_gc > 0) {
        return;
    }
    num = instances_count() / 2;
    for (int i = 0; i < num; i++) {
        WasmInstance *e = &instances[instances_begin];
        wasm32_remove_function(e->func_idx);
        e->tb = NULL;
        instances_begin = (instances_begin + 1) % INSTANCES_BUF_MAX;
    }
    instances_pending_gc += num;
}

static void check_instances_collected(void)
{
    int n = qatomic_xchg(&instances_collected, 0);

    if (n > 0) {
        instances_alive -= n;
        instances_pending_gc -= n;
        if (instances_pending_gc < 0) {
            instances_pending_gc = 0;
        }
    }
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
        wasm32_jit_threshold = atoi(env);
    }
    env = getenv("XEMU_WASM_JIT_DEBUG");
    if (env) {
        wasm32_jit_debug = atoi(env);
    }
    exec_thread = pthread_self();
    wasm_ctx.stack = g_malloc0(TCG_STATIC_CALL_ARGS_SIZE +
                               TCG_STATIC_FRAME_SIZE);
    wasm_ctx.tci_tb_ptr = &tci_tb_ptr;
    wasm32_js_init(&instances_collected);
    info_report("tcg: wasm32 JIT enabled (threshold %d)",
                wasm32_jit_threshold);
}

typedef uint32_t (*wasm_func_ptr)(WasmContext *);

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
            res = ((wasm_func_ptr)(uintptr_t)fidx)(&wasm_ctx);
        } else if (h->counter < wasm32_jit_threshold) {
            h->counter++;
            XPHASE_SET(XPHASE_VCPU, "tci");
            res = tci_exec_tb(env, h->tci_ptr);
            XPHASE_SET(XPHASE_VCPU, NULL);
        } else if (!can_add_instance()) {
            XPHASE_SET(XPHASE_VCPU, "jit_evict");
            remove_instances();
            check_instances_collected();
            XPHASE_SET(XPHASE_VCPU, "tci");
            res = tci_exec_tb(env, h->tci_ptr);
            XPHASE_SET(XPHASE_VCPU, NULL);
        } else {
            XPHASE_SET(XPHASE_VCPU, "jit_compile");
            fidx = wasm32_instantiate(h->wasm_ptr, h->wasm_size,
                                      h->import_ptr, h->import_size / 4);
            XPHASE_SET(XPHASE_VCPU, NULL);
            add_instance(h, fidx);
            if (unlikely(wasm32_jit_debug > 0)) {
                wasm32_jit_debug--;
                fprintf(stderr, "[jit] instantiate tb=%p size=%u imports=%u "
                        "fidx=%d\n", h, h->wasm_size, h->import_size / 4, fidx);
            }
            wasm_ctx.do_init = 1;
            res = ((wasm_func_ptr)(uintptr_t)fidx)(&wasm_ctx);
            if (unlikely(wasm32_jit_debug > 0)) {
                fprintf(stderr, "[jit]  -> res=%lx next=%p\n",
                        (unsigned long)res, wasm_ctx.tb_ptr);
            }
        }
        if (wasm_ctx.tb_ptr == NULL) {
            return res;
        }
    }
}
