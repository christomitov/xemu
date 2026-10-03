#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source SMC eviction policy/locking/range tests; no emulator needed."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "accel/tcg/tb-maint.c").read_text()
SYSTEM = SOURCE.index("static void\ntb_invalidate_phys_page_range__locked")


def function(name, start=0):
    signature = r"(?m)^(?:static )?(?:inline )?(?:void|bool|unsigned)\s+"
    match = re.search(signature + name + r"\([^;]*?\)\n\{", SOURCE[start:])
    assert match, name
    begin = start + match.start()
    # Alternative #if arms contain mutually exclusive opening braces.
    end = SOURCE.index("\n}", start + match.end()) + 2
    return SOURCE[begin:end]


page = re.search(r"struct PageDesc \{.*?\n\};", SOURCE, re.S).group()
iterator = SOURCE[SOURCE.index("#define TB_FOR_EACH_TAGGED"):
                  SOURCE.index("#define TB_FOR_EACH_JMP")]
constants = "\n".join(re.findall(r"^#define SMC_DROP_.*", SOURCE, re.M))

C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define XBOX 1
#define EMSCRIPTEN 1
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE 4096u
#define V_L2_SIZE 16
#define TARGET_PAGE_MASK (~(uint64_t)(TARGET_PAGE_SIZE - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CF_COUNT_MASK 255
#define CF_NOIRQ 256
#define INVALID (1u << 20)
#define g_free bitmap_free
#define tcg_debug_assert assert
#define unlikely(x) (x)
#define g_assert_not_reached() abort()
#define XSMC_PROF_BEGIN(x) do { } while (0)
#define XSMC_PROF_SET(x) do { } while (0)
#define XSMC_PROF_END() do { } while (0)
#define XSTAT_INC(n) (++stats.n)
typedef uint64_t ram_addr_t;
typedef uint64_t tb_page_addr_t;
typedef int QemuSpin;
typedef int PageForEachNext;
typedef struct PageDesc PageDesc;
typedef struct TranslationBlock {
    uint64_t page_addr[2];
    unsigned size, cflags;
    uintptr_t page_next[2];
} TranslationBlock;
struct TCGOps { bool precise_smc; } ops = { true };
struct CPUClass { struct TCGOps *tcg_ops; } cc = { &ops };
typedef struct CPUState {
    struct CPUClass *cc;
    unsigned cflags_next_tb;
} CPUState;
static struct {
    unsigned n_smc_bitmap_miss, n_smc_drop_pages;
    unsigned n_smc_drop_busy, n_smc_drop_changed;
} stats;
#define tb_page_addr0(tb) ((tb)->page_addr[0])
#define tb_page_addr1(tb) ((tb)->page_addr[1])
#define tb_cflags(tb) ((tb)->cflags)
#define PAGE_FOR_EACH_TB(start, last, pd, tb, n) \
    TB_FOR_EACH_TAGGED((pd)->first_tb, tb, n, page_next)
''' + page + "\n" + iterator + constants + r'''
static int smc_policy = -1, smc_threshold;
/* The pre-feature layout: enabling DROP must not enlarge page metadata. */
struct PageBeforeDrop {
    QemuSpin lock;
    uintptr_t first_tb;
    uint32_t smc_miss, smc_writes;
    unsigned long *code_bitmap;
};
_Static_assert(sizeof(PageDesc) == sizeof(struct PageBeforeDrop), "page size");
_Static_assert(offsetof(PageDesc, code_bitmap) ==
               offsetof(struct PageBeforeDrop, code_bitmap), "bitmap offset");
static PageDesc pages[16];
static bool code_dirty[16];
static TranslationBlock a, b, extra, active;
static TranslationBlock *current;
static bool unknown_ra;
static unsigned invalidated, restored, exits, committed, collection_locks;
static jmp_buf unwind;
static void (*lock_hook)(void);
struct page_collection { unsigned dummy; } collection;
static PageDesc *page_find(uint64_t index) {
    return index < 16 ? &pages[index] : NULL;
}
static void assert_page_locked(PageDesc *p) { assert(p->lock); }
static void page_lock(PageDesc *p) { assert(!p->lock); p->lock = 1; }
static void page_unlock(PageDesc *p) { assert(p->lock); p->lock = 0; }
static struct page_collection *page_collection_lock(uint64_t first,
                                                     uint64_t last) {
    assert((first >> 12) == (last >> 12));
    for (unsigned i = 0; i < 16; i++) page_lock(&pages[i]);
    collection_locks++;
    if (lock_hook) { void (*f)(void) = lock_hook; lock_hook = NULL; f(); }
    return &collection;
}
static void page_collection_unlock(struct page_collection *p) {
    assert(p == &collection);
    for (unsigned i = 0; i < 16; i++) page_unlock(&pages[i]);
}
static unsigned bitmap_allocs, bitmap_frees, bitmap_live, bitmap_peak;
static unsigned long *bitmap_new(unsigned bits) {
    assert(bits == TARGET_PAGE_SIZE);
    bitmap_allocs++; bitmap_live += TARGET_PAGE_SIZE / 8;
    if (bitmap_live > bitmap_peak) bitmap_peak = bitmap_live;
    return calloc((bits + 8 * sizeof(long) - 1) / (8 * sizeof(long)),
                  sizeof(long));
}
static void bitmap_free(unsigned long *p) {
    if (p) {
        assert(bitmap_live >= TARGET_PAGE_SIZE / 8);
        bitmap_live -= TARGET_PAGE_SIZE / 8; bitmap_frees++;
    }
    free(p);
}
static void bitmap_set(unsigned long *map, unsigned first, unsigned count) {
    for (unsigned i = first; i < first + count; i++)
        map[i / (8 * sizeof(long))] |= 1ul << (i % (8 * sizeof(long)));
}
static unsigned find_next_bit(const unsigned long *map, unsigned end,
                              unsigned first) {
    assert(end <= TARGET_PAGE_SIZE);
    while (first < end && !(map[first / (8 * sizeof(long))] &
                           (1ul << (first % (8 * sizeof(long)))))) first++;
    return first;
}
static void tlb_protect_code(uint64_t addr) { code_dirty[addr >> 12] = false; }
static void tlb_unprotect_code(uint64_t addr) {
    assert(!pages[addr >> 12].first_tb);
    code_dirty[addr >> 12] = true;
}
static TranslationBlock *tcg_tb_lookup(uintptr_t ra) {
    assert(ra == 123);
    return unknown_ra ? NULL : current;
}
static unsigned curr_cflags(CPUState *cpu) { return 0x400; }
static void cpu_restore_state_from_tb(CPUState *cpu, TranslationBlock *tb,
                                      uintptr_t ra) {
    assert(tb == current && ra == 123); restored++;
}
static void cpu_loop_exit_noexc(CPUState *cpu) {
    for (unsigned i = 0; i < 16; i++) assert(!pages[i].lock);
    exits++; longjmp(unwind, 1);
}
static void xemu_wasm_count(const char *key) {
    assert(!strcmp(key, "noexc:smc"));
}
static void tb_phys_invalidate__locked(TranslationBlock *tb);
'''
for name in ["smc_drop_limit", "smc_drop_count", "smc_drop_reset",
             "tb_page_range", "page_code_bitmap_add", "page_code_bitmap_drop",
             "page_code_bitmap_build", "smc_config", "smc_drop_miss",
             "smc_drop_backoff", "tb_remove_all_1", "tb_page_add",
             "tb_page_remove"]:
    C += function(name) + "\n"
for name in ["tb_invalidate_phys_page_range__locked", "smc_drop_range",
             "tb_invalidate_phys_range_fast"]:
    C += function(name, SYSTEM) + "\n"

C += r'''
static void tb_phys_invalidate__locked(TranslationBlock *tb) {
    assert(!(tb->cflags & INVALID));
    tb->cflags |= INVALID;
    invalidated++;
    unsigned p0 = tb->page_addr[0] >> 12;
    unsigned p1 = tb->page_addr[1] >> 12;
    tb_page_remove(&pages[p0], tb);
    if (tb->page_addr[1] != (uint64_t)-1 && p1 != p0)
        tb_page_remove(&pages[p1], tb);
}
static CPUState cpu = { &cc, 0 };
static void record(TranslationBlock *tb) {
    tb_page_add(&pages[tb->page_addr[0] >> 12], tb, 0);
    if (tb->page_addr[1] != (uint64_t)-1)
        tb_page_add(&pages[tb->page_addr[1] >> 12], tb, 1);
}
static void setup(void) {
    for (unsigned i = 0; i < 16; i++) bitmap_free(pages[i].code_bitmap);
    memset(pages, 0, sizeof(pages));
    memset(&stats, 0, sizeof(stats));
    memset(code_dirty, 0, sizeof(code_dirty));
    invalidated = restored = exits = committed = collection_locks = 0;
    unknown_ra = false; lock_hook = NULL; cpu.cflags_next_tb = 0;
    a = (TranslationBlock){ .page_addr = { 0x1100, -1 }, .size = 32 };
    /* Includes code at the start of a non-contiguous second physical page. */
    b = (TranslationBlock){ .page_addr = { 0x1ff0, 0x3000 }, .size = 32 };
    active = (TranslationBlock){ .page_addr = { 0x8100, -1 }, .size = 16 };
    current = &active;
    struct page_collection *set = page_collection_lock(0x1000, 0x1fff);
    record(&a); record(&b); record(&active);
    page_collection_unlock(set);
}
static void store(uint64_t addr) {
    if (!code_dirty[addr >> 12])
        tb_invalidate_phys_range_fast(&cpu, addr, 4, 123);
    /* Stand-in for the caller's dirty-client updates and actual RAM store. */
    committed++;
    for (unsigned i = 0; i < 16; i++) assert(!pages[i].lock);
}
static void ready(unsigned index) {
    PageDesc *p = &pages[index];
    page_lock(p);
    if (!p->code_bitmap) page_code_bitmap_build(p);
    p->smc_miss = SMC_DROP_INITIAL - 1;
    page_unlock(p);
}
static void publish_at_store(void) {
    extra = (TranslationBlock){ .page_addr = { 0x1400, -1 }, .size = 4 };
    record(&extra);
}
static void publish_elsewhere(void) {
    extra = (TranslationBlock){ .page_addr = { 0x1500, -1 }, .size = 4 };
    record(&extra);
}
static void remove_before_recheck(void) {
    tb_phys_invalidate__locked(&a);
}
static void assert_defaults(void) {
    for (unsigned i = 0; i < 600; i++) store(0x1400);
    assert(!stats.n_smc_drop_pages && !stats.n_smc_drop_busy &&
           !stats.n_smc_drop_changed && !restored && !exits);
}
static void allocation_stress(void) {
    unsigned total_invalidated = 0, total_collections = 0, miss_sum = 0;
    for (unsigned i = 0; i < 2048; i++) {
        setup();
        for (unsigned j = 0; j < 300 + i % 97; j++) store(0x1400);
        if (!(a.cflags & INVALID)) store(0x1100);
        if (a.cflags & INVALID) {
            a.cflags = 0;
            struct page_collection *set = page_collection_lock(0x1000,
                                                               0x1fff);
            record(&a); page_collection_unlock(set);
        }
        for (unsigned j = 0; j < 32; j++) store(0x1400);
        store(0x1100);
        total_invalidated += invalidated;
        total_collections += collection_locks;
        miss_sum += pages[1].smc_miss;
    }
    for (unsigned i = 0; i < 16; i++) {
        page_lock(&pages[i]); page_code_bitmap_drop(&pages[i]);
        page_unlock(&pages[i]);
    }
    assert(bitmap_live == 0 && bitmap_allocs == bitmap_frees);
    assert(bitmap_peak <= 16 * TARGET_PAGE_SIZE / 8);
    printf("TRACE size=%zu bitmap=%u/%u peak=%u inv=%u collections=%u "
           "miss=%u live=%u\n", sizeof(PageDesc), bitmap_allocs, bitmap_frees,
           bitmap_peak, total_invalidated, total_collections, miss_sum,
           bitmap_live);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    if (strcmp(argv[1], "unset")) setenv("XEMU_WASM_SMC_DROP", argv[1], 1);
    else unsetenv("XEMU_WASM_SMC_DROP");
    setenv("XEMU_WASM_SMC_PAGE", argv[2], 1);
    int mode, threshold;
    smc_config(&mode, &threshold);
    setup();
    if (mode != 3) {
        assert_defaults();
        if (mode == 0) assert(!invalidated && !code_dirty[1]);
        else assert(invalidated == 2 && code_dirty[1]);
        allocation_stress();
        return 0;
    }

    /* Consecutive window, successful full drop, lock coverage, no replay. */
    page_lock(&pages[1]); page_code_bitmap_build(&pages[1]);
    page_unlock(&pages[1]);
    for (unsigned i = 0; i < SMC_DROP_INITIAL - 1; i++) store(0x1400);
    assert(!invalidated && !code_dirty[1]);
    store(0x1400);
    assert(stats.n_smc_drop_pages == 1 && invalidated == 2);
    assert(!pages[1].first_tb && !pages[3].first_tb && code_dirty[1]);
    assert(!restored && !exits && committed == SMC_DROP_INITIAL);
    assert(smc_drop_limit(&pages[1]) == 2 * SMC_DROP_INITIAL);
    unsigned locks = collection_locks;
    for (unsigned i = 0; i < 1000; i++) store(0x1400);
    assert(collection_locks == locks); /* subsequent stores bypass code work */

    /* Re-publication (also hash revival) protects and resets the window. */
    a.cflags = 0;
    struct page_collection *set = page_collection_lock(0x1000, 0x1fff);
    record(&a); page_collection_unlock(set);
    assert(!code_dirty[1] && !smc_drop_count(&pages[1]));
    assert(smc_drop_limit(&pages[1]) == 2 * SMC_DROP_INITIAL);
    page_lock(&pages[1]);
    pages[1].smc_miss += 10;
    page_code_bitmap_drop(&pages[1]);
    page_unlock(&pages[1]);
    assert(!smc_drop_count(&pages[1]));
    assert(smc_drop_limit(&pages[1]) == 2 * SMC_DROP_INITIAL);
    page_lock(&pages[1]); page_code_bitmap_build(&pages[1]);
    page_unlock(&pages[1]);
    for (unsigned i = 0; i < 2 * SMC_DROP_INITIAL - 1; i++) store(0x1400);
    assert(stats.n_smc_drop_pages == 1);
    store(0x1400);
    assert(stats.n_smc_drop_pages == 2 && code_dirty[1]);
    assert(smc_drop_limit(&pages[1]) == 4 * SMC_DROP_INITIAL);

    /* Active first OR second page, unknown GETPC, missing CPU/GETPC. */
    for (unsigned kind = 0; kind < 5; kind++) {
        setup(); ready(1);
        if (kind == 0) current = &a;
        if (kind == 1) {
            current = &b;
            /* Test against its second physical page instead. */
            ready(3); store(0x3400);
        } else if (kind == 2) { unknown_ra = true; store(0x1400); }
        else if (kind >= 3) {
            tb_invalidate_phys_range_fast(kind == 3 ? NULL : &cpu, 0x1400,
                                          4, kind == 4 ? 0 : 123);
        } else store(0x1400);
        assert(stats.n_smc_drop_busy == 1 && !stats.n_smc_drop_pages);
        assert(!invalidated && !restored && !exits);
        assert(smc_drop_limit(&pages[kind == 1 ? 3 : 1]) ==
               2 * SMC_DROP_INITIAL);
    }

    /* Coverage can change between the small lock and collection lock. */
    setup(); ready(1); lock_hook = publish_at_store; store(0x1400);
    assert(stats.n_smc_drop_changed == 1 && !stats.n_smc_drop_pages);
    assert(invalidated == 1 && (extra.cflags & INVALID));
    assert(!(a.cflags & INVALID) && !(b.cflags & INVALID));
    assert(!code_dirty[1] && !smc_drop_count(&pages[1]));
    setup(); ready(1); lock_hook = remove_before_recheck; store(0x1400);
    assert(stats.n_smc_drop_changed == 1 && !stats.n_smc_drop_pages);
    assert(invalidated == 1 && !(b.cflags & INVALID));
    setup(); ready(1); lock_hook = publish_elsewhere; store(0x1400);
    assert(stats.n_smc_drop_changed == 1 && !stats.n_smc_drop_pages);
    assert(!invalidated && !smc_drop_count(&pages[1]));

    /* A whole-cache flush resets the streak but retains learned backoff. */
    setup(); ready(1); pages[1].smc_miss |= 3u << SMC_DROP_COUNT_BITS;
    void *root = pages;
    tb_remove_all_1(0, &root);
    assert(!pages[1].first_tb && !pages[1].code_bitmap);
    assert(!smc_drop_count(&pages[1]) && smc_drop_limit(&pages[1]) == 2048);

    /* A real code hit still takes precise SMC, not speculative eviction. */
    setup(); ready(1); current = &a;
    if (!setjmp(unwind)) { store(0x1100); abort(); }
    assert(restored == 1 && exits == 1 && committed == 0);
    assert(cpu.cflags_next_tb == (1 | CF_NOIRQ | curr_cflags(&cpu)));
    assert(!stats.n_smc_drop_pages && !smc_drop_count(&pages[1]));
    assert((a.cflags & INVALID) && !(b.cflags & INVALID));

    /* Saturating backoff, including repeated attempts at the cap. */
    PageDesc p = { 0 };
    for (unsigned i = 0; i < 40; i++) {
        unsigned old = smc_drop_limit(&p);
        p.smc_miss |= old - 1;
        assert(smc_drop_miss(&p)); smc_drop_backoff(&p);
        assert(!smc_drop_count(&p) && smc_drop_limit(&p) >= old);
        assert(smc_drop_limit(&p) <= SMC_DROP_MAX);
    }
    assert(smc_drop_limit(&p) == SMC_DROP_MAX);
    allocation_stress();
    puts("PASS: actual SMC drop policy, backoff, publication, spanning TBs, "
         "lock recheck, current/unknown TB guard, precise-SMC replay");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="test-wasm-smc-drop-") as tmp:
    p = Path(tmp)
    (p / "test.c").write_text(C)
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
                    "-O2", "-Wall", "-Werror", "-Wno-unused-function",
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    str(p / "test.c"), "-o", str(p / "test")], check=True)
    for drop in ["unset", "0", "1", "2", "invalid"]:
        for mode in ["0", "1", "2"]:
            subprocess.run([str(p / "test"), drop, mode],
                           check=True, timeout=10)
    print("PASS: disabled and legacy SMC policies; 15 configurations")
