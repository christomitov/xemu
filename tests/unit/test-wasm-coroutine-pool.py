#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source pool/backend test: real Wasm fibers, Asyncify and mmap/free.

AIO context and thread-exit notifier plumbing are fixtures. This exercises
read/yield/recycle bursts, not a guest game or QEMU's block-device stack.
Set EMCC/NODE to compiler/runner commands; all policies use OS XEMU_* env.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
POOL = (ROOT / "util/qemu-coroutine.c").read_text()
BACKEND = (ROOT / "util/coroutine-wasm.c").read_text()
STACK = (ROOT / "util/oslib-posix.c").read_text()
HEADER = (ROOT / "include/qemu/coroutine_int.h").read_text()


def function(source, name):
    match = re.search(r"(?m)^(?:static )?[\w *]+\b" + name +
                      r"\([^;]*?\)\n\{", source)
    assert match, name
    return source[match.start():source.index("\n}", match.end()) + 2]


C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/fiber.h>
#include "qemu/atomic.h"
#include "qemu/queue.h"
#include "qemu/coroutine-tls.h"
#include "qemu/xemu-wasm-stats.h"
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ROUND_UP(n,d) (((n) + (d) - 1) & ~((d) - 1))
#define g_malloc0(n) calloc(1, (n))
#define g_new(t,n) ((t *)malloc(sizeof(t) * (n)))
#define g_free free
#define qemu_build_assert(x) _Static_assert(x, #x)
#define CONFIG_COROUTINE_POOL
#define coroutine_fn
typedef struct Coroutine Coroutine;
typedef void CoroutineEntry(void *);
typedef struct AioContext { int unused; } AioContext;
typedef struct Notifier {
    void (*notify)(struct Notifier *, void *);
} Notifier;
typedef pthread_mutex_t QemuMutex;
static void qemu_thread_atexit_add(Notifier *n) { (void)n; }
static QemuMutex *take_lock(QemuMutex *p)
{
    assert(!pthread_mutex_lock(p));
    return p;
}
static void release_lock(QemuMutex **p)
{
    if (*p) { assert(!pthread_mutex_unlock(*p)); *p = NULL; }
}
#define WITH_QEMU_LOCK_GUARD(p) \
    for (QemuMutex *guard __attribute__((cleanup(release_lock))) = \
         take_lock(p); guard; release_lock(&guard))
static size_t qemu_real_host_page_size(void) { return 65536; }
void xemu_wasm_lowmem_check(const char *p) { (void)p; }
void xemu_wasm_dbg_ring_put(const char *p, ...) { (void)p; }
#define trace_qemu_aio_coroutine_enter(...) ((void)0)
#define trace_qemu_coroutine_terminate(...) ((void)0)
#define trace_qemu_coroutine_yield(...) ((void)0)
XemuWasmStats xemu_wasm_stats;
'''
C += HEADER[HEADER.index("#define COROUTINE_STACK_SIZE"):
            HEADER.rindex("#endif")]
C += function(STACK, "qemu_alloc_stack")
C += function(STACK, "qemu_free_stack")
C += BACKEND[BACKEND.index("typedef struct {"):]
C += POOL[POOL.index("enum {"):POOL.index("void qemu_aio_coroutine_enter")]
C += function(POOL, "qemu_aio_coroutine_enter")
C += function(POOL, "qemu_coroutine_yield")
C += r'''
static int fd;
static void entry(void *opaque)
{
    uintptr_t id = (uintptr_t)opaque;
    volatile uint64_t sentinel = UINT64_C(0xfedcba9876500000) + id;
    uint8_t buf[4096];

    for (unsigned round = 0; round < 2; round++) {
        assert(pread(fd, buf, sizeof(buf), (id % 512) * sizeof(buf)) ==
               sizeof(buf));
        for (unsigned i = 0; i < sizeof(buf); i++) {
            assert(buf[i] == (i & 255));
        }
        if (!(id % 64)) {
            emscripten_sleep(0);
        }
        assert(sentinel == UINT64_C(0xfedcba9876500000) + id);
        qemu_coroutine_yield();
        assert(sentinel == UINT64_C(0xfedcba9876500000) + id);
    }
}

static void cleanup_local(void)
{
    /* Fixture invokes the actual registered thread-exit cleanup callback. */
    Notifier *n = get_ptr_local_pool_cleanup_notifier();
    if (n->notify) { n->notify(n, NULL); }
}

static void cleanup_global(void)
{
    CoroutinePoolBatch *batch;
    while ((batch = QSLIST_FIRST(&global_pool))) {
        QSLIST_REMOVE_HEAD(&global_pool, next);
        global_pool_size -= batch->size;
        coroutine_pool_batch_delete(batch);
    }
    assert(!global_pool_size);
}

static void bursts(unsigned n, unsigned cycles)
{
    Coroutine **co = calloc(n, sizeof(*co));
    bool small = coroutine_pool_batch_max_size() == 8;

    for (unsigned cycle = 0; cycle < cycles; cycle++) {
        for (unsigned i = 0; i < n; i++) {
            co[i] = qemu_coroutine_create(entry, (void *)(uintptr_t)i);
        }
        assert(xemu_wasm_stats.n_co_live >= n);
        for (unsigned round = 0; round < 3; round++) {
            for (unsigned i = 0; i < n; i++) {
                qemu_aio_coroutine_enter(NULL, co[i]);
            }
        }
        assert(xemu_wasm_stats.n_co_live == xemu_wasm_stats.n_co_pooled);
        assert(xemu_wasm_stats.n_co_allocated - xemu_wasm_stats.n_co_freed ==
               xemu_wasm_stats.n_co_live);
        unsigned local = 0;
        CoroutinePoolBatch *batch;
        QSLIST_FOREACH(batch, get_ptr_local_pool(), next) {
            local += batch->size;
        }
        if (small) {
            assert(local <= 16);
            assert(global_pool_size <= 16);
            assert(xemu_wasm_stats.n_co_live <= 32);
            assert(xemu_wasm_stats.b_co_live < 67 * 1024 * 1024);
        } else if (n == 300) {
            assert(xemu_wasm_stats.n_co_live == 300);
            assert(xemu_wasm_stats.b_co_live > 600 * 1024 * 1024);
        }
        printf("BURST n=%u cycle=%u local=%u global=%u live=%llu "
               "pooled=%llu peak=%llu bytes=%llu malloc_live=%d\n",
               n, cycle, local, global_pool_size,
               (unsigned long long)xemu_wasm_stats.n_co_live,
               (unsigned long long)xemu_wasm_stats.n_co_pooled,
               (unsigned long long)xemu_wasm_stats.n_co_peak,
               (unsigned long long)xemu_wasm_stats.b_co_live,
               mallinfo().uordblks);
    }
    free(co);
}

int main(void)
{
    assert(!pthread_mutex_init(&global_pool_lock, NULL));
    global_pool_hard_max_size = UINT_MAX;
    /* Simulate a block device enlarging the native global cache target. */
    global_pool_max_size += 8192;
    fd = open("/data/reads.bin", O_RDONLY);
    assert(fd >= 0);
    bursts(300, 3);
    cleanup_local();
    /* Refill from the global pool; worker migration is not modeled here. */
    bursts(129, 2);
    cleanup_local();
    cleanup_global();
    assert(!xemu_wasm_stats.n_co_live);
    assert(!xemu_wasm_stats.n_co_pooled);
    assert(!xemu_wasm_stats.b_co_live);
    assert(xemu_wasm_stats.n_co_allocated == xemu_wasm_stats.n_co_freed);
    assert(xemu_wasm_stats.n_co_peak >= 300);
    close(fd);
    printf("PASS allocated=%llu freed=%llu live=0 peak=%llu "
           "leaders=%llu malloc_live=%d\n",
           (unsigned long long)xemu_wasm_stats.n_co_allocated,
           (unsigned long long)xemu_wasm_stats.n_co_freed,
           (unsigned long long)xemu_wasm_stats.n_co_peak,
           (unsigned long long)xemu_wasm_stats.n_co_leaders,
           mallinfo().uordblks);
    emscripten_force_exit(0);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="test-wasm-co-pool-") as tmp:
    work = Path(tmp)
    (work / "test.c").write_text(C)
    (work / "reads.bin").write_bytes(bytes(range(256)) * 8192)
    (work / "env.js").write_text('''
Module.preRun = [function() {
  Object.assign(ENV, Object.fromEntries(Object.entries(process.env)
    .filter(([k]) => k.startsWith('XEMU_'))));
  FS.mkdir('/data');
  FS.mount(NODEFS, {root: @DIR@}, '/data');
}];
'''.replace('@DIR@', json.dumps(tmp)))
    cc = shlex.split(os.environ.get("EMCC", "emcc"))
    node = shlex.split(os.environ.get("NODE", "node"))
    subprocess.run(cc + [str(work / "test.c"), "-I" + str(ROOT / "include"),
                         "-O2", "-pthread", "-sASYNCIFY=1",
                         "-sINITIAL_MEMORY=1GB", "-sSTACK_SIZE=2MB",
                         "-sDEFAULT_PTHREAD_STACK_SIZE=2MB", "-sEXIT_RUNTIME=1",
                         "--pre-js", str(work / "env.js"),
                         "-lnodefs.js", "-o", str(work / "test.cjs")],
                   check=True)
    for policy in (None, "0", "1", "invalid"):
        env = dict(os.environ)
        env.pop("XEMU_WASM_CO_POOL", None)
        if policy is not None:
            env["XEMU_WASM_CO_POOL"] = policy
        print("POLICY", policy, flush=True)
        subprocess.run(node + [str(work / "test.cjs")], env=env, check=True,
                       timeout=90)
