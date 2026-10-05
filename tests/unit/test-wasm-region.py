#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual C shared-region builder/ownership code with native Wasm.

Requires emcc and Node with Wasm tail calls; run with CC=emcc (the default).
The tiny TB fixtures omit guest semantics; emulator unwind/save tests are
required separately. No copy of the production routing algorithm lives here.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "tcg/wasm32.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?[^\n]*\b" + name + r"\(", SOURCE, re.M)
    assert match, name
    brace = SOURCE.index("{", match.start())
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                         r"'(?:\\.|[^'\\])*'|[{}]", SOURCE[brace:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[match.start():brace + token.end()] + "\n"
    raise AssertionError(name)


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <emscripten.h>
typedef struct CPUArchState CPUArchState;
#define QEMU_BUILD_BUG_ON(c) _Static_assert(!(c), #c)
#include "tcg/wasm32.h"
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define g_new0(type, count) ((type *)calloc(count, sizeof(type)))
#define tcg_splitwx_to_rx(p) ((void *)(p))
#define TB_EXIT_REQUESTED 3
#define g_realloc realloc
#define g_free free
#define g_assert assert
#define g_clear_pointer(p, f) do { f(*(p)); *(p) = NULL; } while (0)
static void *g_memdup2(const void *p, size_t n) {
    void *q = malloc(n); assert(q); memcpy(q, p, n); return q;
}
#define qatomic_read(p) (*(p))
#define XPHASE_SET(...) do {} while (0)
#define XTBPHASE_SET(...) do {} while (0)
#define XSTAT_INC(f) (xemu_wasm_stats.f++)
#define XSTAT_ADD(f,n) (xemu_wasm_stats.f += (n))
static struct {
    uint64_t n_jit_compile, ns_jit_compile, n_region_compile, n_region_members;
    uint64_t n_tb_exec, n_tb_region, n_tci_dropped, n_jit_evict;
    uint64_t n_region_shared, n_region_aliases, n_region_cap;
    uint64_t n_region_bytes_max, n_region_cfg_max, n_region_depth_max;
    uint64_t n_direct_region_build, n_direct_region_members, n_direct_build;
    uint64_t n_direct_flags_build, n_direct_jcc_build, n_direct_exec;
    uint64_t n_jit_exit_direct, n_jit_exit_chain0, n_jit_exit_chain1;
    uint64_t n_jit_exit_requested, n_tb_selfloop;
    uint64_t n_direct_checked, n_direct_flags_checked, n_direct_jcc_checked;
    uint64_t n_direct_region_checked, n_direct_mismatch;
    uint64_t n_direct_mixed_build, n_direct_mixed_members;
    uint64_t n_direct_mixed_legacy_members, n_direct_mixed_checked;
    uint64_t n_direct_mixed_legacy_rewind, n_direct_mixed_memory_tb_checked;
    uint64_t n_direct_memory_build, n_direct_memory_members;
} xemu_wasm_stats;
/* These register-only fixtures must never execute a memory integration hook. */
void wasm32_direct_mem_begin(CPUArchState *e, uint32_t p) { abort(); }
void wasm32_direct_mem_observe(CPUArchState *e, uint32_t p, uint32_t a,
                              uint32_t o, uint32_t s, uint64_t v) { abort(); }
uint32_t wasm32_direct_mem_load(CPUArchState *e, uint32_t p, uint32_t a,
                              uint32_t o) { abort(); }
void wasm32_direct_mem_store(CPUArchState *e, uint32_t p, uint32_t a,
                            uint32_t o, uint32_t v) { abort(); }
void wasm32_direct_mem_end(CPUArchState *e) { abort(); }
void wasm32_direct_mem_cancel(void) { abort(); }
void wasm32_direct_mem_miss(CPUArchState *e) { abort(); }
void wasm32_direct_mem_miss_classify(CPUArchState *e, uint32_t a,
                                    uint32_t t, uint32_t p, uint32_t m,
                                    uint32_t s)
{ abort(); }
#define MAX_INSTANCES 16
static WasmInstance instances[MAX_INSTANCES];
static int free_slots[MAX_INSTANCES], n_free = -1, instances_alive, clock_hand;
static bool region_shared = true;
static int region_hints = 1;
static bool direct_metadata;
static double jit_debt_ms;
int wasm32_jit_threshold = 64;
bool wasm32_ic_enabled(void) { return false; }
bool wasm32_ic_for_exit(bool indirect) { return false; }
static void jit_budget_update(double now) { (void)now; }
#define TB_JMP_OFFSET_INVALID UINT16_MAX
typedef struct TranslationBlock {
    struct { void *ptr; } tc;
    uint16_t jmp_reset_offset[2];
    uintptr_t jmp_target_addr[2];
    uint32_t icount;
} TranslationBlock;
static WasmTBHeader headers[8];
static TranslationBlock tbs[8];
static uint32_t next_header[8], visits[8], member_phase;
static TranslationBlock *tcg_tb_lookup(uintptr_t h) {
    for (int i = 0; i < 8; i++) {
        if (h == (uintptr_t)&headers[i]) { return &tbs[i]; }
    }
    return NULL;
}
EM_JS(int, wasm32_instantiate, (const uint8_t *p, int size,
        const uint32_t *imports, int nimports, int s0, int s1, int ic_count), {
    if (nimports || s0 || s1 || ic_count) {
        throw Error('unexpected fixture import');
    }
    const mod = new WebAssembly.Module(HEAPU8.slice(p, p + size));
    const inst = new WebAssembly.Instance(mod, {
        env: { buffer: wasmMemory }, helper: { u: () => 1 },
        chain: { s0: () => 0, s1: () => 0,
                 call: (ctx, f) => getWasmTableEntry(f)(ctx) }
    });
    const f = addFunction(inst.exports.start, 'ii');
    inst.exports.ic_self.value = f;
    return f;
});
EM_JS(void, wasm32_remove_function, (int f), { removeFunction(f); });
typedef struct ByteBuf { uint8_t *p; size_t len, cap; } ByteBuf;
'''

SUFFIX = r'''
static void fixture(int id, int successor)
{
    WasmTBHeader *h = &headers[id];
    ByteBuf mod = {0}, sec = {0}, body = {0};
    WasmReloc *reloc = calloc(3, sizeof(*reloc));
    static WasmReloc unused_rewind_reloc;
    /* The builder only reads this module's type section and body fragments. */
    bb_bytes(&mod, "\0asm\x01\0\0\0", 8);
    bb_uleb(&sec, 3);
    bb_bytes(&sec, "\x60\x01\x7f\x01\x7f", 5);
    bb_bytes(&sec, "\x60\x00\x01\x7f", 4);
    bb_bytes(&sec, "\x60\x02\x7f\x7f\x01\x7f", 6);
    bb_section(&mod, 1, &sec);
    if (wasm32_tlb_hint_enabled()) {
        /* Catch overlap between hint slots and the region's routing locals. */
        bb_u8(&body, 0x41); bb_sleb(&body, 0xabcdef);
        bb_u8(&body, 0x21); bb_uleb(&body, WASM_TLB_HINT_PTR_LOCAL);
        bb_u8(&body, 0x41); bb_sleb(&body, 7);
        bb_u8(&body, 0x21); bb_uleb(&body, WASM_TLB_HINT_MMU_LOCAL);
    }
    /* Same three-op shape as emitted class entry/rewind phase stores. */
    bb_u8(&body, 0x41); bb_sleb(&body, (uintptr_t)&member_phase);
    bb_u8(&body, 0x41); bb_sleb(&body, id + 1);
    bb_u8(&body, 0x36); bb_u8(&body, 2); bb_u8(&body, 0);
    bb_u8(&body, 0x03); bb_u8(&body, 0x40); /* own TB loop */
    reloc[0] = (WasmReloc){.off = body.len, .kind = WASM_RELOC_DIRECT_BEGIN};
    bb_u8(&body, 0x41); bb_sleb(&body, (uintptr_t)&visits[id]);
    bb_u8(&body, 0x41); bb_sleb(&body, (uintptr_t)&visits[id]);
    bb_u8(&body, 0x28); bb_u8(&body, 2); bb_u8(&body, 0);
    bb_u8(&body, 0x41); bb_u8(&body, 1); bb_u8(&body, 0x6a);
    bb_u8(&body, 0x36); bb_u8(&body, 2); bb_u8(&body, 0);
    bb_u8(&body, 0x41); bb_sleb(&body, (uintptr_t)&next_header[id]);
    bb_u8(&body, 0x28); bb_u8(&body, 2); bb_u8(&body, 0);
    bb_u8(&body, 0x21); bb_uleb(&body, REGION_L32_0);
    reloc[1] = (WasmReloc){.off = body.len,
        .kind = WASM_RELOC_DIRECT_VERIFY, .arg = 0xff};
    reloc[2] = (WasmReloc){.off = body.len,
        .kind = WASM_RELOC_GOTO, .arg = 0xff};
    bb_u8(&body, 0x41); bb_sleb(&body, 11 * (id + 1));
    bb_u8(&body, 0x0f); bb_u8(&body, 0x0b); /* return; end loop */
    bb_u8(&body, 0x41); bb_sleb(&body, 11 * (id + 1)); bb_u8(&body, 0x0f);
    h->body_off = mod.len; h->body_len = body.len;
    bb_bytes(&mod, body.p, body.len);
    h->body_b_off = mod.len; h->body_b_len = body.len;
    bb_bytes(&mod, body.p, body.len);
    h->wasm_ptr = mod.p; h->wasm_size = mod.len;
    h->reloc_ptr = reloc + (direct_metadata ? 0 : 2);
    h->reloc_count = direct_metadata ? 3 : 1;
    h->reloc_b_ptr = direct_metadata ? reloc : &unused_rewind_reloc;
    h->reloc_b_count = direct_metadata ? 2 : 0;
    h->counter = 1000; h->icount = 1;
    tbs[id].tc.ptr = h; tbs[id].icount = 1;
    tbs[id].jmp_reset_offset[0] = successor >= 0 ? 0 : TB_JMP_OFFSET_INVALID;
    tbs[id].jmp_reset_offset[1] = TB_JMP_OFFSET_INVALID;
    if (successor >= 0) {
        tbs[id].jmp_target_addr[0] = (uintptr_t)&headers[successor];
    }
    free(body.p); free(sec.p);
}
static uint32_t run(int id, int f)
{
    WasmContext ctx = { .tb_ptr = &headers[id], .do_init = 1 };
    return ((uint32_t (*)(WasmContext *))(uintptr_t)f)(&ctx);
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    setenv("XEMU_WASM_TLB_HINT", argv[1], 1);
    setenv("XEMU_WASM_TB_STATS", argv[2], 1);
    direct_metadata = argv[3][0] == '1';
    for (int i = 0; i < 8; i++) fixture(i, i % 2 == 0 ? i + 1 : -1);
    /* Opaque direct scaffold modules must never enter the legacy merger. */
    uint32_t body_len = headers[0].body_len;
    headers[0].body_len = 0;
    assert(!region_member_ok(&headers[0]));
    assert(compile_region(&headers[0]) == 0);
    headers[0].body_len = body_len;
    int ab = compile_region(&headers[0]);
    int cd = compile_region(&headers[2]);
    if (!(ab > 0 && cd > 0 && ab != cd)) {
        printf("fixture: ab=%d cd=%d caps=%llu cfg=%u/%u "
               "sizes=%u/%u target=%p child=%p\n",
               ab, cd, (unsigned long long)xemu_wasm_stats.n_region_cap,
               headers[0].cfg_score, headers[1].cfg_score,
               headers[0].body_len, headers[1].body_len,
               tb_link_target(&tbs[0], 0), &headers[1]);
    }
    assert(ab > 0 && cd > 0 && ab != cd);
    assert(get_instance(&headers[0]) == ab && get_instance(&headers[1]) == ab);
    assert(get_instance(&headers[2]) == cd && get_instance(&headers[3]) == cd);
    /* Never recompile a live member. */
    assert(compile_region(&headers[1]) == 0);
    assert(run(1, ab) == 22 && visits[0] == 0 && visits[1] == 1);
    assert(member_phase == 2);
    next_header[0] = (uintptr_t)&headers[1];
    assert(run(0, ab) == 22 && visits[0] == 1 && visits[1] == 2);
    assert(member_phase == 2); /* root's phase must not leak into child */
    next_header[0] = (uintptr_t)&headers[2];
    assert(run(0, ab) == 11 && visits[2] == 0); /* foreign region */
    assert(member_phase == 1);
    WasmInstance *old = headers[0].instance;
    headers[6].instance = old; headers[6].instance_member = 1;
    next_header[0] = (uintptr_t)&headers[6];
    assert(run(0, ab) == 11 && visits[1] == 2); /* stale same-owner alias */
    headers[6].instance_member = UINT32_MAX;
    assert(run(0, ab) == 11); /* index checked before member-table load */
    headers[6].instance = NULL;
    assert(run(0, ab) == 11);
    next_header[0] = 0;
    assert(run(0, ab) == 11);
    /* Eviction must not dereference or clear possibly recycled header bytes. */
    WasmTBHeader saved = headers[1];
    old->used = 0; clock_hand = old - instances;
    remove_instances();
    assert(memcmp(&saved, &headers[1], sizeof(saved)) == 0);
    assert(!old->members && !old->n_members);
    /* Regression: a selected child can still reference the slot about to be
     * reused, with the same index that its new group will assign it. */
    headers[5].instance = old; headers[5].instance_member = 1;
    int ef = compile_region(&headers[4]);
    assert(ef > 0 && headers[4].instance == old);
    assert(!wasm_instance_owns(old, &headers[1]));
    next_header[4] = (uintptr_t)&headers[1];
    assert(run(4, ef) == 55 && visits[5] == 0);
    assert(get_instance(&headers[1]) == 0);
    next_header[4] = (uintptr_t)&headers[5];
    assert(run(4, ef) == 66);
    assert(member_phase == 6);
    /* Exercise real region B-body selection, not actual Asyncify suspension. */
    next_header[5] = 0;
    WasmContext rewind = { .tb_ptr = &headers[5], .do_init = 0,
                          .rewind_func = ef };
    member_phase = 0;
    assert(((uint32_t (*)(WasmContext *))(uintptr_t)ef)(&rewind) == 66);
    assert(member_phase == 6);
    memset(&headers[5], 0, sizeof(headers[5]));
    assert(get_instance(&headers[5]) == 0); /* new header after TB flush */
    unsigned score, depth;
    const uint8_t valid[] = {3, 0x40, 0x41, 0, 0x0d, 0, 0x0b};
    assert(region_cfg(valid, sizeof(valid), &score, &depth));
    assert(depth == 1 && score == 2);
    const uint8_t bad[] = {0x41, 0x80};
    assert(!region_cfg(bad, sizeof(bad), &score, &depth));
    assert(!region_cfg((uint8_t[]){0xfd}, 1, &score, &depth));
    uint8_t nested[(REGION_DEPTH_MAX + 1) * 3];
    for (unsigned i = 0; i <= REGION_DEPTH_MAX; i++) {
        nested[2*i] = 2; nested[2*i+1] = 0x40;
        nested[2*(REGION_DEPTH_MAX+1)+i] = 0x0b;
    }
    assert(!region_cfg(nested, sizeof(nested), &score, &depth));
    uint8_t blocks[(REGION_CFG_MAX + 1) * 3];
    for (unsigned i = 0; i <= REGION_CFG_MAX; i++) {
        blocks[3*i] = 2; blocks[3*i+1] = 0x40; blocks[3*i+2] = 0x0b;
    }
    assert(!region_cfg(blocks, sizeof(blocks), &score, &depth));
    assert(xemu_wasm_stats.n_tb_exec == (argv[2][0] == '1' ? 2 : 0));
    assert(xemu_wasm_stats.n_tb_region == (argv[2][0] == '1' ? 2 : 0));
    assert(xemu_wasm_stats.n_region_shared == 3);
    assert(xemu_wasm_stats.n_region_aliases == 3);
    puts("PASS: native multi-entry routing, stale/index/foreign guards, "
         "slot reuse, CFG limits; no-code direct metadata ignored");
    return 0;
}
'''


def main():
    defines = "\n".join(re.findall(r"^#define REGION_[^\n]*", SOURCE, re.M))
    names = ["wasm32_tlb_hint_enabled", "bb_need", "bb_u8", "bb_bytes",
             "bb_uleb", "bb_sleb", "bb_name",
             "bb_section", "rd_uleb", "tb_helper_types", "tb_link_target",
             "region_leb", "region_cfg", "region_member_ok", "bb_count",
             "region_guard", "region_shared_guard", "add_instance",
             "get_instance", "remove_instances", "compile_region"]
    stats = (ROOT / 'include/qemu/xemu-wasm-stats.h').read_text()
    policy = '\n'.join(re.search(r'static inline bool ' + name +
                        r'\(void\).*?\n}', stats, re.S).group() for name in
                        ['xemu_wasm_tb_stats_enabled',
                         'xemu_wasm_direct_profile_enabled'])
    code = (PREFIX + policy + "\n" + defines + "\n" +
            "\n".join(function(n) for n in names if n != 'compile_region') +
            '\n#include "tcg/wasm32-direct-region.c.inc"\n' +
            function('compile_region') + SUFFIX)
    with tempfile.TemporaryDirectory(prefix="test-wasm-region-") as tmp:
        src = Path(tmp) / "test.c"
        out = Path(tmp) / "test.js"
        src.write_text(code)
        subprocess.run([*shlex.split(os.environ.get("CC", "emcc")), "-O1",
                        "-I", str(ROOT), "-I", str(ROOT/'include'),
                        "-pthread", "-sENVIRONMENT=node",
                        "-sINITIAL_MEMORY=32MB", "-sALLOW_TABLE_GROWTH=1",
                        "-sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction",
                        "-sEXIT_RUNTIME=1", str(src), "-o", str(out)],
                       check=True)
        for hint in [0, 1]:
            for stats in [0, 1]:
                for metadata in [0, 1]:
                    command = [*shlex.split(os.environ.get("NODE", "node")),
                               str(out), str(hint), str(stats), str(metadata)]
                    subprocess.run(command, check=True, timeout=30)


if __name__ == "__main__":
    main()
